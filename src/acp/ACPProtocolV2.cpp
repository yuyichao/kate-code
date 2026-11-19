/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#include "ACPProtocolV2.h"

#include <QDebug>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStringList>

#include <utility>

namespace
{
const QString kSessionUpdate = QStringLiteral("sessionUpdate");
const QString kToolCallId = QStringLiteral("toolCallId");
const QString kTerminalId = QStringLiteral("terminalId");
const QString kContent = QStringLiteral("content");
const QString kMeta = QStringLiteral("_meta");

bool isFinalStatus(const QString &status)
{
    return status == QLatin1String("completed") || status == QLatin1String("failed") || status == QLatin1String("cancelled");
}

// v2 marks a capability by an object (empty or not).
bool has(const QJsonObject &object, const QString &key)
{
    return object[key].isObject();
}
}

bool ACPProtocolV2::requestedByEnvironment()
{
    return qEnvironmentVariable("KATECODE_ACP_PROTOCOL") == QLatin1String("2");
}

void ACPProtocolV2::reset()
{
    m_active = false;
    m_initializeId = -1;
    m_requestMethods.clear();
    m_resumeIds.clear();
    m_resumeSessions.clear();
    m_prompts.clear();
    m_heldUserMessages.clear();
    m_ownMessageIds.clear();
    m_cancelSent = false;
    m_toolCalls.clear();
    m_terminals.clear();
}

QJsonObject ACPProtocolV2::initializeParams(const QJsonObject &v1Params) const
{
    if (!m_offered) {
        return v1Params;
    }
    QJsonObject params = v1Params;
    params[QStringLiteral("protocolVersion")] = 2;
    // v2's own fields; v1's stay for an agent that answers with version 1.
    QJsonObject info = v1Params[QStringLiteral("clientInfo")].toObject();
    if (info.isEmpty()) {
        info[QStringLiteral("name")] = QStringLiteral("kate-code");
        info[QStringLiteral("version")] = QStringLiteral("0");
    }
    params[QStringLiteral("info")] = info;
    // No elicitation, no terminal authentication.
    params[QStringLiteral("capabilities")] = QJsonObject();
    return params;
}

ACPProtocolV2::Outgoing ACPProtocolV2::request(const QString &method, const QJsonObject &params)
{
    Outgoing out{method, params, true};
    if (!m_active) {
        return out;
    }
    if (method == QLatin1String("session/load")) {
        out.method = QStringLiteral("session/resume");
        out.params[QStringLiteral("replayFrom")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("start")}};
    }
    if (out.params.contains(QStringLiteral("mcpServers"))) {
        out.params[QStringLiteral("mcpServers")] = v2McpServers(out.params[QStringLiteral("mcpServers")].toArray());
    }
    if (method == QLatin1String("session/set_mode")) {
        // Only a config option in v2.
        out.method = QStringLiteral("session/set_config_option");
        out.params = QJsonObject{{QStringLiteral("sessionId"), params[QStringLiteral("sessionId")]},
                                 {QStringLiteral("configId"), QStringLiteral("mode")},
                                 {QStringLiteral("type"), QStringLiteral("id")},
                                 {QStringLiteral("value"), params[QStringLiteral("modeId")]}};
    } else if (method == QLatin1String("session/set_config_option") && !out.params.contains(QStringLiteral("type"))) {
        out.params[QStringLiteral("type")] = params[QStringLiteral("value")].isBool() ? QStringLiteral("boolean") : QStringLiteral("id");
    }
    return out;
}

ACPProtocolV2::Outgoing ACPProtocolV2::notification(const QString &method, const QJsonObject &params)
{
    Outgoing out{method, params, true};
    if (!m_active || method != QLatin1String("$/cancel_request")) {
        return out;
    }
    // A prompt was answered at once: cancelling it is cancelling the
    // session's foreground work.
    const int id = params[QStringLiteral("id")].toInt(-1);
    for (const PromptState &prompt : std::as_const(m_prompts)) {
        if (prompt.id != id) {
            continue;
        }
        out.send = !m_cancelSent;
        m_cancelSent = true;
        out.method = QStringLiteral("session/cancel");
        out.params = QJsonObject{{QStringLiteral("sessionId"), prompt.sessionId}};
        return out;
    }
    return out;
}

void ACPProtocolV2::requestSent(int id, const QString &method, const QJsonObject &params)
{
    if (id < 0) {
        return;
    }
    if (method == QLatin1String("initialize")) {
        m_initializeId = id;
        return;
    }
    if (!m_active) {
        return;
    }
    m_requestMethods.insert(id, method);
    if (method == QLatin1String("session/load")) {
        m_resumeIds.insert(id);
        m_resumeSessions.insert(id, params[QStringLiteral("sessionId")].toString());
    } else if (method == QLatin1String("session/prompt")) {
        PromptState prompt;
        prompt.id = id;
        prompt.sessionId = params[QStringLiteral("sessionId")].toString();
        prompt.text = textOf(params[QStringLiteral("prompt")].toArray());
        m_prompts.append(prompt);
    }
}

