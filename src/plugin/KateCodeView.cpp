#include "KateCodeView.h"
#include "KateCodePlugin.h"
#include "../acp/ACPModels.h"
#include "../config/SettingsStore.h"
#include "../mcp/EditorDBusService.h"
#include "../ui/ChatWidget.h"
#include "../ui/SessionPanel.h"
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
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QVBoxLayout>
#include <functional>

KateCodeView::KateCodeView(KateCodePlugin *plugin, KTextEditor::MainWindow *mainWindow)
    : QObject(mainWindow)
    , KXMLGUIClient()
    , m_plugin(plugin)
    , m_mainWindow(mainWindow)
    , m_toolView(nullptr)
    , m_diffHighlightManager(nullptr)
{
    createToolView();

    // Remember the last editor view: when a chat tab is active,
    // activeView() no longer names the file the user is working on.
    m_lastEditorView = m_mainWindow->activeView();
    connect(m_mainWindow, &KTextEditor::MainWindow::viewChanged, this, [this](KTextEditor::View *view) {
        if (view) {
            m_lastEditorView = view;
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

    m_sessionPanel = new SessionPanel(m_toolView);
    layout->addWidget(m_sessionPanel);
    connect(m_sessionPanel, &SessionPanel::newSessionRequested, this, &KateCodeView::newSession);
    connect(m_sessionPanel, &SessionPanel::entryActivated, this, [this](int index) {
        const QList<ChatWidget *> chats = liveChats();
        if (index >= 0 && index < chats.size()) {
            activateChat(chats.at(index));
        }
    });
}

ChatWidget *KateCodeView::createChat()
{
    auto *chat = new ChatWidget();

    // Kate owns the chat once it is added, so it can outlive this view (for
    // example when the plugin is unloaded); every callback checks for that.
    QPointer<KateCodeView> self(this);
    QPointer<ChatWidget> chatGuard(chat);
    KateCodePlugin *plugin = m_plugin;

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

    // Process-wide single-agent gate: one connected chat per Kate process,
    // because the editor DBus service (MCP tools) is shared and not yet
    // routed per session. The plugin outlives every chat.
    chat->setAgentSlotHooks(
        [plugin, chat]() { return plugin->acquireAgentSlot(chat); },
        [plugin, chat]() { plugin->releaseAgentSlot(chat); },
        [plugin, chat]() { return plugin->agentSlotAvailableFor(chat); });
    connect(m_plugin, &KateCodePlugin::agentActivityChanged, chat, &ChatWidget::refreshAgentAvailability);

    connect(chat, &ChatWidget::jumpToEditRequested, this, &KateCodeView::jumpToEdit);

    // Shared DBus service -> chat. Each chat ignores these unless it is the
    // connected one.
    connect(m_plugin->dbusService(), &EditorDBusService::editApplied, chat, &ChatWidget::onEditApplied);
    connect(m_plugin->dbusService(), &EditorDBusService::questionRequested, chat, &ChatWidget::showUserQuestion);
    connect(m_plugin->dbusService(), &EditorDBusService::questionCancelled, chat, &ChatWidget::removeUserQuestion);
    connect(chat, &ChatWidget::userQuestionAnswered,
            m_plugin->dbusService(), &EditorDBusService::provideQuestionResponse);

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
    return chat;
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

static QString formatTokens(qint64 tokens)
{
    if (tokens >= 1000000) {
        return QStringLiteral("%1M").arg(tokens / 1000000.0, 0, 'f', 1);
    }
    if (tokens >= 1000) {
        return QStringLiteral("%1k").arg(qRound(tokens / 1000.0));
    }
    return QString::number(tokens);
}

void KateCodeView::refreshSessionPanel()
{
    if (!m_sessionPanel) {
        return;
    }
    QList<SessionPanel::Entry> entries;
    for (ChatWidget *chat : liveChats()) {
        SessionPanel::Entry entry;
        entry.title = chat->sessionTitle();
        switch (chat->connectionStatus()) {
        case ConnectionStatus::Connected:
            entry.status = QStringLiteral("Connected");
            entry.iconName = QStringLiteral("network-connect");
            break;
        case ConnectionStatus::Connecting:
            entry.status = QStringLiteral("Connecting");
            entry.iconName = QStringLiteral("network-connecting");
            break;
        case ConnectionStatus::Error:
            entry.status = QStringLiteral("Error");
            entry.iconName = QStringLiteral("dialog-error");
            break;
        case ConnectionStatus::Disconnected:
            entry.status = QStringLiteral("Not connected");
            entry.iconName = QStringLiteral("network-disconnect");
            break;
        }

        const SessionUsage usage = chat->usage();
        QStringList detail;
        if (usage.used >= 0) {
            if (usage.size > 0) {
                detail << QStringLiteral("Context %1 / %2 (%3%)")
                              .arg(formatTokens(usage.used), formatTokens(usage.size))
                              .arg(qRound(100.0 * usage.used / usage.size));
            } else {
                detail << QStringLiteral("Context %1").arg(formatTokens(usage.used));
            }
        }
        if (usage.cost >= 0) {
            detail << (usage.currency == QStringLiteral("USD")
                           ? QStringLiteral("$%1").arg(usage.cost, 0, 'f', 2)
                           : QStringLiteral("%1 %2").arg(usage.cost, 0, 'f', 2).arg(usage.currency));
        }
        entry.detail = detail.join(QStringLiteral(" · "));
        entry.active = (chat == m_lastActiveChat);
        entries.append(entry);
    }
    m_sessionPanel->setEntries(entries);
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
