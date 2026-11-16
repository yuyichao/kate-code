#include "RemoteProjectsPlugin.h"
#include "RemoteProjectsView.h"

#include "BuildVersion.h"
#include "RemoteLocation.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KPluginFactory>
#include <KSharedConfig>
#include <KTextEditor/Application>
#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/MainWindow>

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusError>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <climits>
#include <unistd.h>

#include <memory>

K_PLUGIN_FACTORY_WITH_JSON(RemoteProjectsPluginFactory, "kateremoteprojects.json", registerPlugin<RemoteProjectsPlugin>();)

namespace
{
const QString DaemonService = QStringLiteral("org.kde.KateRemoteFS");
const QString DaemonPath = QStringLiteral("/org/kde/KateRemoteFS");
const QString DaemonInterface = QStringLiteral("org.kde.KateRemoteFS");
constexpr int MaxRecent = 10;

QDBusMessage daemonCall(const QString &method, const QVariantList &args = {})
{
    QDBusMessage message = QDBusMessage::createMethodCall(DaemonService, DaemonPath, DaemonInterface, method);
    message.setArguments(args);
    return message;
}

// Runs check() on a worker thread (it may block while a host connects) and
// done(result) on context's thread.
void inBackground(QObject *context, std::function<bool()> check, std::function<void(bool)> done)
{
    auto result = std::make_shared<bool>(false);
    QThread *thread = QThread::create([check = std::move(check), result] {
        *result = check();
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    QObject::connect(thread, &QThread::finished, context, [done = std::move(done), result] {
        done(*result);
    });
    thread->start();
}

// Whether the local path of a remote folder is a directory, connecting
// its host. Blocks: for inBackground().
bool isRemoteDir(const QString &path, const QString &mountRoot)
{
    // KIO hosts are mounted from their registered folders (the root of a
    // WebDAV server is often not readable). Older daemons do not know
    // addFolder (the error is ignored).
    if (const auto location = KateRemote::locationForPath(path, mountRoot); location && KateRemote::isKioScheme(location->scheme)) {
        QDBusConnection::sessionBus().call(daemonCall(QStringLiteral("addFolder"), {location->scheme, location->authority, location->path}),
                                           QDBus::Block,
                                           150000);
    }
    return QFileInfo(path).isDir();
}

bool isBelow(const QString &path, const QString &root)
{
    return path == root || path.startsWith(root + QLatin1Char('/'));
}
}

RemoteProjectsPlugin::RemoteProjectsPlugin(QObject *parent, const QVariantList &)
    : KTextEditor::Plugin(parent)
    , m_mountRoot(KateRemote::configuredMountRoot())
{
    loadRecent();
    // Kate restores the session's documents right after loading the plugins:
    // the remote paths must work by then.
    startDaemon();
    watchOpenPaths();
}

void RemoteProjectsPlugin::watchOpenPaths()
{
    m_reportTimer = new QTimer(this);
    m_reportTimer->setSingleShot(true);
    m_reportTimer->setInterval(300);
    connect(m_reportTimer, &QTimer::timeout, this, &RemoteProjectsPlugin::reportOpenPaths);

    // A new daemon (started, or restarted) knows nothing yet.
    m_daemonWatcher = new QDBusServiceWatcher(DaemonService, QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForRegistration, this);
    connect(m_daemonWatcher, &QDBusServiceWatcher::serviceRegistered, this, &RemoteProjectsPlugin::daemonRegistered);

    KTextEditor::Application *app = KTextEditor::Editor::instance()->application();
    connect(app, &KTextEditor::Application::documentCreated, this, [this](KTextEditor::Document *document) {
        watchDocument(document);
        scheduleReport();
    });
    // Still listed when this is emitted: the report comes later.
    connect(app, &KTextEditor::Application::documentWillBeDeleted, this, &RemoteProjectsPlugin::scheduleReport);
    for (KTextEditor::Document *document : app->documents()) {
        watchDocument(document);
    }
    watchProjectPlugin(app->plugin(QStringLiteral("kateprojectplugin")));
    connect(app, &KTextEditor::Application::pluginCreated, this, [this](const QString &name, KTextEditor::Plugin *plugin) {
        if (name == QLatin1String("kateprojectplugin")) {
            watchProjectPlugin(plugin);
            scheduleReport();
        }
    });
    // Once at start, even with nothing open: a client that never reports
    // blocks restarts.
    scheduleReport();
}

void RemoteProjectsPlugin::watchDocument(KTextEditor::Document *document)
{
    // A document usually gets its URL after documentCreated (and Save As
    // changes it).
    connect(document, &KTextEditor::Document::documentUrlChanged, this, &RemoteProjectsPlugin::scheduleReport, Qt::UniqueConnection);
}

void RemoteProjectsPlugin::watchProjectPlugin(QObject *projectPlugin)
{
    if (!projectPlugin) {
        return;
    }
    // The project plugin has no public header: by name.
    for (const char *signal : {SIGNAL(projectAdded(QObject *)), SIGNAL(projectRemoved(QObject *))}) {
        if (projectPlugin->metaObject()->indexOfSignal(QMetaObject::normalizedSignature(signal + 1).constData()) >= 0) {
            connect(projectPlugin, signal, this, SLOT(scheduleReport()), Qt::UniqueConnection);
        } else {
            qWarning() << "[RemoteProjects] The project plugin has no signal" << (signal + 1) << "- open remote projects may not be reported";
        }
    }
}

void RemoteProjectsPlugin::scheduleReport()
{
    if (m_reportTimer) {
        m_reportTimer->start();
    }
}

QStringList RemoteProjectsPlugin::openRemotePaths() const
{
    QStringList paths = openRemoteProjects().value_or(QStringList());
    const QString root = QDir::cleanPath(m_mountRoot);
    const QString canonicalRoot = QDir(root).canonicalPath();
    for (KTextEditor::Document *document : KTextEditor::Editor::instance()->application()->documents()) {
        if (!document->url().isLocalFile()) {
            continue;
        }
        const QString path = QDir::cleanPath(document->url().toLocalFile());
        if (isBelow(path, root) || (!canonicalRoot.isEmpty() && isBelow(path, canonicalRoot))) {
            paths << path;
        }
    }
    paths.sort();
    paths.removeDuplicates();
    return paths;
}

void RemoteProjectsPlugin::reportOpenPaths()
{
    const QStringList paths = openRemotePaths();
    // Compared with the last list sent (not the last answered: calls on one
    // connection arrive in order, and a reply may take long).
    if (m_reported == paths) {
        return;
    }
    m_reported = paths;
    QDBusMessage message = daemonCall(QStringLiteral("setOpenPaths"), {paths});
    // Not a reason to start the daemon: it gets the list when it starts.
    message.setAutoStartService(false);
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *call) {
        call->deleteLater();
        if (call->isError()) {
            // Not running (sent when it starts), or older than this: sent
            // again next time.
            m_reported.reset();
            if (call->error().type() != QDBusError::ServiceUnknown) {
                qInfo() << "[RemoteProjects] Open remote paths not reported:" << call->error().message();
            }
        }
    });
}

