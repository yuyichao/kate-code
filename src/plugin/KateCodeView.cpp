#include "KateCodeView.h"
#include "KateCodePlugin.h"
#include "../acp/ACPModels.h"
#include "../acp/ACPSessionDeleter.h"
#include "../config/SettingsStore.h"
#include "../mcp/EditorDBusService.h"
#include "../mcp/EditorSession.h"
#include "../ui/ChatWidget.h"
#include "../ui/SessionPanel.h"
#include "../ui/SessionSelectionDialog.h"
#include "../util/AgentIcons.h"
#include "../util/SessionStore.h"
#include "../util/SummaryStore.h"
#include "../util/DiffHighlightManager.h"

#include <KActionCollection>
#include <KLocalizedString>
#include <KTextEditor/Application>
#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/View>
#include <KXMLGUIFactory>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QQuickWidget>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QTextStream>
#include <QVBoxLayout>
#include <functional>

KateCodeView::KateCodeView(KateCodePlugin *plugin, KTextEditor::MainWindow *mainWindow)
    : QObject(mainWindow)
    , KXMLGUIClient()
    , m_plugin(plugin)
    , m_mainWindow(mainWindow)
    , m_toolView(nullptr)
    , m_sessionStore(new SessionStore(this))
    , m_summaryStore(new SummaryStore(this))
    , m_diffHighlightManager(nullptr)
{
    createToolView();

    // Remember the last editor view: when a chat tab is active,
    // activeView() no longer names the file the user is working on.
    m_lastEditorView = m_mainWindow->activeView();
    connect(m_mainWindow, &KTextEditor::MainWindow::viewChanged, this, [this](KTextEditor::View *view) {
        if (view) {
            m_lastEditorView = view;
            // The resumable sessions follow the current project.
            scheduleSessionPanelRefresh();
        }
    });

    // Track the chat the user last worked in (for quick actions).
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        if (!now) {
            return;
        }
        for (ChatWidget *chat : liveChats()) {
            if (chat == now || chat->isAncestorOf(now)) {
                if (m_lastActiveChat != chat) {
                    m_lastActiveChat = chat;
                    scheduleSessionPanelRefresh();
                }
                return;
            }
        }
    });

    // Create diff highlight manager for showing pending edits in editor
    m_diffHighlightManager = new DiffHighlightManager(m_mainWindow, m_plugin->settings(), this);

    // Editor diff highlighting disabled - diffs are shown inline in tool call UI instead
    // If re-enabling, connect ChatWidget::toolCallHighlightRequested/toolCallClearRequested
    // to m_diffHighlightManager->highlightToolCall() and clearToolCallHighlights()
    Q_UNUSED(m_diffHighlightManager);

    // Load UI definition file from Qt resources
    setXMLFile(QStringLiteral(":/katecode/katecodeui.rc"));
    qDebug() << "[KateCodeView] Loaded UI file from resources: :/katecode/katecodeui.rc";

    QAction *newSessionAction = new QAction(QIcon::fromTheme(QStringLiteral("code-context")),
                                            i18n("New Kate Code Session"),
                                            this);
    actionCollection()->addAction(QStringLiteral("kate_code_new_session"), newSessionAction);
    connect(newSessionAction, &QAction::triggered, this, &KateCodeView::newSession);

    // Create context menu action for adding selection to context
    QAction *addContextAction = new QAction(QIcon::fromTheme(QStringLiteral("list-add")),
                                           i18n("Add to Claude Context..."),
                                           this);
    actionCollection()->addAction(QStringLiteral("kate_code_add_context"), addContextAction);
    actionCollection()->setDefaultShortcut(addContextAction, Qt::CTRL | Qt::ALT | Qt::Key_A);
    connect(addContextAction, &QAction::triggered, this, &KateCodeView::addSelectionToContext);
    qDebug() << "[KateCodeView] Registered action: kate_code_add_context with shortcut Ctrl+Alt+A";

    // Quick Action: Explain Code
    QAction *explainAction = new QAction(QIcon::fromTheme(QStringLiteral("help-about")),
                                         i18n("Explain Code"),
                                         this);
    actionCollection()->addAction(QStringLiteral("kate_code_explain"), explainAction);
    actionCollection()->setDefaultShortcut(explainAction, Qt::CTRL | Qt::ALT | Qt::Key_E);
    connect(explainAction, &QAction::triggered, this, &KateCodeView::explainCode);

    // Quick Action: Find Bugs
    QAction *findBugsAction = new QAction(QIcon::fromTheme(QStringLiteral("tools-report-bug")),
                                          i18n("Find Bugs"),
                                          this);
    actionCollection()->addAction(QStringLiteral("kate_code_find_bugs"), findBugsAction);
    actionCollection()->setDefaultShortcut(findBugsAction, Qt::CTRL | Qt::ALT | Qt::Key_B);
    connect(findBugsAction, &QAction::triggered, this, &KateCodeView::findBugs);

    // Quick Action: Suggest Improvements
    QAction *improvementsAction = new QAction(QIcon::fromTheme(QStringLiteral("tools-wizard")),
                                              i18n("Suggest Improvements"),
                                              this);
    actionCollection()->addAction(QStringLiteral("kate_code_improvements"), improvementsAction);
    actionCollection()->setDefaultShortcut(improvementsAction, Qt::CTRL | Qt::ALT | Qt::Key_I);
    connect(improvementsAction, &QAction::triggered, this, &KateCodeView::suggestImprovements);

    // Quick Action: Add Tests
    QAction *addTestsAction = new QAction(QIcon::fromTheme(QStringLiteral("document-new")),
                                          i18n("Add Tests"),
                                          this);
    actionCollection()->addAction(QStringLiteral("kate_code_add_tests"), addTestsAction);
    actionCollection()->setDefaultShortcut(addTestsAction, Qt::CTRL | Qt::ALT | Qt::Key_T);
    connect(addTestsAction, &QAction::triggered, this, &KateCodeView::addTests);

    qDebug() << "[KateCodeView] Registered Quick Actions: Explain, Find Bugs, Improvements, Add Tests";

    // Register the action with Kate's editor context menu
    m_mainWindow->guiFactory()->addClient(this);
    qDebug() << "[KateCodeView] Added XMLGUI client to Kate";
}

