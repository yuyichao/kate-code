#include "RemoteFs.h"
#include "RemoteFsService.h"

#include "../RemoteLocation.h"

#include <KConfig>
#include <KConfigGroup>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <algorithm>
#include <thread>

static RemoteFs::Config loadConfig()
{
    RemoteFs::Config config;
    KConfig file(KateRemote::configFileName(), KConfig::SimpleConfig);
    const KConfigGroup group(&file, QStringLiteral("General"));
    config.mountRoot = KateRemote::cleanMountRoot(group.readEntry("MountRoot", QString()));
    config.idleTimeout = group.readEntry("IdleTimeout", config.idleTimeout);
    config.retryAfterFailure = group.readEntry("RetryAfterFailure", config.retryAfterFailure);
    config.mountTimeout = group.readEntry("MountTimeout", config.mountTimeout);
    config.sshfsProgram = group.readEntry("SshfsProgram", config.sshfsProgram);
    config.sshCommand = group.readEntry("SshCommand", config.sshCommand);
    config.sshfsOptions = group.readEntry("SshfsOptions", QStringList());
    config.backendDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/kate-remote-fs");
    config.hostsFile = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/kate-remote-fs/hosts");
    return config;
}

int main(int argc, char **argv)
{
    // Termination signals are read from a signalfd on the main thread; block
    // them before any thread exists so that none of them gets one.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    // A broken pipe to a helper must not kill the daemon.
    signal(SIGPIPE, SIG_IGN);

    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("kate-remote-fs"));

    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCritical() << "No session bus";
        return 1;
    }
    const RemoteFs::Config config = loadConfig();
    // One instance per user. A lock file rather than the DBus name: the name
    // is taken only once mounted, so that a DBus-activated client (or
    // systemd, Type=dbus) knows the paths work when the name appears.
    if (!QDir().mkpath(config.backendDir)) {
        qCritical() << "Cannot create" << config.backendDir;
        return 1;
    }
    const int lockFd = open(QFile::encodeName(config.backendDir + QStringLiteral("/lock")).constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lockFd < 0 || flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
        qCritical() << "kate-remote-fs is already running";
        return 1;
    }

    RemoteFs fs(config);
    if (const QString error = fs.mount(); !error.isEmpty()) {
        qCritical().noquote() << error;
        return 1;
    }

    RemoteFsService service(&fs);
    bus.registerObject(RemoteFsService::objectPath(), &service, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals);
    if (!bus.registerService(RemoteFsService::serviceName())) {
        qCritical() << "Cannot register" << RemoteFsService::serviceName();
        return 1;
    }

    // Host events come from FUSE threads; signal them from the main thread.
    RemoteFs::Events events;
    events.mounted = [&service](const QString &scheme, const QString &authority) {
        QMetaObject::invokeMethod(&service, [&service, scheme, authority] {
            Q_EMIT service.mounted(scheme, authority);
        }, Qt::QueuedConnection);
    };
    events.unmounted = [&service](const QString &scheme, const QString &authority) {
        QMetaObject::invokeMethod(&service, [&service, scheme, authority] {
            Q_EMIT service.unmounted(scheme, authority);
        }, Qt::QueuedConnection);
    };
    events.mountFailed = [&service](const QString &scheme, const QString &authority, const QString &error) {
        QMetaObject::invokeMethod(&service, [&service, scheme, authority, error] {
            Q_EMIT service.mountFailed(scheme, authority, error);
        }, Qt::QueuedConnection);
    };
    events.hostsChanged = [&service] {
        QMetaObject::invokeMethod(&service, [&service] {
            Q_EMIT service.hostsChanged();
        }, Qt::QueuedConnection);
    };
    fs.setEvents(events);

    std::thread fuseThread([&fs, &app] {
        const int result = fs.run();
        qInfo() << "FUSE loop ended" << result;
        QMetaObject::invokeMethod(&app, [] {
            QCoreApplication::quit();
        }, Qt::QueuedConnection);
    });

    const int signalFd = signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
    QSocketNotifier signalNotifier(signalFd, QSocketNotifier::Read);
    QObject::connect(&signalNotifier, &QSocketNotifier::activated, &app, [&fs, signalFd] {
        signalfd_siginfo info;
        while (read(signalFd, &info, sizeof(info)) == sizeof(info)) {
            qInfo() << "Signal" << info.ssi_signo << "- exiting";
        }
        fs.requestExit();
    });

    QTimer idleTimer;
    const int idle = fs.config().idleTimeout;
    if (idle > 0) {
        idleTimer.setInterval(std::clamp(idle * 1000 / 4, 1000, 60000));
        QObject::connect(&idleTimer, &QTimer::timeout, &app, [&fs] {
            fs.unmountIdle();
        });
        idleTimer.start();
    }

    const int result = app.exec();
    idleTimer.stop();
    fs.requestExit();
    fuseThread.join();
    fs.unmountAll();
    bus.unregisterObject(RemoteFsService::objectPath());
    bus.unregisterService(RemoteFsService::serviceName());
    close(signalFd);
    close(lockFd);
    return result;
}
