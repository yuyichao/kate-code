#pragma once

#include "../acp/ACPModels.h"
#include <QWebEngineView>
#include <QStringList>

class WebBridge;

class ChatWebView : public QWebEngineView
{
    Q_OBJECT

public:
    explicit ChatWebView(QWidget *parent = nullptr);
    ~ChatWebView() override;

    // Where a navigation of the chat's page goes: the chat page itself
    // (and anchors in it) stays; links the user follows to web or mail
    // addresses open in the desktop's browser or mail program (never in
    // the chat: the page would be replaced); anything else is refused.
    enum class LinkAction {
        Stay,
        OpenExternally,
        Refuse,
    };
    static LinkAction linkAction(const QUrl &url, bool userClicked);

    void addMessage(const Message &message);
    void updateMessage(const QString &messageId, const QString &content);
    void finishMessage(const QString &messageId);
    void addToolCall(const QString &messageId, const ToolCall &toolCall);
    void updateToolCall(const QString &messageId, const QString &toolCallId, const QString &status, const QString &result, const QString &filePath = QString(), const QString &toolName = QString());
    void setToolCallDiff(const QString &messageId, const QString &toolCallId, const QString &filePath, const QString &oldText, const QString &newText);
    void setToolCallImages(const QString &messageId, const QString &toolCallId, const QList<ImageAttachment> &images);
    void showPermissionRequest(const PermissionRequest &request);
    void updateTodos(const QList<TodoItem> &todos);
    void clearMessages();

    // Terminal support
    void updateTerminalOutput(const QString &terminalId, const QString &output, bool finished);
    void setToolCallTerminalId(const QString &messageId, const QString &toolCallId, const QString &terminalId);
    // The tool call's command runs in the background, or ended there.
    void setToolCallBackground(const QString &messageId, const QString &toolCallId, bool running);
    // A loaded session's history arrives: kept, and drawn once it has
    // (newest messages first).
    void setReplaying(bool replaying);

    // User question support (MCP AskUserQuestion tool)
    void showUserQuestion(const QString &requestId, const QString &questionsJson);
    void removeUserQuestion(const QString &requestId);

    // Edit tracking support
    void addTrackedEdit(const TrackedEdit &edit);
    void clearEditSummary();

    // Diff color scheme support
    void updateDiffColors(const QString &removeBackground, const QString &addBackground);

    // Show a distinct error banner in the chat (ACP/session errors).
    void showError(const QString &text);

    // Connection failure notice with a Reconnect button (reconnectRequested)
    // and, with installLabel, a button installing the agent on the remote
    // host (agentInstallRequested).
    void showReconnectPrompt(const QString &text, const QString &installLabel = QString());
    // Disable the Reconnect buttons shown so far (a new attempt has started).
    void disableReconnectPrompts();

Q_SIGNALS:
    void reconnectRequested();
    void agentInstallRequested();
    void permissionResponseReady(int requestId, const QString &optionId);
    void jumpToEditRequested(const QString &filePath, int startLine, int endLine);
    void webViewReady();
    void userQuestionAnswered(const QString &requestId, const QJsonObject &answers);

private Q_SLOTS:
    void onLoadFinished(bool ok);

private:
    void runJavaScript(const QString &script);
    QString escapeJsString(const QString &str);
    void injectColorScheme();
    void setupBridge();

    bool m_isLoaded;
    // Events can arrive while a session is starting and the WebEngine page is
    // still loading.  Keep their JavaScript calls in order and replay them
    // once the page is ready instead of silently dropping agent output.
    QStringList m_pendingScripts;
    WebBridge *m_bridge;
};

// Bridge class for JavaScript to call C++
class WebBridge : public QObject
{
    Q_OBJECT

public:
    explicit WebBridge(QObject *parent = nullptr) : QObject(parent) {}

public Q_SLOTS:
    Q_INVOKABLE void respondToPermission(int requestId, const QString &optionId);
    Q_INVOKABLE void logFromJS(const QString &message);
    Q_INVOKABLE void jumpToEdit(const QString &filePath, int startLine, int endLine);
    Q_INVOKABLE void submitQuestionAnswers(const QString &requestId, const QString &answersJson);
    Q_INVOKABLE void requestReconnect();
    Q_INVOKABLE void requestAgentInstall();

Q_SIGNALS:
    void permissionResponse(int requestId, const QString &optionId);
    void jumpToEditRequested(const QString &filePath, int startLine, int endLine);
    void questionAnswersSubmitted(const QString &requestId, const QString &answersJson);
    void reconnectRequested();
    void agentInstallRequested();
};
