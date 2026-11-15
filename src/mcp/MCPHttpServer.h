/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2025 Kate Code contributors
*/

#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>

class MCPServer;
class QTcpServer;
class QTcpSocket;

/**
 * MCP's Streamable HTTP transport for MCPServer, for agents on remote
 * hosts (reached through an ssh remote forward): POST /mcp with one
 * JSON-RPC message (or a batch) answers with application/json (202 for
 * notifications only); GET (server-sent events) is 405, which clients
 * accept. Requests need "Authorization: Bearer <token>": the forwarded
 * port is open to every user of the remote host.
 *
 * Listens on 127.0.0.1 only. Requests are answered one at a time, as on
 * stdio (a question to the user blocks the others).
 */
class MCPHttpServer : public QObject
{
    Q_OBJECT

public:
    MCPHttpServer(MCPServer *server, const QByteArray &token, QObject *parent = nullptr);

    // Listens on a free port of 127.0.0.1; 0 on failure.
    quint16 listen();

private:
    void onConnection();
    void onReadyRead(QTcpSocket *socket);
    // Handles one complete request at the start of the socket's buffer;
    // false while it is incomplete.
    bool handleRequest(QTcpSocket *socket);
    void reply(QTcpSocket *socket, int status, const QByteArray &reason, const QByteArray &body = QByteArray(),
               const QByteArray &extraHeaders = QByteArray());

    MCPServer *m_server;
    QByteArray m_token;
    QTcpServer *m_tcp;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};
