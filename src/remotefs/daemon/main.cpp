#include "RemoteFs.h"
#include "RemoteFsService.h"

#include "../RemoteLocation.h"

#include <KConfig>
#include <KConfigGroup>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

extern char **environ;

static RemoteFs::Config loadConfig()
{
    RemoteFs::Config config;
    KConfig file(KateRemote::configFileName(), KConfig::SimpleConfig);
    const KConfigGroup group(&file, QStringLiteral("General"));
    config.mountRoot = KateRemote::cleanMountRoot(group.readEntry("MountRoot", QString()));
    config.idleTimeout = group.readEntry("IdleTimeout", config.idleTimeout);
    config.retryAfterFailure = group.readEntry("RetryAfterFailure", config.retryAfterFailure);
    config.mountTimeout = group.readEntry("MountTimeout", config.mountTimeout);
    config.sshfsProgram = group.readEntry("SshfsProgram", config.sshfsProgram);
    config.sshCommand = group.readEntry("SshCommand", config.sshCommand);
    config.kdePasswords = group.readEntry("KdePasswords", config.kdePasswords);
    config.sshfsOptions = group.readEntry("SshfsOptions", QStringList());
    config.backendDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/kate-remote-fs");
    config.hostsFile = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/kate-remote-fs/hosts");
    return config;
}

namespace
{
const QLatin1String Deleted(" (deleted)");

// This process's executable as the kernel shows it: " (deleted)" appended
// once the file was replaced (a package update).
QString executableLink()
{
    char target[PATH_MAX];
    const ssize_t length = readlink("/proc/self/exe", target, sizeof(target) - 1);
    return length > 0 ? QFile::decodeName(QByteArray(target, length)) : QString();
}

// The installed executable (the replacement, after an update).
QString executablePath()
{
    QString path = executableLink();
    if (path.endsWith(Deleted)) {
        path.chop(Deleted.size());
    }
    return path;
}

bool executableReplaced()
{
    return executableLink().endsWith(Deleted) && QFileInfo(executablePath()).isExecutable();
}

// The systemd unit this process runs in, if it is ours (kate-remote-fs*.service,
// from our own cgroup; never another unit, e.g. dbus.service when the bus
// started us without systemd).
QByteArray systemdUnit()
{
    QFile cgroup(QStringLiteral("/proc/self/cgroup"));
    if (!cgroup.open(QIODevice::ReadOnly)) {
        return {};
    }
    for (const QByteArray &line : cgroup.readAll().split('\n')) {
        if (!line.startsWith("0::")) {
            continue;
        }
        const QByteArray unit = line.mid(line.lastIndexOf('/') + 1);
        if (unit.startsWith("kate-remote-fs") && unit.endsWith(".service")) {
            return unit;
        }
    }
    return {};
}

// This user's processes with a file, working directory or mapping below
// root (what keeps the kernel from unmounting it), as "name (pid N)".
QStringList processesUsing(const QString &root)
{
    const QByteArray prefix = QFile::encodeName(root);
    const auto below = [&prefix](const QByteArray &path) {
        return path == prefix || path.startsWith(QByteArray(prefix + '/'));
    };
    const auto link = [](const QByteArray &path) {
        char target[PATH_MAX];
        const ssize_t length = readlink(path.constData(), target, sizeof(target) - 1);
        return length > 0 ? QByteArray(target, length) : QByteArray();
    };
    QStringList result;
    const QStringList pids = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &pid : pids) {
        bool numeric = false;
        const int id = pid.toInt(&numeric);
        struct stat st;
        const QByteArray dir = "/proc/" + pid.toLatin1();
        if (!numeric || id == getpid() || stat(dir.constData(), &st) != 0 || st.st_uid != getuid()) {
            continue;
        }
        bool uses = below(link(dir + "/cwd")) || below(link(dir + "/root"));
        if (!uses) {
            const QStringList fds = QDir(QString::fromLatin1(dir + "/fd")).entryList(QDir::Files | QDir::System | QDir::NoDotAndDotDot);
            for (const QString &fd : fds) {
                if (below(link(dir + "/fd/" + fd.toLatin1()))) {
                    uses = true;
                    break;
                }
            }
        }
        if (!uses) {
            QFile maps(QString::fromLatin1(dir + "/maps"));
            if (maps.open(QIODevice::ReadOnly)) {
                for (const QByteArray &line : maps.readAll().split('\n')) {
                    const qsizetype slash = line.indexOf('/');
                    if (slash >= 0 && below(line.mid(slash))) {
                        uses = true;
                        break;
                    }
                }
            }
        }
        if (uses) {
            QFile comm(QString::fromLatin1(dir + "/comm"));
            const QString name = comm.open(QIODevice::ReadOnly) ? QString::fromLocal8Bit(comm.readAll()).trimmed() : QString();
            result << QStringLiteral("%1 (pid %2)").arg(name, pid);
        }
    }
    return result;
}
}

