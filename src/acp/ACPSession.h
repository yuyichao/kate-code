#pragma once

#include "ACPModels.h"
#include "RateLimits.h"
#include <QDeadlineTimer>
#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

namespace KTextEditor {
class Document;
}

class ACPService;
class RemoteHelper;
class QTimer;

class EditTracker;
class TerminalManager;
class TranscriptWriter;

// Context-window usage and accumulated cost, from ACP usage_update.
struct SessionUsage {
    qint64 used = -1;     // tokens in the context window; -1 = not reported yet
    qint64 size = 0;      // context window size in tokens (0 = unknown)
    double cost = -1.0;   // accumulated cost; < 0 = not reported
    QString currency;
};

// A JSON-RPC error from the agent as text, with the agent's details if any.
QString describeACPError(const QJsonObject &error);

class ACPSession : public QObject
{
    Q_OBJECT

public:
    explicit ACPSession(QObject *parent = nullptr);
    ~ACPSession() override;

    void setExecutable(const QString &executable, const QStringList &args = QStringList());
    void setMcpConfigPath(const QString &path);
    // DBus object of this chat's editor endpoint, passed to the Kate MCP
    // server as KATECODE_DBUS_PATH (empty: the shared legacy object).
    void setEditorObjectPath(const QString &path) { m_editorObjectPath = path; }
    // Agents on remote hosts: run Kate Code's helper (kate-code-helper) on
    // the host too, installing it when missing (default on). Read at start().
    void setRemoteHelperEnabled(bool enabled) { m_remoteHelperEnabled = enabled; }
    // The helper of the running remote agent, if any (ready or not).
    RemoteHelper *remoteHelper() const { return m_remoteHelper; }
    void setSessionConfig(const QJsonObject &config);
    void start(const QString &workingDir, const QString &permissionMode = QStringLiteral("default"));
    void stop();

    // Session management - called after initializeComplete signal
    void createNewSession();
    void loadSession(const QString &sessionId);

    void sendMessage(const QString &content, const QString &filePath = QString(), const QString &selection = QString(), const QList<ContextChunk> &contextChunks = QList<ContextChunk>(), const QList<ImageAttachment> &images = QList<ImageAttachment>());
    void sendPermissionResponse(int requestId, const QJsonObject &outcome);
    void setMode(const QString &modeId);

    // Session config options advertised by the agent (model, effort, ...).
    // configOptionsChanged() fires whenever the agent reports a new list.
    QJsonArray configOptions() const { return m_availableConfigOptions; }
    // Interactive change of one config option. Rapid changes to the same
    // option are coalesced; a rejected change re-emits the confirmed list.
    void setConfigOption(const QString &configId, const QJsonValue &value);

    // Title of the session: the agent's (session_info_update), or the one
    // set with setTitle(). titleChanged() fires when it changes.
    QString title() const { return m_title; }
    // Rename the session. Agents advertising sessionCapabilities.title store
    // it (session/set_title); for others it is kept by this client only.
    void setTitle(const QString &title);
    bool supportsSetTitle() const { return m_supportsSetTitle; }

    // Latest usage reported by the agent for this session.
    SessionUsage usage() const { return m_usage; }

    bool isConnected() const { return m_status == ConnectionStatus::Connected; }
    bool isPromptRunning() const { return m_promptRequestId >= 0; }
    ConnectionStatus status() const { return m_status; }
    QString sessionId() const { return m_sessionId; }
    // The directory the session was started in (empty before first start).
    QString workingDir() const { return m_workingDir; }

    // Passthrough to the TranscriptWriter — empty if no session has started.
    QString transcriptFilePath() const;

