#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 12)

#include "RemoteFs.h"

#include "../RemoteLocation.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QStandardPaths>

#include <fuse.h>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace
{

constexpr unsigned long FuseSuperMagic = 0x65735546;
RemoteFs *s_instance = nullptr;

// Per open file or directory below a host.
struct Handle {
    int fd = -1;
    DIR *dir = nullptr;
    RemoteFs::Mount *mount = nullptr;
    std::mutex dirMutex;
};

Handle *handleOf(const fuse_file_info *fi)
{
    return fi ? reinterpret_cast<Handle *>(fi->fh) : nullptr;
}

int errnoResult(int result)
{
    return result < 0 ? -errno : result;
}

QString programPath(const QString &name)
{
    const QString path = QStandardPaths::findExecutable(name);
    return path.isEmpty() ? name : path;
}

// Runs a helper program without our file descriptors (the FUSE device, host
// root fds) and with the default signal mask (the daemon blocks the
// termination signals in all threads). Returns the exit code, -1 if it could
// not run or timed out.
int runProgram(const QString &program,
               const QStringList &args,
               int timeoutSeconds,
               QString *output = nullptr,
               const QProcessEnvironment &env = QProcessEnvironment::systemEnvironment())
{
    QProcess process;
    process.setProgram(programPath(program));
    process.setArguments(args);
    process.setProcessEnvironment(env);
    process.setStandardInputFile(QProcess::nullDevice());
    process.setProcessChannelMode(QProcess::MergedChannels);
    // A new session: no controlling terminal, so ssh asks through SSH_ASKPASS.
    process.setUnixProcessParameters(QProcess::UnixProcessFlag::CloseFileDescriptors | QProcess::UnixProcessFlag::ResetSignalHandlers
                                     | QProcess::UnixProcessFlag::CreateNewSession);
    process.setChildProcessModifier([]() {
        sigset_t set;
        sigemptyset(&set);
        pthread_sigmask(SIG_SETMASK, &set, nullptr);
    });
    process.start();
    if (!process.waitForStarted()) {
        if (output) {
            *output = process.errorString();
        }
        return -1;
    }
    const bool finished = process.waitForFinished(timeoutSeconds * 1000);
    if (!finished) {
        process.kill();
        process.waitForFinished(2000);
    }
    if (output) {
        *output = QString::fromLocal8Bit(process.readAll()).trimmed();
        if (!finished) {
            *output = QStringLiteral("timed out after %1 s").arg(timeoutSeconds);
        }
    }
    if (!finished || process.exitStatus() != QProcess::NormalExit) {
        return -1;
    }
    return process.exitCode();
}

QString unescapeMountInfo(const QByteArray &field)
{
    QByteArray result;
    for (qsizetype i = 0; i < field.size(); ++i) {
        if (field[i] == '\\' && i + 3 < field.size()) {
            bool ok = false;
            const int value = field.mid(i + 1, 3).toInt(&ok, 8);
            if (ok) {
                result.append(char(value));
                i += 3;
                continue;
            }
        }
        result.append(field[i]);
    }
    return QString::fromLocal8Bit(result);
}

struct MountInfo {
    QString mountPoint;
    QString fsType;
};

std::vector<MountInfo> readMountInfo()
{
    std::vector<MountInfo> mounts;
    QFile file(QStringLiteral("/proc/self/mountinfo"));
    if (!file.open(QIODevice::ReadOnly)) {
        return mounts;
    }
    for (const QByteArray &line : file.readAll().split('\n')) {
        const QList<QByteArray> fields = line.split(' ');
        const qsizetype separator = fields.indexOf(QByteArray("-"));
        if (fields.size() < 5 || separator < 0 || separator + 1 >= fields.size()) {
            continue;
        }
        mounts.push_back({unescapeMountInfo(fields[4]), QString::fromLatin1(fields[separator + 1])});
    }
    return mounts;
}

bool isMountPoint(const QString &path)
{
    for (const auto &mount : readMountInfo()) {
        if (mount.mountPoint == path) {
            return true;
        }
    }
    return false;
}

bool unmountPath(const QString &path, bool lazy)
{
    QStringList args{QStringLiteral("-u"), QStringLiteral("-q")};
    if (lazy) {
        args << QStringLiteral("-z");
    }
    args << path;
    return runProgram(QStringLiteral("fusermount3"), args, 30) == 0;
}

void virtualDirStat(struct stat *st)
{
    static const time_t started = time(nullptr);
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFDIR | 0755;
    st->st_nlink = 2;
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_atime = st->st_mtime = st->st_ctime = started;
}

std::string linkDirOf(const std::string &rel)
{
    const size_t slash = rel.rfind('/');
    return slash == std::string::npos ? std::string() : rel.substr(0, slash);
}

// --- FUSE callbacks ---

void *fsInit(fuse_conn_info *conn, fuse_config *cfg)
{
    // Lookups in one directory (e.g. two hosts under sftp/) must not wait
    // for each other while one host connects.
    if (conn->capable & FUSE_CAP_PARALLEL_DIROPS) {
        conn->want |= FUSE_CAP_PARALLEL_DIROPS;
    }
    cfg->use_ino = 0;
    cfg->nullpath_ok = 0;
    // Operations on unlinked open files use their fd; no .fuse_hidden files
    // on the remote.
    cfg->hard_remove = 1;
    // sshfs caches; caching again here would only delay remote changes.
    cfg->entry_timeout = 0;
    cfg->attr_timeout = 0;
    cfg->negative_timeout = 0;
    cfg->kernel_cache = 0;
    return s_instance;
}

int fsGetattr(const char *path, struct stat *st, fuse_file_info *fi)
{
    if (Handle *handle = handleOf(fi); handle && handle->fd >= 0) {
        return errnoResult(fstat(handle->fd, st));
    }
    auto r = s_instance->resolve(path);
    switch (r.kind) {
    case RemoteFs::Kind::Root:
    case RemoteFs::Kind::Scheme:
        virtualDirStat(st);
        return 0;
    case RemoteFs::Kind::Invalid:
        return -ENOENT;
    case RemoteFs::Kind::Host:
        break;
    }
    return s_instance->withMount(r.mount, [&](int root) {
        return errnoResult(fstatat(root, r.rel.c_str(), st, AT_SYMLINK_NOFOLLOW));
    });
}

int fsAccess(const char *path, int mask)
{
    auto r = s_instance->resolve(path);
    switch (r.kind) {
    case RemoteFs::Kind::Root:
    case RemoteFs::Kind::Scheme:
        return (mask & W_OK) ? -EACCES : 0;
    case RemoteFs::Kind::Invalid:
        return -ENOENT;
    case RemoteFs::Kind::Host:
        break;
    }
    return s_instance->withMount(r.mount, [&](int root) {
        return errnoResult(faccessat(root, r.rel.c_str(), mask, 0));
    });
}

int fsReadlink(const char *path, char *buf, size_t size)
{
    auto r = s_instance->resolve(path);
    if (r.kind != RemoteFs::Kind::Host) {
        return r.kind == RemoteFs::Kind::Invalid ? -ENOENT : -EINVAL;
    }
    std::string target;
    const int result = s_instance->withMount(r.mount, [&](int root) {
        std::vector<char> link(PATH_MAX + 1);
        const ssize_t n = readlinkat(root, r.rel.c_str(), link.data(), link.size() - 1);
        if (n < 0) {
            return -errno;
        }
        target.assign(link.data(), size_t(n));
        return 0;
    });
    if (result < 0) {
        return result;
    }
    target = KateRemote::hostSymlinkTarget(linkDirOf(r.rel), target);
    if (size == 0) {
        return -EINVAL;
    }
    const size_t n = std::min(size - 1, target.size());
    memcpy(buf, target.data(), n);
    buf[n] = '\0';
    return 0;
}

int fsOpendir(const char *path, fuse_file_info *fi)
{
    auto r = s_instance->resolve(path);
    switch (r.kind) {
    case RemoteFs::Kind::Root:
    case RemoteFs::Kind::Scheme:
        fi->fh = 0;
        return 0;
    case RemoteFs::Kind::Invalid:
        return -ENOENT;
    case RemoteFs::Kind::Host:
        break;
    }
    return s_instance->withMount(r.mount, [&](int root) {
        const int fd = openat(root, r.rel.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            return -errno;
        }
        DIR *dir = fdopendir(fd);
        if (!dir) {
            const int error = errno;
            close(fd);
            return -error;
        }
        auto *handle = new Handle;
        handle->dir = dir;
        handle->mount = r.mount;
        r.mount->openHandles++;
        fi->fh = reinterpret_cast<uint64_t>(handle);
        return 0;
    });
}

int fsReaddir(const char *path, void *buf, fuse_fill_dir_t filler, off_t, fuse_file_info *fi, fuse_readdir_flags)
{
    const auto fill = [&](const char *name) {
        filler(buf, name, nullptr, 0, fuse_fill_dir_flags(0));
    };
    if (Handle *handle = handleOf(fi)) {
        std::lock_guard lock(handle->dirMutex);
        handle->mount->lastUse = RemoteFs::now();
        rewinddir(handle->dir);
        errno = 0;
        while (dirent *entry = readdir(handle->dir)) {
            // The type saves consumers an lstat (a round trip) per entry.
            struct stat st {};
            st.st_ino = entry->d_ino;
            st.st_mode = DTTOIF(entry->d_type);
            filler(buf, entry->d_name, &st, 0, fuse_fill_dir_flags(0));
        }
        return -errno;
    }
    auto r = s_instance->resolve(path);
    fill(".");
    fill("..");
    if (r.kind == RemoteFs::Kind::Root) {
        fill("sftp");
    } else if (r.kind == RemoteFs::Kind::Scheme) {
        // Only connected hosts: listing must not connect anywhere (file
        // managers and indexers list directories freely).
        const QString prefix = QString::fromStdString(r.scheme) + QLatin1Char('/');
        for (const QString &host : s_instance->mountedHosts()) {
            if (host.startsWith(prefix)) {
                fill(host.mid(prefix.size()).toUtf8().constData());
            }
        }
    }
    return 0;
}

int fsReleasedir(const char *, fuse_file_info *fi)
{
    if (Handle *handle = handleOf(fi)) {
        closedir(handle->dir);
        handle->mount->openHandles--;
        delete handle;
    }
    return 0;
}

// Operations that change a path below a host.
template<typename Op>
int hostOp(const char *path, Op op)
{
    auto r = s_instance->resolve(path);
    switch (r.kind) {
    case RemoteFs::Kind::Root:
    case RemoteFs::Kind::Scheme:
        return -EACCES;
    case RemoteFs::Kind::Invalid:
        return -ENOENT;
    case RemoteFs::Kind::Host:
        break;
    }
    return s_instance->withMount(r.mount, [&](int root) {
        return op(root, r.rel.c_str());
    });
}

// Creating or removing an entry: never the host directory itself (that
// would connect to a host named like the new entry).
template<typename Op>
int entryOp(const char *path, Op op)
{
    auto r = s_instance->resolve(path);
    if ((r.kind == RemoteFs::Kind::Host && r.rel == ".") || r.kind == RemoteFs::Kind::Invalid) {
        return -EACCES;
    }
    return hostOp(path, op);
}

int fsMkdir(const char *path, mode_t mode)
{
    return entryOp(path, [&](int root, const char *rel) {
        return errnoResult(mkdirat(root, rel, mode));
    });
}

int fsMknod(const char *path, mode_t mode, dev_t dev)
{
    return entryOp(path, [&](int root, const char *rel) {
        return errnoResult(mknodat(root, rel, mode, dev));
    });
}

int fsUnlink(const char *path)
{
    return entryOp(path, [&](int root, const char *rel) {
        return errnoResult(unlinkat(root, rel, 0));
    });
}

int fsRmdir(const char *path)
{
    return entryOp(path, [&](int root, const char *rel) {
        if (strcmp(rel, ".") == 0) {
            return -EBUSY;
        }
        return errnoResult(unlinkat(root, rel, AT_REMOVEDIR));
    });
}

int fsSymlink(const char *target, const char *path)
{
    return entryOp(path, [&](int root, const char *rel) {
        return errnoResult(symlinkat(target, root, rel));
    });
}

// Two paths on the same host, else EXDEV.
template<typename Op>
int hostOp2(const char *from, const char *to, Op op)
{
    auto a = s_instance->resolve(from);
    auto b = s_instance->resolve(to);
    if (a.kind == RemoteFs::Kind::Invalid || b.kind == RemoteFs::Kind::Invalid) {
        return -ENOENT;
    }
    if (a.kind != RemoteFs::Kind::Host || b.kind != RemoteFs::Kind::Host) {
        return -EACCES;
    }
    if (a.mount != b.mount) {
        return -EXDEV;
    }
    if (a.rel == "." || b.rel == ".") {
        return -EACCES;
    }
    return s_instance->withMount(a.mount, [&](int root) {
        return op(root, a.rel.c_str(), b.rel.c_str());
    });
}

int fsRename(const char *from, const char *to, unsigned int flags)
{
    return hostOp2(from, to, [&](int root, const char *a, const char *b) {
        return errnoResult(flags ? renameat2(root, a, root, b, flags) : renameat(root, a, root, b));
    });
}

int fsLink(const char *from, const char *to)
{
    return hostOp2(from, to, [&](int root, const char *a, const char *b) {
        return errnoResult(linkat(root, a, root, b, 0));
    });
}

int fsChmod(const char *path, mode_t mode, fuse_file_info *fi)
{
    if (Handle *handle = handleOf(fi); handle && handle->fd >= 0) {
        return errnoResult(fchmod(handle->fd, mode));
    }
    return hostOp(path, [&](int root, const char *rel) {
        return errnoResult(fchmodat(root, rel, mode, 0));
    });
}

int fsChown(const char *path, uid_t uid, gid_t gid, fuse_file_info *fi)
{
    if (Handle *handle = handleOf(fi); handle && handle->fd >= 0) {
        return errnoResult(fchown(handle->fd, uid, gid));
    }
    return hostOp(path, [&](int root, const char *rel) {
        return errnoResult(fchownat(root, rel, uid, gid, AT_SYMLINK_NOFOLLOW));
    });
}

int fsTruncate(const char *path, off_t size, fuse_file_info *fi)
{
    if (Handle *handle = handleOf(fi); handle && handle->fd >= 0) {
        return errnoResult(ftruncate(handle->fd, size));
    }
    return hostOp(path, [&](int root, const char *rel) {
        const int fd = openat(root, rel, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            return -errno;
        }
        const int result = errnoResult(ftruncate(fd, size));
        close(fd);
        return result;
    });
}

int fsUtimens(const char *path, const timespec tv[2], fuse_file_info *fi)
{
    if (Handle *handle = handleOf(fi); handle && handle->fd >= 0) {
        return errnoResult(futimens(handle->fd, tv));
    }
    return hostOp(path, [&](int root, const char *rel) {
        return errnoResult(utimensat(root, rel, tv, AT_SYMLINK_NOFOLLOW));
    });
}

int openFile(const char *path, fuse_file_info *fi, int flags, mode_t mode)
{
    auto r = s_instance->resolve(path);
    switch (r.kind) {
    case RemoteFs::Kind::Root:
    case RemoteFs::Kind::Scheme:
        return (flags & O_CREAT) ? -EACCES : -EISDIR;
    case RemoteFs::Kind::Invalid:
        return (flags & O_CREAT) ? -EACCES : -ENOENT;
    case RemoteFs::Kind::Host:
        break;
    }
    if ((flags & O_CREAT) && r.rel == ".") {
        return -EACCES;
    }
    return s_instance->withMount(r.mount, [&](int root) {
        // The kernel has followed symlinks already; never follow one here.
        const int fd = openat(root, r.rel.c_str(), flags | O_CLOEXEC | O_NOFOLLOW, mode);
        if (fd < 0) {
            return -errno;
        }
        auto *handle = new Handle;
        handle->fd = fd;
        handle->mount = r.mount;
        r.mount->openHandles++;
        fi->fh = reinterpret_cast<uint64_t>(handle);
        return 0;
    });
}

int fsOpen(const char *path, fuse_file_info *fi)
{
    return openFile(path, fi, fi->flags, 0);
}

int fsCreate(const char *path, mode_t mode, fuse_file_info *fi)
{
    return openFile(path, fi, fi->flags | O_CREAT, mode);
}

int fsRead(const char *, char *buf, size_t size, off_t offset, fuse_file_info *fi)
{
    Handle *handle = handleOf(fi);
    handle->mount->lastUse = RemoteFs::now();
    return errnoResult(int(pread(handle->fd, buf, size, offset)));
}

int fsWrite(const char *, const char *buf, size_t size, off_t offset, fuse_file_info *fi)
{
    Handle *handle = handleOf(fi);
    handle->mount->lastUse = RemoteFs::now();
    return errnoResult(int(pwrite(handle->fd, buf, size, offset)));
}

int fsStatfs(const char *path, struct statvfs *st)
{
    auto r = s_instance->resolve(path);
    if (r.kind == RemoteFs::Kind::Host) {
        return s_instance->withMount(r.mount, [&](int root) {
            return errnoResult(fstatvfs(root, st));
        });
    }
    memset(st, 0, sizeof(*st));
    st->f_bsize = st->f_frsize = 4096;
    st->f_namemax = 255;
    return 0;
}

int fsFlush(const char *, fuse_file_info *fi)
{
    // Closing a duplicate makes the backend flush, as close() would.
    Handle *handle = handleOf(fi);
    const int fd = dup(handle->fd);
    return fd < 0 ? -errno : errnoResult(close(fd));
}

int fsRelease(const char *, fuse_file_info *fi)
{
    if (Handle *handle = handleOf(fi)) {
        close(handle->fd);
        handle->mount->openHandles--;
        delete handle;
    }
    return 0;
}

int fsFsync(const char *, int datasync, fuse_file_info *fi)
{
    Handle *handle = handleOf(fi);
    return errnoResult(datasync ? fdatasync(handle->fd) : fsync(handle->fd));
}

const fuse_operations s_operations = [] {
    fuse_operations ops{};
    ops.init = fsInit;
    ops.getattr = fsGetattr;
    ops.access = fsAccess;
    ops.readlink = fsReadlink;
    ops.opendir = fsOpendir;
    ops.readdir = fsReaddir;
    ops.releasedir = fsReleasedir;
    ops.mkdir = fsMkdir;
    ops.mknod = fsMknod;
    ops.unlink = fsUnlink;
    ops.rmdir = fsRmdir;
    ops.symlink = fsSymlink;
    ops.rename = fsRename;
    ops.link = fsLink;
    ops.chmod = fsChmod;
    ops.chown = fsChown;
    ops.truncate = fsTruncate;
    ops.utimens = fsUtimens;
    ops.open = fsOpen;
    ops.create = fsCreate;
    ops.read = fsRead;
    ops.write = fsWrite;
    ops.statfs = fsStatfs;
    ops.flush = fsFlush;
    ops.release = fsRelease;
    ops.fsync = fsFsync;
    return ops;
}();

} // namespace

