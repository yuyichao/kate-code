#pragma once

#include <QString>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>

struct fuse;

/**
 * kate-remote-fs: a FUSE file system at the mount root that mirrors remote
 * URLs (<root>/sftp/<authority>/<remote path>). The first access below a host
 * directory mounts that host's "/" with sshfs in a private directory
 * ($XDG_RUNTIME_DIR/kate-remote-fs); every operation is passed through to it.
 * Idle hosts are unmounted again.
 *
 * Threading: FUSE requests run on libfuse worker threads; config, the idle
 * timer and DBus run on the Qt main thread.
 */
class RemoteFs
{
public:
    struct Config {
        QString mountRoot;
        QString backendDir; // private directory for the per-host mounts
        QString hostsFile; // registered hosts; written only by the daemon
        int idleTimeout = 900; // seconds; 0 = never unmount
        int retryAfterFailure = 30; // seconds a failed host is not retried
        int mountTimeout = 120; // seconds for sshfs (may wait for a password prompt)
        QString sshfsProgram = QStringLiteral("sshfs");
        QString sshCommand = QStringLiteral("ssh"); // run by sshfs, with -E <log>
        bool kdePasswords = true; // login passwords from KDE's password cache (kate-remote-fs-askpass)
        QStringList sshfsOptions; // "-o" options
    };

    struct Mount {
        QString scheme;
        QString authority;
        bool kio = false; // mounted through kio-fuse, else sshfs
        std::string backendPath; // sshfs mount point; kio-fuse: its path for the host

        enum class State { Unmounted, Mounting, Mounted, Failed };
        // Guards state, failedAt, lastError; mounting/unmounting holds it.
        std::mutex stateMutex;
        std::condition_variable stateChanged;
        State state = State::Unmounted;
        std::chrono::steady_clock::time_point failedAt;
        QString lastError;

        // Shared by operations using rootFd, exclusive to change it.
        std::shared_mutex opLock;
        std::atomic<int> rootFd{-1}; // atomic only for the lock-free listing
        uint64_t generation = 0; // counts mounts; guarded by opLock

        std::atomic<int64_t> lastUse{0}; // steady clock seconds
        std::atomic<int> openHandles{0};
    };

    struct Events {
        std::function<void(const QString &scheme, const QString &authority)> mounted;
        std::function<void(const QString &scheme, const QString &authority)> unmounted;
        std::function<void(const QString &scheme, const QString &authority, const QString &error)> mountFailed;
        std::function<void()> hostsChanged;
    };

    explicit RemoteFs(Config config);
    ~RemoteFs();

    static RemoteFs *instance();

    // Prepares the directories (cleaning up after a crashed instance) and
    // mounts the file system. Returns an error message on failure.
    QString mount();
    // Runs the FUSE loop; returns when unmounted.
    int run();
    // Ends run() (any thread) by unmounting the file system.
    void requestExit();
    // Unmounts all hosts (after run() returned).
    void unmountAll();

    void setEvents(Events events)
    {
        m_events = std::move(events);
    }

    const Config &config() const
    {
        return m_config;
    }

    // Unmounts idle hosts; called periodically from the main thread.
    void unmountIdle();
    // Unmounts one host unless files are open. Returns false if it stays
    // mounted.
    bool unmountHost(const QString &scheme, const QString &authority);

    QStringList mountedHosts(); // "sftp/<authority>"
    // kate-remote-fs-askpass, if found and KdePasswords is on.
    QString askpassProgram() const
    {
        return m_config.kdePasswords ? m_askpassProgram : QString();
    }
    QString lastError(const QString &scheme, const QString &authority);

    // Hosts the daemon connects to; other names do not exist (tools stat
    // names such as HEAD or Cargo.toml in every parent directory, and those
    // are valid host names). Kept in Config::hostsFile, which only this
    // process writes; others change the list over DBus.
    QStringList hosts(); // "sftp/<authority>"
    bool addHost(const QString &scheme, const QString &authority);
    // Disconnects the host first; false if files are open on it.
    bool removeHost(const QString &scheme, const QString &authority);

    // --- used by the FUSE callbacks ---
    enum class Kind { Root, Scheme, Host, Invalid };
    struct Resolved {
        Kind kind = Kind::Invalid;
        std::string scheme;
        Mount *mount = nullptr;
        std::string rel; // relative to the host's remote root, "." for the root
    };
    Resolved resolve(const char *path);

    // Runs op(rootFd) with the host mounted; remounts and retries once if
    // the backend has gone away. Returns a negative errno on failure.
    int withMount(Mount *mount, const std::function<int(int rootFd)> &op);

    static int64_t now();

private:
    Mount *findOrCreateMount(const std::string &scheme, const std::string &authority);
    int ensureMounted(Mount *mount);
    int mountBackend(Mount *mount, QString *error);
    int mountKioFuse(Mount *mount, QString *error);
    void markBroken(Mount *mount, uint64_t generation);
    // Unmounts with state and opLock held exclusively. Returns false if busy.
    bool unmountLocked(Mount *mount, bool lazy);
    void cleanupStaleMounts();
    bool isRegistered(const std::string &key);
    void loadHosts();
    bool writeHostsLocked();

    Config m_config;
    Events m_events;
    struct fuse *m_fuse = nullptr;
    bool m_noContainSymlinks = false; // sshfs knows -o no_contain_symlinks
    QString m_askpassProgram; // kate-remote-fs-askpass

    std::mutex m_hostsMutex;
    std::set<std::string> m_hosts; // "sftp/<authority>"

    std::mutex m_mountsMutex;
    std::map<std::string, std::unique_ptr<Mount>> m_mounts;
};