    // Agent prompt capabilities, parsed from the initialize response.
    bool supportsImage() const { return m_supportsImage; }
    bool supportsEmbeddedContext() const { return m_supportsEmbeddedContext; }
    bool supportsPromptQueueing() const { return m_supportsPromptQueueing; }
    // session/load is replaying the session's history (session/update
    // notifications before its response): shown in the chat, without the
    // side effects of a live turn (diff highlights, remote host holds,
    // document checks).
    bool isReplaying() const { return m_sessionLoadRequestId >= 0; }
    // Claude Code's work outside a prompt (claude-agent-acp, which marks
    // itself with agentCapabilities._meta.claudeCode; Kate Code advertises
    // its display terminals, clientCapabilities._meta.terminal_output):
    // - commands running in the background: a Bash call that completed with
    //   a background task id and its display terminal still open, until the
    //   agent reports the terminal's exit (also after the turn);
    // - a cycle Claude Code runs on its own (e.g. when a background task
    //   ended): its output arrives outside a prompt and is shown as an
    //   assistant message until the cycle's result (the usage_update with
    //   its cost), a prompt or cancelPrompt().
    int backgroundCommandCount() const { return m_backgroundTerminals.size(); }
    bool isWorkingOnItsOwn() const { return m_autonomousTurn; }
    // History replayed by session/load: the user message being collected
    // (shown when it ends), and helpers ending the replayed messages.
    QStringList m_replayUserChunks; // content blocks of the user message
    int m_replayCounter = 0;
    void flushReplayedUserMessage();
    // A user message content block as shown: other clients' and Kate Code's
    // context blocks left out (empty if nothing is left).
    static QString shownUserText(QString text);
    void finishReplayedMessage();
    void startReplayedAssistantMessage();
    // agentCapabilities.loadSession: session/load is available.
    bool supportsLoadSession() const { return m_supportsLoadSession; }
    // A slash command ("/name" or "/name args"): sent alone, exactly as
    // typed, since agents only recognise a command that is the whole prompt.
    // Once the agent has listed its commands (available_commands_update),
    // only those count, so a message starting with a path such as "/tmp" is
    // not taken for one; before that, any "/name" first word does.
    bool isSlashCommand(const QString &text) const;

    void cancelPrompt();

    // Ask the live agent for a session summary without touching the chat UI or
    // the transcript. Any running prompt is cancelled first (the session is
    // about to end). The outcome arrives via summaryResult().
    void requestSummary(const QString &prompt);
    void cancelSummary();
    bool isSummaryRunning() const { return m_summaryRequestId >= 0 || m_summaryAfterPromptId >= 0; }

    QJsonArray availableModes() const { return m_availableModes; }
    QString currentMode() const { return m_currentMode; }
    QList<SlashCommand> availableCommands() const { return m_availableCommands; }

    // Set terminal size based on view width (columns calculated from pixel width)
    void setTerminalSize(int columns, int rows = 40);

    // Document provider for Kate integration (reads/writes use Kate documents when open)
    using DocumentProvider = std::function<KTextEditor::Document*(const QString &path)>;
    void setDocumentProvider(DocumentProvider provider);

    // Edit tracker for tracking file modifications
    EditTracker *editTracker() const { return m_editTracker; }

Q_SIGNALS:
    void statusChanged(ConnectionStatus status);
    void messageAdded(const Message &message);
    void messageUpdated(const QString &messageId, const QString &content);
    void messageFinished(const QString &messageId);
    void toolCallAdded(const QString &messageId, const ToolCall &toolCall);
    void toolCallUpdated(const QString &messageId, const QString &toolCallId, const QString &status, const QString &result, const QString &filePath = QString(), const QString &toolName = QString());
    void todosUpdated(const QList<TodoItem> &todos);
    void permissionRequested(const PermissionRequest &request);
    void modesAvailable(const QJsonArray &modes);
    void configOptionsChanged(const QJsonArray &configOptions);
    void usageChanged(const SessionUsage &usage);
    // The plan's usage limits (Claude Code: usage_update's
    // _meta["_claude/rateLimit"], sent when they change).
    void rateLimitsChanged(const RateLimits &limits);
    void titleChanged(const QString &title);
    void modeChanged(const QString &modeId);
    void commandsAvailable(const QList<SlashCommand> &commands);
    void errorOccurred(const QString &message);
    // The agent of a remote folder was not found on its host (exit 127):
    // before errorOccurred() and the status change.
    void remoteAgentMissing();
    // Emitted when the agent reports a systemError threadStatus in session_info_update.
    void sessionError(const QString &message);
    void promptCancelled();

    // Debug: raw JSON-RPC payloads (direction is ">>" for sent, "<<" for received)
    void jsonPayload(const QString &direction, const QString &json);