QList<ACPProtocolV2::Incoming> ACPProtocolV2::incomingResponse(int id, const QJsonObject &result, const QJsonObject &error)
{
    Incoming response;
    response.isResponse = true;
    response.id = id;
    response.result = result;
    response.error = error;

    if (id == m_initializeId) {
        m_initializeId = -1;
        if (m_offered && error.isEmpty() && result[QStringLiteral("protocolVersion")].toInt() >= 2) {
            qDebug() << "[ACPProtocolV2] The agent speaks ACP v2";
            m_active = true;
            response.result = v1InitializeResult(result);
        }
        return {response};
    }
    if (!m_active) {
        return {response};
    }

    const QString method = m_requestMethods.take(id);
    if (method == QLatin1String("session/prompt")) {
        for (int i = 0; i < m_prompts.size(); ++i) {
            if (m_prompts[i].id != id) {
                continue;
            }
            const QString sessionId = m_prompts[i].sessionId;
            if (!error.isEmpty()) {
                // Not inserted: the prompt failed.
                m_prompts.removeAt(i);
                return QList<Incoming>{response} + releaseHeldUserMessages(sessionId);
            }
            // Inserted: answered when the work ends (state_update idle).
            m_prompts[i].acknowledged = true;
            const QString messageId = result[QStringLiteral("messageId")].toString();
            if (!messageId.isEmpty()) {
                m_ownMessageIds.insert(messageId);
            }
            return releaseHeldUserMessages(sessionId);
        }
        // Already answered (a cancelled turn ended before its response).
        return {};
    }

    QList<Incoming> out;
    const QString resumedSession = m_resumeSessions.take(id);
    if (method == QLatin1String("session/load")) {
        m_resumeIds.remove(id);
    }
    if (!error.isEmpty()) {
        return {response};
    }
    if (result.contains(QStringLiteral("configOptions"))) {
        response.result[QStringLiteral("configOptions")] = v1ConfigOptions(result[QStringLiteral("configOptions")].toArray());
    }
    out.append(response);
    // v2 answers session/new and session/resume with the commands too.
    if ((method == QLatin1String("session/new") || method == QLatin1String("session/load"))
        && result.contains(QStringLiteral("availableCommands"))) {
        QJsonObject update{{kSessionUpdate, QStringLiteral("available_commands_update")},
                           {QStringLiteral("availableCommands"), v1Commands(result[QStringLiteral("availableCommands")].toArray())}};
        const QString sessionId = method == QLatin1String("session/new") ? result[QStringLiteral("sessionId")].toString() : resumedSession;
        out.append(notificationOf(sessionId, update));
    }
    return out;
}

QList<ACPProtocolV2::Incoming> ACPProtocolV2::incomingMessage(const QString &method, const QJsonObject &params, int requestId)
{
    Incoming message;
    message.method = method;
    message.params = params;
    message.requestId = requestId;
    if (!m_active) {
        return {message};
    }
    if (method == QLatin1String("session/update")) {
        return sessionUpdate(params);
    }
    if (method == QLatin1String("session/request_permission")) {
        // v1: the tool call itself; v2: a subject (a tool call, or a command).
        const QJsonObject subject = params[QStringLiteral("subject")].toObject();
        QJsonObject toolCall;
        if (subject[QStringLiteral("type")].toString() == QLatin1String("tool_call")) {
            toolCall = subject[QStringLiteral("toolCall")].toObject();
            toolCall[kContent] = v1ToolCallContent(toolCall[kContent].toArray());
            if (toolCall.contains(QStringLiteral("status"))) {
                toolCall[QStringLiteral("status")] = v1ToolCallStatus(toolCall[QStringLiteral("status")].toString());
            }
        } else if (subject[QStringLiteral("type")].toString() == QLatin1String("command")) {
            toolCall[kToolCallId] = subject[kToolCallId].toString(subject[kTerminalId].toString());
            toolCall[QStringLiteral("kind")] = QStringLiteral("execute");
            toolCall[QStringLiteral("rawInput")] = QJsonObject{{QStringLiteral("command"), subject[QStringLiteral("command")]}};
        }
        if (!toolCall.contains(QStringLiteral("title")) || toolCall[QStringLiteral("title")].toString().isEmpty()) {
            toolCall[QStringLiteral("title")] = params[QStringLiteral("title")];
        }
        message.params = QJsonObject{{QStringLiteral("sessionId"), params[QStringLiteral("sessionId")]},
                                     {QStringLiteral("toolCall"), toolCall},
                                     {QStringLiteral("options"), params[QStringLiteral("options")]}};
        return {message};
    }
    return {message};
}