void RemoteProjectsPlugin::daemonRegistered()
{
    m_reported.reset();
    scheduleReport();
    if (!m_restartWindow) {
        return;
    }
    QPointer<KTextEditor::MainWindow> window = std::exchange(m_restartWindow, nullptr);
    if (m_restartTimeout) {
        m_restartTimeout->stop();
    }
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(daemonCall(QStringLiteral("version"))), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, window](QDBusPendingCallWatcher *call) {
        call->deleteLater();
        const QDBusPendingReply<QString> version = *call;
        const QString mine = KateRemote::buildVersion();
        if (version.isError()) {
            showMessage(window, i18n("kate-remote-fs restarted."), QStringLiteral("Info"));
        } else if (version.value() == mine) {
            showMessage(window, i18n("kate-remote-fs restarted (version %1).", version.value()), QStringLiteral("Info"));
        } else {
            showMessage(window,
                        i18n("kate-remote-fs restarted, but it is version %1 and this Remote Projects plugin %2: restart Kate too if it was running before the update.",
                             version.value(),
                             mine),
                        QStringLiteral("Warning"));
        }
    });
}

void RemoteProjectsPlugin::restartDaemon(KTextEditor::MainWindow *window)
{
    // This Kate's latest open paths first (the daemon checks them).
    reportOpenPaths();
    QDBusMessage message = daemonCall(QStringLiteral("restart"));
    message.setAutoStartService(false);
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    const QPointer<KTextEditor::MainWindow> target(window);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, target](QDBusPendingCallWatcher *call) {
        call->deleteLater();
        const QDBusPendingReply<QString> reply = *call;
        if (reply.isError()) {
            if (reply.error().type() == QDBusError::ServiceUnknown) {
                showMessage(target, i18n("kate-remote-fs is not running: it starts when a remote folder is opened."), QStringLiteral("Info"));
            } else if (reply.error().type() == QDBusError::UnknownMethod) {
                showMessage(target,
                            i18n("This kate-remote-fs is too old to restart on request. Close remote projects and documents in every Kate (remote folders are disconnected meanwhile), then run: systemctl --user restart kate-remote-fs"),
                            QStringLiteral("Warning"));
            } else {
                showMessage(target, i18n("kate-remote-fs did not restart: %1", reply.error().message()), QStringLiteral("Error"));
            }
            return;
        }
        if (!reply.value().isEmpty()) {
            showMessage(target, i18n("kate-remote-fs did not restart. %1", reply.value()), QStringLiteral("Warning"));
            return;
        }
        showMessage(target, i18n("Restarting kate-remote-fs…"), QStringLiteral("Info"));
        // Told when its name has a new owner (shutting down may take a while).
        m_restartWindow = target;
        if (!m_restartTimeout) {
            m_restartTimeout = new QTimer(this);
            m_restartTimeout->setSingleShot(true);
            m_restartTimeout->setInterval(120000);
            connect(m_restartTimeout, &QTimer::timeout, this, [this] {
                if (QPointer<KTextEditor::MainWindow> window = std::exchange(m_restartWindow, nullptr)) {
                    showMessage(window, i18n("kate-remote-fs has not come back after restarting; it starts again when a remote folder is opened."), QStringLiteral("Warning"));
                }
            });
        }
        m_restartTimeout->start();
    });
}