    // Emitted after initialize completes, before session creation
    void initializeComplete();

    // Emitted if session/load fails (caller should fall back to createNewSession)
    void sessionLoadFailed(const QString &error);

    // Outcome of requestSummary(); error is empty on success.
    void summaryResult(const QString &summary, const QString &error);

    // Terminal signals for UI updates
    void terminalOutputUpdated(const QString &terminalId, const QString &output, bool finished);
    void toolCallTerminalIdSet(const QString &messageId, const QString &toolCallId, const QString &terminalId);

    // A remote agent's tool (not one of Kate's) may change files of its host
    // (below hostDir), which Kate sees only later (EditorSession::holdRemoteFiles()).
    void remoteHostActivity(const QString &hostDir);

    // backgroundCommandCount() or isWorkingOnItsOwn() changed.
    void backgroundActivityChanged();
    // A tool call's command runs in the background (running), or ended
    // there.
    void toolCallBackgroundChanged(const QString &messageId, const QString &toolCallId, bool running);

    // Emitted when a built-in Edit/Write tool writes a file, with old and new content for diff display
    void fsEditApplied(const QString &filePath, const QString &oldText, const QString &newText);

private Q_SLOTS:
    void onConnected();
    void onDisconnected(int exitCode);
    void onNotification(const QString &method, const QJsonObject &params, int requestId);
    void onResponse(int id, const QJsonObject &result, const QJsonObject &error);
    void onError(const QString &message);

private:
    // A follow-up prompt queued while another session/prompt is in flight.
    struct QueuedPrompt {
        QString content;
        QString filePath;
        QString selection;
        QList<ContextChunk> contextChunks;
        QList<ImageAttachment> images;
        // A slash command (decided when it was sent): sent alone.
        bool command = false;
    };

    // Send the assistant placeholder and session/prompt request.
    void dispatchPrompt(const QueuedPrompt &prompt);
    // The content blocks of a prompt. Takes the pending editor reminder
    // (unless the prompt is a slash command).
    QJsonArray buildPromptBlocks(const QueuedPrompt &prompt);
    // Send a prompt while a turn runs, to an agent that takes them
    // (promptQueueing): Claude Code folds it into the running turn at its next
    // step, or runs it next.
    void sendPromptMidTurn(const QueuedPrompt &prompt);
    // Finish the streaming assistant message and start a new one (output
    // after a message sent mid-turn shows below it); the id is a new msg_N
    // unless given.
    void startAssistantMessage(const QString &id = QString());

    // Send the silent summary session/prompt now (requestSummary defers this
    // while a cancelled prompt's response is outstanding).
    void sendSummaryPrompt(const QString &prompt);
    // Clear per-session protocol state; shared by stop() and unexpected
    // disconnects so a reconnect always starts clean.
    void resetSessionState();
    void handleInitializeResponse(int id, const QJsonObject &result);
    void handleSessionNewResponse(int id, const QJsonObject &result);
    void handleSessionLoadResponse(int id, const QJsonObject &result, const QJsonObject &error);
    void handleSessionConfigResponse(const QJsonObject &result, const QJsonObject &error);
    // Handler for interactive (dropdown) mode-change requests.
    void handleInteractiveModeResponse(const QJsonObject &result, const QJsonObject &error);
    // Handler for interactive set_config_option requests other than mode.
    void handleInteractiveConfigResponse(int id, const QJsonObject &result, const QJsonObject &error);
    void sendConfigOption(const QString &configId, const QJsonValue &value);
    void handleSessionUpdate(const QJsonObject &params);
    void handlePermissionRequest(const QJsonObject &params, int requestId);
    QJsonArray buildMcpServers() const;
    void parseSessionSetupResult(const QJsonObject &result);
    void updateSessionConfigOptions(const QJsonArray &configOptions, bool emitChanges);
    void beginSessionConfiguration(bool loadedSession);
    void sendNextSessionConfigOption();
    void completeSessionSetup(bool loadedSession);

