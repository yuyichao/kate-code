#include "RemoteProjectsPlugin.h"
#include "RemoteProjectsView.h"

#include "RemoteLocation.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KPluginFactory>
#include <KSharedConfig>
#include <KTextEditor/Application>
#include <KTextEditor/Editor>
#include <KTextEditor/MainWindow>

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QThread>

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
}

QObject *RemoteProjectsPlugin::createView(KTextEditor::MainWindow *mainWindow)
{
    return new RemoteProjectsView(this, mainWindow);
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
    } else {
        qWarning() << "[RemoteProjects] kate-remote-fs did not answer:" << reply.errorMessage();
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
            inBackground(
                context,
                [path] {
                    return QFileInfo(path).isDir();
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
    for (const QString &path : std::as_const(m_sessionProjects)) {
        inBackground(
            this,
            [path] {
                return QFileInfo(path).isDir();
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