RemoteFs::RemoteFs(Config config)
    : m_config(std::move(config))
{
    s_instance = this;
    loadHosts();
    QString help;
    runProgram(m_config.sshfsProgram, {QStringLiteral("-h")}, 10, &help);
    m_noContainSymlinks = help.contains(QLatin1String("no_contain_symlinks"));
}

RemoteFs::~RemoteFs()
{
    if (m_fuse) {
        fuse_unmount(m_fuse);
        fuse_destroy(m_fuse);
    }
    s_instance = nullptr;
}

RemoteFs *RemoteFs::instance()
{
    return s_instance;
}

int64_t RemoteFs::now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void RemoteFs::cleanupStaleMounts()
{
    // Host mounts left by a crashed instance. Only our own backend directory,
    // and only unmounted, never removed: the mounts expose remote files.
    const QString backendPrefix = m_config.backendDir + QLatin1Char('/');
    for (const auto &mount : readMountInfo()) {
        if (mount.mountPoint.startsWith(backendPrefix)) {
            qInfo() << "Unmounting stale" << mount.fsType << "mount" << mount.mountPoint;
            unmountPath(mount.mountPoint, true);
        }
    }
}

QString RemoteFs::mount()
{
    const QString root = m_config.mountRoot;
    cleanupStaleMounts();

    for (const auto &mount : readMountInfo()) {
        if (mount.mountPoint != root) {
            continue;
        }
        if (mount.fsType != QLatin1String("fuse.kate-remote")) {
            return QStringLiteral("%1 is already a mount point (%2)").arg(root, mount.fsType);
        }
        // Left by a crashed instance (or another instance still running,
        // which the DBus name already excludes).
        qInfo() << "Unmounting stale mount" << root;
        unmountPath(root, true);
    }

    if (!QDir().mkpath(m_config.backendDir) || chmod(QFile::encodeName(m_config.backendDir).constData(), 0700) != 0) {
        return QStringLiteral("Cannot create %1").arg(m_config.backendDir);
    }
    const QByteArray rootPath = QFile::encodeName(root);
    if (!QFileInfo::exists(root)) {
        QDir().mkpath(QFileInfo(root).absolutePath());
        if (mkdir(rootPath.constData(), 0700) != 0) {
            return QStringLiteral("Cannot create %1: %2").arg(root, QString::fromLocal8Bit(strerror(errno)));
        }
    }
    struct stat st;
    if (lstat(rootPath.constData(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        return QStringLiteral("%1 is not a directory").arg(root);
    }
    if (st.st_uid != getuid()) {
        return QStringLiteral("%1 is not owned by the current user").arg(root);
    }
    if (!QDir(root).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
        // Mounting would hide its contents.
        return QStringLiteral("%1 is not empty").arg(root);
    }

    fuse_args args = FUSE_ARGS_INIT(0, nullptr);
    fuse_opt_add_arg(&args, "kate-remote-fs");
    fuse_opt_add_arg(&args, "-o");
    fuse_opt_add_arg(&args, "fsname=kate-remote,subtype=kate-remote");
    m_fuse = fuse_new(&args, &s_operations, sizeof(s_operations), this);
    fuse_opt_free_args(&args);
    if (!m_fuse) {
        return QStringLiteral("Cannot initialize FUSE");
    }
    if (fuse_mount(m_fuse, rootPath.constData()) != 0) {
        fuse_destroy(m_fuse);
        m_fuse = nullptr;
        return QStringLiteral("Cannot mount %1").arg(root);
    }
    qInfo() << "Mounted" << root;
    return {};
}

int RemoteFs::run()
{
    fuse_loop_config *config = fuse_loop_cfg_create();
    // Requests for a host that is connecting wait for it; other hosts (and
    // the virtual directories) must not run out of threads meanwhile.
    fuse_loop_cfg_set_max_threads(config, 128);
    const int result = fuse_loop_mt(m_fuse, config);
    fuse_loop_cfg_destroy(config);
    return result;
}

void RemoteFs::requestExit()
{
    if (m_fuse) {
        fuse_exit(m_fuse);
        // Unmounting wakes the worker threads blocked on /dev/fuse.
        unmountPath(m_config.mountRoot, true);
    }
}

void RemoteFs::unmountAll()
{
    std::vector<Mount *> mounts;
    {
        std::lock_guard lock(m_mountsMutex);
        for (auto &entry : m_mounts) {
            mounts.push_back(entry.second.get());
        }
    }
    for (Mount *mount : mounts) {
        std::unique_lock stateLock(mount->stateMutex);
        std::unique_lock opLock(mount->opLock);
        if (mount->state == Mount::State::Mounted) {
            unmountLocked(mount, true);
            mount->state = Mount::State::Unmounted;
        }
    }
}

RemoteFs::Mount *RemoteFs::findOrCreateMount(const std::string &scheme, const std::string &authority)
{
    const std::string key = scheme + '/' + authority;
    std::lock_guard lock(m_mountsMutex);
    auto it = m_mounts.find(key);
    if (it != m_mounts.end()) {
        return it->second.get();
    }
    const QString qScheme = QString::fromStdString(scheme);
    const QString qAuthority = QString::fromStdString(authority);
    if (!KateRemote::isValidAuthority(qScheme, qAuthority)) {
        return nullptr;
    }
    auto mount = std::make_unique<Mount>();
    mount->scheme = qScheme;
    mount->authority = qAuthority;
    mount->backendPath = QFile::encodeName(m_config.backendDir + QLatin1Char('/') + QString::fromStdString(key)).toStdString();
    return m_mounts.emplace(key, std::move(mount)).first->second.get();
}

RemoteFs::Resolved RemoteFs::resolve(const char *path)
{
    Resolved result;
    const std::string p(path);
    if (p.empty() || p[0] != '/') {
        return result;
    }
    if (p == "/") {
        result.kind = Kind::Root;
        return result;
    }
    const size_t schemeEnd = p.find('/', 1);
    result.scheme = p.substr(1, schemeEnd == std::string::npos ? std::string::npos : schemeEnd - 1);
    if (!KateRemote::isSupportedScheme(QString::fromStdString(result.scheme))) {
        return result;
    }
    if (schemeEnd == std::string::npos) {
        result.kind = Kind::Scheme;
        return result;
    }
    const size_t authorityEnd = p.find('/', schemeEnd + 1);
    const std::string authority = p.substr(schemeEnd + 1, authorityEnd == std::string::npos ? std::string::npos : authorityEnd - schemeEnd - 1);
    if (!isRegistered(result.scheme + '/' + authority)) {
        return result;
    }
    result.mount = findOrCreateMount(result.scheme, authority);
    if (!result.mount) {
        return result;
    }
    result.kind = Kind::Host;
    result.rel = authorityEnd == std::string::npos ? std::string(".") : p.substr(authorityEnd + 1);
    if (result.rel.empty()) {
        result.rel = ".";
    }
    return result;
}

int RemoteFs::ensureMounted(Mount *mount)
{
    std::unique_lock lock(mount->stateMutex);
    while (true) {
        switch (mount->state) {
        case Mount::State::Mounted:
            return 0;
        case Mount::State::Mounting:
            mount->stateChanged.wait(lock);
            continue;
        case Mount::State::Failed:
            if (std::chrono::steady_clock::now() - mount->failedAt < std::chrono::seconds(m_config.retryAfterFailure)) {
                return EHOSTUNREACH;
            }
            break;
        case Mount::State::Unmounted:
            break;
        }
        break;
    }

    mount->state = Mount::State::Mounting;
    lock.unlock();
    QString error;
    const int fd = mountBackend(mount, &error);
    lock.lock();
    if (fd >= 0) {
        {
            std::unique_lock opLock(mount->opLock);
            mount->rootFd = fd;
            ++mount->generation;
        }
        mount->lastUse = now();
        mount->state = Mount::State::Mounted;
        mount->lastError.clear();
        qInfo() << "Connected" << mount->scheme << mount->authority;
        if (m_events.mounted) {
            m_events.mounted(mount->scheme, mount->authority);
        }
    } else {
        mount->state = Mount::State::Failed;
        mount->failedAt = std::chrono::steady_clock::now();
        mount->lastError = error;
        qWarning() << "Cannot connect" << mount->scheme << mount->authority << error;
        if (m_events.mountFailed) {
            m_events.mountFailed(mount->scheme, mount->authority, error);
        }
    }
    mount->stateChanged.notify_all();
    return fd >= 0 ? 0 : EHOSTUNREACH;
}

int RemoteFs::mountBackend(Mount *mount, QString *error)
{
    const QString path = QFile::decodeName(QByteArray::fromStdString(mount->backendPath));
    const auto target = KateRemote::parseSshAuthority(mount->authority);
    if (!target) {
        *error = QStringLiteral("Invalid host");
        return -1;
    }
    if (isMountPoint(path)) {
        unmountPath(path, true);
    }
    if (!QDir().mkpath(path)) {
        *error = QStringLiteral("Cannot create %1").arg(path);
        return -1;
    }
    if (!QDir(path).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
        *error = QStringLiteral("%1 is not empty").arg(path);
        return -1;
    }

    // No "reconnect": sshfs then exits when the connection drops, and the
    // next access remounts here, with failures cached (with "reconnect",
    // every access to a vanished host would wait for the connect timeout).
    QStringList options{
        QStringLiteral("ServerAliveInterval=15"),
        QStringLiteral("ServerAliveCountMax=3"),
        QStringLiteral("ConnectTimeout=10"),
        // Remote files of the remote user show as ours.
        QStringLiteral("idmap=user"),
        // Re-read a file when its (cached) mtime changed. The directory cache
        // keeps sshfs's default timeout (20 s): a cold cache costs a round
        // trip per file (git status: 15 s vs 1.3 s for 600 files).
        QStringLiteral("auto_cache"),
    };
    if (m_noContainSymlinks) {
        // Symlink targets are rewritten by us (hostSymlinkTarget()).
        options << QStringLiteral("no_contain_symlinks");
    }
    // sshfs discards ssh's messages unless in the foreground; ssh logs its
    // errors (unknown host, refused, host key mismatch...) to a file instead.
    const QString logDir = m_config.backendDir + QStringLiteral("/logs");
    QDir().mkpath(logDir);
    const QString sshLog = logDir + QLatin1Char('/') + mount->scheme + QLatin1Char('_') + mount->authority + QStringLiteral(".log");
    QFile::remove(sshLog);
    if (!m_config.sshCommand.contains(QLatin1Char(' ')) && !sshLog.contains(QLatin1Char(' ')) && !sshLog.contains(QLatin1Char(','))) {
        options << QStringLiteral("ssh_command=%1 -E %2").arg(m_config.sshCommand, sshLog);
    } else if (m_config.sshCommand != QLatin1String("ssh")) {
        options << QStringLiteral("ssh_command=%1").arg(m_config.sshCommand);
    }
    options << m_config.sshfsOptions;

    QStringList args{target->destination() + QStringLiteral(":/"), path, QStringLiteral("-o"), options.join(QLatin1Char(','))};
    if (target->port > 0) {
        args << QStringLiteral("-p") << QString::number(target->port);
    }

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!env.contains(QStringLiteral("SSH_ASKPASS")) && (env.contains(QStringLiteral("DISPLAY")) || env.contains(QStringLiteral("WAYLAND_DISPLAY")))) {
        const QString askpass = QStandardPaths::findExecutable(QStringLiteral("ksshaskpass"));
        if (!askpass.isEmpty()) {
            env.insert(QStringLiteral("SSH_ASKPASS"), askpass);
            if (!env.contains(QStringLiteral("SSH_ASKPASS_REQUIRE"))) {
                env.insert(QStringLiteral("SSH_ASKPASS_REQUIRE"), QStringLiteral("prefer"));
            }
        }
    }

    QString output;
    // sshfs returns once the file system is mounted (or failed).
    const int exitCode = runProgram(m_config.sshfsProgram, args, m_config.mountTimeout, &output, env);
    if (exitCode != 0) {
        QFile log(sshLog);
        if (log.open(QIODevice::ReadOnly)) {
            const QString sshOutput = QString::fromLocal8Bit(log.readAll()).trimmed();
            if (!sshOutput.isEmpty()) {
                output = sshOutput;
            }
        }
        const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        *error = lines.isEmpty() ? QStringLiteral("sshfs failed (exit code %1)").arg(exitCode) : lines.mid(std::max<qsizetype>(0, lines.size() - 3)).join(QLatin1Char('\n'));
        if (isMountPoint(path)) {
            unmountPath(path, true);
        }
        return -1;
    }

    const int fd = open(mount->backendPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct statfs fs;
    if (fd < 0 || fstatfs(fd, &fs) != 0 || static_cast<unsigned long>(fs.f_type) != FuseSuperMagic) {
        // Never pass operations through to the plain directory.
        if (fd >= 0) {
            close(fd);
        }
        *error = QStringLiteral("sshfs did not mount %1").arg(path);
        if (isMountPoint(path)) {
            unmountPath(path, true);
        }
        return -1;
    }
    return fd;
}

int RemoteFs::withMount(Mount *mount, const std::function<int(int)> &op)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (const int error = ensureMounted(mount)) {
            return -error;
        }
        std::shared_lock lock(mount->opLock);
        if (mount->rootFd < 0) {
            continue; // unmounted meanwhile
        }
        mount->lastUse = now();
        const uint64_t generation = mount->generation;
        const int result = op(mount->rootFd);
        if (result == -ENOTCONN || result == -ECONNABORTED) {
            // sshfs is gone; remount and retry once.
            lock.unlock();
            markBroken(mount, generation);
            continue;
        }
        return result;
    }
    return -EHOSTUNREACH;
}

