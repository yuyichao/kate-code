#include "RemoteProjectsView.h"
#include "RemoteProjectsPlugin.h"
#include "RemoteTerminal.h"

#include "RemoteLocation.h"

#include <KIO/AuthInfo>
#include <KIO/StatJob>
#include <KPasswdServerClient>
#include <KActionCollection>
#include <KActionMenu>
#include <KLocalizedString>
#include <KTextEditor/Document>
#include <KTextEditor/MainWindow>
#include <KTextEditor/View>
#include <KXMLGUIFactory>

#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QMenu>
#include <QMessageBox>
#include <QThread>
#include <QVBoxLayout>

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

    // The Remote Terminal tab appears once a remote folder on an ssh host
    // is active in this window.
    connect(m_mainWindow, &KTextEditor::MainWindow::viewChanged, this, &RemoteProjectsView::updateTerminalLocation);
    connect(m_mainWindow, &KTextEditor::MainWindow::pluginViewCreated, this, [this](const QString &name, QObject *view) {
        if (name == QLatin1String("kateprojectplugin")) {
            watchProjectView(view);
            updateTerminalLocation();
        }
    });
    watchProjectView(m_mainWindow->pluginView(QStringLiteral("kateprojectplugin")));
    updateTerminalLocation();
}

void RemoteProjectsView::createTerminalToolView()
{
    // Next to Kate's terminal: a shell on the current remote folder's host.
    m_terminalToolView = m_mainWindow->createToolView(m_plugin,
                                                      QStringLiteral("kateremoteterminal"),
                                                      KTextEditor::MainWindow::Bottom,
                                                      QIcon::fromTheme(QStringLiteral("utilities-terminal")),
                                                      i18n("Remote Terminal"));
    if (!m_terminalToolView) {
        return;
    }
    m_terminalPanel = new RemoteTerminalPanel(m_terminalToolView);
    auto *layout = qobject_cast<QBoxLayout *>(m_terminalToolView->layout());
    if (!layout) {
        layout = new QVBoxLayout(m_terminalToolView);
        layout->setContentsMargins(0, 0, 0, 0);
    }
    if (layout->indexOf(m_terminalPanel) < 0) {
        layout->addWidget(m_terminalPanel);
    }
    m_terminalToolView->setFocusProxy(m_terminalPanel);
    // Like Kate's terminal: the panel hides when its shell ends.
    connect(m_terminalPanel, &RemoteTerminalPanel::currentEnded, this, [this] {
        if (m_mainWindow && m_terminalToolView) {
            m_mainWindow->hideToolView(m_terminalToolView);
        }
    });
}

void RemoteProjectsView::watchProjectView(QObject *projectView)
{
    if (!projectView) {
        return;
    }
    // The project plugin's view has no public header: by name.
    // projectFileNameChanged: another project became the current one.
    for (const char *signal : {SIGNAL(projectFileNameChanged()), SIGNAL(projectMapChanged())}) {
        if (projectView->metaObject()->indexOfSignal(QMetaObject::normalizedSignature(signal + 1).constData()) >= 0) {
            connect(projectView, signal, this, SLOT(updateTerminalLocation()), Qt::UniqueConnection);
        }
    }
}

std::optional<KateRemote::Location> RemoteProjectsView::currentSshLocation() const
{
    if (!m_mainWindow) {
        return std::nullopt;
    }
    const QString mountRoot = m_plugin->mountRoot();
    const auto sshLocation = [&mountRoot](const QString &path) -> std::optional<KateRemote::Location> {
        if (path.isEmpty()) {
            return std::nullopt;
        }
        auto location = KateRemote::locationForPath(path, mountRoot);
        if (!location || !KateRemote::isSshScheme(location->scheme)) {
            return std::nullopt;
        }
        return location;
    };
    QObject *projectView = m_mainWindow->pluginView(QStringLiteral("kateprojectplugin"));
    if (auto location = sshLocation(projectView ? projectView->property("projectBaseDir").toString() : QString())) {
        return location;
    }
    KTextEditor::View *view = m_mainWindow->activeView();
    const QUrl url = view ? view->document()->url() : QUrl();
    return url.isLocalFile() ? sshLocation(QFileInfo(url.toLocalFile()).absolutePath()) : std::nullopt;
}

void RemoteProjectsView::updateTerminalLocation()
{
    const auto location = currentSshLocation();
    if (!m_terminalToolView && location) {
        createTerminalToolView();
    }
    if (m_terminalPanel) {
        m_terminalPanel->setLocation(location);
    }
}

RemoteProjectsView::~RemoteProjectsView()
{
    // Tool views belong to the main window: unloading the plugin ends the
    // terminals too.
    delete m_terminalToolView.data();
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
