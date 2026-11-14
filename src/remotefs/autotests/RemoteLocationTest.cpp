#include "RemoteLocation.h"

#include <QDir>
#include <QTest>

using namespace KateRemote;

class RemoteLocationTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void mountRoot()
    {
        const QString home = QDir::homePath();
        QCOMPARE(cleanMountRoot(QString()), home + QStringLiteral("/.kate-remote"));
        QCOMPARE(cleanMountRoot(QStringLiteral("relative/dir")), defaultMountRoot());
        QCOMPARE(cleanMountRoot(QStringLiteral("~/remote/")), home + QStringLiteral("/remote"));
        QCOMPARE(cleanMountRoot(QStringLiteral("/mnt//r/./x/..")), QStringLiteral("/mnt/r"));
        // Never the root or the home directory itself.
        QCOMPARE(cleanMountRoot(QStringLiteral("/")), defaultMountRoot());
        QCOMPARE(cleanMountRoot(QStringLiteral("~")), defaultMountRoot());
    }

    void sshAuthority_data()
    {
        QTest::addColumn<QString>("authority");
        QTest::addColumn<bool>("valid");
        QTest::addColumn<QString>("user");
        QTest::addColumn<QString>("host");
        QTest::addColumn<int>("port");
        QTest::addColumn<QString>("destination");

        QTest::newRow("host") << "yyc-mba" << true << "" << "yyc-mba" << -1 << "yyc-mba";
        QTest::newRow("user@host:port") << "me@build.example.org:2222" << true << "me" << "build.example.org" << 2222 << "me@build.example.org";
        QTest::newRow("underscore alias") << "my_box" << true << "" << "my_box" << -1 << "my_box";
        QTest::newRow("ipv4") << "10.0.0.1:22" << true << "" << "10.0.0.1" << 22 << "10.0.0.1";
        QTest::newRow("ipv6") << "u@[fe80::1]:22" << true << "u" << "fe80::1" << 22 << "u@[fe80::1]";

        QTest::newRow("empty") << "" << false << "" << "" << -1 << "";
        QTest::newRow("option host") << "-oProxyCommand=x" << false << "" << "" << -1 << "";
        QTest::newRow("option user") << "-l@host" << false << "" << "" << -1 << "";
        QTest::newRow("dot git") << ".git" << false << "" << "" << -1 << "";
        QTest::newRow("dot") << "." << false << "" << "" << -1 << "";
        QTest::newRow("dotdot") << ".." << false << "" << "" << -1 << "";
        QTest::newRow("space") << "a b" << false << "" << "" << -1 << "";
        QTest::newRow("bad port") << "host:0" << false << "" << "" << -1 << "";
        QTest::newRow("big port") << "host:65536" << false << "" << "" << -1 << "";
        QTest::newRow("two ats") << "a@b@c" << false << "" << "" << -1 << "";
        QTest::newRow("brackets no colon") << "[abc]" << false << "" << "" << -1 << "";
        QTest::newRow("shell") << "host;rm" << false << "" << "" << -1 << "";
    }

    void sshAuthority()
    {
        QFETCH(QString, authority);
        QFETCH(bool, valid);
        const auto target = parseSshAuthority(authority);
        QCOMPARE(target.has_value(), valid);
        QCOMPARE(isValidAuthority(QStringLiteral("sftp"), authority), valid);
        if (!valid) {
            return;
        }
        QTEST(target->user, "user");
        QTEST(target->host, "host");
        QTEST(target->port, "port");
        QTEST(target->destination(), "destination");
    }

    void pathMapping()
    {
        const QString root = QStringLiteral("/home/u/.kate-remote");
        const Location location{QStringLiteral("sftp"), QStringLiteral("me@host:2222"), QStringLiteral("/srv/proj/a.cpp")};
        const QString path = root + QStringLiteral("/sftp/me@host:2222/srv/proj/a.cpp");
        QCOMPARE(pathForLocation(location, root), path);
        QCOMPARE(locationForPath(path, root).value_or(Location()), location);
        QCOMPARE(location.url(), QUrl(QStringLiteral("sftp://me@host:2222/srv/proj/a.cpp")));
        QCOMPARE(locationForUrl(location.url()).value_or(Location()), location);

        // The host directory is the remote root.
        const Location hostRoot{QStringLiteral("sftp"), QStringLiteral("host"), QStringLiteral("/")};
        QCOMPARE(locationForPath(root + QStringLiteral("/sftp/host"), root).value_or(Location()), hostRoot);
        QCOMPARE(locationForPath(root + QStringLiteral("/sftp/host/"), root).value_or(Location()), hostRoot);
        QCOMPARE(pathForLocation(hostRoot, root), root + QStringLiteral("/sftp/host"));
        QCOMPARE(locationForUrl(QUrl(QStringLiteral("sftp://host"))).value_or(Location()), hostRoot);

        // Not below a host.
        QVERIFY(!locationForPath(root, root));
        QVERIFY(!locationForPath(root + QStringLiteral("/sftp"), root));
        QVERIFY(!locationForPath(root + QStringLiteral("x/sftp/host/a"), root));
        QVERIFY(!locationForPath(QStringLiteral("/home/u/other"), root));
        QVERIFY(!locationForPath(root + QStringLiteral("/smb/host/a"), root));
        QVERIFY(!locationForPath(root + QStringLiteral("/sftp/.git"), root));
        QVERIFY(!locationForPath(QStringLiteral("relative/sftp/host"), root));
        // Cleaned before mapping.
        QCOMPARE(locationForPath(root + QStringLiteral("/sftp/host/a/../b"), root)->path, QStringLiteral("/b"));

        // URLs: passwords dropped, unsupported schemes rejected, IPv6 kept.
        QCOMPARE(locationForUrl(QUrl(QStringLiteral("sftp://me:secret@host/x")))->authority, QStringLiteral("me@host"));
        QVERIFY(!locationForUrl(QUrl(QStringLiteral("smb://host/x"))));
        QVERIFY(!locationForUrl(QUrl(QStringLiteral("file:///x"))));
        QCOMPARE(locationForUrl(QUrl(QStringLiteral("sftp://[fe80::1]:22/x")))->authority, QStringLiteral("[fe80::1]:22"));
    }

    void symlinkTargets_data()
    {
        QTest::addColumn<QString>("linkDir");
        QTest::addColumn<QString>("target");
        QTest::addColumn<QString>("expected");

        // Staying inside: unchanged.
        QTest::newRow("sibling") << "home/u" << "f.txt" << "f.txt";
        QTest::newRow("up within") << "home/u/proj" << "../../x" << "../../x";
        QTest::newRow("up to root") << "home/u" << "../../etc" << "../../etc";
        QTest::newRow("dot") << "home" << "./a/b" << "./a/b";
        // Absolute: relative to the link's directory, through the host root.
        QTest::newRow("absolute") << "home/u" << "/etc/passwd" << "../../etc/passwd";
        QTest::newRow("absolute at root") << "" << "/usr/lib" << "usr/lib";
        QTest::newRow("absolute root") << "home/u" << "/" << "../..";
        QTest::newRow("absolute root at root") << "" << "/" << ".";
        QTest::newRow("absolute dotdot") << "a" << "/../../b" << "../b";
        // Climbing above the remote root: clamped there, as on the remote.
        QTest::newRow("escape") << "home/u" << "../../../../.ssh/id_rsa" << "../../.ssh/id_rsa";
        QTest::newRow("escape at root") << "" << "../x" << "x";
        QTest::newRow("escape and back") << "a" << "../../a/b" << "../a/b";
        QTest::newRow("empty") << "a" << "" << "";
    }

    void symlinkTargets()
    {
        QFETCH(QString, linkDir);
        QFETCH(QString, target);
        QFETCH(QString, expected);
        QCOMPARE(QString::fromStdString(hostSymlinkTarget(linkDir.toStdString(), target.toStdString())), expected);
    }
};

QTEST_GUILESS_MAIN(RemoteLocationTest)

#include "RemoteLocationTest.moc"
