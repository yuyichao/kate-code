#include "RemoteProjectsView.h"
#include "RemoteProjectsPlugin.h"

#include "RemoteLocation.h"

#include <KIO/AuthInfo>
#include <KIO/StatJob>
#include <KPasswdServerClient>
#include <KActionCollection>
#include <KActionMenu>
#include <KLocalizedString>
#include <KTextEditor/MainWindow>
#include <KXMLGUIFactory>

#include <QDir>
#include <QFileDialog>
#include <QMenu>
#include <QMessageBox>
#include <QThread>

#include <memory>

RemoteProjectsView::RemoteProjectsView(RemoteProjectsPlugin *plugin, KTextEditor::MainWindow *mainWindow)
    : QObject(mainWindow)
    , m_plugin(plugin)
    , m_mainWindow(mainWindow)
{
    setXMLFile(QStringLiteral(":/kateremoteprojects/ui.rc"));

    auto *open = new QAction(QIcon::fromTheme(QStringLiteral("folder-remote")), i18n("Open Remote Folder…"), this);
    open->setToolTip(i18n("Open a folder on another computer (sftp://host/path) as a project"));
    connect(open, &QAction::triggered, this, &RemoteProjectsView::askRemoteFolder);
    actionCollection()->addAction(QStringLiteral("remote_projects_open"), open);

    m_recentMenu = new KActionMenu(QIcon::fromTheme(QStringLiteral("document-open-recent")), i18n("Open Recent Remote Folder"), this);
    m_recentMenu->setPopupMode(QToolButton::InstantPopup);
    actionCollection()->addAction(QStringLiteral("remote_projects_open_recent"), m_recentMenu);
    connect(m_plugin, &RemoteProjectsPlugin::recentUrlsChanged, this, &RemoteProjectsView::rebuildRecentMenu);
    rebuildRecentMenu();

    m_mainWindow->guiFactory()->addClient(this);
}

RemoteProjectsView::~RemoteProjectsView()
{
    if (m_mainWindow && m_mainWindow->guiFactory()) {
        m_mainWindow->guiFactory()->removeClient(this);
    }
}

void RemoteProjectsView::rebuildRecentMenu()
{
    QMenu *menu = m_recentMenu->menu();
    menu->clear();
    const QList<QUrl> urls = m_plugin->recentUrls();
    for (const QUrl &url : urls) {
        QAction *action = menu->addAction(QIcon::fromTheme(QStringLiteral("folder-remote")), url.toDisplayString());
        connect(action, &QAction::triggered, this, [this, url] {
            openUrl(url);
        });
    }
    if (!urls.isEmpty()) {
        menu->addSeparator();
        QAction *clear = menu->addAction(QIcon::fromTheme(QStringLiteral("edit-clear-history")), i18n("Clear List"));
        connect(clear, &QAction::triggered, m_plugin, &RemoteProjectsPlugin::clearRecentUrls);
    }
    m_recentMenu->setEnabled(!urls.isEmpty());
}

std::optional<QUrl> RemoteProjectsView::parseInput(const QString &text, const QString &mountRoot, QString *error)
{
    KateRemote::InputError inputError;
    const auto url = KateRemote::urlForInput(text, mountRoot, &inputError);
    switch (inputError) {
    case KateRemote::InputError::None:
        break;
    case KateRemote::InputError::Empty:
        *error = i18n("Enter a remote folder.");
        break;
    case KateRemote::InputError::LocalPath:
        *error = i18n("%1 is a local folder; use File > Open Folder for local folders.", text.trimmed());
        break;
    case KateRemote::InputError::Syntax:
        *error = i18n("Enter sftp://host/path or host:/path.");
        break;
    case KateRemote::InputError::RelativePath:
        *error = i18n("Use an absolute path on the remote computer (host:/path).");
        break;
    case KateRemote::InputError::Unsupported:
        *error = i18n("%1 cannot be opened: not a remote folder on a valid host (supported: %2).",
                      text.trimmed(),
                      QStringList(QStringList{QStringLiteral("sftp")} + KateRemote::kioSchemes()).join(QStringLiteral(", ")));
        break;
    case KateRemote::InputError::RemoteRoot:
        *error = i18n("Choose a folder (host:/path): a project of the whole remote file system would list every file on it.");
        break;
    }
    return url;
}

// remote:/ entries ("network folders") are links to their real URL; the
// dialog may return the link itself.
static QUrl resolveNetworkFolder(const QUrl &url)
{
    if (url.scheme() != QLatin1String("remote")) {
        return url;
    }
    KIO::StatJob *job = KIO::stat(url, KIO::StatJob::SourceSide, KIO::StatDefaultDetails, KIO::HideProgressInfo);
    if (!job->exec()) {
        return url;
    }
    const QString target = job->statResult().stringValue(KIO::UDSEntry::UDS_TARGET_URL);
    if (!target.isEmpty()) {
        return QUrl(target);
    }
    // Redirected (an entry below a network folder).
    return job->url();
}