ACPProtocolV2::Incoming ACPProtocolV2::notificationOf(const QString &sessionId, const QJsonObject &update) const
{
    Incoming message;
    message.method = QStringLiteral("session/update");
    message.params = QJsonObject{{QStringLiteral("sessionId"), sessionId}, {QStringLiteral("update"), update}};
    return message;
}

QString ACPProtocolV2::textOf(const QJsonArray &blocks)
{
    QStringList texts;
    for (const QJsonValue &value : blocks) {
        const QJsonObject block = value.toObject();
        if (block[QStringLiteral("type")].toString() == QLatin1String("text")) {
            texts.append(block[QStringLiteral("text")].toString());
        }
    }
    return texts.join(QLatin1Char('\n'));
}

QList<ACPProtocolV2::Incoming> ACPProtocolV2::userMessage(const QString &sessionId, const QJsonObject &update)
{
    // Where one message starts (v1 has no such update), then its content.
    QList<Incoming> out;
    out.append(notificationOf(sessionId, QJsonObject{{kSessionUpdate, QStringLiteral("user_message")},
                                                     {QStringLiteral("messageId"), update[QStringLiteral("messageId")]}}));
    for (const QJsonValue &block : update[kContent].toArray()) {
        out.append(notificationOf(sessionId, QJsonObject{{kSessionUpdate, QStringLiteral("user_message_chunk")},
                                                         {QStringLiteral("messageId"), update[QStringLiteral("messageId")]},
                                                         {kContent, block}}));
    }
    return out;
}

QList<ACPProtocolV2::Incoming> ACPProtocolV2::releaseHeldUserMessages(const QString &sessionId)
{
    QList<Incoming> out;
    for (const PromptState &prompt : std::as_const(m_prompts)) {
        if (!prompt.acknowledged) {
            // Another prompt may still name one of them.
            return out;
        }
    }
    const QList<QJsonObject> held = std::exchange(m_heldUserMessages, {});
    for (const QJsonObject &update : held) {
        if (!m_ownMessageIds.contains(update[QStringLiteral("messageId")].toString())) {
            out += userMessage(sessionId, update);
        }
    }
    return out;
}

bool ACPProtocolV2::isOwnEcho(const QJsonObject &update)
{
    const QString messageId = update[QStringLiteral("messageId")].toString();
    if (!messageId.isEmpty() && m_ownMessageIds.contains(messageId)) {
        return true;
    }
    // The echo may come before the response that names it: one of the
    // prompts not answered yet, with the same text.
    const QString text = textOf(update[kContent].toArray());
    for (const PromptState &prompt : std::as_const(m_prompts)) {
        if (!prompt.acknowledged && prompt.text == text) {
            if (!messageId.isEmpty()) {
                m_ownMessageIds.insert(messageId);
            }
            return true;
        }
    }
    return false;
}

