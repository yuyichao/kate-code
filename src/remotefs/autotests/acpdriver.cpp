// Test driver for ACPService with agents on remote hosts (test_remotefs.py):
// starts the agent for a folder, runs initialize, session/new and one
// session/prompt, answers fs/read_text_file, and prints what it sees:
//   MSG <json>       a message from the agent (as ACPService delivers it)
//   ERROR <text>     errorOccurred()
//   DONE             the prompt's response arrived
//   DISCONNECTED <n> the agent exited
//   MISSING          remoteAgentMissing()
// acpdriver --install <folder> <registry entry JSON file> <program>
// runs RemoteAgentInstaller instead:
//   PROGRESS <text>, then FINISHED <0|1> <message>
// acpdriver --helper <folder> <pin JSON file> <file>
// runs RemoteHelper (kate-code-helper on the host; file:// downloads):
//   PROGRESS <text>, READY, DIGEST <hex> (file's), WATCHING (file is
//   watched: change it now), CHANGED <paths>; or STOPPED <message>
#include "ACPService.h"
#include "RemoteAgentInstaller.h"
#include "RemoteHelper.h"

#include <QDir>
#include <QFile>

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
    if (args.size() == 5 && args.at(1) == QLatin1String("--install")) {
        QFile entry(args.at(3));
        if (!entry.open(QIODevice::ReadOnly)) {
            return 2;
        }
        RemoteAgentInstaller installer(QJsonDocument::fromJson(entry.readAll()).object(), args.at(2), args.at(4));
        installer.setAllowFileUrls(true);
        if (qEnvironmentVariableIsSet("ACPDRIVER_DOWNLOADS")) {
            installer.setDownloadDirectory(qEnvironmentVariable("ACPDRIVER_DOWNLOADS"));
        }
        out(QStringLiteral("CANINSTALL %1").arg(RemoteAgentInstaller::canInstall(QJsonDocument::fromJson((entry.seek(0), entry.readAll())).object(), args.at(4)) ? 1 : 0));
        QObject::connect(&installer, &RemoteAgentInstaller::progress, [](const QString &text) {
            out(QStringLiteral("PROGRESS ") + text);
        });
        QObject::connect(&installer, &RemoteAgentInstaller::finished, [](bool ok, const QString &message) {
            out(QStringLiteral("FINISHED %1 %2").arg(ok ? 1 : 0).arg(QString(message).replace(QLatin1Char('\n'), QLatin1String(" | "))));
            QCoreApplication::exit(ok ? 0 : 1);
        });
        QTimer::singleShot(0, &installer, &RemoteAgentInstaller::start);
        return app.exec();
    }
    if (args.size() == 5 && args.at(1) == QLatin1String("--helper")) {
        QFile pinFile(args.at(3));
        if (!pinFile.open(QIODevice::ReadOnly)) {
            return 2;
        }
        const auto pin = KateRemote::parseRemoteHelperPin(pinFile.readAll(), true);
        if (!pin) {
            out(QStringLiteral("STOPPED invalid pin"));
            return 1;
        }
        const QString folder = args.at(2);
        const QString root = KateRemote::configuredMountRoot();
        const auto location = KateRemote::locationForPath(folder, root);
        if (!location) {
            return 2;
        }
        const QString hostDir = QDir::cleanPath(KateRemote::pathForLocation({location->scheme, location->authority, QStringLiteral("/")}, root));
        const QString file = args.at(4);
        RemoteHelper helper(folder, KateRemote::RemotePathMap(hostDir));
        helper.setPin(*pin);
        helper.setAllowFileUrls(true);
        if (qEnvironmentVariableIsSet("ACPDRIVER_DOWNLOADS")) {
            helper.setDownloadDirectory(qEnvironmentVariable("ACPDRIVER_DOWNLOADS"));
        }
        QObject::connect(&helper, &RemoteHelper::progress, [](const QString &text) {
            out(QStringLiteral("PROGRESS ") + text);
        });
        QObject::connect(&helper, &RemoteHelper::stopped, [](const QString &message) {
            out(QStringLiteral("STOPPED ") + QString(message).replace(QLatin1Char('\n'), QLatin1String(" | ")));
            QCoreApplication::exit(1);
        });
        QObject::connect(&helper, &RemoteHelper::changed, [](const QStringList &paths) {
            out(QStringLiteral("CHANGED ") + paths.join(QLatin1Char(' ')));
            QCoreApplication::exit(0);
        });
        QObject::connect(&helper, &RemoteHelper::ready, &app, [&helper, file] {
            out(QStringLiteral("READY"));
            helper.digest({file}, [&helper, file](const QHash<QString, QByteArray> &digests) {
                out(QStringLiteral("DIGEST ") + QString::fromLatin1(digests.value(file).toHex()));
                helper.setWatched({file});
                // Answered after the watch request (the helper answers in order).
                helper.request(QStringLiteral("stat"), QJsonObject{{QStringLiteral("path"), QStringLiteral("/")}}, [](const QJsonObject &, const QJsonObject &) {
                    out(QStringLiteral("WATCHING"));
                });
            });
        }, Qt::SingleShotConnection);
        QTimer::singleShot(40000, &app, [] {
            out(QStringLiteral("TIMEOUT"));
            QCoreApplication::exit(3);
        });
        QTimer::singleShot(0, &helper, &RemoteHelper::start);
        return app.exec();
    }
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

    QObject::connect(&service, &ACPService::remoteAgentMissing, [] {
        out(QStringLiteral("MISSING"));
    });
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
