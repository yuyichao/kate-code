/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

// ACPProtocolV2: ACP v2 messages (shapes as claude-agent-acp 0.106's v2 core
// sends them) and the v1 shapes ACPSession gets.

#include "ACPProtocolV2.h"

#include <QJsonDocument>
#include <QTest>

namespace
{
QJsonObject json(const char *text)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray(text), &error);
    if (error.error != QJsonParseError::NoError) {
        qFatal("bad test JSON: %s in %s", qPrintable(error.errorString()), text);
    }
    return document.object();
}

QJsonObject update(const QJsonObject &u)
{
    return QJsonObject{{QStringLiteral("sessionId"), QStringLiteral("s1")}, {QStringLiteral("update"), u}};
}


const char *kInitializeResult = R"({"protocolVersion": 2,
    "info": {"name": "@agentclientprotocol/claude-agent-acp", "title": "Claude Agent", "version": "0.106.0"},
    "capabilities": {"session": {"prompt": {"image": {}, "embeddedContext": {}}, "mcp": {"stdio": {}, "http": {}},
                                 "delete": {}, "fork": {}, "additionalDirectories": {}, "title": {}},
                     "auth": {"status": {}}, "_meta": {"claudeCode": {"promptQueueing": true}}},
    "authMethods": []})";
}

class ACPProtocolV2Test : public QObject
{
    Q_OBJECT

private:
    // An active translator (initialize answered with version 2).
    static void activate(ACPProtocolV2 &protocol)
    {
        protocol.setOffered(true);
        protocol.requestSent(1, QStringLiteral("initialize"), QJsonObject());
        protocol.incomingResponse(1, json(kInitializeResult), QJsonObject());
        QVERIFY(protocol.isActive());
    }
    static QList<ACPProtocolV2::Incoming> receive(ACPProtocolV2 &protocol, const char *u)
    {
        return protocol.incomingMessage(QStringLiteral("session/update"), update(json(u)), -1);
    }
    static QJsonObject updateOf(const ACPProtocolV2::Incoming &message)
    {
        return message.params[QStringLiteral("update")].toObject();
    }

private Q_SLOTS:
    void initialize()
    {
        ACPProtocolV2 protocol;
        const QJsonObject v1 = json(R"({"protocolVersion": 1, "clientCapabilities": {"terminal": true}})");
        // Not offered: v1 as it is, and nothing translated.
        QCOMPARE(protocol.initializeParams(v1), v1);

        protocol.setOffered(true);
        const QJsonObject params = protocol.initializeParams(v1);
        QCOMPARE(params[QStringLiteral("protocolVersion")].toInt(), 2);
        // Both versions' fields: a v1 agent reads its own.
        QCOMPARE(params[QStringLiteral("clientCapabilities")], v1[QStringLiteral("clientCapabilities")]);
        QCOMPARE(params[QStringLiteral("info")].toObject()[QStringLiteral("name")].toString(), QStringLiteral("kate-code"));
        QVERIFY(params[QStringLiteral("capabilities")].isObject());

        protocol.requestSent(1, QStringLiteral("initialize"), params);
        const auto out = protocol.incomingResponse(1, json(kInitializeResult), QJsonObject());
        QCOMPARE(out.size(), 1);
        QVERIFY(protocol.isActive());
        const QJsonObject agent = out[0].result[QStringLiteral("agentCapabilities")].toObject();
        QVERIFY(agent[QStringLiteral("loadSession")].toBool());
        QVERIFY(agent[QStringLiteral("promptCapabilities")].toObject()[QStringLiteral("image")].toBool());
        QVERIFY(agent[QStringLiteral("promptCapabilities")].toObject()[QStringLiteral("embeddedContext")].toBool());
        QVERIFY(!agent[QStringLiteral("promptCapabilities")].toObject()[QStringLiteral("audio")].toBool());
        QVERIFY(agent[QStringLiteral("mcpCapabilities")].toObject()[QStringLiteral("http")].toBool());
        QVERIFY(agent[QStringLiteral("sessionCapabilities")].toObject()[QStringLiteral("title")].isObject());
        QVERIFY(agent[QStringLiteral("sessionCapabilities")].toObject()[QStringLiteral("list")].isObject());
        QVERIFY(agent[QStringLiteral("_meta")].toObject()[QStringLiteral("claudeCode")].toObject()[QStringLiteral("promptQueueing")].toBool());
        QCOMPARE(out[0].result[QStringLiteral("agentInfo")].toObject()[QStringLiteral("version")].toString(), QStringLiteral("0.106.0"));
    }