QList<ACPProtocolV2::Incoming> ACPProtocolV2::sessionUpdate(const QJsonObject &params)
{
    const QString sessionId = params[QStringLiteral("sessionId")].toString();
    QJsonObject update = params[QStringLiteral("update")].toObject();
    const QString type = update[kSessionUpdate].toString();
    const bool replaying = !m_resumeIds.isEmpty();

    if (type == QLatin1String("user_message") || type == QLatin1String("agent_message") || type == QLatin1String("agent_thought")) {
        // A whole message (or its start): its content as v1 chunks.
        if (type == QLatin1String("user_message")) {
            if (!replaying) {
                if (isOwnEcho(update)) {
                    return {};
                }
                bool unacknowledged = false;
                for (const PromptState &prompt : std::as_const(m_prompts)) {
                    unacknowledged = unacknowledged || !prompt.acknowledged;
                }
                if (unacknowledged) {
                    m_heldUserMessages.append(update);
                    return {};
                }
            }
            return userMessage(sessionId, update);
        }
        QList<Incoming> out;
        const QString chunkType = type + QStringLiteral("_chunk");
        for (const QJsonValue &block : update[kContent].toArray()) {
            out.append(notificationOf(sessionId, QJsonObject{{kSessionUpdate, chunkType},
                                                             {QStringLiteral("messageId"), update[QStringLiteral("messageId")]},
                                                             {kContent, block}}));
        }
        return out;
    }
    if (type == QLatin1String("user_message_chunk")) {
        if (!replaying && m_ownMessageIds.contains(update[QStringLiteral("messageId")].toString())) {
            return {};
        }
        return {notificationOf(sessionId, update)};
    }
    if (type == QLatin1String("tool_call_update")) {
        return toolCallUpdate(sessionId, update);
    }
    if (type == QLatin1String("tool_call_content_chunk")) {
        const QString toolCallId = update[kToolCallId].toString();
        QJsonArray content = m_toolCalls.value(toolCallId).content;
        content.append(update[kContent]);
        return toolCallUpdate(sessionId, QJsonObject{{kSessionUpdate, QStringLiteral("tool_call_update")},
                                                     {kToolCallId, toolCallId},
                                                     {kContent, content}});
    }
    if (type == QLatin1String("terminal_update")) {
        const QString terminalId = update[kTerminalId].toString();
        TerminalState &terminal = m_terminals[terminalId];
        QList<Incoming> out;
        const QJsonObject output = update[QStringLiteral("output")].toObject();
        if (output.contains(QStringLiteral("data"))) {
            out += terminalOutput(sessionId, terminalId, QByteArray::fromBase64(output[QStringLiteral("data")].toString().toLatin1()), true);
        }
        // A concrete object (even empty) marks the terminal as exited.
        if (update[QStringLiteral("exitStatus")].isObject() && !terminal.exited) {
            terminal.exited = true;
            // Nothing more is added to it.
            terminal.output = QByteArray();
            const QJsonObject status = update[QStringLiteral("exitStatus")].toObject();
            QJsonObject exit{{QStringLiteral("terminal_id"), terminalId},
                             {QStringLiteral("exit_code"), status.contains(QStringLiteral("exitCode")) ? status[QStringLiteral("exitCode")] : QJsonValue()},
                             {QStringLiteral("signal"), status.contains(QStringLiteral("signal")) ? status[QStringLiteral("signal")] : QJsonValue()}};
            out.append(notificationOf(sessionId, QJsonObject{{kSessionUpdate, QStringLiteral("tool_call_update")},
                                                             {kToolCallId, toolCallOfTerminal(terminalId)},
                                                             {kMeta, QJsonObject{{QStringLiteral("terminal_exit"), exit}}}}));
        }
        return out;
    }
    if (type == QLatin1String("terminal_output_chunk")) {
        const QString terminalId = update[kTerminalId].toString();
        return terminalOutput(sessionId, terminalId, QByteArray::fromBase64(update[QStringLiteral("data")].toString().toLatin1()), false);
    }
    if (type == QLatin1String("state_update")) {
        QJsonArray background;
        for (const QJsonValue &value : update[QStringLiteral("background")].toArray()) {
            QJsonObject entry = value.toObject();
            if (entry.contains(kTerminalId) && !entry.contains(kToolCallId)) {
                entry[kToolCallId] = toolCallOfTerminal(entry[kTerminalId].toString());
            }
            background.append(entry);
        }
        if (update.contains(QStringLiteral("background")) && !update[QStringLiteral("background")].isNull()) {
            update[QStringLiteral("background")] = background;
        }
        QList<Incoming> out;
        if (update[QStringLiteral("state")].toString() == QLatin1String("idle")) {
            out += finishPrompts(sessionId, update);
        }
        out.append(notificationOf(sessionId, update));
        return out;
    }
    if (type == QLatin1String("plan_update")) {
        const QJsonObject plan = update[QStringLiteral("plan")].toObject();
        if (plan[QStringLiteral("type")].toString() != QLatin1String("items")) {
            return {};
        }
        return {notificationOf(sessionId, QJsonObject{{kSessionUpdate, QStringLiteral("plan")},
                                                      {QStringLiteral("entries"), plan[QStringLiteral("entries")]}})};
    }
    if (type == QLatin1String("available_commands_update")) {
        update[QStringLiteral("availableCommands")] = v1Commands(update[QStringLiteral("availableCommands")].toArray());
        return {notificationOf(sessionId, update)};
    }
    if (type == QLatin1String("config_option_update")) {
        update[QStringLiteral("configOptions")] = v1ConfigOptions(update[QStringLiteral("configOptions")].toArray());
        return {notificationOf(sessionId, update)};
    }
    if (type == QLatin1String("compaction_update") || type == QLatin1String("compaction_summary_chunk")) {
        qDebug() << "[ACPProtocolV2] Not shown:" << type << update[QStringLiteral("status")].toString();
        return {};
    }
    // agent_message_chunk, agent_thought_chunk, usage_update,
    // session_info_update, notice: as they are.
    return {notificationOf(sessionId, update)};
}

QString ACPProtocolV2::toolCallOfTerminal(const QString &terminalId) const
{
    const auto it = m_terminals.constFind(terminalId);
    if (it != m_terminals.constEnd() && !it->toolCallId.isEmpty()) {
        return it->toolCallId;
    }
    // claude-agent-acp names a command's terminal after its tool call (and
    // announces the terminal first).
    return terminalId;
}

