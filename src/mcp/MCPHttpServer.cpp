/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2025 Kate Code contributors
*/

#include "MCPHttpServer.h"
#include "MCPServer.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>

#include <cstdio>

namespace
{
// Larger requests are refused (a katecode_write of a big file is well
// below).
constexpr qsizetype MaxRequest = 64 * 1024 * 1024;
constexpr qsizetype MaxHeaders = 64 * 1024;
}

MCPHttpServer::MCPHttpServer(MCPServer *server, const QByteArray &token, QObject *parent)
    : QObject(parent)
    , m_server(server)
    , m_token(token)
    , m_tcp(new QTcpServer(this))
{
    connect(m_tcp, &QTcpServer::newConnection, this, &MCPHttpServer::onConnection);
}

quint16 MCPHttpServer::listen()
{
    return m_tcp->listen(QHostAddress::LocalHost, 0) ? m_tcp->serverPort() : 0;
}

void MCPHttpServer::onConnection()
{
    while (QTcpSocket *socket = m_tcp->nextPendingConnection()) {
        m_buffers.insert(socket, QByteArray());
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
            onReadyRead(socket);
        });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            m_buffers.remove(socket);
            socket->deleteLater();
        });
    }
}

void MCPHttpServer::onReadyRead(QTcpSocket *socket)
{
    if (!m_buffers.contains(socket)) {
        return;
    }
    m_buffers[socket] += socket->readAll();
    // Keep-alive: several requests may follow each other.
    while (m_buffers.contains(socket) && handleRequest(socket)) {
    }
}

static bool sameToken(const QByteArray &a, const QByteArray &b)
{
    // Compare digests: no early exit on the first differing byte.
    return QCryptographicHash::hash(a, QCryptographicHash::Sha256) == QCryptographicHash::hash(b, QCryptographicHash::Sha256);
}

bool MCPHttpServer::handleRequest(QTcpSocket *socket)
{
    QByteArray &buffer = m_buffers[socket];
    const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        if (buffer.size() > MaxHeaders) {
            reply(socket, 431, "Request Header Fields Too Large");
            socket->disconnectFromHost();
            m_buffers.remove(socket);
        }
        return false;
    }
    const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
    const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
    const QByteArray method = requestLine.value(0);
    QByteArray target = requestLine.value(1);
    if (const qsizetype query = target.indexOf('?'); query >= 0) {
        target.truncate(query);
    }
    qsizetype contentLength = 0;
    bool chunked = false;
    QByteArray authorization;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0) {
            continue;
        }
        const QByteArray name = line.left(colon).trimmed().toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        if (name == "content-length") {
            contentLength = value.toLongLong();
        } else if (name == "transfer-encoding") {
            chunked = value.toLower().contains("chunked");
        } else if (name == "authorization") {
            authorization = value;
        }
    }
    if (chunked || contentLength < 0 || contentLength > MaxRequest) {
        reply(socket, chunked ? 411 : 413, chunked ? "Length Required" : "Content Too Large");
        socket->disconnectFromHost();
        m_buffers.remove(socket);
        return false;
    }
    const qsizetype bodyStart = headerEnd + 4;
    if (buffer.size() < bodyStart + contentLength) {
        return false;
    }
    const QByteArray body = buffer.mid(bodyStart, contentLength);
    buffer.remove(0, bodyStart + contentLength);

    if (!sameToken(authorization, "Bearer " + m_token)) {
        reply(socket, 401, "Unauthorized", QByteArray(), "WWW-Authenticate: Bearer\r\n");
        return true;
    }
    if (target != "/mcp") {
        reply(socket, 404, "Not Found");
        return true;
    }
    if (method != "POST") {
        // No server-sent events stream (GET) and no sessions (DELETE).
        reply(socket, 405, "Method Not Allowed", QByteArray(), "Allow: POST\r\n");
        return true;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || (!document.isObject() && !document.isArray())) {
        const QJsonObject parseError{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                     {QStringLiteral("id"), QJsonValue::Null},
                                     {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), -32700}, {QStringLiteral("message"), QStringLiteral("Parse error")}}}};
        reply(socket, 400, "Bad Request", QJsonDocument(parseError).toJson(QJsonDocument::Compact));
        return true;
    }
    QPointer<QTcpSocket> alive(socket);
    if (document.isObject()) {
        const QJsonObject response = m_server->handleMessage(document.object());
        if (!alive) {
            return false;
        }
        if (response.isEmpty()) {
            reply(socket, 202, "Accepted");
        } else {
            reply(socket, 200, "OK", QJsonDocument(response).toJson(QJsonDocument::Compact));
        }
        return m_buffers.contains(socket);
    }
    QJsonArray responses;
    for (const QJsonValue &message : document.array()) {
        const QJsonObject response = m_server->handleMessage(message.toObject());
        if (!response.isEmpty()) {
            responses.append(response);
        }
    }
    if (!alive) {
        return false;
    }
    if (responses.isEmpty()) {
        reply(socket, 202, "Accepted");
    } else {
        reply(socket, 200, "OK", QJsonDocument(responses).toJson(QJsonDocument::Compact));
    }
    return m_buffers.contains(socket);
}

void MCPHttpServer::reply(QTcpSocket *socket, int status, const QByteArray &reason, const QByteArray &body, const QByteArray &extraHeaders)
{
    // KATECODE_MCP_LOG=1: one line per response on stderr (debugging).
    static const bool log = qEnvironmentVariableIntValue("KATECODE_MCP_LOG") > 0;
    if (log) {
        fprintf(stderr, "kate-mcp-server: HTTP %d %s\n", status, body.left(160).constData());
    }
    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason + "\r\n";
    if (!body.isEmpty()) {
        response += "Content-Type: application/json\r\n";
    }
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n" + extraHeaders + "\r\n" + body;
    socket->write(response);
}