// The daemon; *restart: it unmounted for a restart (main() then starts the
// installed executable).
static int run(int argc, char **argv, bool *restart)
{
    // Termination signals are read from a signalfd on the main thread; block
    // them before any thread exists so that none of them gets one.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    // A broken pipe to a helper must not kill the daemon.
    signal(SIGPIPE, SIG_IGN);

    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("kate-remote-fs"));

    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCritical() << "No session bus";
        return 1;
    }
    const RemoteFs::Config config = loadConfig();
    // One instance per user. A lock file rather than the DBus name: the name
    // is taken only once mounted, so that a DBus-activated client (or
    // systemd, Type=dbus) knows the paths work when the name appears.
    if (!QDir().mkpath(config.backendDir)) {
        qCritical() << "Cannot create" << config.backendDir;
        return 1;
    }
    const int lockFd = open(QFile::encodeName(config.backendDir + QStringLiteral("/lock")).constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lockFd < 0 || flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
        qCritical() << "kate-remote-fs is already running";
        return 1;
    }

    RemoteFs fs(config);
    if (const QString error = fs.mount(); !error.isEmpty()) {
        qCritical().noquote() << error;
        return 1;
    }

    RemoteFsService service(&fs);
    bus.registerObject(RemoteFsService::objectPath(), &service, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals);
    if (!bus.registerService(RemoteFsService::serviceName())) {
        qCritical() << "Cannot register" << RemoteFsService::serviceName();
        return 1;
    }

    // Host events come from FUSE threads; signal them from the main thread.
    RemoteFs::Events events;
    events.mounted = [&service](const QString &scheme, const QString &authority) {
        QMetaObject::invokeMethod(&service, [&service, scheme, authority] {
            Q_EMIT service.mounted(scheme, authority);
        }, Qt::QueuedConnection);
    };
    events.unmounted = [&service](const QString &scheme, const QString &authority) {
        QMetaObject::invokeMethod(&service, [&service, scheme, authority] {
            Q_EMIT service.unmounted(scheme, authority);
        }, Qt::QueuedConnection);
    };
    events.mountFailed = [&service](const QString &scheme, const QString &authority, const QString &error) {
        QMetaObject::invokeMethod(&service, [&service, scheme, authority, error] {
            Q_EMIT service.mountFailed(scheme, authority, error);
        }, Qt::QueuedConnection);
    };
    events.hostsChanged = [&service] {
        QMetaObject::invokeMethod(&service, [&service] {
            Q_EMIT service.hostsChanged();
        }, Qt::QueuedConnection);
    };
    fs.setEvents(events);

    std::thread fuseThread([&fs, &app] {
        const int result = fs.run();
        qInfo() << "FUSE loop ended" << result;
        QMetaObject::invokeMethod(&app, [] {
            QCoreApplication::quit();
        }, Qt::QueuedConnection);
    });

    const int signalFd = signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
    QSocketNotifier signalNotifier(signalFd, QSocketNotifier::Read);
    QObject::connect(&signalNotifier, &QSocketNotifier::activated, &app, [&fs, signalFd] {
        signalfd_siginfo info;
        while (read(signalFd, &info, sizeof(info)) == sizeof(info)) {
            qInfo() << "Signal" << info.ssi_signo << "- exiting";
        }
        fs.requestExit();
    });

    // Restarting (restart(), or on its own below): unmounts unless in use,
    // which ends the FUSE loop; main() then starts the installed executable.
    // Every check that can fail comes first: the unmount cannot be undone.
    const QString mountRoot = fs.config().mountRoot;
    service.setRestartHandler([&fs, restart, mountRoot]() -> QString {
        if (!QFileInfo(executablePath()).isExecutable()) {
            return QStringLiteral("%1 is missing").arg(executablePath());
        }
        if (const QString error = fs.unmountRootIfUnused(); !error.isEmpty()) {
            const QStringList users = processesUsing(mountRoot);
            return users.isEmpty() ? QStringLiteral("%1 is in use (%2)").arg(mountRoot, error)
                                   : QStringLiteral("files or working directories below %1 are in use by %2").arg(mountRoot, users.join(QStringLiteral(", ")));
        }
        *restart = true;
        return {};
    });

    // Idle hosts are disconnected; an executable replaced by an update is
    // restarted once nothing holds the mount: no host connected, no remote
    // paths open in a Kate (or a client that may have some), twice in a row,
    // and the unmount succeeds.
    QTimer idleTimer;
    const int idle = fs.config().idleTimeout;
    idleTimer.setInterval(idle > 0 ? std::clamp(idle * 1000 / 4, 1000, 60000) : 60000);
    int quietTicks = 0;
    QString lastRefusal;
    QObject::connect(&idleTimer, &QTimer::timeout, &app, [&fs, &service, idle, &quietTicks, &lastRefusal] {
        if (idle > 0) {
            fs.unmountIdle();
        }
        if (!executableReplaced() || fs.hasActiveMounts() || service.hasHolders()) {
            quietTicks = 0;
            return;
        }
        if (++quietTicks < 2) {
            return;
        }
        const QString error = service.restartNow();
        if (error.isEmpty()) {
            qInfo() << "Restarting: the executable was updated";
        } else if (error != lastRefusal) {
            qInfo().noquote() << "Not restarting after the update yet:" << error;
        }
        lastRefusal = error;
    });
    idleTimer.start();

    const int result = app.exec();
    idleTimer.stop();
    fs.requestExit();
    fuseThread.join();
    fs.unmountAll();
    bus.unregisterObject(RemoteFsService::objectPath());
    bus.unregisterService(RemoteFsService::serviceName());
    close(signalFd);
    close(lockFd);
    return result;
}