    // A path from the agent (a tool call title): absolute paths of a remote
    // agent's host as local ones, relative ones below the working directory.
    QString agentPath(const QString &path) const;
    // An agent on a remote host changes files there itself; Kate does not
    // notice (no file notifications through the mount). After its tool
    // calls and turns, open documents of that host are compared with their
    // files: unmodified ones are reloaded, modified ones flagged as changed
    // on disk.
    void scheduleRemoteDocumentCheck();
    void checkRemoteDocuments();
    // With the helper (kate-code-helper on the host): the host's own
    // digests, and change notifications for the open documents of the host.
    // A change is looked at RemoteHelperSettleMs after its notification and
    // confirmed by a second digest (Kate's own saves notify too), then a
    // modified document is flagged as changed on disk, and an unmodified one
    // reloaded once the mount shows the host's contents (helperVerifyMount()).
    // sshfs shows a host change only about 1 s after the next open of the
    // file (measured; a stat does not do it, and until then it answers from
    // its cache for up to 20 s): changed files are opened at once
    // (kickMount()).
    void startRemoteHelper();
    void stopRemoteHelper();
    QStringList openRemoteDocumentPaths() const;
    void syncRemoteHelperWatches();
    void helperCheckDocuments(const QStringList &paths, bool confirmed);
    void kickMount(const QStringList &paths);
    // Reads the files of m_helperVerifying through the mount (in a thread):
    // reloads those that show the host's digest, flags those that do not by
    // their deadline, polls again for the others.
    void helperVerifyMount();

    // Terminal request handlers
    void handleTerminalCreate(const QJsonObject &params, int requestId);
    void handleTerminalOutput(const QJsonObject &params, int requestId);
    void handleTerminalWaitForExit(const QJsonObject &params, int requestId);
    void handleTerminalKill(const QJsonObject &params, int requestId);
    void handleTerminalRelease(const QJsonObject &params, int requestId);

    // Filesystem request handlers
    void handleFsReadTextFile(const QJsonObject &params, int requestId);
    void handleFsWriteTextFile(const QJsonObject &params, int requestId);

    ACPService *m_service;
    TerminalManager *m_terminalManager;
    TranscriptWriter *m_transcript;
    ConnectionStatus m_status;
    QString m_sessionId;
    QString m_workingDir;
    QString m_currentMode;
    QJsonArray m_availableModes;
    QList<SlashCommand> m_availableCommands;
    QString m_currentMessageId;
    QString m_currentMessageContent;  // Accumulated content for transcript
    QDateTime m_currentMessageTimestamp;
    QHash<QString, QJsonObject> m_toolCallInputs;  // Track tool inputs by toolCallId

    // Request tracking
    int m_initializeRequestId;
    int m_sessionNewRequestId;
    int m_sessionLoadRequestId;
    int m_sessionConfigRequestId;
    int m_promptRequestId;
    QString m_sessionLoadId;

    // Follow-up prompts queued while a session/prompt round-trip is in flight.
    QList<QueuedPrompt> m_promptQueue;

    // In-flight silent summary prompt (requestSummary): while m_summaryRequestId
    // is set, agent_message_chunk text accumulates here instead of the chat.
    // When a prompt had to be cancelled first, the summary prompt is parked in
    // m_pendingSummaryPrompt until the cancelled request's response arrives
    // (m_summaryAfterPromptId) so stale chunks cannot leak into the summary.
    int m_summaryRequestId = -1;
    int m_summaryAfterPromptId = -1;
    QString m_pendingSummaryPrompt;
    QString m_summaryCollected;

    // Interactive mode-change state (separate from the startup config flow).
    // m_interactiveModeRequestId: id of the in-flight session/set_config_option or
    //   session/set_mode request triggered by the user's dropdown selection (-1 = none).
    // m_pendingModeValue: the value sent in that request.
    // m_queuedModeValue:  the latest value requested while a request is in flight;
    //   sent as a follow-up once the current response arrives.
    int m_interactiveModeRequestId = -1;
    QString m_pendingModeValue;
    QString m_queuedModeValue;

    // Interactive changes of other config options: in-flight request per
    // option id, and the latest value requested while one is in flight.
    QHash<int, QString> m_configRequestOption;
    QHash<QString, QJsonValue> m_queuedConfigValues;

    SessionUsage m_usage;

    // Message counter
    int m_messageCounter;

    // Kate document provider
    DocumentProvider m_documentProvider;

