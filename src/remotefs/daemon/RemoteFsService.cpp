#include "RemoteFsService.h"

#include "BuildVersion.h"

#include "RemoteFs.h"

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
