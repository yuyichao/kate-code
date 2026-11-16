#pragma once

#include <QDBusContext>
#include <QDBusMessage>
#include <QHash>
#include <QObject>
#include <QStringList>

#include <functional>

class QDBusServiceWatcher;
class RemoteFs;

/**
 * DBus interface of kate-remote-fs (org.kde.KateRemoteFS at
 * /org/kde/KateRemoteFS), for the Remote Projects plugin and Kate Code.
 */
class RemoteFsService : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.KateRemoteFS")

public:
    explicit RemoteFsService(RemoteFs *fs, QObject *parent = nullptr);

    static QString serviceName();
    static QString objectPath();

    // Restarting (restart(), and on its own when the executable was
    // replaced) unmounts the mount root, which makes inotify watches on it
    // go away: Kate reports open documents as deleted and stops following
    // them, and open projects stop following their files. Kate's Remote
    // Projects plugin reports its open remote projects and documents
    // (setOpenPaths()); a connection that uses the daemon without ever
    // reporting (e.g. a Kate with an older plugin) may hold some too.
    // These "holders" block a restart while they are connected.

    // Runs the restart: unmounts unless in use (then why, and nothing
    // changes). Set by main().
    void setRestartHandler(std::function<QString()> handler)
    {
        m_restartHandler = std::move(handler);
    }
    // Whether a connection holds remote paths (or may: never reported).
    bool hasHolders();
    // The automatic restart (holders checked by the caller): empty, or why
    // not.
    QString restartNow()
    {
        return m_restartHandler ? m_restartHandler() : QStringLiteral("not supported");
    }

public Q_SLOTS:
    QString mountRoot();
    // The daemon's build (KateRemote::buildVersion()): the Remote Projects
    // plugin warns when it differs from its own (left running across an
    // upgrade).
    QString version();
    // kate-remote-fs-askpass if login passwords come from KDE's password
    // cache (KdePasswords), else empty: for other ssh clients (the Remote
    // Terminal, with KATE_REMOTE_ASKPASS_TTY).
    QString askpassProgram();
    // Connected hosts as "<scheme>/<authority>".
    QStringList mounts();
    // Why the last connection attempt to the host failed (empty if it did not).
    QString lastError(const QString &scheme, const QString &authority);
    // Disconnects the host unless files are open on it; also forgets a failed
    // attempt so that the next access retries at once.
    bool unmount(const QString &scheme, const QString &authority);
    // Registered hosts ("<scheme>/<authority>"): only these are connected
    // to; other names under <root>/<scheme>/ do not exist. The daemon owns
    // the list (~/.local/state/kate-remote-fs/hosts); change it here.
    QStringList hosts();
    bool addHost(const QString &scheme, const QString &authority);
    // Disconnects and forgets the host; false if files are open on it.
    bool removeHost(const QString &scheme, const QString &authority);
    // Registers a folder (remote absolute path) of a registered host and
    // connects it: empty, or the error (then it is not registered). KIO
    // hosts are mounted from their registered folders, not from "/" (often
    // not readable on WebDAV servers). Answers when done (may wait for a
    // login dialog).
    QString addFolder(const QString &scheme, const QString &authority, const QString &path, const QDBusMessage &message);
    // The caller's open remote projects and documents (local paths below
    // the mount root; all of them, replacing what it sent before; an empty
    // list too, so that it counts as reporting). Forgotten when the caller
    // disconnects.
    void setOpenPaths(const QStringList &paths);
    // Restarts the daemon (the installed executable, e.g. after an update):
    // empty if it restarts, else why not (nothing changes then): remote
    // paths open in a Kate (the caller's too), a connection that never
    // reported them, or files/working directories in use below the mount
    // root. Connected hosts are disconnected.
    QString restart();

Q_SIGNALS:
    void mounted(const QString &scheme, const QString &authority);
    void unmounted(const QString &scheme, const QString &authority);
    void mountFailed(const QString &scheme, const QString &authority, const QString &error);
    void hostsChanged();

private:
    struct Client {
        uint pid = 0;
        bool reports = false; // called setOpenPaths()
        QStringList paths;
    };
    // Records the calling connection (watched until it disconnects).
    void noteCaller();
    // Drops connections that are gone (a disconnect may be missed).
    void pruneClients();
    // Who holds remote paths, for messages; except: a caller exempt from
    // the "never reported" rule.
    QStringList holders(const QString &except);

    RemoteFs *m_fs;
    QHash<QString, Client> m_clients; // by unique bus name
    QDBusServiceWatcher *m_watcher;
    std::function<QString()> m_restartHandler;
};