void RemoteProjectsPlugin::showMessage(KTextEditor::MainWindow *window, const QString &text, const QString &type)
{
    qInfo().noquote() << "[RemoteProjects]" << type << text;
    if (!window) {
        return;
    }
    QVariantMap message;
    message[QStringLiteral("text")] = text;
    message[QStringLiteral("type")] = type;
    message[QStringLiteral("category")] = i18n("Remote Projects");
    window->showMessage(message);
}

QObject *RemoteProjectsPlugin::createView(KTextEditor::MainWindow *mainWindow)
{
    auto *view = new RemoteProjectsView(this, mainWindow);
    if (!m_versionWarning.isEmpty()) {
        // Once per Kate instance, in the first window.
        QVariantMap message;
        message[QStringLiteral("text")] = m_versionWarning;
        message[QStringLiteral("type")] = QStringLiteral("Warning");
        message[QStringLiteral("category")] = i18n("Remote Projects");
        m_versionWarning.clear();
        QPointer<KTextEditor::MainWindow> window(mainWindow);
        QTimer::singleShot(0, view, [window, message] {
            if (window) {
                window->showMessage(message);
            }
        });
    }
    return view;
}

void RemoteProjectsPlugin::startDaemon()
{
    auto bus = QDBusConnection::sessionBus();
    QDBusConnectionInterface *interface = bus.interface();
    if (!interface) {
        return;
    }
    if (!interface->isServiceRegistered(DaemonService)) {
        const QDBusReply<QStringList> activatable = interface->activatableServiceNames();
        if (!activatable.isValid() || !activatable.value().contains(DaemonService)) {
            return; // not installed
        }
    }
    // Blocks Kate's startup until the daemon has mounted (DBus activation
    // completes once it owns its name, which it takes after mounting).
    const QDBusMessage reply = bus.call(daemonCall(QStringLiteral("mountRoot")), QDBus::Block, 15000);
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        m_mountRoot = reply.arguments().constFirst().toString();
        m_daemonAvailable = true;
        checkDaemonVersion();
    } else {
        qWarning() << "[RemoteProjects] kate-remote-fs did not answer:" << reply.errorMessage();
    }
}