KateCodeView::~KateCodeView()
{
    // Remove this view from the plugin's list so onAboutToQuit() cannot
    // dereference us after we are destroyed (e.g. when a window is closed
    // before the application quits).  We do NOT call prepareForShutdown() here:
    // the main window may already be tearing down its chat tabs.  ChatWidget
    // severs its own signals in its destructor, and onAboutToQuit() still runs
    // prepareForShutdown() on the application-quit path while everything is
    // alive.
    m_plugin->removeView(this);

    // Remove XMLGUI client before destruction to prevent leaks and crashes
    if (m_mainWindow && m_mainWindow->guiFactory()) {
        m_mainWindow->guiFactory()->removeClient(this);
    }

    // ToolView is owned by MainWindow and cleaned up automatically. Chat tabs
    // are owned by the main window too; when the plugin is unloaded while the
    // window stays open, close them with it. While the window itself is
    // closing (hidden first), leave them to its own teardown.
    QWidget *window = m_mainWindow ? m_mainWindow->window() : nullptr;
    if (window && window->isVisible()) {
        for (ChatWidget *chat : liveChats()) {
            m_mainWindow->removeWidget(chat);
        }
    }
}

void KateCodeView::createToolView()
{
    // Side panel: the session list. The chats themselves are editor tabs.
    m_toolView = m_mainWindow->createToolView(
        m_plugin,
        QStringLiteral("katecode"),
        KTextEditor::MainWindow::Left,
        QIcon::fromTheme(QStringLiteral("code-context")),
        i18n("Kate Code")
    );

    if (!m_toolView) {
        qWarning() << "[KateCode] createToolView returned null";
        return;
    }

    QVBoxLayout *layout = qobject_cast<QVBoxLayout *>(m_toolView->layout());
    if (!layout) {
        layout = new QVBoxLayout(m_toolView);
    }
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // Chats render through a QWebEngineView, which needs the main window to
    // composite through the GPU (RHI). The first such widget in a window that
    // is already shown makes Qt destroy and recreate the native window, which
    // the compositor animates like a window closing and reopening. A hidden
    // QQuickWidget (what QWebEngineView uses internally) switches the window
    // over now, while the plugin view is set up, usually before it is shown.
    auto *rhiWarmup = new QQuickWidget(m_toolView);
    rhiWarmup->setObjectName(QStringLiteral("katecodeRhiWarmup"));
    rhiWarmup->setFixedSize(0, 0);
    rhiWarmup->hide();

    m_sessionPanel = new SessionPanel(m_toolView);
    layout->addWidget(m_sessionPanel);
    connect(m_sessionPanel, &SessionPanel::newSessionRequested, this, &KateCodeView::newSession);
    connect(m_sessionPanel, &SessionPanel::browseRequested, this, &KateCodeView::browseSessions);
    connect(m_sessionPanel, &SessionPanel::resumeRequested, this, &KateCodeView::resumeSavedSession);
    connect(m_sessionPanel, &SessionPanel::deleteRequested, this, &KateCodeView::deleteSavedSession);
    connect(m_sessionPanel, &SessionPanel::showAllDirectoriesChanged, this, [this](bool showAll) {
        m_showAllSessions = showAll;
        scheduleSessionPanelRefresh();
    });
    connect(m_sessionPanel, &SessionPanel::entryActivated, this, [this](int index) {
        const QList<ChatWidget *> chats = liveChats();
        if (index >= 0 && index < chats.size()) {
            activateChat(chats.at(index));
        }
    });
}