QList<ACPProtocolV2::Incoming> ACPProtocolV2::terminalOutput(const QString &sessionId, const QString &terminalId, const QByteArray &output, bool replace)
{
    TerminalState &terminal = m_terminals[terminalId];
    QJsonObject chunk{{QStringLiteral("terminal_id"), terminalId}};
    QString text;
    if (!replace || output.startsWith(terminal.output)) {
        // Appended (a chunk, or a snapshot that continues the last one).
        const QByteArray added = replace ? output.mid(terminal.output.size()) : output;
        terminal.output += added;
        text = terminal.decoder->decode(added);
    } else {
        // Not a continuation: the output so far is replaced.
        terminal.output = output;
        terminal.decoder->resetState();
        text = terminal.decoder->decode(output);
        chunk[QStringLiteral("replace")] = true;
    }
    if (text.isEmpty() && !chunk.contains(QStringLiteral("replace"))) {
        return {};
    }
    chunk[QStringLiteral("data")] = text;
    return {notificationOf(sessionId, QJsonObject{{kSessionUpdate, QStringLiteral("tool_call_update")},
                                                  {kToolCallId, toolCallOfTerminal(terminalId)},
                                                  {kMeta, QJsonObject{{QStringLiteral("terminal_output"), chunk}}}})};
}

QList<ACPProtocolV2::Incoming> ACPProtocolV2::toolCallUpdate(const QString &sessionId, QJsonObject update)
{
    const QString toolCallId = update[kToolCallId].toString();
    const bool isNew = !m_toolCalls.contains(toolCallId);
    ToolCallState &state = m_toolCalls[toolCallId];
    const bool wasFinished = state.finished;

    QJsonObject meta = update[kMeta].toObject();
    QJsonObject claudeCode = meta[QStringLiteral("claudeCode")].toObject();
    if (update.contains(QStringLiteral("name"))) {
        state.name = update[QStringLiteral("name")].toString();
    }
    if (!state.name.isEmpty()) {
        claudeCode[QStringLiteral("toolName")] = state.name;
    }
    if (update.contains(QStringLiteral("title"))) {
        state.title = update[QStringLiteral("title")].toString();
    }
    if (update.contains(QStringLiteral("locations"))) {
        state.locations = update[QStringLiteral("locations")].toArray();
    }
    if (update.contains(QStringLiteral("kind"))) {
        state.kind = update[QStringLiteral("kind")].isNull() ? QStringLiteral("other") : update[QStringLiteral("kind")].toString();
    }
    if (update.contains(QStringLiteral("rawInput")) && update[QStringLiteral("rawInput")].isObject()) {
        state.rawInput = update[QStringLiteral("rawInput")].toObject();
    }
    if (update.contains(kContent)) {
        const QJsonArray content = update[kContent].toArray();
        state.content = content;
        for (const QJsonValue &value : content) {
            const QJsonObject item = value.toObject();
            if (item[QStringLiteral("type")].toString() == QLatin1String("terminal")) {
                state.terminalId = item[kTerminalId].toString();
                m_terminals[state.terminalId].toolCallId = toolCallId;
            }
        }
        update[kContent] = v1ToolCallContent(content);
    }
    QString status;
    if (update.contains(QStringLiteral("status"))) {
        status = update[QStringLiteral("status")].isNull() ? QStringLiteral("pending") : update[QStringLiteral("status")].toString();
        state.status = status;
        update[QStringLiteral("status")] = v1ToolCallStatus(status);
    }
    if (isNew && !state.terminalId.isEmpty()) {
        meta[QStringLiteral("terminal_info")] = QJsonObject{{QStringLiteral("terminal_id"), state.terminalId}};
    }
    // A command that went to the background completes while it runs: say so
    // as claude-agent-acp's v1 core does (the task's id is not given).
    if (status == QLatin1String("completed") && !state.terminalId.isEmpty()
        && state.rawInput[QStringLiteral("run_in_background")].toBool()
        && !m_terminals.value(state.terminalId).exited) {
        QJsonObject toolResponse = claudeCode[QStringLiteral("toolResponse")].toObject();
        toolResponse[QStringLiteral("backgroundTaskId")] = state.terminalId;
        claudeCode[QStringLiteral("toolResponse")] = toolResponse;
    }
    if (!claudeCode.isEmpty()) {
        meta[QStringLiteral("claudeCode")] = claudeCode;
    }
    // What is shown of it changed: the whole tool call.
    if (!isNew && !wasFinished
        && (update.contains(QStringLiteral("rawInput")) || update.contains(QStringLiteral("title")) || update.contains(QStringLiteral("kind"))
            || update.contains(QStringLiteral("name")) || update.contains(kContent))) {
        QJsonObject whole{{kToolCallId, toolCallId},
                          {QStringLiteral("title"), state.title},
                          {QStringLiteral("kind"), state.kind.isEmpty() ? QStringLiteral("other") : state.kind},
                          {QStringLiteral("status"), v1ToolCallStatus(state.status.isEmpty() ? QStringLiteral("pending") : state.status)},
                          {QStringLiteral("rawInput"), state.rawInput},
                          {kContent, v1ToolCallContent(state.content)},
                          {QStringLiteral("locations"), state.locations}};
        QJsonObject wholeMeta;
        if (!state.name.isEmpty()) {
            wholeMeta[QStringLiteral("claudeCode")] = QJsonObject{{QStringLiteral("toolName"), state.name}};
        }
        if (!state.terminalId.isEmpty()) {
            wholeMeta[QStringLiteral("terminal_info")] = QJsonObject{{QStringLiteral("terminal_id"), state.terminalId}};
        }
        whole[kMeta] = wholeMeta;
        meta[QStringLiteral("_kate/toolCall")] = whole;
    }
    if (!meta.isEmpty()) {
        update[kMeta] = meta;
    }
    update[kSessionUpdate] = isNew ? QStringLiteral("tool_call") : QStringLiteral("tool_call_update");
    if (isNew) {
        // v1's tool_call is complete: v2's defaults for what is left out.
        if (!update.contains(QStringLiteral("status"))) {
            update[QStringLiteral("status")] = QStringLiteral("pending");
        }
        if (!update.contains(QStringLiteral("kind"))) {
            update[QStringLiteral("kind")] = QStringLiteral("other");
        }
    }
    if (isFinalStatus(status)) {
        // Shown in full already; its terminal's exit may still come.
        state.finished = true;
        state.rawInput = QJsonObject();
        state.content = QJsonArray();
        state.locations = QJsonArray();
    }
    return {notificationOf(sessionId, update)};
}