void RemoteFs::markBroken(Mount *mount, uint64_t generation)
{
    std::unique_lock stateLock(mount->stateMutex);
    if (mount->state != Mount::State::Mounted) {
        return;
    }
    std::unique_lock opLock(mount->opLock);
    if (mount->generation != generation) {
        return; // already remounted by another thread
    }
    qWarning() << "Connection lost" << mount->scheme << mount->authority;
    unmountLocked(mount, true);
    mount->state = Mount::State::Unmounted;
    if (m_events.unmounted) {
        m_events.unmounted(mount->scheme, mount->authority);
    }
}

bool RemoteFs::unmountLocked(Mount *mount, bool lazy)
{
    if (mount->rootFd >= 0) {
        close(mount->rootFd);
        mount->rootFd = -1;
    }
    const QString path = QFile::decodeName(QByteArray::fromStdString(mount->backendPath));
    if (unmountPath(path, lazy) || !isMountPoint(path)) {
        QDir().rmdir(path);
        return true;
    }
    // Busy: keep it.
    mount->rootFd = open(mount->backendPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (mount->rootFd < 0) {
        unmountPath(path, true);
        return true;
    }
    return false;
}

void RemoteFs::unmountIdle()
{
    if (m_config.idleTimeout <= 0) {
        return;
    }
    std::vector<Mount *> mounts;
    {
        std::lock_guard lock(m_mountsMutex);
        for (auto &entry : m_mounts) {
            mounts.push_back(entry.second.get());
        }
    }
    const int64_t current = now();
    for (Mount *mount : mounts) {
        std::unique_lock stateLock(mount->stateMutex, std::try_to_lock);
        if (!stateLock.owns_lock() || mount->state != Mount::State::Mounted || current - mount->lastUse < m_config.idleTimeout) {
            continue;
        }
        std::unique_lock opLock(mount->opLock, std::try_to_lock);
        if (!opLock.owns_lock() || mount->openHandles > 0) {
            continue;
        }
        if (unmountLocked(mount, false)) {
            mount->state = Mount::State::Unmounted;
            qInfo() << "Disconnected idle" << mount->scheme << mount->authority;
            if (m_events.unmounted) {
                m_events.unmounted(mount->scheme, mount->authority);
            }
        }
    }
}

bool RemoteFs::unmountHost(const QString &scheme, const QString &authority)
{
    Mount *mount = nullptr;
    {
        std::lock_guard lock(m_mountsMutex);
        auto it = m_mounts.find(QString(scheme + QLatin1Char('/') + authority).toStdString());
        if (it == m_mounts.end()) {
            return true;
        }
        mount = it->second.get();
    }
    std::unique_lock stateLock(mount->stateMutex);
    if (mount->state == Mount::State::Failed) {
        // Forget the failure: the next access tries again.
        mount->state = Mount::State::Unmounted;
        return true;
    }
    if (mount->state != Mount::State::Mounted) {
        return mount->state == Mount::State::Unmounted;
    }
    std::unique_lock opLock(mount->opLock);
    if (mount->openHandles > 0 || !unmountLocked(mount, false)) {
        return false;
    }
    mount->state = Mount::State::Unmounted;
    if (m_events.unmounted) {
        m_events.unmounted(mount->scheme, mount->authority);
    }
    return true;
}

QStringList RemoteFs::mountedHosts()
{
    QStringList result;
    std::lock_guard lock(m_mountsMutex);
    for (auto &entry : m_mounts) {
        // Read without the state lock: a racy answer is fine for listings,
        // and a host that is connecting must not block them.
        if (entry.second->rootFd >= 0) {
            result << QString::fromStdString(entry.first);
        }
    }
    return result;
}

QString RemoteFs::lastError(const QString &scheme, const QString &authority)
{
    Mount *mount = nullptr;
    {
        std::lock_guard lock(m_mountsMutex);
        auto it = m_mounts.find(QString(scheme + QLatin1Char('/') + authority).toStdString());
        if (it == m_mounts.end()) {
            return {};
        }
        mount = it->second.get();
    }
    std::lock_guard stateLock(mount->stateMutex);
    return mount->lastError;
}

void RemoteFs::loadHosts()
{
    std::lock_guard lock(m_hostsMutex);
    m_hosts.clear();
    QFile file(m_config.hostsFile);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    for (const QByteArray &line : file.readAll().split('\n')) {
        const QString entry = QString::fromUtf8(line).trimmed();
        const qsizetype slash = entry.indexOf(QLatin1Char('/'));
        if (slash > 0 && KateRemote::isSupportedScheme(entry.left(slash)) && KateRemote::isValidAuthority(entry.left(slash), entry.mid(slash + 1))) {
            m_hosts.insert(entry.toStdString());
        }
    }
}

bool RemoteFs::isRegistered(const std::string &key)
{
    std::lock_guard lock(m_hostsMutex);
    return m_hosts.count(key) > 0;
}

bool RemoteFs::writeHostsLocked()
{
    QDir().mkpath(QFileInfo(m_config.hostsFile).absolutePath());
    QSaveFile file(m_config.hostsFile);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "Cannot write" << m_config.hostsFile << file.errorString();
        return false;
    }
    for (const std::string &key : m_hosts) {
        file.write(QByteArray::fromStdString(key) + '\n');
    }
    if (!file.commit()) {
        qWarning() << "Cannot write" << m_config.hostsFile << file.errorString();
        return false;
    }
    if (m_events.hostsChanged) {
        m_events.hostsChanged();
    }
    return true;
}

QStringList RemoteFs::hosts()
{
    std::lock_guard lock(m_hostsMutex);
    QStringList result;
    for (const std::string &key : m_hosts) {
        result << QString::fromStdString(key);
    }
    return result;
}

bool RemoteFs::addHost(const QString &scheme, const QString &authority)
{
    if (!KateRemote::isSupportedScheme(scheme) || !KateRemote::isValidAuthority(scheme, authority)) {
        return false;
    }
    std::lock_guard lock(m_hostsMutex);
    if (m_hosts.insert(QString(scheme + QLatin1Char('/') + authority).toStdString()).second) {
        return writeHostsLocked();
    }
    return true;
}

bool RemoteFs::removeHost(const QString &scheme, const QString &authority)
{
    if (!unmountHost(scheme, authority)) {
        return false;
    }
    std::lock_guard lock(m_hostsMutex);
    if (m_hosts.erase(QString(scheme + QLatin1Char('/') + authority).toStdString())) {
        return writeHostsLocked();
    }
    return true;
}