ChatWidget *KateCodeView::createChat(bool startSession)
{
    auto *chat = new ChatWidget();

    // Kate owns the chat once it is added, so it can outlive this view (for
    // example when the plugin is unloaded); every callback checks for that.
    QPointer<KateCodeView> self(this);
    QPointer<ChatWidget> chatGuard(chat);
    KateCodePlugin *plugin = m_plugin;
    // Resuming a saved session that another chat (in any window) already has
    // open switches to that chat instead of loading it a second time.
    chat->setOpenSessionLocator([plugin, chatGuard](const QString &sessionId) {
        for (KateCodeView *view : plugin->views()) {
            if (view->activateChatWithSession(sessionId, chatGuard)) {
                return true;
            }
        }
        return false;
    });

    chat->setFilePathProvider([self]() { return self ? self->getCurrentFilePath() : QString(); });
    chat->setSelectionProvider([self]() { return self ? self->getCurrentSelection() : QString(); });
    chat->setProjectRootProvider([self]() { return self ? self->getProjectRoot() : QDir::homePath(); });
    chat->setFileListProvider([self](const QString &root) {
        return self ? self->getProjectPluginFiles(root) : QStringList();
    });
    chat->setDocumentProvider([self](const QString &path) {
        return self ? self->findDocumentByPath(path) : nullptr;
    });

    chat->setSettingsStore(m_plugin->settings());


    connect(chat, &ChatWidget::jumpToEditRequested, this, &KateCodeView::jumpToEdit);

    // The chat's own editor DBus endpoint: its agent's Kate MCP server calls
    // this object, so questions and edit notifications reach this chat only.
    // Owned by the chat; the terminal tool uses this chat's window.
    EditorSession *editorSession = m_plugin->dbusService()->createSession(chat);
    editorSession->setMainWindowProvider([self]() { return self ? self->m_mainWindow : nullptr; });
    chat->setEditorSession(editorSession);

    connect(chat, &ChatWidget::debugLogMessage, this, [this](const QString &message) {
        QVariantMap msg;
        msg[QStringLiteral("text")] = message;
        msg[QStringLiteral("type")] = QStringLiteral("Log");
        msg[QStringLiteral("category")] = QStringLiteral("Kate Code");
        m_mainWindow->showMessage(msg);
    });

    connect(chat, &ChatWidget::sessionInfoChanged, this, &KateCodeView::scheduleSessionPanelRefresh);
    connect(chat, &QObject::destroyed, this, [this]() {
        m_chats.removeAll(nullptr);
        scheduleSessionPanelRefresh();
    });

    m_chats.append(chat);
    m_lastActiveChat = chat;

    if (!m_mainWindow->addWidget(chat)) {
        // Hosts older than KTextEditor 5.98 cannot show widgets as tabs.
        qWarning() << "[KateCode] Main window refused the chat widget";
        delete chat;
        return nullptr;
    }
    scheduleSessionPanelRefresh();
    // Connect right away; there is no manual connect step.
    if (startSession) {
        chat->startSession();
    }
    return chat;
}

void KateCodeView::resumeSavedSession(const QString &sessionId, const QString &projectRoot)
{
    if (sessionId.isEmpty() || projectRoot.isEmpty()) {
        return;
    }
    // Already open in a chat (in any window): switch to it.
    for (KateCodeView *view : m_plugin->views()) {
        if (view->activateChatWithSession(sessionId, nullptr)) {
            return;
        }
    }
    const QString providerId = m_sessionStore->findSession(projectRoot, sessionId).providerId;
    if (ChatWidget *chat = createChat(false)) {
        activateChat(chat);
        chat->resumeSavedSession(sessionId, projectRoot, providerId);
    }
}

void KateCodeView::browseSessions()
{
    const QString projectRoot = getProjectRoot();
    QWidget *parent = m_mainWindow->window();
    if (m_summaryStore->listTranscriptSessions(projectRoot).isEmpty()) {
        QMessageBox::information(parent, QStringLiteral("Kate Code"),
                                 QStringLiteral("No previous sessions found for %1.").arg(projectRoot));
        return;
    }
    SessionSelectionDialog dialog(projectRoot, m_summaryStore, parent);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    switch (dialog.selectedResult()) {
    case SessionSelectionDialog::Result::Resume:
        resumeSavedSession(dialog.selectedSessionId(), projectRoot);
        break;
    case SessionSelectionDialog::Result::NewSession:
        newSession();
        break;
    case SessionSelectionDialog::Result::Cancelled:
        break;
    }
}