QList<ACPProtocolV2::Incoming> ACPProtocolV2::finishPrompts(const QString &sessionId, const QJsonObject &stateUpdate)
{
    const QJsonValue stopReasonValue = stateUpdate[QStringLiteral("stopReason")];
    if (!stopReasonValue.isString()) {
        // Not the end of foreground work (e.g. a background command ended).
        return {};
    }
    const QString stopReason = stopReasonValue.toString();
    const bool cancelled = stopReason == QLatin1String("cancelled");
    m_cancelSent = false;
    QList<Incoming> out;
    for (int i = 0; i < m_prompts.size();) {
        const PromptState prompt = m_prompts[i];
        // Prompts not inserted yet start work of their own, unless all of it
        // was cancelled.
        if (!prompt.acknowledged && !cancelled) {
            ++i;
            continue;
        }
        // Its method stays known (when not acknowledged yet), so a late
        // response is dropped.
        m_prompts.removeAt(i);
        Incoming response;
        response.isResponse = true;
        response.id = prompt.id;
        if (stopReason == QLatin1String("error")) {
            QJsonObject error = stateUpdate[QStringLiteral("error")].toObject();
            if (error.isEmpty()) {
                error = QJsonObject{{QStringLiteral("code"), -32603}, {QStringLiteral("message"), QStringLiteral("The agent stopped with an error")}};
            }
            response.error = error;
        } else {
            response.result = QJsonObject{{QStringLiteral("stopReason"), stopReason}};
        }
        out.append(response);
    }
    if (!m_heldUserMessages.isEmpty()) {
        out += releaseHeldUserMessages(sessionId);
    }
    return out;
}

