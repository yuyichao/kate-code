#include "RemoteLocation.h"

#include <QDir>
#include <QStandardPaths>
#include <QTest>

using namespace KateRemote;

class RemoteLocationTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // Defaults, not the user's kate-remoterc (KioSchemes).
        QStandardPaths::setTestModeEnabled(true);
    }

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

    void kioSchemesAndAuthorities()
    {
        QVERIFY(isKioScheme(QStringLiteral("smb")));
        QVERIFY(isKioScheme(QStringLiteral("fish")));
        QVERIFY(!isKioScheme(QStringLiteral("sftp")));
        QVERIFY(isSupportedScheme(QStringLiteral("sftp")));
        QVERIFY(isSupportedScheme(QStringLiteral("webdavs")));
        QVERIFY(!isSupportedScheme(QStringLiteral("file")));
        QVERIFY(!isSupportedScheme(QStringLiteral("trash")));
        QVERIFY(isValidAuthority(QStringLiteral("smb"), QStringLiteral("WORK;bob@nas")));
        QVERIFY(!isValidAuthority(QStringLiteral("sftp"), QStringLiteral("WORK;bob@nas")));
        QVERIFY(!isValidAuthority(QStringLiteral("smb"), QStringLiteral(".git")));
        QVERIFY(!isValidAuthority(QStringLiteral("smb"), QStringLiteral("-x")));
        QVERIFY(!isValidAuthority(QStringLiteral("smb"), QStringLiteral("a b")));
        const QString root = QStringLiteral("/r");
        const Location smb{QStringLiteral("smb"), QStringLiteral("nas:445"), QStringLiteral("/share/dir")};
        QCOMPARE(pathForLocation(smb, root), QStringLiteral("/r/smb/nas:445/share/dir"));
        QCOMPARE(locationForPath(QStringLiteral("/r/smb/nas:445/share/dir"), root).value_or(Location()), smb);
        QCOMPARE(smb.url(), QUrl(QStringLiteral("smb://nas:445/share/dir")));
    }

    void kioPasswordCache()
    {
        QCOMPARE(kioAuthUrl(*parseSshAuthority(QStringLiteral("host"))), QUrl(QStringLiteral("sftp://host")));
        QCOMPARE(kioAuthUrl(*parseSshAuthority(QStringLiteral("me@host:22"))), QUrl(QStringLiteral("sftp://me@host")));
        QCOMPARE(kioAuthUrl(*parseSshAuthority(QStringLiteral("me@host:2222"))), QUrl(QStringLiteral("sftp://me@host:2222")));
        QCOMPARE(kioAuthUrl(*parseSshAuthority(QStringLiteral("[fe80::1]"))), QUrl(QStringLiteral("sftp://[fe80::1]")));

        QVERIFY(isLoginPasswordPrompt(QStringLiteral("alice@host's password: ")));
        QVERIFY(isLoginPasswordPrompt(QStringLiteral("(alice@host) Password: ")));
        QVERIFY(isLoginPasswordPrompt(QStringLiteral("Password: ")));
        QVERIFY(isLoginPasswordPrompt(QStringLiteral("Password for alice@host: ")));
        QVERIFY(!isLoginPasswordPrompt(QStringLiteral("Enter passphrase for key '/home/a/.ssh/id_ed25519': ")));
        QVERIFY(!isLoginPasswordPrompt(QStringLiteral("Are you sure you want to continue connecting (yes/no/[fingerprint])? ")));
        QVERIFY(!isLoginPasswordPrompt(QStringLiteral("Verification code: ")));
        QVERIFY(!isLoginPasswordPrompt(QStringLiteral("New password: ")));
        QVERIFY(!isLoginPasswordPrompt(QStringLiteral("Retype new password: ")));
        QVERIFY(!isLoginPasswordPrompt(QStringLiteral("Current password: ")));

        QCOMPARE(userFromPasswordPrompt(QStringLiteral("alice@host's password: ")), QStringLiteral("alice"));
        QCOMPARE(userFromPasswordPrompt(QStringLiteral("(bob@build.example.org) Password: ")), QStringLiteral("bob"));
        QCOMPARE(userFromPasswordPrompt(QStringLiteral("Password: ")), QString());
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
        QVERIFY(!locationForPath(root + QStringLiteral("/gopher/host/a"), root));
        QVERIFY(!locationForPath(root + QStringLiteral("/sftp/.git"), root));
        QVERIFY(!locationForPath(QStringLiteral("relative/sftp/host"), root));
        // Cleaned before mapping.
        QCOMPARE(locationForPath(root + QStringLiteral("/sftp/host/a/../b"), root)->path, QStringLiteral("/b"));

        // URLs: passwords dropped, unsupported schemes rejected, IPv6 kept.
        QCOMPARE(locationForUrl(QUrl(QStringLiteral("sftp://me:secret@host/x")))->authority, QStringLiteral("me@host"));
        QVERIFY(!locationForUrl(QUrl(QStringLiteral("gopher://host/x"))));
        QVERIFY(!locationForUrl(QUrl(QStringLiteral("trash:/x"))));
        QVERIFY(!locationForUrl(QUrl(QStringLiteral("file:///x"))));
        QCOMPARE(locationForUrl(QUrl(QStringLiteral("sftp://[fe80::1]:22/x")))->authority, QStringLiteral("[fe80::1]:22"));
    }

    void userInput_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("url");
        QTest::addColumn<int>("error");
        using E = InputError;
        const QString root = QDir::homePath() + QStringLiteral("/.kate-remote");
        QTest::newRow("url") << "sftp://host/srv/app" << "sftp://host/srv/app" << int(E::None);
        QTest::newRow("url user port") << " sftp://me@host:2222/a/ " << "sftp://me@host:2222/a" << int(E::None);
        QTest::newRow("scp") << "host:/srv/app" << "sftp://host/srv/app" << int(E::None);
        QTest::newRow("scp user") << "me@host:/a/b" << "sftp://me@host/a/b" << int(E::None);
        QTest::newRow("mount path") << QString(root + QStringLiteral("/sftp/host/srv")) << "sftp://host/srv" << int(E::None);
        QTest::newRow("mount path tilde") << "~/.kate-remote/sftp/host/srv" << "sftp://host/srv" << int(E::None);
        QTest::newRow("empty") << "  " << "" << int(E::Empty);
        QTest::newRow("local") << "/etc" << "" << int(E::LocalPath);
        QTest::newRow("no colon") << "host" << "" << int(E::Syntax);
        QTest::newRow("relative") << "host:proj" << "" << int(E::RelativePath);
        QTest::newRow("gopher") << "gopher://host/share" << "" << int(E::Unsupported);
        QTest::newRow("smb") << "smb://host/share" << "smb://host/share" << int(E::None);
        QTest::newRow("smb domain user") << "smb://WORK;bob@nas/share" << "smb://WORK;bob@nas/share" << int(E::None);
        QTest::newRow("smb root") << "smb://nas" << "" << int(E::RemoteRoot);
        QTest::newRow("bad host") << "-oProxyCommand=x:/a" << "" << int(E::Unsupported);
        QTest::newRow("url root") << "sftp://host" << "" << int(E::RemoteRoot);
        QTest::newRow("url slash") << "sftp://host/" << "" << int(E::RemoteRoot);
        QTest::newRow("scp empty") << "host:" << "" << int(E::RemoteRoot);
        QTest::newRow("scp root") << "host:/" << "" << int(E::RemoteRoot);
        QTest::newRow("scp dotdot") << "host:/a/.." << "" << int(E::RemoteRoot);
        QTest::newRow("mount host dir") << QString(root + QStringLiteral("/sftp/host")) << "" << int(E::RemoteRoot);
    }

    void userInput()
    {
        QFETCH(QString, input);
        QFETCH(QString, url);
        QFETCH(int, error);
        InputError result;
        const auto parsed = urlForInput(input, QDir::homePath() + QStringLiteral("/.kate-remote"), &result);
        QCOMPARE(int(result), error);
        QCOMPARE(parsed.has_value(), url.length() > 0);
        if (parsed) {
            QCOMPARE(*parsed, QUrl(url));
        }
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