void KateCodeView::deleteSavedSession(const QString &sessionId, const QString &projectRoot,
                                      const QString &transcriptPath)
{
    if (sessionId.isEmpty() || projectRoot.isEmpty()) {
        return;
    }
    QWidget *parent = m_mainWindow->window();
    if (isSessionOpen(sessionId)) {
        QMessageBox::information(parent, QStringLiteral("Kate Code"),
                                 QStringLiteral("This session is open in a chat. Close the chat to delete it."));
        return;
    }

    // Sessions restored by context injection continue in a new agent
    // session; the old ones are hidden from the list and go with it.
    const QList<SessionEntry> stored = m_sessionStore->listSessions(projectRoot);
    QStringList ids = {sessionId};
    for (int i = 0; i < ids.size(); ++i) {
        for (const SessionEntry &entry : stored) {
            if (entry.replacedBy == ids.at(i) && !ids.contains(entry.id) && !isSessionOpen(entry.id)) {
                ids << entry.id;
            }
        }
    }
    QHash<QString, SessionEntry> byId;
    for (const SessionEntry &entry : stored) {
        byId.insert(entry.id, entry);
    }

    // The agents that keep their own copy of these sessions.
    SettingsStore *settings = m_plugin->settings();
    QHash<QString, QStringList> idsByProvider;
    QStringList agentNames;
    for (const QString &id : std::as_const(ids)) {
        const QString providerId = byId.value(id).providerId;
        const ACPProvider provider = providerId.isEmpty() ? ACPProvider() : settings->providerById(providerId);
        if (provider.executable.isEmpty() || !SettingsStore::isExecutableAvailable(provider.executable)) {
            continue;
        }
        if (!idsByProvider.contains(providerId)) {
            agentNames << provider.description;
        }
        idsByProvider[providerId] << id;
    }

    const SessionEntry session = byId.value(sessionId);
    QString title = !session.title.isEmpty() ? session.title : session.firstPrompt;
    if (title.isEmpty()) {
        title = m_transcriptInfo.value(transcriptPath).firstPrompt;
    }
    if (title.size() > 80) {
        title = title.left(79).trimmed() + QChar(0x2026);
    }
    QString text = title.isEmpty() ? QStringLiteral("Delete this session?")
                                   : QStringLiteral("Delete the session \u201c%1\u201d?").arg(title);
    QString info = QStringLiteral("Its transcript and summary are removed from Kate Code");
    if (ids.size() > 1) {
        info += QStringLiteral(", together with the %1 earlier session(s) it was restored from").arg(ids.size() - 1);
    }
    info += QStringLiteral(". This cannot be undone.");

    QMessageBox box(QMessageBox::Question, QStringLiteral("Delete Session"), text,
                    QMessageBox::Cancel, parent);
    box.setTextFormat(Qt::PlainText);
    box.setInformativeText(info);
    QPushButton *deleteButton = box.addButton(QStringLiteral("Delete"), QMessageBox::DestructiveRole);
    deleteButton->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete")));
    box.setDefaultButton(QMessageBox::Cancel);
    QCheckBox *agentCheck = nullptr;
    if (!agentNames.isEmpty()) {
        agentCheck = new QCheckBox(QStringLiteral("Also delete it from %1's own session history")
                                       .arg(agentNames.join(QStringLiteral(", "))),
                                   &box);
        agentCheck->setToolTip(QStringLiteral("Asks the agent to delete the session (ACP session/delete), "
                                              "so it can no longer be resumed elsewhere either"));
        agentCheck->setChecked(true);
        box.setCheckBox(agentCheck);
    }
    box.exec();
    if (box.clickedButton() != deleteButton) {
        return;
    }
    const bool deleteFromAgent = agentCheck && agentCheck->isChecked();

    // The panel may have been refreshed (or the session resumed elsewhere)
    // while the box was open.
    for (const QString &id : std::as_const(ids)) {
        if (isSessionOpen(id)) {
            QMessageBox::information(parent, QStringLiteral("Kate Code"),
                                     QStringLiteral("This session has been opened in a chat meanwhile; it was not deleted."));
            return;
        }
    }

    QStringList failed;
    auto removeFile = [&failed](const QString &path) {
        if (!path.isEmpty() && QFileInfo::exists(path) && !QFile::remove(path)) {
            failed << path;
        }
    };
    for (const QString &id : std::as_const(ids)) {
        // The listed transcript's own path: in the all-directories list the
        // directory comes from its header and the folder name is lossy.
        const QString path = id == sessionId && !transcriptPath.isEmpty()
            ? transcriptPath
            : m_summaryStore->transcriptPath(projectRoot, id);
        removeFile(path);
        removeFile(m_summaryStore->summaryPath(projectRoot, id));
        m_sessionStore->removeSession(projectRoot, id);
        m_transcriptInfo.remove(path);
        for (KateCodeView *view : m_plugin->views()) {
            for (ChatWidget *chat : view->liveChats()) {
                chat->forgetSavedSession(id);
            }
        }
    }
    if (!failed.isEmpty()) {
        QMessageBox::warning(parent, QStringLiteral("Kate Code"),
                             QStringLiteral("Could not delete:\n%1").arg(failed.join(QLatin1Char('\n'))));
    }

    if (deleteFromAgent) {
        for (auto it = idsByProvider.cbegin(); it != idsByProvider.cend(); ++it) {
            const ACPProvider provider = settings->providerById(it.key());
            const QStringList args = provider.options.isEmpty() ? QStringList()
                                                                : QProcess::splitCommand(provider.options);
            const QString workingDir = QFileInfo(projectRoot).isDir() ? projectRoot : QDir::homePath();
            // Owned by the plugin, so it finishes even if this window closes.
            auto *deleter = new ACPSessionDeleter(provider.executable, args, workingDir, it.value(), m_plugin);
            QPointer<KateCodeView> self(this);
            const QString agentName = provider.description;
            const int count = it.value().size();
            connect(deleter, &ACPSessionDeleter::finished, m_plugin,
                    [self, agentName, count](int deleted, const QString &error) {
                        if (!self) {
                            return;
                        }
                        QVariantMap msg;
                        msg[QStringLiteral("category")] = QStringLiteral("Kate Code");
                        if (error.isEmpty()) {
                            msg[QStringLiteral("type")] = QStringLiteral("Log");
                            msg[QStringLiteral("text")] =
                                QStringLiteral("Deleted %1 session(s) from %2.").arg(deleted).arg(agentName);
                        } else {
                            msg[QStringLiteral("type")] = QStringLiteral("Warning");
                            msg[QStringLiteral("text")] =
                                QStringLiteral("Could not delete %1 of %2 session(s) from %3: %4")
                                    .arg(count - deleted).arg(count).arg(agentName, error);
                        }
                        qDebug() << "[KateCodeView]" << msg.value(QStringLiteral("text")).toString();
                        self->m_mainWindow->showMessage(msg);
                    });
            deleter->start();
        }
    }

    for (KateCodeView *view : m_plugin->views()) {
        view->scheduleSessionPanelRefresh();
    }
}