void RemoteProjectsPlugin::checkDaemonVersion()
{
    // A running daemon is not restarted when its package is upgraded: the
    // old one keeps serving (without newer fixes, e.g. password reuse).
    auto bus = QDBusConnection::sessionBus();
    // Daemons with restart() also restart on their own once nothing is open;
    // older ones (the one running when this first arrives) do not.
    const QString restart = i18n("Close remote projects and documents in every Kate, then use Tools > Restart Remote Folder Service (remote folders are disconnected meanwhile); for an older kate-remote-fs: systemctl --user restart kate-remote-fs");
    const QDBusReply<uint> pid = bus.interface()->servicePid(DaemonService);
    if (pid.isValid() && pid.value() > 0) {
        char target[PATH_MAX];
        const ssize_t length = readlink(QStringLiteral("/proc/%1/exe").arg(pid.value()).toLocal8Bit().constData(), target, sizeof(target) - 1);
        if (length > 0 && QByteArray(target, length).endsWith(" (deleted)")) {
            m_versionWarning = i18n("kate-remote-fs (remote folders) was updated while it was running and still runs the old version. %1", restart);
            qWarning() << "[RemoteProjects] kate-remote-fs runs a replaced executable:" << QByteArray(target, length);
            return;
        }
    }
    const QString mine = KateRemote::buildVersion();
    const QDBusMessage reply = bus.call(daemonCall(QStringLiteral("version")), QDBus::Block, 5000);
    const QString theirs = reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty() ? reply.arguments().constFirst().toString() : QString();
    if (theirs.isEmpty()) {
        // No version() yet: from before this check.
        m_versionWarning = i18n("kate-remote-fs (remote folders) is older than Kate's Remote Projects plugin (version %1): it was left running across an update. %2",
                                mine,
                                restart);
    } else if (theirs != mine) {
        m_versionWarning = i18n("kate-remote-fs (remote folders) is version %1, but Kate's Remote Projects plugin is version %2: one of them was updated without a restart. %3 Restart Kate too if it was running before the update.",
                                theirs,
                                mine,
                                restart);
    }
    if (!m_versionWarning.isEmpty()) {
        qWarning() << "[RemoteProjects] kate-remote-fs version" << theirs << "differs from" << mine;
    }
}

bool RemoteProjectsPlugin::daemonAvailable() const
{
    return m_daemonAvailable;
}

QString RemoteProjectsPlugin::mountRoot() const
{
    return m_mountRoot;
}

void RemoteProjectsPlugin::loadRecent()
{
    // Another Kate instance may have changed the list.
    KSharedConfig::openConfig()->reparseConfiguration();
    const KConfigGroup group(KSharedConfig::openConfig(), QStringLiteral("Remote Projects"));
    m_recent.clear();
    for (const QString &entry : group.readEntry("Recent", QStringList())) {
        const QUrl url(entry);
        if (url.isValid() && !m_recent.contains(url)) {
            m_recent.append(url);
        }
    }
}

void RemoteProjectsPlugin::saveRecent()
{
    QStringList entries;
    for (const QUrl &url : std::as_const(m_recent)) {
        entries << url.toString();
    }
    KConfigGroup group(KSharedConfig::openConfig(), QStringLiteral("Remote Projects"));
    group.writeEntry("Recent", entries);
    group.sync();
    Q_EMIT recentUrlsChanged();
}

void RemoteProjectsPlugin::addRecentUrl(const QUrl &url)
{
    loadRecent();
    m_recent.removeAll(url);
    m_recent.prepend(url);
    while (m_recent.size() > MaxRecent) {
        m_recent.removeLast();
    }
    saveRecent();
}

void RemoteProjectsPlugin::clearRecentUrls()
{
    m_recent.clear();
    saveRecent();
}

void RemoteProjectsPlugin::prepareUrl(const QUrl &url, QObject *context, std::function<void(const QString &, const QString &)> done)
{
    const auto location = KateRemote::locationForUrl(url);
    if (!location) {
        done({}, i18n("%1 is not a remote folder that can be opened.", url.toDisplayString()));
        return;
    }
    if (!m_daemonAvailable) {
        startDaemon();
    }
    if (!m_daemonAvailable) {
        done({}, i18n("The kate-remote-fs service is not installed or could not be started."));
        return;
    }

    const QString scheme = location->scheme;
    const QString authority = location->authority;
    const QString path = KateRemote::pathForLocation(*location, m_mountRoot);
    const QString hostKey = scheme + QLatin1Char('/') + authority;
    auto bus = QDBusConnection::sessionBus();

    // A host registered only for this attempt is forgotten again if the
    // attempt fails (e.g. a mistyped host name).
    auto fail = [=](bool forgetHost, const QString &error) {
        if (forgetHost) {
            QDBusConnection::sessionBus().asyncCall(daemonCall(QStringLiteral("removeHost"), {scheme, authority}));
        }
        done({}, error);
    };

    auto *hostsWatcher = new QDBusPendingCallWatcher(bus.asyncCall(daemonCall(QStringLiteral("hosts"))), context);
    QObject::connect(hostsWatcher, &QDBusPendingCallWatcher::finished, context, [=](QDBusPendingCallWatcher *hostsCall) {
        hostsCall->deleteLater();
        const QDBusPendingReply<QStringList> hosts = *hostsCall;
        const bool newHost = hosts.isError() || !hosts.value().contains(hostKey);

        auto *watcher = new QDBusPendingCallWatcher(
            QDBusConnection::sessionBus().asyncCall(daemonCall(QStringLiteral("addHost"), {scheme, authority})), context);
        QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context, [=](QDBusPendingCallWatcher *call) {
            call->deleteLater();
            const QDBusPendingReply<bool> reply = *call;
            if (reply.isError() || !reply.value()) {
                done({}, reply.isError() ? reply.error().message() : i18n("%1 is not a valid host.", authority));
                return;
            }
            const QString mountRoot = m_mountRoot;
            inBackground(
                context,
                [path, mountRoot] {
                    return isRemoteDir(path, mountRoot);
                },
                [=](bool isDir) {
                    if (isDir) {
                        done(path, {});
                        return;
                    }
                    // Why: the host's connection error, if any.
                    auto *errorWatcher = new QDBusPendingCallWatcher(
                        QDBusConnection::sessionBus().asyncCall(daemonCall(QStringLiteral("lastError"), {scheme, authority})), context);
                    QObject::connect(errorWatcher, &QDBusPendingCallWatcher::finished, context, [=](QDBusPendingCallWatcher *errorCall) {
                        errorCall->deleteLater();
                        const QDBusPendingReply<QString> error = *errorCall;
                        if (!error.isError() && !error.value().isEmpty()) {
                            fail(newHost, i18n("Cannot connect to %1: %2", authority, error.value()));
                        } else {
                            fail(newHost, i18n("%1 is not a folder.", url.toDisplayString()));
                        }
                    });
                });
        });
    });
}

