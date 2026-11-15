// Test driver for ACPService with agents on remote hosts (test_remotefs.py):
// starts the agent for a folder, runs initialize, session/new and one
// session/prompt, answers fs/read_text_file, and prints what it sees:
//   MSG <json>       a message from the agent (as ACPService delivers it)
//   ERROR <text>     errorOccurred()
//   DONE             the prompt's response arrived
//   DISCONNECTED <n> the agent exited
#include "ACPService.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>

#include <cstdio>

static void out(const QString &line)
{
    std::printf("%s\n", line.toUtf8().constData());
    std::fflush(stdout);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 3) {
        std::fprintf(stderr, "usage: acpdriver <folder> <agent> [args...]\n");
        return 2;
    }
    const QString folder = args.at(1);
    ACPService service;
    service.setExecutable(args.at(2), args.mid(3));
    // Kate's MCP server for the remote agent (as ACPSession sets it up).
    if (qEnvironmentVariableIsSet("ACPDRIVER_MCP_SERVER")) {
        service.setRemoteMcpServer(qEnvironmentVariable("ACPDRIVER_MCP_SERVER"), QProcessEnvironment::systemEnvironment());
    }
    int initId = -1;
    int newId = -1;
    int promptId = -1;
    bool done = false;

    QObject::connect(&service, &ACPService::connected, [&] {
        out(QStringLiteral("REMOTE %1").arg(service.isRemote() ? service.remoteHost() : QStringLiteral("-")));
        initId = service.sendRequest(QStringLiteral("initialize"), QJsonObject{{QStringLiteral("protocolVersion"), 1}});
    });
    QObject::connect(&service, &ACPService::responseReceived, [&](int id, const QJsonObject &result, const QJsonObject &error) {
        out(QStringLiteral("MSG ") + QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("result"), result}, {QStringLiteral("error"), error}}).toJson(QJsonDocument::Compact)));
        if (id == initId) {
            auto sessionNew = [&] {
                QJsonArray servers;
                if (!service.remoteMcpUrl().isEmpty()) {
                    out(QStringLiteral("MCP ") + service.remoteMcpUrl());
                    servers.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("http")},
                                               {QStringLiteral("name"), QStringLiteral("kate")},
                                               {QStringLiteral("url"), service.remoteMcpUrl()},
                                               {QStringLiteral("headers"),
                                                QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("Authorization")},
                                                                       {QStringLiteral("value"), QString(QStringLiteral("Bearer ") + service.remoteMcpToken())}}}}});
                }
                newId = service.sendRequest(QStringLiteral("session/new"), QJsonObject{{QStringLiteral("cwd"), folder}, {QStringLiteral("mcpServers"), servers}});
            };
            if (service.isRemoteMcpPending()) {
                QObject::connect(&service, &ACPService::remoteMcpReady, &app, sessionNew, Qt::SingleShotConnection);
            } else {
                sessionNew();
            }
        } else if (id == newId) {
            const QJsonObject text{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), QStringLiteral("Look at %1/f.txt").arg(folder)}};
            promptId = service.sendRequest(QStringLiteral("session/prompt"),
                                           QJsonObject{{QStringLiteral("sessionId"), result[QStringLiteral("sessionId")]}, {QStringLiteral("prompt"), QJsonArray{text}}});
        } else if (id == promptId) {
            out(QStringLiteral("DONE"));
            done = true;
            service.stop();
            app.quit();
        }
    });
    QObject::connect(&service, &ACPService::notificationReceived, [&](const QString &method, const QJsonObject &params, int requestId) {
        out(QStringLiteral("MSG ") + QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("method"), method}, {QStringLiteral("params"), params}}).toJson(QJsonDocument::Compact)));
        if (method == QLatin1String("fs/read_text_file")) {
            // Contents are not paths: never translated.
            service.sendResponse(requestId, QJsonObject{{QStringLiteral("content"), QStringLiteral("content mentions %1/f.txt").arg(folder)}});
        }
    });
    QObject::connect(&service, &ACPService::errorOccurred, [&](const QString &message) {
        out(QStringLiteral("ERROR ") + QString(message).replace(QLatin1Char('\n'), QLatin1String(" | ")));
    });
    QObject::connect(&service, &ACPService::disconnected, [&](int code) {
        if (done) {
            return;
        }
        out(QStringLiteral("DISCONNECTED %1").arg(code));
        app.exit(1);
    });
    QTimer::singleShot(30000, &app, [&] {
        out(QStringLiteral("TIMEOUT"));
        app.exit(3);
    });
    if (!service.start(folder)) {
        return 1;
    }
    return app.exec();
}