bool KateCodeView::isSessionOpen(const QString &sessionId) const
{
    for (KateCodeView *view : m_plugin->views()) {
        for (ChatWidget *chat : view->liveChats()) {
            if (chat->hasAgentSession(sessionId)) {
                return true;
            }
        }
    }
    return false;
}

// The project a transcript belongs to (from its header), whether it has a
// user message and the first one's text. The latter are only a fallback for
// sessions the session store does not track: the injected context of a
// restored session embeds an earlier transcript, which cannot be told apart
// from real messages. Reads only as far as needed.
static void readTranscriptStart(const QString &path, QString &projectRoot,
                                bool &hasUserMessage, QString &firstPrompt)
{
    projectRoot.clear();
    hasUserMessage = false;
    firstPrompt.clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    QTextStream stream(&file);
    bool inHeader = true;
    bool inUserMessage = false;
    qint64 linesRead = 0;
    QString line;
    while (stream.readLineInto(&line) && ++linesRead < 5000) {
        // The header written by TranscriptWriter::startSession() ends at the
        // first rule.
        if (inHeader) {
            if (line.startsWith(QStringLiteral("- **Project:** "))) {
                projectRoot = line.mid(15).trimmed();
            } else if (line == QStringLiteral("---")) {
                inHeader = false;
            }
            continue;
        }
        if (line.startsWith(QStringLiteral("## "))) {
            inUserMessage = line.startsWith(QStringLiteral("## User ("));
            continue;
        }
        if (!inUserMessage) {
            continue;
        }
        const QString text = line.trimmed();
        if (text.isEmpty()) {
            continue;
        }
        if (text.startsWith(QStringLiteral("<system-reminder>"))) {
            inUserMessage = false;  // a session restore's injected context
            continue;
        }
        hasUserMessage = true;
        firstPrompt = text;
        return;
    }
}

void KateCodeView::newSession()
{
    if (ChatWidget *chat = createChat()) {
        activateChat(chat);
    }
}

void KateCodeView::activateChat(ChatWidget *chat)
{
    if (!chat) {
        return;
    }
    m_lastActiveChat = chat;
    m_mainWindow->activateWidget(chat);
    chat->setFocus();
    scheduleSessionPanelRefresh();
}

bool KateCodeView::activateChatWithSession(const QString &sessionId, ChatWidget *exclude)
{
    for (ChatWidget *chat : liveChats()) {
        if (chat != exclude && chat->hasAgentSession(sessionId)) {
            if (QWidget *window = m_mainWindow->window()) {
                window->activateWindow();
            }
            activateChat(chat);
            return true;
        }
    }
    return false;
}

QList<ChatWidget *> KateCodeView::liveChats() const
{
    QList<ChatWidget *> chats;
    for (const QPointer<ChatWidget> &chat : m_chats) {
        if (chat) {
            chats.append(chat);
        }
    }
    return chats;
}

ChatWidget *KateCodeView::targetChat()
{
    const QList<ChatWidget *> chats = liveChats();
    if (auto *active = qobject_cast<ChatWidget *>(m_mainWindow->activeWidget()); active && chats.contains(active)) {
        return active;
    }
    if (m_lastActiveChat) {
        return m_lastActiveChat;
    }
    if (!chats.isEmpty()) {
        return chats.constLast();
    }
    return createChat();
}

KTextEditor::View *KateCodeView::editorView() const
{
    if (KTextEditor::View *view = m_mainWindow->activeView()) {
        return view;
    }
    return m_lastEditorView;
}

void KateCodeView::scheduleSessionPanelRefresh()
{
    if (m_panelRefreshPending) {
        return;
    }
    m_panelRefreshPending = true;
    QMetaObject::invokeMethod(this, [this]() {
        m_panelRefreshPending = false;
        refreshSessionPanel();
    }, Qt::QueuedConnection);
}