void RemoteProjectsView::askRemoteFolder()
{
    // KDE's folder dialog (KIO) browses sftp:// itself; its result is opened
    // through kate-remote-fs instead of KIO. Folders below the mount root
    // (local paths of remote folders) are accepted too.
    const QString mountRoot = m_plugin->mountRoot();
    // Start in KIO's Network view (remote:/, network folders); recent remote
    // folders are in Open Recent Remote Folder.
    QUrl start(QStringLiteral("remote:/"));
    const QStringList schemes = QStringList{QStringLiteral("sftp"), QStringLiteral("remote"), QStringLiteral("file")} + KateRemote::kioSchemes();
    while (true) {
        const QUrl chosen = QFileDialog::getExistingDirectoryUrl(m_mainWindow->window(),
                                                                 i18n("Open Remote Folder (sftp://host/path)"),
                                                                 start,
                                                                 QFileDialog::ShowDirsOnly,
                                                                 schemes);
        if (chosen.isEmpty() || !m_mainWindow) {
            return;
        }
        const QUrl resolved = resolveNetworkFolder(chosen);
        if (resolved != chosen) {
            qInfo() << "[RemoteProjects] Network folder" << chosen << "is" << resolved;
        }
        QString error;
        const auto url = parseInput(resolved.isLocalFile() ? resolved.toLocalFile() : resolved.toString(), mountRoot, &error);
        if (url) {
            openUrl(*url, true);
            return;
        }
        // Started by the user (not by an agent): a modal message is fine.
        QMessageBox::warning(m_mainWindow->window(), i18n("Open Remote Folder"), error);
        start = chosen;
    }
}

void RemoteProjectsView::openUrl(const QUrl &requested, bool userFromKio)
{
    if (!m_mainWindow->pluginView(QStringLiteral("kateprojectplugin"))) {
        showMessage(i18n("Opening a remote folder needs the Project plugin: enable it in Settings > Configure Kate > Plugins."), QStringLiteral("Error"));
        return;
    }
    const auto location = KateRemote::locationForUrl(requested);
    // Only sftp: sshfs (ssh) chooses the user itself; KIO's workers (the
    // other schemes, through kio-fuse) find the user in the cache as well.
    const auto target = location && location->scheme == QLatin1String("sftp") ? KateRemote::parseSshAuthority(location->authority) : std::nullopt;
    if (!userFromKio || !target || !target->user.isEmpty()) {
        openUrlAs(requested);
        return;
    }
    // Chosen in the folder dialog without a user: KIO may have logged in to
    // the host as someone else (a username typed into its login dialog
    // while browsing). Its password cache knows (kept while this window is
    // open); use that user, or ssh would log in as the default one. Looked
    // up off the UI thread (the client waits in an event loop).
    const qlonglong windowId = m_mainWindow->window()->winId();
    const QUrl authUrl = KateRemote::kioAuthUrl(*target);
    auto user = std::make_shared<QString>();
    QThread *thread = QThread::create([authUrl, windowId, user] {
        KIO::AuthInfo info;
        info.url = authUrl;
        KPasswdServerClient client;
        if (client.checkAuthInfo(&info, windowId, 0)) {
            *user = info.username;
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, requested, user] {
        QUrl url = requested;
        if (!user->isEmpty()) {
            url.setUserName(*user);
            if (!KateRemote::locationForUrl(url)) {
                url = requested; // a user name ssh would not accept
            }
        }
        openUrlAs(url);
    });
    thread->start();
}

void RemoteProjectsView::openUrlAs(const QUrl &url)
{
    showMessage(i18n("Connecting to %1…", url.host()), QStringLiteral("Info"));
    m_plugin->prepareUrl(url, this, [this, url](const QString &path, const QString &error) {
        if (!error.isEmpty()) {
            showMessage(error, QStringLiteral("Error"));
            return;
        }
        QObject *projectView = m_mainWindow ? m_mainWindow->pluginView(QStringLiteral("kateprojectplugin")) : nullptr;
        if (!projectView) {
            return;
        }
        // Opens (or switches to) the project and shows the project panel.
        QMetaObject::invokeMethod(projectView, "openDirectoryOrProject", Q_ARG(QDir, QDir(path)));
        m_plugin->projectOpened(path);
        m_plugin->addRecentUrl(url);
    });
}

void RemoteProjectsView::showMessage(const QString &text, const QString &type)
{
    if (!m_mainWindow) {
        return;
    }
    QVariantMap message;
    message[QStringLiteral("text")] = text;
    message[QStringLiteral("type")] = type;
    message[QStringLiteral("category")] = i18n("Remote Projects");
    m_mainWindow->showMessage(message);
}