QJsonObject ACPProtocolV2::v1InitializeResult(const QJsonObject &result)
{
    const QJsonObject capabilities = result[QStringLiteral("capabilities")].toObject();
    const QJsonObject session = capabilities[QStringLiteral("session")].toObject();
    const QJsonObject prompt = session[QStringLiteral("prompt")].toObject();
    const QJsonObject mcp = session[QStringLiteral("mcp")].toObject();

    QJsonObject agent;
    // session/resume with replay is part of v2's baseline.
    agent[QStringLiteral("loadSession")] = true;
    agent[QStringLiteral("promptCapabilities")] = QJsonObject{{QStringLiteral("image"), has(prompt, QStringLiteral("image"))},
                                                              {QStringLiteral("audio"), has(prompt, QStringLiteral("audio"))},
                                                              {QStringLiteral("embeddedContext"), has(prompt, QStringLiteral("embeddedContext"))}};
    agent[QStringLiteral("mcpCapabilities")] = QJsonObject{{QStringLiteral("http"), has(mcp, QStringLiteral("http"))}, {QStringLiteral("sse"), false}};
    QJsonObject sessionCapabilities;
    // Baseline in v2: listing, resuming, closing.
    sessionCapabilities[QStringLiteral("list")] = QJsonObject();
    sessionCapabilities[QStringLiteral("resume")] = QJsonObject();
    sessionCapabilities[QStringLiteral("close")] = QJsonObject();
    for (auto it = session.constBegin(); it != session.constEnd(); ++it) {
        if (it.key() != QLatin1String("prompt") && it.key() != QLatin1String("mcp")) {
            sessionCapabilities[it.key()] = it.value();
        }
    }
    agent[QStringLiteral("sessionCapabilities")] = sessionCapabilities;
    if (capabilities.contains(QStringLiteral("auth"))) {
        agent[QStringLiteral("auth")] = capabilities[QStringLiteral("auth")];
    }
    if (capabilities.contains(kMeta)) {
        agent[kMeta] = capabilities[kMeta];
    }

    QJsonObject v1;
    v1[QStringLiteral("protocolVersion")] = result[QStringLiteral("protocolVersion")];
    v1[QStringLiteral("agentCapabilities")] = agent;
    v1[QStringLiteral("agentInfo")] = result[QStringLiteral("info")];
    QJsonArray authMethods;
    for (const QJsonValue &value : result[QStringLiteral("authMethods")].toArray()) {
        QJsonObject method = value.toObject();
        if (method.contains(QStringLiteral("methodId"))) {
            method[QStringLiteral("id")] = method.take(QStringLiteral("methodId"));
        }
        authMethods.append(method);
    }
    v1[QStringLiteral("authMethods")] = authMethods;
    return v1;
}

QJsonArray ACPProtocolV2::v1ConfigOptions(const QJsonArray &options)
{
    QJsonArray v1;
    for (const QJsonValue &value : options) {
        QJsonObject option = value.toObject();
        if (option.contains(QStringLiteral("configId"))) {
            option[QStringLiteral("id")] = option.take(QStringLiteral("configId"));
        }
        v1.append(option);
    }
    return v1;
}

QJsonArray ACPProtocolV2::v1Commands(const QJsonArray &commands)
{
    QJsonArray v1;
    for (const QJsonValue &value : commands) {
        QJsonObject command = value.toObject();
        const QString type = command.take(QStringLiteral("type")).toString();
        if (!type.isEmpty() && type != QLatin1String("command")) {
            continue;
        }
        if (command.contains(QStringLiteral("input"))) {
            QJsonObject input = command[QStringLiteral("input")].toObject();
            input.remove(QStringLiteral("type"));
            command[QStringLiteral("input")] = input;
        }
        v1.append(command);
    }
    return v1;
}

QJsonArray ACPProtocolV2::v2McpServers(const QJsonArray &servers)
{
    QJsonArray v2;
    for (const QJsonValue &value : servers) {
        QJsonObject server = value.toObject();
        if (!server.contains(QStringLiteral("type"))) {
            // v1 leaves the type out for stdio servers.
            server[QStringLiteral("type")] = server.contains(QStringLiteral("url")) ? QStringLiteral("http") : QStringLiteral("stdio");
        }
        v2.append(server);
    }
    return v2;
}

QString ACPProtocolV2::v1ToolCallStatus(const QString &status)
{
    if (status == QLatin1String("cancelled")) {
        return QStringLiteral("failed");
    }
    if (status == QLatin1String("paused")) {
        return QStringLiteral("in_progress");
    }
    return status;
}

QJsonArray ACPProtocolV2::v1ToolCallContent(const QJsonArray &content)
{
    QJsonArray v1;
    for (const QJsonValue &value : content) {
        const QJsonObject item = value.toObject();
        if (item[QStringLiteral("type")].toString() == QLatin1String("diff")) {
            for (const QJsonValue &diff : v1Diffs(item)) {
                v1.append(diff);
            }
        } else {
            v1.append(item);
        }
    }
    return v1;
}