void KateCodeView::refreshSessionPanel()
{
    if (!m_sessionPanel) {
        return;
    }
    QList<SessionPanel::Entry> entries;
    for (ChatWidget *chat : liveChats()) {
        SessionPanel::Entry entry;
        entry.title = chat->chatTitle().isEmpty() ? QStringLiteral("New session") : chat->chatTitle();
        switch (chat->connectionStatus()) {
        case ConnectionStatus::Connected:
            entry.status = QStringLiteral("Connected");
            entry.statusColor = QColor(0x5c, 0xb8, 0x5c);
            break;
        case ConnectionStatus::Connecting:
            entry.status = QStringLiteral("Connecting");
            entry.statusColor = QColor(0xf0, 0xad, 0x4e);
            break;
        case ConnectionStatus::Error:
            entry.status = QStringLiteral("Error");
            entry.statusColor = QColor(0xd9, 0x53, 0x4f);
            break;
        case ConnectionStatus::Disconnected:
            entry.status = QStringLiteral("Not connected");
            entry.statusColor = QColor(0x88, 0x88, 0x88);
            break;
        }

        entry.icon = chat->providerIcon();
        // Context usage is shown in the chat's own status row.
        const SessionUsage usage = chat->usage();
        if (usage.cost >= 0) {
            entry.detail = usage.currency == QStringLiteral("USD")
                ? QStringLiteral("$%1").arg(usage.cost, 0, 'f', 2)
                : QStringLiteral("%1 %2").arg(usage.cost, 0, 'f', 2).arg(usage.currency);
        }
        entry.active = (chat == m_lastActiveChat);
        entries.append(entry);
    }
    m_sessionPanel->setEntries(entries);
    refreshResumableSessions();
}

void KateCodeView::refreshResumableSessions()
{
    constexpr int maxEntries = 20;
    constexpr int maxTitleLength = 80;

    const QString currentRoot = getProjectRoot();
    auto locationName = [](const QString &root) {
        if (QDir::cleanPath(root) == QDir::homePath()) {
            return QStringLiteral("~");
        }
        const QString name = QDir(root).dirName();
        return name.isEmpty() ? root : name;
    };

    // Most recently active first.
    const QStringList paths = m_showAllSessions
        ? m_summaryStore->listAllTranscriptPaths()
        : [this, &currentRoot]() {
              QStringList list;
              for (const QString &id : m_summaryStore->listTranscriptSessions(currentRoot)) {
                  list << m_summaryStore->transcriptPath(currentRoot, id);
              }
              return list;
          }();

    SettingsStore *settings = m_plugin->settings();
    QHash<QString, QHash<QString, SessionEntry>> stored; // root -> id -> entry
    QList<SessionPanel::ResumableEntry> entries;
    for (const QString &path : paths) {
        if (entries.size() >= maxEntries) {
            break;
        }
        const QFileInfo file(path);
        const QString sessionId = file.completeBaseName();
        const QDateTime modified = file.lastModified();
        TranscriptInfo &info = m_transcriptInfo[path];
        if (info.modified != modified) {
            info.modified = modified;
            readTranscriptStart(path, info.projectRoot, info.hasUserMessage, info.firstPrompt);
        }
        // The transcript header has the real directory (the folder name
        // encoding is lossy); the current project's list uses its root.
        const QString root = m_showAllSessions ? info.projectRoot : currentRoot;
        if (root.isEmpty() || sessionId.isEmpty() || isSessionOpen(sessionId)) {
            continue;
        }

        if (!stored.contains(root)) {
            QHash<QString, SessionEntry> byId;
            for (const SessionEntry &entry : m_sessionStore->listSessions(root)) {
                byId.insert(entry.id, entry);
            }
            stored.insert(root, byId);
        }
        const SessionEntry session = stored.value(root).value(sessionId);
        // Restored into a newer session, which is listed instead.
        if (!session.replacedBy.isEmpty()) {
            continue;
        }
        // A session the user never prompted has nothing to resume.
        const QString firstPrompt = session.tracked ? session.firstPrompt
            : info.hasUserMessage ? info.firstPrompt : QString();
        if (firstPrompt.isEmpty()) {
            continue;
        }

        SessionPanel::ResumableEntry entry;
        entry.sessionId = sessionId;
        entry.projectRoot = root;
        entry.transcriptPath = path;
        const QString fullTitle = !session.title.isEmpty() ? session.title : firstPrompt;
        entry.title = fullTitle.size() > maxTitleLength
            ? fullTitle.left(maxTitleLength - 1).trimmed() + QChar(0x2026)
            : fullTitle;
        const QString when = modified.toString(QStringLiteral("yyyy-MM-dd hh:mm"));
        entry.detail = m_showAllSessions ? QStringLiteral("%1 · %2").arg(locationName(root), when) : when;
        const ACPProvider provider = session.providerId.isEmpty()
            ? ACPProvider()
            : settings->providerById(session.providerId);
        entry.icon = settings->agentIcons()->iconFor(provider);
        QStringList tip = {fullTitle, QStringLiteral("Directory: %1").arg(root)};
        if (!provider.description.isEmpty()) {
            tip << QStringLiteral("Agent: %1").arg(provider.description);
        }
        tip << QStringLiteral("Last active: %1").arg(when)
            << QStringLiteral("Session: %1").arg(sessionId)
            << QStringLiteral("Double-click to resume it in a new chat; right-click to delete it.");
        entry.toolTip = tip.join(QLatin1Char('\n'));
        entries.append(entry);
    }

    if (m_showAllSessions) {
        m_sessionPanel->setResumable(QStringLiteral("Previous sessions in all directories"), QString(), entries);
    } else {
        m_sessionPanel->setResumable(QStringLiteral("Previous sessions in %1").arg(locationName(currentRoot)),
                                     currentRoot, entries);
    }
}