int main(int argc, char **argv)
{
    bool restart = false;
    const int result = run(argc, argv, &restart);
    if (!restart) {
        return result;
    }
    // Everything is unmounted and closed (the lock, the bus name).
    const QByteArray unit = systemdUnit();
    if (!unit.isEmpty()) {
        // Restarted by systemd rather than exec'd in place: an exec drops the
        // bus name for a moment, and systemd is expected to stop a Type=dbus
        // service whose name goes away (assumed; not tried with the real
        // unit).
        fprintf(stderr, "Restarting %s\n", unit.constData());
        char *args[] = {const_cast<char *>("systemctl"), const_cast<char *>("--user"), const_cast<char *>("--no-block"),
                        const_cast<char *>("restart"), const_cast<char *>(unit.constData()), nullptr};
        // The user manager's runtime directory, whatever this process was
        // given.
        std::vector<QByteArray> envStrings;
        std::vector<char *> envp;
        for (char **e = environ; *e; ++e) {
            if (strncmp(*e, "XDG_RUNTIME_DIR=", 16) != 0) {
                envStrings.emplace_back(*e);
            }
        }
        envStrings.push_back("XDG_RUNTIME_DIR=/run/user/" + QByteArray::number(getuid()));
        for (QByteArray &entry : envStrings) {
            envp.push_back(entry.data());
        }
        envp.push_back(nullptr);
        pid_t pid = 0;
        if (posix_spawnp(&pid, "systemctl", nullptr, nullptr, args, envp.data()) != 0) {
            fprintf(stderr, "Cannot run systemctl: %s\n", strerror(errno));
            return 1;
        }
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
    }
    const QByteArray path = QFile::encodeName(executablePath());
    fprintf(stderr, "Restarting %s\n", path.constData());
    // The signals the old process blocked stay blocked (the new one blocks
    // the same ones).
    execv(path.constData(), argv);
    fprintf(stderr, "Cannot run %s: %s\n", path.constData(), strerror(errno));
    return 1;
}
