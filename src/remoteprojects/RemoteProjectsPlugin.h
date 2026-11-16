#pragma once

#include <KTextEditor/Plugin>
#include <KTextEditor/SessionConfigInterface>

#include <QList>
#include <QPointer>
#include <QStringList>
#include <QUrl>
#include <QVariant>

#include <functional>
#include <optional>

class QDBusServiceWatcher;
class QTimer;

namespace KTextEditor
{
class Document;
class MainWindow;
}

/**
 * Remote Projects: opens remote folders (sftp://host/path) as Kate projects
 * through kate-remote-fs, which mirrors them at stable local paths
 * (<MountRoot>/sftp/<host>/<path>), so Kate's own history, session restore
 * and project features work with them.
 *
 * The plugin makes sure the daemon runs before Kate restores the session's
 * documents, keeps a list of recently opened remote folders (as URLs), and
 * reopens remote projects of the session that the project plugin dropped
 * because their paths were not reachable yet when it restored them.
 */
class RemoteProjectsPlugin : public KTextEditor::Plugin, public KTextEditor::SessionConfigInterface
{
    Q_OBJECT
    Q_INTERFACES(KTextEditor::SessionConfigInterface)

public:
    explicit RemoteProjectsPlugin(QObject *parent, const QVariantList & = QVariantList());

    QObject *createView(KTextEditor::MainWindow *mainWindow) override;

    void readSessionConfig(const KConfigGroup &config) override;
    void writeSessionConfig(KConfigGroup &config) override;

    // Whether kate-remote-fs is running (or can be started over DBus).
    bool daemonAvailable() const;
    // Its mount root (the configured one if it does not run).
    QString mountRoot() const;

    QList<QUrl> recentUrls() const
    {
        return m_recent;
    }
    void addRecentUrl(const QUrl &url);
    // The user opened the project: it no longer waits to be reopened.
    void projectOpened(const QString &path)
    {
        m_pendingProjects.removeAll(path);
    }
    void clearRecentUrls();

    // Registers the URL's host with the daemon, then checks in a background
    // thread that its local path is a directory (this connects). Calls
    // done(localPath, error) on the main thread; error is empty on success.
    void prepareUrl(const QUrl &url, QObject *context, std::function<void(const QString &, const QString &)> done);

    // Local paths of the open projects that are below the mount root;
    // nullopt when no window has a project plugin view to ask.
    std::optional<QStringList> openRemoteProjects() const;

    // Asks kate-remote-fs to restart (e.g. after an update); the outcome is
    // shown in window's Output view. It refuses while remote projects or
    // documents are open in any Kate.
    void restartDaemon(KTextEditor::MainWindow *window);

Q_SIGNALS:
    void recentUrlsChanged();

private Q_SLOTS:
    // Sends the open remote paths to the daemon shortly (changes come in
    // bursts).
    void scheduleReport();

private:
    void startDaemon();
    // A restart of kate-remote-fs unmounts the mount root, which ends the
    // inotify watches Kate keeps there: open documents would show as
    // deleted, projects stop following their files. So the daemon is told
    // which remote projects and documents are open here (setOpenPaths(),
    // again when it restarts) and refuses to restart meanwhile.
    void watchOpenPaths();
    void watchDocument(KTextEditor::Document *document);
    void watchProjectPlugin(QObject *projectPlugin);
    void reportOpenPaths();
    // Local paths of open remote projects and documents.
    QStringList openRemotePaths() const;
    void daemonRegistered();
    void showMessage(KTextEditor::MainWindow *window, const QString &text, const QString &type);
    // Sets m_versionWarning when the running daemon is not this build.
    void checkDaemonVersion();
    void loadRecent();
    void saveRecent();

    bool m_daemonAvailable = false;
    QString m_mountRoot;
    // Shown once, in the first window.
    QString m_versionWarning;
    QList<QUrl> m_recent;
    // Remote projects of the session (as last read) and those of them not
    // reopened yet (host unreachable at startup): kept in the session so
    // that an offline start does not forget them.
    QStringList m_sessionProjects;
    QStringList m_pendingProjects;

    QTimer *m_reportTimer = nullptr;
    std::optional<QStringList> m_reported; // last sent to the daemon (reset on errors and when it restarts)
    QDBusServiceWatcher *m_daemonWatcher = nullptr;
    // The window that asked for a restart: told when the daemon is back.
    QPointer<KTextEditor::MainWindow> m_restartWindow;
    QTimer *m_restartTimeout = nullptr;
};