QString KateCodeView::getCurrentFilePath() const
{
    KTextEditor::View *view = editorView();
    if (view && view->document()) {
        return view->document()->url().toLocalFile();
    }
    return QString();
}

QString KateCodeView::getCurrentSelection() const
{
    KTextEditor::View *view = editorView();
    if (view) {
        return view->selectionText();
    }
    return QString();
}

QString KateCodeView::getProjectRoot() const
{
    QString projectRoot;

    // 1. First try Kate's project plugin (if available)
    QObject *projectPlugin = m_mainWindow->pluginView(QStringLiteral("kateprojectplugin"));
    if (projectPlugin) {
        QVariant baseDir = projectPlugin->property("projectBaseDir");
        if (baseDir.isValid() && !baseDir.toString().isEmpty()) {
            projectRoot = baseDir.toString();
            qDebug() << "[KateCode] Found project root from Kate project plugin:" << projectRoot;
            return projectRoot;
        }
    }

    // 2. Fall back to searching from active document
    QString filePath = getCurrentFilePath();
    if (filePath.isEmpty()) {
        return QDir::homePath();
    }

    QDir currentDir(QFileInfo(filePath).absolutePath());

    // 3. Walk up directory tree looking for project indicators
    while (currentDir.exists() && currentDir.path() != QDir::rootPath()) {
        QString dirPath = currentDir.absolutePath();

        // Check for version control systems
        if (currentDir.exists(QStringLiteral(".git")) ||
            currentDir.exists(QStringLiteral(".hg")) ||
            currentDir.exists(QStringLiteral(".svn")) ||
            currentDir.exists(QStringLiteral(".gitignore"))) {
            qDebug() << "[KateCode] Found project root via VCS marker:" << dirPath;
            return dirPath;
        }

        // Check for build system files
        if (currentDir.exists(QStringLiteral("CMakeLists.txt")) ||
            currentDir.exists(QStringLiteral("Makefile")) ||
            currentDir.exists(QStringLiteral("package.json")) ||
            currentDir.exists(QStringLiteral("Cargo.toml")) ||
            currentDir.exists(QStringLiteral("build.gradle")) ||
            currentDir.exists(QStringLiteral("pom.xml")) ||
            currentDir.exists(QStringLiteral("setup.py")) ||
            currentDir.exists(QStringLiteral("pyproject.toml")) ||
            currentDir.exists(QStringLiteral("go.mod"))) {
            qDebug() << "[KateCode] Found project root via build file:" << dirPath;
            return dirPath;
        }

        // Check for IDE project files
        if (currentDir.exists(QStringLiteral(".idea")) ||
            currentDir.exists(QStringLiteral(".vscode")) ||
            currentDir.exists(QStringLiteral(".project")) ||
            currentDir.exists(QStringLiteral(".kate-project"))) {
            qDebug() << "[KateCode] Found project root via IDE marker:" << dirPath;
            return dirPath;
        }

        if (!currentDir.cdUp()) {
            break;
        }
    }

    // Fallback to document directory
    qDebug() << "[KateCode] No project root found, using document directory";
    return QFileInfo(filePath).absolutePath();
}

QStringList KateCodeView::getProjectPluginFiles(const QString &root) const
{
    // Kate's project plugin already keeps the active project's file list
    // (built in the background, VCS-ignore aware). Use it when that project
    // contains root; otherwise return nothing and let ChatWidget scan.
    QObject *projectPlugin = m_mainWindow->pluginView(QStringLiteral("kateprojectplugin"));
    if (!projectPlugin || root.isEmpty()) {
        return QStringList();
    }

    const QString baseDir = projectPlugin->property("projectBaseDir").toString();
    if (baseDir.isEmpty()) {
        return QStringList();
    }
    const QString cleanRoot = QDir::cleanPath(root);
    const QString cleanBase = QDir::cleanPath(baseDir);
    if (cleanRoot != cleanBase && !cleanRoot.startsWith(cleanBase + QLatin1Char('/'))) {
        return QStringList();
    }

    const QStringList absoluteFiles = projectPlugin->property("projectFiles").toStringList();
    const QDir rootDir(cleanRoot);
    QStringList files;
    files.reserve(absoluteFiles.size());
    for (const QString &path : absoluteFiles) {
        const QString relative = rootDir.relativeFilePath(path);
        if (!relative.startsWith(QLatin1String("../")) && !QDir::isAbsolutePath(relative)) {
            files.append(relative);
        }
    }
    files.sort();

    qDebug() << "[KateCode] Using" << files.size() << "files from the Kate project plugin for" << cleanRoot;
    return files;
}

KTextEditor::Document *KateCodeView::findDocumentByPath(const QString &path) const
{
    if (path.isEmpty()) {
        return nullptr;
    }

    // Normalize the file path
    QString normalizedPath = QDir::cleanPath(path);
    QUrl fileUrl = QUrl::fromLocalFile(normalizedPath);

    // Get all documents from the application
    KTextEditor::Application *app = KTextEditor::Editor::instance()->application();
    if (!app) {
        qWarning() << "[KateCodeView] No KTextEditor application available";
        return nullptr;
    }

    const QList<KTextEditor::Document *> docs = app->documents();
    for (KTextEditor::Document *doc : docs) {
        if (doc->url() == fileUrl || doc->url().toLocalFile() == normalizedPath) {
            return doc;
        }
    }

    return nullptr;
}

