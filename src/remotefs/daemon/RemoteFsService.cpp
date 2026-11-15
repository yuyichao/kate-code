#include "RemoteFsService.h"

#include "BuildVersion.h"

#include "RemoteFs.h"

#include <QDBusConnection>

#include <thread>

RemoteFsService::RemoteFsService(RemoteFs *fs, QObject *parent)
    : QObject(parent)
    , m_fs(fs)
{
}

QString RemoteFsService::serviceName()
{
    return QStringLiteral("org.kde.KateRemoteFS");
}

QString RemoteFsService::objectPath()
{
    return QStringLiteral("/org/kde/KateRemoteFS");
}

QString RemoteFsService::mountRoot() const
{
    return m_fs->config().mountRoot;
}

QString RemoteFsService::version() const
{
    return KateRemote::buildVersion();
}

QString RemoteFsService::askpassProgram() const
{
    return m_fs->askpassProgram();
}

QStringList RemoteFsService::mounts() const
{
    return m_fs->mountedHosts();
}

QString RemoteFsService::lastError(const QString &scheme, const QString &authority) const
{
    return m_fs->lastError(scheme, authority);
}

bool RemoteFsService::unmount(const QString &scheme, const QString &authority)
{
    return m_fs->unmountHost(scheme, authority);
}

QStringList RemoteFsService::hosts() const
{
    return m_fs->hosts();
}

bool RemoteFsService::addHost(const QString &scheme, const QString &authority)
{
    return m_fs->addHost(scheme, authority);
}

bool RemoteFsService::removeHost(const QString &scheme, const QString &authority)
{
    return m_fs->removeHost(scheme, authority);
}

QString RemoteFsService::addFolder(const QString &scheme, const QString &authority, const QString &path, const QDBusMessage &message)
{
    // Off the main thread: connecting may wait for a login dialog.
    message.setDelayedReply(true);
    RemoteFs *fs = m_fs;
    std::thread([fs, scheme, authority, path, message] {
        const QString error = fs->addFolder(scheme, authority, path);
        QDBusConnection::sessionBus().send(message.createReply(error));
    }).detach();
    return {};
}
