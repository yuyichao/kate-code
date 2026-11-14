#pragma once

#include <QObject>
#include <QStringList>

class RemoteFs;

/**
 * DBus interface of kate-remote-fs (org.kde.KateRemoteFS at
 * /org/kde/KateRemoteFS), for the Remote Projects plugin and Kate Code.
 */
class RemoteFsService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.KateRemoteFS")

public:
    explicit RemoteFsService(RemoteFs *fs, QObject *parent = nullptr);

    static QString serviceName();
    static QString objectPath();

public Q_SLOTS:
    QString mountRoot() const;
    // Connected hosts as "<scheme>/<authority>".
    QStringList mounts() const;
    // Why the last connection attempt to the host failed (empty if it did not).
    QString lastError(const QString &scheme, const QString &authority) const;
    // Disconnects the host unless files are open on it; also forgets a failed
    // attempt so that the next access retries at once.
    bool unmount(const QString &scheme, const QString &authority);
    // Registered hosts ("<scheme>/<authority>"): only these are connected
    // to; other names under <root>/<scheme>/ do not exist. The daemon owns
    // the list (~/.local/state/kate-remote-fs/hosts); change it here.
    QStringList hosts() const;
    bool addHost(const QString &scheme, const QString &authority);
    // Disconnects and forgets the host; false if files are open on it.
    bool removeHost(const QString &scheme, const QString &authority);

Q_SIGNALS:
    void mounted(const QString &scheme, const QString &authority);
    void unmounted(const QString &scheme, const QString &authority);
    void mountFailed(const QString &scheme, const QString &authority, const QString &error);
    void hostsChanged();

private:
    RemoteFs *m_fs;
};