void KateCodeView::addSelectionToContext()
{
    KTextEditor::View *view = editorView();
    if (!view || !view->document()) {
        qWarning() << "[KateCode] No active view or document";
        return;
    }

    QString selection = view->selectionText();
    if (selection.isEmpty()) {
        qWarning() << "[KateCode] No text selected";
        return;
    }

    QString filePath = view->document()->url().toLocalFile();
    if (filePath.isEmpty()) {
        qWarning() << "[KateCode] No file path for document";
        return;
    }

    // Get line numbers for the selection
    auto selectionRange = view->selectionRange();
    int startLine = selectionRange.start().line() + 1;  // Convert to 1-based
    int endLine = selectionRange.end().line() + 1;

    // Add to the target chat's context
    if (ChatWidget *chat = targetChat()) {
        chat->addContextChunk(filePath, startLine, endLine, selection);
        qDebug() << "[KateCode] Added selection to context:" << filePath
                 << "lines" << startLine << "-" << endLine;
    } else {
        qWarning() << "[KateCode] Chat widget not available";
    }
}

void KateCodeView::explainCode()
{
    sendQuickAction(i18n("Please explain what this code does, including its purpose, key logic, and any important details."));
}

void KateCodeView::findBugs()
{
    sendQuickAction(i18n("Please analyze this code for potential bugs, errors, or issues. Consider edge cases, error handling, and correctness."));
}

void KateCodeView::suggestImprovements()
{
    sendQuickAction(i18n("Please suggest improvements for this code. Consider readability, performance, maintainability, and best practices."));
}

void KateCodeView::addTests()
{
    sendQuickAction(i18n("Please generate comprehensive test cases for this code. Include unit tests covering normal cases, edge cases, and error conditions."));
}

void KateCodeView::sendQuickAction(const QString &prompt)
{
    KTextEditor::View *view = editorView();
    if (!view || !view->document()) {
        qWarning() << "[KateCode] No active view or document for quick action";
        return;
    }

    QString selection = view->selectionText();
    if (selection.isEmpty()) {
        qWarning() << "[KateCode] No text selected for quick action";
        return;
    }

    QString filePath = view->document()->url().toLocalFile();

    // Send prompt with selection through the target chat
    if (ChatWidget *chat = targetChat()) {
        chat->sendPromptWithSelection(prompt, filePath, selection);

        // Show the chat so the user sees the response
        activateChat(chat);

        qDebug() << "[KateCode] Sent quick action prompt with selection from:" << filePath;
    } else {
        qWarning() << "[KateCode] Chat widget not available for quick action";
    }
}

void KateCodeView::prepareForShutdown()
{
    for (ChatWidget *chat : liveChats()) {
        chat->prepareForShutdown();
    }
}

void KateCodeView::jumpToEdit(const QString &filePath, int startLine, int endLine)
{
    qDebug() << "[KateCodeView] jumpToEdit:" << filePath << "lines" << startLine << "-" << endLine;

    if (filePath.isEmpty()) {
        qWarning() << "[KateCodeView] Cannot jump to edit: empty file path";
        return;
    }

    // Open the file if not already open
    QUrl fileUrl = QUrl::fromLocalFile(filePath);
    m_mainWindow->openUrl(fileUrl);

    // Get the view for this document
    KTextEditor::View *view = m_mainWindow->activeView();
    if (!view) {
        qWarning() << "[KateCodeView] No active view after opening file";
        return;
    }

    // Verify we have the right document
    if (view->document()->url().toLocalFile() != filePath) {
        // Try to find the right view
        KTextEditor::Application *app = KTextEditor::Editor::instance()->application();
        const QList<KTextEditor::Document *> docs = app->documents();
        for (KTextEditor::Document *doc : docs) {
            if (doc->url().toLocalFile() == filePath) {
                view = m_mainWindow->activateView(doc);
                break;
            }
        }
    }

    if (!view) {
        qWarning() << "[KateCodeView] Could not get view for file:" << filePath;
        return;
    }

    // Calculate the selection range
    // startLine and endLine are 0-based
    int docLines = view->document()->lines();

    // Clamp to valid range
    int selStart = qBound(0, startLine, docLines - 1);
    int selEnd = qBound(selStart, endLine, docLines);

    // Create selection from start of startLine to start of endLine (or end of last line)
    KTextEditor::Cursor startCursor(selStart, 0);
    KTextEditor::Cursor endCursor;

    if (selEnd >= docLines) {
        // Select to end of document
        int lastLine = docLines - 1;
        endCursor = KTextEditor::Cursor(lastLine, view->document()->lineLength(lastLine));
    } else {
        // Select to start of endLine
        endCursor = KTextEditor::Cursor(selEnd, 0);
    }

    KTextEditor::Range range(startCursor, endCursor);

    // Set the selection and cursor position
    view->setSelection(range);
    view->setCursorPosition(startCursor);

    qDebug() << "[KateCodeView] Selected range:" << selStart + 1 << "-" << selEnd + 1;
}