    void v1Agent()
    {
        // An agent that answers with version 1: nothing is translated.
        ACPProtocolV2 protocol;
        protocol.setOffered(true);
        protocol.requestSent(1, QStringLiteral("initialize"), QJsonObject());
        const QJsonObject result = json(R"({"protocolVersion": 1, "agentCapabilities": {"loadSession": true}})");
        const auto out = protocol.incomingResponse(1, result, QJsonObject());
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0].result, result);
        QVERIFY(!protocol.isActive());
        const QJsonObject load = json(R"({"sessionId": "s1", "cwd": "/work", "mcpServers": []})");
        QCOMPARE(protocol.request(QStringLiteral("session/load"), load).method, QStringLiteral("session/load"));
        const char *chunk = R"({"sessionUpdate": "user_message_chunk", "content": {"type": "text", "text": "hi"}})";
        const auto messages = receive(protocol, chunk);
        QCOMPARE(messages.size(), 1);
        QCOMPARE(updateOf(messages[0]), json(chunk));
        QCOMPARE(protocol.notification(QStringLiteral("$/cancel_request"), json(R"({"id": 5})")).method, QStringLiteral("$/cancel_request"));
    }

    void promptLifecycle()
    {
        ACPProtocolV2 protocol;
        activate(protocol);
        const QJsonObject prompt = json(R"({"sessionId": "s1", "prompt": [{"type": "text", "text": "Do it"}]})");
        QCOMPARE(protocol.request(QStringLiteral("session/prompt"), prompt).method, QStringLiteral("session/prompt"));
        protocol.requestSent(4, QStringLiteral("session/prompt"), prompt);

        // The echo comes before the response that names it: dropped.
        QVERIFY(receive(protocol, R"({"sessionUpdate": "user_message", "messageId": "m-own", "content": [{"text": "Do it", "type": "text"}]})").isEmpty());
        // The response only acknowledges: held back.
        QVERIFY(protocol.incomingResponse(4, json(R"({"messageId": "m-own"})"), QJsonObject()).isEmpty());
        QVERIFY(receive(protocol, R"({"sessionUpdate": "user_message_chunk", "messageId": "m-own", "content": {"type": "text", "text": "x"}})").isEmpty());

        auto out = receive(protocol, R"({"sessionUpdate": "state_update", "state": "running", "background": []})");
        QCOMPARE(out.size(), 1);
        QCOMPARE(updateOf(out[0])[QStringLiteral("state")].toString(), QStringLiteral("running"));
        out = receive(protocol, R"({"sessionUpdate": "agent_message_chunk", "content": {"type": "text", "text": "DONE"}, "messageId": "a1"})");
        QCOMPARE(out.size(), 1);
        QCOMPARE(updateOf(out[0])[QStringLiteral("content")].toObject()[QStringLiteral("text")].toString(), QStringLiteral("DONE"));

        // An idle without a stop reason ends nothing.
        out = receive(protocol, R"({"sessionUpdate": "state_update", "state": "idle", "background": []})");
        QCOMPARE(out.size(), 1);
        QVERIFY(!out[0].isResponse);

        // The end of the work answers the prompt, before the state update.
        out = receive(protocol, R"({"sessionUpdate": "state_update", "state": "idle", "stopReason": "end_turn", "background": []})");
        QCOMPARE(out.size(), 2);
        QVERIFY(out[0].isResponse);
        QCOMPARE(out[0].id, 4);
        QCOMPARE(out[0].result[QStringLiteral("stopReason")].toString(), QStringLiteral("end_turn"));
        QVERIFY(!out[1].isResponse);

        // Answered once only.
        out = receive(protocol, R"({"sessionUpdate": "state_update", "state": "idle", "stopReason": "end_turn"})");
        QCOMPARE(out.size(), 1);
        QVERIFY(!out[0].isResponse);
    }

    void remoteMessage()
    {
        // A message of another client (Remote Control): v1's chunks.
        ACPProtocolV2 protocol;
        activate(protocol);
        const auto out = receive(protocol, R"({"sessionUpdate": "user_message", "messageId": "r1",
            "content": [{"type": "text", "text": "From the phone"}, {"type": "text", "text": "second"}]})");
        QCOMPARE(out.size(), 3);
        QCOMPARE(updateOf(out[0])[QStringLiteral("sessionUpdate")].toString(), QStringLiteral("user_message"));
        QCOMPARE(updateOf(out[1])[QStringLiteral("sessionUpdate")].toString(), QStringLiteral("user_message_chunk"));
        QCOMPARE(updateOf(out[1])[QStringLiteral("content")].toObject()[QStringLiteral("text")].toString(), QStringLiteral("From the phone"));
        QCOMPARE(updateOf(out[2])[QStringLiteral("content")].toObject()[QStringLiteral("text")].toString(), QStringLiteral("second"));
    }

    void heldUserMessages()
    {
        // While a prompt is not acknowledged, a user message that is not its
        // echo by text waits for the response's message id.
        ACPProtocolV2 protocol;
        activate(protocol);
        protocol.requestSent(4, QStringLiteral("session/prompt"), json(R"({"sessionId": "s1", "prompt": [{"type": "text", "text": "x"}, {"type": "resource_link", "uri": "file:\/\/\/w/f", "name": "f"}]})"));
        QVERIFY(receive(protocol, R"({"sessionUpdate": "user_message", "messageId": "m-own", "content": [{"type": "text", "text": "x\n@f"}]})").isEmpty());
        QVERIFY(receive(protocol, R"({"sessionUpdate": "user_message", "messageId": "r1", "content": [{"type": "text", "text": "REMOTE"}]})").isEmpty());
        const auto out = protocol.incomingResponse(4, json(R"({"messageId": "m-own"})"), QJsonObject());
        QCOMPARE(out.size(), 2);
        QCOMPARE(updateOf(out[0])[QStringLiteral("messageId")].toString(), QStringLiteral("r1"));
        QCOMPARE(updateOf(out[1])[QStringLiteral("content")].toObject()[QStringLiteral("text")].toString(), QStringLiteral("REMOTE"));
    }

    void midTurnPrompt()
    {
        // The second prompt is inserted (and acknowledged) only when the
        // running work takes it.
        ACPProtocolV2 protocol;
        activate(protocol);
        const QJsonObject first = json(R"({"sessionId": "s1", "prompt": [{"type": "text", "text": "first"}]})");
        const QJsonObject second = json(R"({"sessionId": "s1", "prompt": [{"type": "text", "text": "second"}]})");
        protocol.requestSent(4, QStringLiteral("session/prompt"), first);
        QVERIFY(protocol.incomingResponse(4, json(R"({"messageId": "m1"})"), QJsonObject()).isEmpty());
        protocol.requestSent(5, QStringLiteral("session/prompt"), second);
        auto out = receive(protocol, R"({"sessionUpdate": "state_update", "state": "idle", "stopReason": "end_turn"})");
        QCOMPARE(out.size(), 2);
        QCOMPARE(out[0].id, 4);
        QVERIFY(receive(protocol, R"({"sessionUpdate": "user_message", "messageId": "m2", "content": [{"type": "text", "text": "second"}]})").isEmpty());
        QVERIFY(protocol.incomingResponse(5, json(R"({"messageId": "m2"})"), QJsonObject()).isEmpty());
        out = receive(protocol, R"({"sessionUpdate": "state_update", "state": "idle", "stopReason": "end_turn"})");
        QCOMPARE(out.size(), 2);
        QCOMPARE(out[0].id, 5);
    }

    void cancel()
    {
        ACPProtocolV2 protocol;
        activate(protocol);
        const QJsonObject first = json(R"({"sessionId": "s1", "prompt": [{"type": "text", "text": "first"}]})");
        const QJsonObject second = json(R"({"sessionId": "s1", "prompt": [{"type": "text", "text": "second"}]})");
        protocol.requestSent(4, QStringLiteral("session/prompt"), first);
        protocol.incomingResponse(4, json(R"({"messageId": "m1"})"), QJsonObject());
        protocol.requestSent(5, QStringLiteral("session/prompt"), second);

        // Cancelling a prompt (answered already) is session/cancel, once.
        auto out = protocol.notification(QStringLiteral("$/cancel_request"), json(R"({"id": 4})"));
        QVERIFY(out.send);
        QCOMPARE(out.method, QStringLiteral("session/cancel"));
        QCOMPARE(out.params[QStringLiteral("sessionId")].toString(), QStringLiteral("s1"));
        QVERIFY(!protocol.notification(QStringLiteral("$/cancel_request"), json(R"({"id": 5})")).send);
        // Other requests' cancellation as it is.
        out = protocol.notification(QStringLiteral("$/cancel_request"), json(R"({"id": 9})"));
        QCOMPARE(out.method, QStringLiteral("$/cancel_request"));

        // A cancelled end answers every prompt, inserted or not.
        const auto messages = receive(protocol, R"({"sessionUpdate": "state_update", "state": "idle", "stopReason": "cancelled", "background": []})");
        QCOMPARE(messages.size(), 3);
        QCOMPARE(messages[0].id, 4);
        QCOMPARE(messages[1].id, 5);
        QCOMPARE(messages[1].result[QStringLiteral("stopReason")].toString(), QStringLiteral("cancelled"));
        // A late response of a prompt answered so: dropped.
        QVERIFY(protocol.incomingResponse(5, json(R"({"messageId": "m2"})"), QJsonObject()).isEmpty());
        // The next cancel goes out again.
        protocol.requestSent(6, QStringLiteral("session/prompt"), first);
        QVERIFY(protocol.notification(QStringLiteral("$/cancel_request"), json(R"({"id": 6})")).send);
    }

    void errors()
    {
        ACPProtocolV2 protocol;
        activate(protocol);
        const QJsonObject prompt = json(R"({"sessionId": "s1", "prompt": [{"type": "text", "text": "x"}]})");
        // Refused: the error is the answer.
        protocol.requestSent(4, QStringLiteral("session/prompt"), prompt);
        auto out = protocol.incomingResponse(4, QJsonObject(), json(R"({"code": -32602, "message": "bad"})"));
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0].error[QStringLiteral("code")].toInt(), -32602);
        // Stopped with an error.
        protocol.requestSent(5, QStringLiteral("session/prompt"), prompt);
        protocol.incomingResponse(5, json(R"({"messageId": "m"})"), QJsonObject());
        out = receive(protocol, R"({"sessionUpdate": "state_update", "state": "idle", "stopReason": "error", "error": {"code": -32603, "message": "boom"}})");
        QCOMPARE(out.size(), 2);
        QCOMPARE(out[0].id, 5);
        QCOMPARE(out[0].error[QStringLiteral("message")].toString(), QStringLiteral("boom"));
    }

    void requests()
    {
        ACPProtocolV2 protocol;
        activate(protocol);
        auto out = protocol.request(QStringLiteral("session/load"),
                                    json(R"({"sessionId": "s1", "cwd": "/work", "mcpServers": [{"name": "kate", "command": "/usr/bin/kate-mcp-server", "args": [], "env": []},
                                                                                         {"type": "http", "name": "web", "url": "http:\/\/127.0.0.1:1/mcp", "headers": []}]})"));
        QCOMPARE(out.method, QStringLiteral("session/resume"));
        QCOMPARE(out.params[QStringLiteral("replayFrom")].toObject()[QStringLiteral("type")].toString(), QStringLiteral("start"));
        const QJsonArray servers = out.params[QStringLiteral("mcpServers")].toArray();
        QCOMPARE(servers[0].toObject()[QStringLiteral("type")].toString(), QStringLiteral("stdio"));
        QCOMPARE(servers[1].toObject()[QStringLiteral("type")].toString(), QStringLiteral("http"));

        out = protocol.request(QStringLiteral("session/set_config_option"), json(R"({"sessionId": "s1", "configId": "model", "value": "haiku"})"));
        QCOMPARE(out.params[QStringLiteral("type")].toString(), QStringLiteral("id"));
        out = protocol.request(QStringLiteral("session/set_config_option"), json(R"({"sessionId": "s1", "configId": "fast", "value": true})"));
        QCOMPARE(out.params[QStringLiteral("type")].toString(), QStringLiteral("boolean"));
        out = protocol.request(QStringLiteral("session/set_mode"), json(R"({"sessionId": "s1", "modeId": "plan"})"));
        QCOMPARE(out.method, QStringLiteral("session/set_config_option"));
        QCOMPARE(out.params[QStringLiteral("configId")].toString(), QStringLiteral("mode"));
        QCOMPARE(out.params[QStringLiteral("value")].toString(), QStringLiteral("plan"));
    }

    void sessionSetup()
    {
        ACPProtocolV2 protocol;
        activate(protocol);
        protocol.requestSent(2, QStringLiteral("session/new"), json(R"({"cwd": "/work"})"));
        const auto out = protocol.incomingResponse(2, json(R"({"sessionId": "s1",
            "configOptions": [{"name": "Mode", "category": "mode", "type": "select", "currentValue": "auto", "configId": "mode",
                               "options": [{"value": "default", "name": "Manual"}, {"value": "auto", "name": "Auto"}]}],
            "availableCommands": [{"type": "command", "name": "review", "description": "Review", "input": {"type": "text", "hint": "target"}},
                                  {"type": "future", "name": "x", "description": "y"}]})"), QJsonObject());
        QCOMPARE(out.size(), 2);
        const QJsonObject option = out[0].result[QStringLiteral("configOptions")].toArray()[0].toObject();
        QCOMPARE(option[QStringLiteral("id")].toString(), QStringLiteral("mode"));
        QVERIFY(!option.contains(QStringLiteral("configId")));
        const QJsonObject commands = updateOf(out[1]);
        QCOMPARE(commands[QStringLiteral("sessionUpdate")].toString(), QStringLiteral("available_commands_update"));
        const QJsonArray list = commands[QStringLiteral("availableCommands")].toArray();
        QCOMPARE(list.size(), 1);
        QCOMPARE(list[0].toObject()[QStringLiteral("input")].toObject()[QStringLiteral("hint")].toString(), QStringLiteral("target"));
        QVERIFY(!list[0].toObject().contains(QStringLiteral("type")));
    }

    void replay()
    {
        ACPProtocolV2 protocol;
        activate(protocol);
        protocol.requestSent(3, QStringLiteral("session/load"), json(R"({"sessionId": "s1", "cwd": "/work"})"));
        // Earlier prompts are not echoes while replaying.
        auto out = receive(protocol, R"({"messageId": "m1", "content": [], "sessionUpdate": "user_message"})");
        QCOMPARE(out.size(), 1);
        QCOMPARE(updateOf(out[0])[QStringLiteral("sessionUpdate")].toString(), QStringLiteral("user_message"));
        out = receive(protocol, R"({"sessionUpdate": "user_message_chunk", "content": {"type": "text", "text": "Do it"}, "messageId": "m1"})");
        QCOMPARE(out.size(), 1);
        out = receive(protocol, R"({"messageId": "a1", "content": [], "sessionUpdate": "agent_message"})");
        QVERIFY(out.isEmpty());
        out = receive(protocol, R"({"messageId": "a1", "content": [{"type": "text", "text": "whole"}], "sessionUpdate": "agent_message"})");
        QCOMPARE(out.size(), 1);
        QCOMPARE(updateOf(out[0])[QStringLiteral("sessionUpdate")].toString(), QStringLiteral("agent_message_chunk"));
        out = protocol.incomingResponse(3, json(R"({"configOptions": [], "availableCommands": []})"), QJsonObject());
        QCOMPARE(out.size(), 2);
        // Live again: an echo of an own prompt is dropped.
        protocol.requestSent(4, QStringLiteral("session/prompt"), json(R"({"sessionId": "s1", "prompt": [{"type": "text", "text": "Do it"}]})"));
        QVERIFY(receive(protocol, R"({"sessionUpdate": "user_message", "messageId": "m2", "content": [{"type": "text", "text": "Do it"}]})").isEmpty());
    }

    void toolCalls()
    {
        ACPProtocolV2 protocol;
        activate(protocol);
        auto out = receive(protocol, R"({"toolCallId": "t1", "name": "Read", "rawInput": {}, "status": "pending", "title": "Read File", "kind": "read",
                                       "locations": [], "content": [], "sessionUpdate": "tool_call_update"})");
        QCOMPARE(out.size(), 1);
        QJsonObject u = updateOf(out[0]);
        QCOMPARE(u[QStringLiteral("sessionUpdate")].toString(), QStringLiteral("tool_call"));
        QCOMPARE(u[QStringLiteral("_meta")].toObject()[QStringLiteral("claudeCode")].toObject()[QStringLiteral("toolName")].toString(), QStringLiteral("Read"));
        out = receive(protocol, R"({"toolCallId": "t1", "rawInput": {"file_path": "/work/notes.txt"}, "title": "Read notes.txt", "sessionUpdate": "tool_call_update"})");
        u = updateOf(out[0]);
        QCOMPARE(u[QStringLiteral("sessionUpdate")].toString(), QStringLiteral("tool_call_update"));
        QVERIFY(!u.contains(QStringLiteral("status")));
        QCOMPARE(u[QStringLiteral("title")].toString(), QStringLiteral("Read notes.txt"));
        // With the whole tool call as it is now.
        const QJsonObject whole = u[QStringLiteral("_meta")].toObject()[QStringLiteral("_kate/toolCall")].toObject();
        QCOMPARE(whole[QStringLiteral("rawInput")].toObject()[QStringLiteral("file_path")].toString(), QStringLiteral("/work/notes.txt"));
        QCOMPARE(whole[QStringLiteral("status")].toString(), QStringLiteral("pending"));
        QCOMPARE(whole[QStringLiteral("kind")].toString(), QStringLiteral("read"));
        QCOMPARE(whole[QStringLiteral("_meta")].toObject()[QStringLiteral("claudeCode")].toObject()[QStringLiteral("toolName")].toString(), QStringLiteral("Read"));
        // Not for a status alone.
        out = receive(protocol, R"({"toolCallId": "t1", "status": "in_progress", "sessionUpdate": "tool_call_update"})");
        QVERIFY(!updateOf(out[0])[QStringLiteral("_meta")].toObject().contains(QStringLiteral("_kate/toolCall")));

        // A new id that starts with only its id: v2's defaults.
        out = receive(protocol, R"({"toolCallId": "t2", "status": "cancelled", "sessionUpdate": "tool_call_update"})");
        u = updateOf(out[0]);
        QCOMPARE(u[QStringLiteral("sessionUpdate")].toString(), QStringLiteral("tool_call"));
        QCOMPARE(u[QStringLiteral("status")].toString(), QStringLiteral("failed"));
        QCOMPARE(u[QStringLiteral("kind")].toString(), QStringLiteral("other"));

        // A late update of a finished tool call: not a new one, not shown
        // again.
        receive(protocol, R"({"toolCallId": "t1", "status": "completed", "sessionUpdate": "tool_call_update"})");
        out = receive(protocol, R"({"toolCallId": "t1", "rawInput": {}, "sessionUpdate": "tool_call_update"})");
        u = updateOf(out[0]);
        QCOMPARE(u[QStringLiteral("sessionUpdate")].toString(), QStringLiteral("tool_call_update"));
        QVERIFY(!u[QStringLiteral("_meta")].toObject().contains(QStringLiteral("_kate/toolCall")));

        // Appended content.
        receive(protocol, R"({"toolCallId": "t3", "status": "in_progress", "sessionUpdate": "tool_call_update",
                              "content": [{"type": "content", "content": {"type": "text", "text": "a"}}]})");
        out = receive(protocol, R"({"toolCallId": "t3", "sessionUpdate": "tool_call_content_chunk", "content": {"type": "content", "content": {"type": "text", "text": "b"}}})");
        u = updateOf(out[0]);
        QCOMPARE(u[QStringLiteral("sessionUpdate")].toString(), QStringLiteral("tool_call_update"));
        QCOMPARE(u[QStringLiteral("content")].toArray().size(), 2);
    }

    void terminals()
    {
        ACPProtocolV2 protocol;
        activate(protocol);
        // The terminal is announced first, then the tool call that shows it.
        QVERIFY(receive(protocol, R"({"sessionUpdate": "terminal_update", "terminalId": "t1"})").isEmpty());
        auto out = receive(protocol, R"({"toolCallId": "t1", "name": "Bash", "rawInput": {}, "status": "pending", "title": "Terminal", "kind": "execute",
                                       "content": [{"type": "terminal", "terminalId": "t1"}], "sessionUpdate": "tool_call_update"})");
        QJsonObject u = updateOf(out[0]);
        QCOMPARE(u[QStringLiteral("_meta")].toObject()[QStringLiteral("terminal_info")].toObject()[QStringLiteral("terminal_id")].toString(), QStringLiteral("t1"));
        QCOMPARE(u[QStringLiteral("content")].toArray()[0].toObject()[QStringLiteral("terminalId")].toString(), QStringLiteral("t1"));

        auto output = [&protocol](const QByteArray &snapshot) {
            const QJsonObject u{{QStringLiteral("sessionUpdate"), QStringLiteral("terminal_update")},
                                {QStringLiteral("terminalId"), QStringLiteral("t1")},
                                {QStringLiteral("output"), QJsonObject{{QStringLiteral("data"), QString::fromLatin1(snapshot.toBase64())}}}};
            return protocol.incomingMessage(QStringLiteral("session/update"), update(u), -1);
        };
        auto outputOf = [](const QList<ACPProtocolV2::Incoming> &messages) {
            return updateOf(messages.value(0))[QStringLiteral("_meta")].toObject()[QStringLiteral("terminal_output")].toObject();
        };
        out = output("tick 1\n");
        QCOMPARE(updateOf(out[0])[QStringLiteral("toolCallId")].toString(), QStringLiteral("t1"));
        QCOMPARE(outputOf(out)[QStringLiteral("data")].toString(), QStringLiteral("tick 1\n"));
        // A snapshot that continues: what is new.
        out = output("tick 1\ntick 2\n");
        QCOMPARE(outputOf(out)[QStringLiteral("data")].toString(), QStringLiteral("tick 2\n"));
        QVERIFY(!outputOf(out).contains(QStringLiteral("replace")));
        QVERIFY(output("tick 1\ntick 2\n").isEmpty());
        // One that does not: replaces it.
        out = output("c\n");
        QCOMPARE(outputOf(out)[QStringLiteral("data")].toString(), QStringLiteral("c\n"));
        QVERIFY(outputOf(out)[QStringLiteral("replace")].toBool());
        // Appended bytes (a character split between chunks).
        const QByteArray euro = "\xe2\x82\xac";
        auto chunk = [&protocol](const QByteArray &bytes) {
            const QJsonObject u{{QStringLiteral("sessionUpdate"), QStringLiteral("terminal_output_chunk")},
                                {QStringLiteral("terminalId"), QStringLiteral("t1")},
                                {QStringLiteral("data"), QString::fromLatin1(bytes.toBase64())}};
            return protocol.incomingMessage(QStringLiteral("session/update"), update(u), -1);
        };
        // Half a character: nothing yet.
        QVERIFY(chunk(euro.left(1)).isEmpty());
        out = chunk(euro.mid(1));
        QCOMPARE(outputOf(out)[QStringLiteral("data")].toString().right(1), QString::fromUtf8(euro));

        // An empty exit status is an exit, without a code.
        out = receive(protocol, R"({"sessionUpdate": "terminal_update", "terminalId": "t1", "exitStatus": {}})");
        QCOMPARE(out.size(), 1);
        const QJsonObject exit = updateOf(out[0])[QStringLiteral("_meta")].toObject()[QStringLiteral("terminal_exit")].toObject();
        QCOMPARE(exit[QStringLiteral("terminal_id")].toString(), QStringLiteral("t1"));
        QVERIFY(exit[QStringLiteral("exit_code")].isNull());
        // Once.
        QVERIFY(receive(protocol, R"({"sessionUpdate": "terminal_update", "terminalId": "t1", "exitStatus": {}, "capabilities": null})").isEmpty());

        receive(protocol, R"({"toolCallId": "t2", "name": "Bash", "status": "pending", "kind": "execute",
                              "content": [{"type": "terminal", "terminalId": "t2"}], "sessionUpdate": "tool_call_update"})");
        out = receive(protocol, R"({"sessionUpdate": "terminal_update", "terminalId": "t2", "exitStatus": {"exitCode": 3}})");
        QCOMPARE(updateOf(out[0])[QStringLiteral("_meta")].toObject()[QStringLiteral("terminal_exit")].toObject()[QStringLiteral("exit_code")].toInt(), 3);
    }

    void background()
    {
        ACPProtocolV2 protocol;
        activate(protocol);
        receive(protocol, R"({"toolCallId": "b1", "name": "Bash", "rawInput": {}, "status": "pending", "kind": "execute",
                              "content": [{"type": "terminal", "terminalId": "b1"}], "sessionUpdate": "tool_call_update"})");
        receive(protocol, R"({"toolCallId": "b1", "rawInput": {"command": "sleep 12", "run_in_background": true}, "sessionUpdate": "tool_call_update"})");
        auto out = receive(protocol, R"({"toolCallId": "b1", "status": "completed", "sessionUpdate": "tool_call_update"})");
        QVERIFY(!updateOf(out[0])[QStringLiteral("_meta")].toObject()[QStringLiteral("claudeCode")].toObject()[QStringLiteral("toolResponse")].toObject()[QStringLiteral("backgroundTaskId")].toString().isEmpty());
        out = receive(protocol, R"({"sessionUpdate": "state_update", "state": "running", "background": [{"type": "terminal", "terminalId": "b1"}, {"type": "tool_call", "toolCallId": "x"}]})");
        const QJsonArray background = updateOf(out[0])[QStringLiteral("background")].toArray();
        QCOMPARE(background[0].toObject()[QStringLiteral("toolCallId")].toString(), QStringLiteral("b1"));
        QCOMPARE(background[1].toObject()[QStringLiteral("toolCallId")].toString(), QStringLiteral("x"));

        // A foreground command: no background task.
        receive(protocol, R"({"toolCallId": "f1", "name": "Bash", "rawInput": {"command": "true"}, "status": "pending", "kind": "execute",
                              "content": [{"type": "terminal", "terminalId": "f1"}], "sessionUpdate": "tool_call_update"})");
        out = receive(protocol, R"({"toolCallId": "f1", "status": "completed", "sessionUpdate": "tool_call_update"})");
        QVERIFY(!updateOf(out[0])[QStringLiteral("_meta")].toObject()[QStringLiteral("claudeCode")].toObject().contains(QStringLiteral("toolResponse")));
    }

    void diffs()
    {
        // claude-agent-acp's Edit and Write.
        QJsonArray v1 = ACPProtocolV2::v1Diffs(json(R"({"type": "diff", "changes": [{"operation": "modify", "path": "/work/notes.txt", "fileType": "text"}],
            "patch": {"format": "git_patch", "text": "diff --git /work/notes.txt /work/notes.txt\n--- /work/notes.txt\n+++ /work/notes.txt\n@@ -1,2 +1,2 @@\n line one\n-line two\n+line 2\n"}})"));
        QCOMPARE(v1.size(), 1);
        QJsonObject diff = v1[0].toObject();
        QCOMPARE(diff[QStringLiteral("type")].toString(), QStringLiteral("diff"));
        QCOMPARE(diff[QStringLiteral("path")].toString(), QStringLiteral("/work/notes.txt"));
        QCOMPARE(diff[QStringLiteral("oldText")].toString(), QStringLiteral("line one\nline two\n"));
        QCOMPARE(diff[QStringLiteral("newText")].toString(), QStringLiteral("line one\nline 2\n"));

        v1 = ACPProtocolV2::v1Diffs(json(R"({"type": "diff", "changes": [{"operation": "add", "path": "/work/other.txt", "fileType": "text"}],
            "patch": {"format": "git_patch", "text": "diff --git /work/other.txt /work/other.txt\nnew file mode 100644\n--- /dev/null\n+++ /work/other.txt\n@@ -0,0 +1 @@\n+hi\n\\ No newline at end of file\n"}})"));
        diff = v1[0].toObject();
        QVERIFY(diff[QStringLiteral("oldText")].isNull());
        QCOMPARE(diff[QStringLiteral("newText")].toString(), QStringLiteral("hi"));

        // Two files, two hunks, a removed line that looks like a file header,
        // an empty context line.
        v1 = ACPProtocolV2::v1Diffs(json(R"({"type": "diff", "changes": [{"operation": "modify", "path": "/w/a.sql"}, {"operation": "delete", "path": "/w/b"}],
            "patch": {"format": "git_patch", "text": "diff --git a/a.sql b/a.sql\n--- a/a.sql\n+++ b/a.sql\n@@ -1,3 +1,2 @@\n x\n--- comment\n\n@@ -9 +8 @@\n-old\n+new\ndiff --git a/b b/b\n--- a/b\n+++ /dev/null\n@@ -1 +0,0 @@\n-gone\n"}})"));
        QCOMPARE(v1.size(), 2);
        diff = v1[0].toObject();
        QCOMPARE(diff[QStringLiteral("path")].toString(), QStringLiteral("/w/a.sql"));
        QCOMPARE(diff[QStringLiteral("oldText")].toString(), QStringLiteral("x\n-- comment\n\n⋯\nold\n"));
        QCOMPARE(diff[QStringLiteral("newText")].toString(), QStringLiteral("x\n\n⋯\nnew\n"));
        diff = v1[1].toObject();
        QCOMPARE(diff[QStringLiteral("path")].toString(), QStringLiteral("/w/b"));
        QCOMPARE(diff[QStringLiteral("oldText")].toString(), QStringLiteral("gone\n"));
        QCOMPARE(diff[QStringLiteral("newText")].toString(), QString());

        // No patch: a note.
        v1 = ACPProtocolV2::v1Diffs(json(R"({"type": "diff", "changes": [{"operation": "move", "oldPath": "/w/a", "path": "/w/b"}]})"));
        QCOMPARE(v1.size(), 1);
        QCOMPARE(v1[0].toObject()[QStringLiteral("content")].toObject()[QStringLiteral("text")].toString(), QStringLiteral("move /w/a → /w/b"));
    }

    void permission()
    {
        ACPProtocolV2 protocol;
        activate(protocol);
        const auto out = protocol.incomingMessage(QStringLiteral("session/request_permission"), json(R"({"sessionId": "s1", "title": "Write other.txt",
            "subject": {"type": "tool_call", "toolCall": {"toolCallId": "w1", "name": "Write", "rawInput": {"file_path": "/work/other.txt", "content": "hi"},
                        "title": "Write other.txt", "kind": "edit", "locations": [{"path": "/work/other.txt"}],
                        "content": [{"type": "diff", "changes": [{"operation": "add", "path": "/work/other.txt"}],
                                     "patch": {"format": "git_patch", "text": "--- /dev/null\n+++ /work/other.txt\n@@ -0,0 +1 @@\n+hi\n"}}]}},
            "options": [{"optionId": "allow-once", "name": "Yes", "kind": "allow_once"}, {"optionId": "reject", "name": "No", "kind": "reject_once"}]})"), 7);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0].requestId, 7);
        const QJsonObject toolCall = out[0].params[QStringLiteral("toolCall")].toObject();
        QCOMPARE(toolCall[QStringLiteral("toolCallId")].toString(), QStringLiteral("w1"));
        QCOMPARE(toolCall[QStringLiteral("rawInput")].toObject()[QStringLiteral("content")].toString(), QStringLiteral("hi"));
        QCOMPARE(toolCall[QStringLiteral("content")].toArray()[0].toObject()[QStringLiteral("newText")].toString(), QStringLiteral("hi\n"));
        QCOMPARE(out[0].params[QStringLiteral("options")].toArray().size(), 2);

        const auto command = protocol.incomingMessage(QStringLiteral("session/request_permission"), json(R"({"sessionId": "s1", "title": "Run",
            "subject": {"type": "command", "command": "rm -r x", "cwd": "/work", "toolCallId": "c1"}, "options": []})"), 8);
        const QJsonObject commandCall = command[0].params[QStringLiteral("toolCall")].toObject();
        QCOMPARE(commandCall[QStringLiteral("toolCallId")].toString(), QStringLiteral("c1"));
        QCOMPARE(commandCall[QStringLiteral("rawInput")].toObject()[QStringLiteral("command")].toString(), QStringLiteral("rm -r x"));
        QCOMPARE(commandCall[QStringLiteral("title")].toString(), QStringLiteral("Run"));
    }

    void plans()
    {
        ACPProtocolV2 protocol;
        activate(protocol);
        auto out = receive(protocol, R"({"sessionUpdate": "plan_update", "plan": {"type": "items", "planId": "p",
                                       "entries": [{"content": "a", "priority": "high", "status": "pending"}]}})");
        QCOMPARE(out.size(), 1);
        QCOMPARE(updateOf(out[0])[QStringLiteral("sessionUpdate")].toString(), QStringLiteral("plan"));
        QCOMPARE(updateOf(out[0])[QStringLiteral("entries")].toArray().size(), 1);
        QVERIFY(receive(protocol, R"({"sessionUpdate": "compaction_update", "compactionId": "c", "status": "started"})").isEmpty());
        out = receive(protocol, R"({"sessionUpdate": "config_option_update", "configOptions": [{"configId": "model", "type": "select", "currentValue": "haiku", "options": []}]})");
        QCOMPARE(updateOf(out[0])[QStringLiteral("configOptions")].toArray()[0].toObject()[QStringLiteral("id")].toString(), QStringLiteral("model"));
    }
};

QTEST_GUILESS_MAIN(ACPProtocolV2Test)
#include "ACPProtocolV2Test.moc"
