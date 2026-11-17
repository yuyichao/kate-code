#pragma once

#include <KTextEditor/Document>
#include <KTextEditor/MainWindow>
#include <KXMLGUIClient>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>

class KateCodePlugin;
class ChatWidget;
class DiffHighlightManager;
class SessionPanel;
class SessionStore;
class SummaryStore;
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
    // Resume a saved session of the project in a new chat tab (or switch to
    // the chat that has it open).
    void resumeSavedSession(const QString &sessionId, const QString &projectRoot);
    // Delete a saved session (after confirmation): its transcript, summary
    // and stored entry, those of the sessions it was restored from, and
    // optionally the agent's own copy.
    void deleteSavedSession(const QString &sessionId, const QString &projectRoot,
                            const QString &transcriptPath);
    // The session browser with summaries, for the current project.
    void browseSessions();
    // Open a session started elsewhere (listed by the provider's agent) in a
    // new chat: always session/load (Kate Code has no transcript of it).
    void resumeElsewhereSession(const QString &sessionId, const QString &projectRoot, const QString &providerId);

public:
    // Switch to the chat (other than exclude) that has the given agent
    // session open; false if this window has none.
    bool activateChatWithSession(const QString &sessionId, ChatWidget *exclude);

private Q_SLOTS:
    void addSelectionToContext();

    // Quick Actions
    void explainCode();
    void findBugs();
    void suggestImprovements();
    void addTests();

    // Edit navigation
    void jumpToEdit(const QString &filePath, int startLine, int endLine);

    // The session list follows the current directory; also reloads the
    // saved sessions (other Kate instances may have changed them).
    void scheduleSessionPanelRefresh();

private:
    void sendQuickAction(const QString &prompt);
    // Refresh the session list when the project plugin's current project
    // changes (e.g. a directory is opened).
    void watchProjectPlugin(QObject *pluginView);
    void createToolView();
    // startSession: connect right away (a new session); false when the
    // caller resumes one instead.
    ChatWidget *createChat(bool startSession = true);
    // The chat that quick actions and "add to context" go to: the active chat
    // tab, else the last used chat, else a new one.
    ChatWidget *targetChat();
    void activateChat(ChatWidget *chat);
    KTextEditor::View *editorView() const;
    void refreshSessionPanel();
    void refreshResumableSessions();
    // The "started elsewhere" section: asks the agents (session/list) when
    // it is expanded and the location changed, or when forced (refresh
    // button); otherwise filters the last answers again (sessions resumed
    // since then drop out).
    void refreshElsewhere(bool force);
    // Starts the next queued listing (a remote folder: one at a time).
    void startNextElsewhereLister(int generation);
    void showElsewhere();
    bool isSessionOpen(const QString &sessionId) const;
    QList<ChatWidget *> liveChats() const;

    KateCodePlugin *m_plugin;
    KTextEditor::MainWindow *m_mainWindow;
    QWidget *m_toolView;
    SessionPanel *m_sessionPanel = nullptr;
    QList<QPointer<ChatWidget>> m_chats;
    QPointer<ChatWidget> m_lastActiveChat;
    QPointer<KTextEditor::View> m_lastEditorView;
    bool m_panelRefreshPending = false;
    SessionStore *m_sessionStore;
    SummaryStore *m_summaryStore;
    bool m_showAllSessions = false; // resumable list: all directories
    // Start of each transcript, by path, valid for its mtime.
    struct TranscriptInfo {
        QDateTime modified;
        QString projectRoot;
        bool hasUserMessage = false;
        QString firstPrompt;
    };
    QHash<QString, TranscriptInfo> m_transcriptInfo;
    // Sessions started elsewhere, as the agents listed them for
    // m_elsewhereKey (a directory, or "*" for all).
    struct ElsewhereSession {
        QString sessionId;
        QString cwd;
        QString title;
        QDateTime updatedAt;
        QString providerId;
    };
    bool m_elsewhereExpanded = false;
    QString m_elsewhereKey;
    int m_elsewhereGeneration = 0;
    int m_elsewherePending = 0;
    QList<ElsewhereSession> m_elsewhereSessions;
    QStringList m_elsewhereErrors;
    struct ElsewhereLister {
        QString providerId;
        QString agentName;
        QString executable;
        QStringList args;
    };
    QList<ElsewhereLister> m_elsewhereQueue;
    QString m_elsewhereWorkingDir;
    QString m_elsewhereFilter;
    DiffHighlightManager *m_diffHighlightManager;
};