QJsonArray ACPProtocolV2::v1Diffs(const QJsonObject &diff)
{
    const QJsonArray changes = diff[QStringLiteral("changes")].toArray();
    // The path of a change as the patch names it (claude-agent-acp: the
    // absolute path; git's a/ and b/ prefixes otherwise).
    auto changeFor = [&changes](const QString &patchPath) -> QJsonObject {
        QString path = patchPath;
        for (int pass = 0; pass < 2; ++pass) {
            for (const QJsonValue &value : changes) {
                const QJsonObject change = value.toObject();
                if (change[QStringLiteral("path")].toString() == path || change[QStringLiteral("oldPath")].toString() == path) {
                    return change;
                }
            }
            if (path.startsWith(QLatin1String("a/")) || path.startsWith(QLatin1String("b/"))) {
                path = path.mid(1);
            }
            for (const QJsonValue &value : changes) {
                const QJsonObject change = value.toObject();
                if (change[QStringLiteral("path")].toString().endsWith(path)) {
                    return change;
                }
            }
        }
        return QJsonObject();
    };

    struct FileDiff {
        QString oldPath;
        QString newPath;
        QString oldText;
        QString newText;
        bool hunks = false;
    };
    QList<FileDiff> files;
    const QJsonObject patch = diff[QStringLiteral("patch")].toObject();
    const QStringList lines = patch[QStringLiteral("text")].toString().split(QLatin1Char('\n'));
    // Lines of the current hunk still to come (its header counts them).
    int oldLeft = 0;
    int newLeft = 0;
    QChar lastSide;
    static const QRegularExpression hunkHeader(QStringLiteral("^@@ -\\d+(?:,(\\d+))? \\+\\d+(?:,(\\d+))? @@"));
    for (const QString &line : lines) {
        if (oldLeft > 0 || newLeft > 0) {
            FileDiff &file = files.last();
            const QChar side = line.isEmpty() ? QLatin1Char(' ') : line.at(0);
            const QString text = line.mid(1) + QLatin1Char('\n');
            if (side == QLatin1Char(' ')) {
                file.oldText += text;
                file.newText += text;
                --oldLeft;
                --newLeft;
            } else if (side == QLatin1Char('-')) {
                file.oldText += text;
                --oldLeft;
            } else if (side == QLatin1Char('+')) {
                file.newText += text;
                --newLeft;
            } else if (side == QLatin1Char('\\')) {
                continue;
            } else {
                // Malformed: the hunk ends here.
                oldLeft = newLeft = 0;
                continue;
            }
            lastSide = side;
            continue;
        }
        if (line.startsWith(QLatin1Char('\\')) && !files.isEmpty()) {
            // "\ No newline at end of file": of the line before.
            FileDiff &file = files.last();
            if ((lastSide == QLatin1Char('-') || lastSide == QLatin1Char(' ')) && file.oldText.endsWith(QLatin1Char('\n'))) {
                file.oldText.chop(1);
            }
            if ((lastSide == QLatin1Char('+') || lastSide == QLatin1Char(' ')) && file.newText.endsWith(QLatin1Char('\n'))) {
                file.newText.chop(1);
            }
            continue;
        }
        lastSide = QChar();
        if (line.startsWith(QLatin1String("diff ")) || (line.startsWith(QLatin1String("--- ")) && (files.isEmpty() || files.last().hunks))) {
            files.append(FileDiff());
        }
        if (files.isEmpty()) {
            continue;
        }
        FileDiff &file = files.last();
        if (line.startsWith(QLatin1String("--- "))) {
            file.oldPath = line.mid(4).trimmed();
        } else if (line.startsWith(QLatin1String("+++ "))) {
            file.newPath = line.mid(4).trimmed();
        } else if (const QRegularExpressionMatch match = hunkHeader.match(line); match.hasMatch()) {
            if (file.hunks) {
                // Lines left out between two hunks.
                file.oldText += QStringLiteral("⋯\n");
                file.newText += QStringLiteral("⋯\n");
            }
            file.hunks = true;
            oldLeft = match.captured(1).isEmpty() ? 1 : match.captured(1).toInt();
            newLeft = match.captured(2).isEmpty() ? 1 : match.captured(2).toInt();
        }
    }

    QJsonArray v1;
    QSet<QString> shown;
    for (const FileDiff &file : std::as_const(files)) {
        const bool added = file.oldPath == QLatin1String("/dev/null");
        const bool deleted = file.newPath == QLatin1String("/dev/null");
        const QJsonObject change = changeFor(deleted ? file.oldPath : file.newPath);
        QString path = change[QStringLiteral("path")].toString();
        if (path.isEmpty()) {
            path = deleted ? file.oldPath : file.newPath;
        }
        QJsonObject item{{QStringLiteral("type"), QStringLiteral("diff")},
                         {QStringLiteral("path"), path},
                         {QStringLiteral("oldText"), added ? QJsonValue() : QJsonValue(file.oldText)},
                         {QStringLiteral("newText"), deleted ? QString() : file.newText}};
        v1.append(item);
        shown.insert(path);
    }
    // Changes without a patch: shown as a note.
    for (const QJsonValue &value : changes) {
        const QJsonObject change = value.toObject();
        const QString path = change[QStringLiteral("path")].toString();
        if (path.isEmpty() || shown.contains(path)) {
            continue;
        }
        const QString operation = change[QStringLiteral("operation")].toString();
        QString text = operation + QLatin1Char(' ') + path;
        if (change.contains(QStringLiteral("oldPath"))) {
            text = operation + QLatin1Char(' ') + change[QStringLiteral("oldPath")].toString() + QStringLiteral(" → ") + path;
        }
        v1.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("content")},
                              {kContent, QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), text}}}});
    }
    return v1;
}