std::optional<QStringList> RemoteProjectsPlugin::openRemoteProjects() const
{
    QStringList result;
    bool asked = false;
    auto *app = KTextEditor::Editor::instance()->application();
    const QString root = QDir::cleanPath(m_mountRoot);
    const QString canonicalRoot = QDir(root).canonicalPath();
    for (KTextEditor::MainWindow *window : app->mainWindows()) {
        QObject *projectView = window->pluginView(QStringLiteral("kateprojectplugin"));
        if (!projectView) {
            continue;
        }
        // Projects are shared by all windows; one view lists them all.
        asked = true;
        const auto projects = projectView->property("allProjects").value<QMap<QString, QString>>();
        for (auto it = projects.begin(); it != projects.end(); ++it) {
            const QString dir = QDir::cleanPath(it.key());
            if ((isBelow(dir, root) || (!canonicalRoot.isEmpty() && isBelow(dir, canonicalRoot))) && !result.contains(dir)) {
                result << dir;
            }
        }
        break;
    }
    if (!asked) {
        return std::nullopt;
    }
    return result;
}

void RemoteProjectsPlugin::writeSessionConfig(KConfigGroup &config)
{
    QStringList projects = openRemoteProjects().value_or(m_sessionProjects);
    for (const QString &path : std::as_const(m_pendingProjects)) {
        if (!projects.contains(path)) {
            projects << path;
        }
    }
    config.writeEntry("Projects", projects);
}

void RemoteProjectsPlugin::readSessionConfig(const KConfigGroup &config)
{
    // The project plugin restores projects before this plugin is loaded and
    // drops those whose directory does not exist yet (kate-remote-fs not
    // running then). Open those again; projectForDir() returns open ones as
    // they are.
    const QStringList paths = config.readEntry("Projects", QStringList());
    m_sessionProjects.clear();
    for (const QString &path : paths) {
        if (KateRemote::locationForPath(path, m_mountRoot)) {
            m_sessionProjects << path;
        }
    }
    m_pendingProjects = m_sessionProjects;
    QObject *projectPlugin = KTextEditor::Editor::instance()->application()->plugin(QStringLiteral("kateprojectplugin"));
    if (m_pendingProjects.isEmpty() || !projectPlugin || !m_daemonAvailable) {
        return;
    }
    const QPointer<QObject> plugin(projectPlugin);
    const QPointer<RemoteProjectsPlugin> self(this);
    const QString mountRoot = m_mountRoot;
    for (const QString &path : std::as_const(m_sessionProjects)) {
        inBackground(
            this,
            [path, mountRoot] {
                return isRemoteDir(path, mountRoot);
            },
            [self, plugin, path](bool isDir) {
                if (isDir && plugin && self) {
                    qInfo() << "[RemoteProjects] Reopening session project" << path;
                    self->m_pendingProjects.removeAll(path);
                    QMetaObject::invokeMethod(plugin, "projectForDir", Q_ARG(QDir, QDir(path)), Q_ARG(bool, true));
                } else {
                    qInfo() << "[RemoteProjects] Session project not reachable:" << path;
                }
            });
    }
}

#include "RemoteProjectsPlugin.moc"
