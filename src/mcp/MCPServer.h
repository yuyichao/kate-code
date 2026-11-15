/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2025 Kate Code contributors
*/

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include "RemotePathMap.h"

class MCPServer
{
public:
    MCPServer();

    // For an agent on a remote host: tool paths are the host's (translated
    // to Kate's local paths and back), only that host's documents are
    // listed, and there is no terminal tool (Kate's terminal is local).
    void setRemote(const KateRemote::RemotePathMap &map)
    {
        m_remote = map;
    }

    // Process a single JSON-RPC message and return the response.
    // Returns a null QJsonObject for notifications (no response needed).
    QJsonObject handleMessage(const QJsonObject &msg);

private:
    QJsonObject handleInitialize(int id, const QJsonObject &params);
    QJsonObject handleToolsList(int id, const QJsonObject &params);
    QJsonObject handleToolsCall(int id, const QJsonObject &params);
    QJsonObject dispatchTool(int id, const QString &toolName, const QJsonObject &arguments);

    QJsonObject executeDocuments(const QJsonObject &arguments);
    QJsonObject executeRead(const QJsonObject &arguments);
    QJsonObject executeEdit(const QJsonObject &arguments);
    QJsonObject executeWrite(const QJsonObject &arguments);
    QJsonObject executeAskUserQuestion(const QJsonObject &arguments);
    QJsonObject executePasteToTerminal(const QJsonObject &arguments);

    QJsonObject makeResponse(int id, const QJsonObject &result);
    QJsonObject makeErrorResponse(int id, int code, const QString &message);
    QJsonObject makeErrorResult(const QString &message);

    bool m_initialized = false;
    KateRemote::RemotePathMap m_remote;
};
