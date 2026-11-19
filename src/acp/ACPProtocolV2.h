/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringDecoder>

#include <memory>

// ACP v2 (draft) for a chat's agent: offered in initialize when
// KATECODE_ACP_PROTOCOL=2 (no setting yet), used when the agent answers with
// protocol version 2 (claude-agent-acp 0.106: its v2 core). ACPSession is
// written against ACP v1's messages; this class translates between the two at
// its boundary, so the rest of Kate Code sees v1 shapes:
//
// - initialize: both versions' fields go out (a v1 agent reads its own and
//   answers version 1: nothing is translated then); a v2 answer is turned
//   into v1's agentCapabilities.
// - session/load is session/resume with replayFrom start; config options are
//   keyed by configId (v1: id) and set with a value type; MCP servers name
//   their type; a plan is plan_update; commands carry a type.
// - A prompt's response only acknowledges it (messageId); the turn ends with
//   a state_update idle with a stopReason. The response is held back and
//   given then ({stopReason}, or an error), so ACPSession's turn handling
//   (mid-turn prompts, cancelling, summaries) is the same. The agent echoes a
//   prompt as user_message (before or after the response): dropped, as v1
//   never echoes Kate Code's own prompts; other live user messages (Remote
//   Control) go on as user_message_chunk.
// - $/cancel_request of a prompt (already answered in v2) becomes
//   session/cancel.
// - Tool calls are upserts (tool_call_update only): the first one of an id is
//   v1's tool_call. Statuses cancelled → failed, paused → in_progress. Diffs
//   (changes + a git patch) become v1's per-file oldText/newText. Agent-owned
//   terminals (terminal_update with base64 output snapshots,
//   terminal_output_chunk, exitStatus) become Zed's display-terminal _meta
//   of the tool call that shows them (terminal_info, terminal_output,
//   terminal_exit), as Kate Code gets them with ACP v1; a snapshot that does
//   not extend the previous output carries terminal_output.replace. The
//   input of a tool call streams in after it is announced: an update that
//   changes its input, title, kind or content carries the whole tool call
//   (v1 shape) in _meta["_kate/toolCall"].
// - state_update is passed on (ACPSession handles it: the agent's own work
//   and the commands in the background), its background entries given the
//   toolCallId of their terminal.
//
// v2 has no fs/* or terminal/* requests from the agent: it reads and writes
// files and runs commands itself (as a remote agent does).
class ACPProtocolV2
{
public:
    // A message as ACPSession handles it (v1 shapes).
    struct Incoming {
        bool isResponse = false;
        // Notification or request from the agent.
        QString method;
        QJsonObject params;
        int requestId = -1;
        // Response.
        int id = -1;
        QJsonObject result;
        QJsonObject error;
    };
    struct Outgoing {
        QString method;
        QJsonObject params;
        // False: nothing goes out (e.g. a second session/cancel).
        bool send = true;
    };

    // KATECODE_ACP_PROTOCOL=2 in the environment.
    static bool requestedByEnvironment();

    // Offer v2 in initialize (else nothing is ever translated).
    void setOffered(bool offered) { m_offered = offered; }
    bool isOffered() const { return m_offered; }
    // The agent answered initialize with version 2.
    bool isActive() const { return m_active; }
    // Forget everything (new connection).
    void reset();

    // Outgoing. initializeParams() adds v2's fields to v1's when offered.
    QJsonObject initializeParams(const QJsonObject &v1Params) const;
    Outgoing request(const QString &method, const QJsonObject &params);
    Outgoing notification(const QString &method, const QJsonObject &params);
    // After request(): the id it went out with (v1 method and parameters).
    void requestSent(int id, const QString &method, const QJsonObject &params);

    // Incoming, in the order to handle them (none, one or several).
    QList<Incoming> incomingMessage(const QString &method, const QJsonObject &params, int requestId);
    QList<Incoming> incomingResponse(int id, const QJsonObject &result, const QJsonObject &error);

    // Conversions (static: unit-tested on their own).
    static QJsonObject v1InitializeResult(const QJsonObject &result);
    static QJsonArray v1ConfigOptions(const QJsonArray &options);
    static QJsonArray v1Commands(const QJsonArray &commands);
    static QJsonArray v2McpServers(const QJsonArray &servers);
    // A v2 diff ({changes, patch}) as v1 diffs ({type: diff, path, oldText,
    // newText}, one per file of the patch).
    static QJsonArray v1Diffs(const QJsonObject &diff);
    static QJsonArray v1ToolCallContent(const QJsonArray &content);
    static QString v1ToolCallStatus(const QString &status);

private:
    struct ToolCallState {
        QString name;
        QString title;
        QString kind;
        QString status;
        QJsonObject rawInput;
        QJsonArray content;
        QJsonArray locations;
        QString terminalId;
        // Final status given: input and content dropped (a late update is
        // not a new tool call, and does not show it again).
        bool finished = false;
    };
    struct TerminalState {
        QString toolCallId;
        // The output so far (bytes, until the terminal exits) and its
        // decoder (decodes only what was added).
        QByteArray output;
        std::shared_ptr<QStringDecoder> decoder = std::make_shared<QStringDecoder>(QStringDecoder::Utf8);
        bool exited = false;
    };
    struct PromptState {
        int id = -1;
        QString sessionId;
        QString text;
        bool acknowledged = false;
    };

    QList<Incoming> sessionUpdate(const QJsonObject &params);
    QList<Incoming> toolCallUpdate(const QString &sessionId, QJsonObject update);
    QList<Incoming> terminalOutput(const QString &sessionId, const QString &terminalId, const QByteArray &output, bool replace);
    QList<Incoming> finishPrompts(const QString &sessionId, const QJsonObject &stateUpdate);
    Incoming notificationOf(const QString &sessionId, const QJsonObject &update) const;
    QString toolCallOfTerminal(const QString &terminalId) const;
    bool isOwnEcho(const QJsonObject &update);
    static QString textOf(const QJsonArray &blocks);

    bool m_offered = false;
    bool m_active = false;
    int m_initializeId = -1;
    QHash<int, QString> m_requestMethods;
    // session/resume requests not answered yet (their history replays).
    QSet<int> m_resumeIds;
    QList<PromptState> m_prompts;
    // A live user_message that is not a known echo while a prompt is not
    // acknowledged: held until it is (the echo may differ from the prompt's
    // text), then dropped (its id) or given on as another client's.
    QList<QJsonObject> m_heldUserMessages;
    QList<Incoming> releaseHeldUserMessages(const QString &sessionId);
    QList<Incoming> userMessage(const QString &sessionId, const QJsonObject &update);
    QHash<int, QString> m_resumeSessions;
    QSet<QString> m_ownMessageIds;
    bool m_cancelSent = false;
    QHash<QString, ToolCallState> m_toolCalls;
    QHash<QString, TerminalState> m_terminals;
};
