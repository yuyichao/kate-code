#pragma once

#include <KTextEditor/Document>
#include <KTextEditor/MainWindow>
#include <KXMLGUIClient>
#include <QList>
#include <QObject>
#include <QPointer>

class KateCodePlugin;
class ChatWidget;
class DiffHighlightManager;
class SessionPanel;
class QWidget;

namespace KTextEditor {
class View;
}

/**
 * Per-window plugin view. Chats are editor-area widgets (one tab each, added
 * with MainWindow::addWidget, so they can be moved between splits and several
 * can be open); the "Kate Code" side panel lists them with their status and
 * usage.
 */
class KateCodeView : public QObject, public KXMLGUIClient
{
    Q_OBJECT

public:
    explicit KateCodeView(KateCodePlugin *plugin, KTextEditor::MainWindow *mainWindow);
    ~KateCodeView() override;

    // Kate context getters. They refer to the last active editor view, since
    // the active tab is often a chat.
    QString getCurrentFilePath() const;
    QString getCurrentSelection() const;
    QString getProjectRoot() const;
    QStringList getProjectPluginFiles(const QString &root) const;
    KTextEditor::Document *findDocumentByPath(const QString &path) const;

    // Shutdown hook for summary generation
    void prepareForShutdown();

public Q_SLOTS:
    // Open a new chat tab and switch to it.
    void newSession();

private Q_SLOTS:
    void addSelectionToContext();

    // Quick Actions
    void explainCode();
    void findBugs();
    void suggestImprovements();
    void addTests();

    // Edit navigation
    void jumpToEdit(const QString &filePath, int startLine, int endLine);

private:
    void sendQuickAction(const QString &prompt);
    void createToolView();
    ChatWidget *createChat();
    // The chat that quick actions and "add to context" go to: the active chat
    // tab, else the last used chat, else a new one.
    ChatWidget *targetChat();
    void activateChat(ChatWidget *chat);
    KTextEditor::View *editorView() const;
    void scheduleSessionPanelRefresh();
    void refreshSessionPanel();
    QList<ChatWidget *> liveChats() const;

    KateCodePlugin *m_plugin;
    KTextEditor::MainWindow *m_mainWindow;
    QWidget *m_toolView;
    SessionPanel *m_sessionPanel = nullptr;
    QList<QPointer<ChatWidget>> m_chats;
    QPointer<ChatWidget> m_lastActiveChat;
    QPointer<KTextEditor::View> m_lastEditorView;
    bool m_panelRefreshPending = false;
    DiffHighlightManager *m_diffHighlightManager;
};
