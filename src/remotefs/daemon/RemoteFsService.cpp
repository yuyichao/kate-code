#include "RemoteFsService.h"

#include "BuildVersion.h"

#include "RemoteFs.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDebug>
#include <QFile>

#include <thread>

RemoteFsService::RemoteFsService(RemoteFs *fs, QObject *parent)
    : QObject(parent)
    , m_fs(fs)
    , m_watcher(new QDBusServiceWatcher(QString(), QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForUnregistration, this))
{
    connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this](const QString &name) {
        m_watcher->removeWatchedService(name);
        m_clients.remove(name);
    });
}

void RemoteFsService::noteCaller()
{
    if (!calledFromDBus()) {
        return;
    }
    const QString name = message().service();
    if (name.isEmpty() || m_clients.contains(name)) {
        return;
    }
    Client client;
    if (QDBusConnectionInterface *bus = QDBusConnection::sessionBus().interface()) {
        const QDBusReply<uint> pid = bus->servicePid(name);
        client.pid = pid.isValid() ? pid.value() : 0;
    }
    m_clients.insert(name, client);
    m_watcher->addWatchedService(name);
}

void RemoteFsService::pruneClients()
{
    QDBusConnectionInterface *bus = QDBusConnection::sessionBus().interface();
    if (!bus) {
        return;
    }
    for (auto it = m_clients.begin(); it != m_clients.end();) {
        const QDBusReply<bool> registered = bus->isServiceRegistered(it.key());
        if (registered.isValid() && !registered.value()) {
            m_watcher->removeWatchedService(it.key());
            it = m_clients.erase(it);
        } else {
            ++it;
        }
    }
}

QStringList RemoteFsService::holders(const QString &except)
{
    pruneClients();
    QStringList result;
    for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
        const Client &client = it.value();
        QFile comm(QStringLiteral("/proc/%1/comm").arg(client.pid));
        QString who = comm.open(QIODevice::ReadOnly) ? QString::fromLocal8Bit(comm.readAll()).trimmed() : it.key();
        who = QStringLiteral("%1 (pid %2)").arg(who).arg(client.pid);
        if (!client.paths.isEmpty()) {
            QStringList shown = client.paths.mid(0, 5);
            if (client.paths.size() > shown.size()) {
                shown << QStringLiteral("and %1 more").arg(client.paths.size() - shown.size());
            }
            result << QStringLiteral("%1 has remote folders or documents open: %2").arg(who, shown.join(QStringLiteral(", ")));
        } else if (!client.reports && it.key() != except) {
            result << QStringLiteral("%1 uses remote folders but does not report which are open (an older Kate?)").arg(who);
        }
    }
    return result;
}

bool RemoteFsService::hasHolders()
{
    return !holders(QString()).isEmpty();
}

void RemoteFsService::setOpenPaths(const QStringList &paths)
{
    noteCaller();
    if (!calledFromDBus()) {
        return;
    }
    Client &client = m_clients[message().service()];
    client.reports = true;
    client.paths = paths;
}

QString RemoteFsService::restart()
{
    noteCaller();
    const QStringList busy = holders(calledFromDBus() ? message().service() : QString());
    if (!busy.isEmpty()) {
        return QStringLiteral("Not restarted: close them first. %1.").arg(busy.join(QStringLiteral("; ")));
    }
    if (!m_restartHandler) {
        return QStringLiteral("Not restarted: not supported");
    }
    const QString error = m_restartHandler();
    if (!error.isEmpty()) {
        return QStringLiteral("Not restarted: %1").arg(error);
    }
    qInfo() << "Restarting on request of" << (calledFromDBus() ? message().service() : QString());
    return {};
}

QString RemoteFsService::serviceName()
{
    return QStringLiteral("org.kde.KateRemoteFS");
}

QString RemoteFsService::objectPath()
{
    return QStringLiteral("/org/kde/KateRemoteFS");
}

QString RemoteFsService::mountRoot()
{
    noteCaller();
    return m_fs->config().mountRoot;
}

QString RemoteFsService::version()
{
    noteCaller();
    return KateRemote::buildVersion();
}

QString RemoteFsService::askpassProgram()
{
    noteCaller();
    return m_fs->askpassProgram();
}

QStringList RemoteFsService::mounts()
{
    noteCaller();
    return m_fs->mountedHosts();
}

QString RemoteFsService::lastError(const QString &scheme, const QString &authority)
{
    noteCaller();
    return m_fs->lastError(scheme, authority);
}

bool RemoteFsService::unmount(const QString &scheme, const QString &authority)
{
    noteCaller();
    return m_fs->unmountHost(scheme, authority);
}

QStringList RemoteFsService::hosts()
{
    noteCaller();
    return m_fs->hosts();
}

bool RemoteFsService::addHost(const QString &scheme, const QString &authority)
{
    noteCaller();
    return m_fs->addHost(scheme, authority);
}

bool RemoteFsService::removeHost(const QString &scheme, const QString &authority)
{
    noteCaller();
    return m_fs->removeHost(scheme, authority);
}

QString RemoteFsService::addFolder(const QString &scheme, const QString &authority, const QString &path, const QDBusMessage &message)
{
    noteCaller();
    // Off the main thread: connecting may wait for a login dialog.
    message.setDelayedReply(true);
    RemoteFs *fs = m_fs;
    std::thread([fs, scheme, authority, path, message] {
        const QString error = fs->addFolder(scheme, authority, path);
        QDBusConnection::sessionBus().send(message.createReply(error));
    }).detach();
    return {};
}