    // Edit tracker
    EditTracker *m_editTracker;

    // Current tool call ID for edit tracking
    QString m_currentToolCallId;

    // MCP config path for loading external MCP servers
    QString m_mcpConfigPath;
    QString m_editorObjectPath;

    // Per-provider ACP session configuration applied after session/new or load.
    QJsonObject m_sessionConfig;
    QJsonArray m_availableConfigOptions;
    QStringList m_pendingSessionConfigKeys;
    QString m_currentSessionConfigKey;
    bool m_configuringLoadedSession = false;

    // Agent prompt capabilities from the initialize response (default false).
    bool m_supportsImage = false;
    bool m_supportsEmbeddedContext = false;
    bool m_supportsPromptQueueing = false;
    bool m_supportsSetTitle = false;
    bool m_supportsMcpHttp = false;
    bool m_supportsLoadSession = false;
    // Tool calls of a remote agent that change nothing on its host (Kate's
    // own tools, reads, searches).
    QSet<QString> m_harmlessToolCalls;
    // The editor <system-reminder> goes with the first ordinary prompt (not
    // with a slash command, which must be sent alone).
    bool m_editorReminderPending = false;
    // session/prompt requests sent while m_promptRequestId's turn runs. The
    // turn ends (and the next queued prompt goes out) once all have answered.
    QList<int> m_midTurnPromptIds;
    // Work outside a prompt (see backgroundCommandCount()).
    bool m_claudeCodeAgent = false;
    // The message of each tool call: its updates can come after its turn.
    QHash<QString, QString> m_toolCallMessages;
    // Display terminals (_meta.terminal_info) not exited yet; tool calls
    // whose result named a background task; the commands running in the
    // background.
    QSet<QString> m_displayTerminals;
    QSet<QString> m_backgroundTaskToolCalls;
    QSet<QString> m_backgroundTerminals;
    // Output of each display terminal so far (appended chunks).
    QHash<QString, QString> m_displayTerminalOutput;
    bool m_autonomousTurn = false;
    int m_autonomousCounter = 0;
    // A cancelled prompt whose response has not come: its trailing output
    // is not a cycle of its own.
    int m_cancelledPromptId = -1;
    void startAutonomousTurn();
    void finishAutonomousTurn();
    // A message typed in another client of the session (Claude Code's Remote
    // Control: claude.ai, the mobile app), sent as user_message_chunk outside
    // a replay: shown at once, recorded in the transcript once the agent
    // sends something else (m_remoteUserMessage); what follows answers it.
    Message m_remoteUserMessage;
    int m_remoteCounter = 0;
    void handleRemoteUserChunk(const QJsonObject &content);
    void recordRemoteUserMessage();
    void clearBackgroundWork();
    // _meta.terminal_* of a tool call update (claude-agent-acp's display
    // terminals). Returns the command output it carries, if any.
    QString handleDisplayTerminal(const QString &messageId, const QString &toolCallId, const QJsonObject &update);
    // A terminal of the tool call's content that the agent only displays
    // (not one Kate Code runs for it, TerminalManager).
    bool isAgentDisplayTerminal(const QString &terminalId, const QString &toolCallId, const QJsonObject &update) const;
    QString m_title;
    // In-flight session/set_title requests.
    QSet<int> m_setTitleRequestIds;

    QTimer *m_remoteCheckTimer = nullptr;
    QTimer *m_remoteLateCheckTimer = nullptr;
    bool m_remoteCheckRunning = false;
    bool m_remoteCheckPending = false;

    bool m_remoteHelperEnabled = true;
    RemoteHelper *m_remoteHelper = nullptr;
    QTimer *m_helperSettleTimer = nullptr; // changed() -> check
    QTimer *m_helperWatchTimer = nullptr; // documents opened/closed -> watch set
    QSet<QString> m_helperChangedPaths;
    struct HelperVerify {
        QByteArray digest; // the host's
        QDeadlineTimer deadline;
    };
    QHash<QString, HelperVerify> m_helperVerifying; // unmodified documents to reload when the mount shows the digest
    bool m_helperVerifyRunning = false;
    QTimer *m_helperVerifyTimer = nullptr;
};
