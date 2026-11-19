#include "RemoteLocation.h"
#include "RemotePathMap.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
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

    void sshTerminal_data()
    {
        QTest::addColumn<QString>("scheme");
        QTest::addColumn<QString>("authority");
        QTest::addColumn<QString>("path");
        QTest::addColumn<QStringList>("expected"); // after "ssh -t", empty: refused

        const auto l = [](std::initializer_list<const char *> items) {
            QStringList list;
            for (const char *item : items) {
                list << QString::fromUtf8(item);
            }
            return list;
        };
        const char *cd = "cd '/home/u/proj'; exec \"$SHELL\" -l";
        QTest::newRow("sftp") << "sftp" << "host" << "/home/u/proj" << l({"--", "host", cd});
        QTest::newRow("fish") << "fish" << "host" << "/home/u/proj" << l({"--", "host", cd});
        QTest::newRow("user and port") << "sftp" << "alice@host:2222" << "/home/u/proj" << l({"-l", "alice", "-p", "2222", "--", "host", cd});
        QTest::newRow("ipv6") << "sftp" << "bob@[::1]:22" << "/home/u/proj" << l({"-l", "bob", "-p", "22", "--", "::1", cd});
        QTest::newRow("quoting") << "sftp" << "host" << "/a b/it's/$x`y`"
                                 << l({"--", "host", "cd '/a b/it'\\''s/$x`y`'; exec \"$SHELL\" -l"});
        QTest::newRow("smb") << "smb" << "host" << "/share" << QStringList{};
        QTest::newRow("fish domain user") << "fish" << "DOM;alice@host" << "/x" << QStringList{};
        QTest::newRow("option-like host") << "sftp" << "-oProxyCommand=x" << "/x" << QStringList{};
        QTest::newRow("relative path") << "sftp" << "host" << "x" << QStringList{};
    }

    void sshTerminal()
    {
        QFETCH(QString, scheme);
        QFETCH(QString, authority);
        QFETCH(QString, path);
        QFETCH(QStringList, expected);
        const QStringList ssh{QStringLiteral("ssh"), QStringLiteral("-F"), QStringLiteral("/cfg")};
        const QStringList command = sshTerminalCommand(Location{scheme, authority, path}, ssh);
        if (expected.isEmpty()) {
            QVERIFY(command.isEmpty());
        } else {
            QCOMPARE(command, ssh + QStringList{QStringLiteral("-t")} + expected);
        }
    }

    void windowsPaths_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("expected");
        QTest::newRow("folder") << "/C:/Users/me/proj" << "C:\\Users\\me\\proj";
        QTest::newRow("drive root") << "/d:" << "D:\\";
        QTest::newRow("posix") << "/home/me" << "";
        QTest::newRow("no drive") << "/C/Users" << "";
        QTest::newRow("long name") << "/CD:/x" << "";
    }

    void windowsPaths()
    {
        QFETCH(QString, path);
        QFETCH(QString, expected);
        QCOMPARE(windowsPathForSftpPath(path), expected);
    }

    void windowsTerminal_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("script"); // PowerShell, before encoding
        QTest::newRow("folder") << "/C:/Users/me/proj" << "Set-Location -LiteralPath 'C:\\Users\\me\\proj'";
        QTest::newRow("quote, dollar, non-ASCII") << "/E:/it's $x/\u00e9t\u00e9" << "Set-Location -LiteralPath 'E:\\it''s $x\\\u00e9t\u00e9'";
    }

    // Windows hosts: PowerShell in the folder, the script base64-encoded
    // (UTF-16LE) so that neither cmd nor PowerShell (the server's default
    // shell) changes it.
    void windowsTerminal()
    {
        QFETCH(QString, path);
        QFETCH(QString, script);
        const QStringList command = sshTerminalCommand(Location{QStringLiteral("sftp"), QStringLiteral("me@win"), path}, {QStringLiteral("ssh")});
        QCOMPARE(command.mid(0, 5), (QStringList{QStringLiteral("ssh"), QStringLiteral("-t"), QStringLiteral("-l"), QStringLiteral("me"), QStringLiteral("--")}));
        QCOMPARE(command.size(), 7);
        QCOMPARE(command.at(5), QStringLiteral("win"));
        const QString prefix = QStringLiteral("powershell -NoLogo -NoExit -EncodedCommand ");
        QVERIFY(command.at(6).startsWith(prefix));
        const QString encoded = command.at(6).mid(prefix.size());
        QVERIFY(QRegularExpression(QStringLiteral("^[A-Za-z0-9+/=]+$")).match(encoded).hasMatch());
        const QByteArray utf16 = QByteArray::fromBase64(encoded.toLatin1());
        QCOMPARE(utf16.size() % 2, 0);
        QString decoded;
        for (qsizetype i = 0; i < utf16.size(); i += 2) {
            decoded += QChar(char16_t(uchar(utf16.at(i)) | (uchar(utf16.at(i + 1)) << 8)));
        }
        QCOMPARE(decoded, script);
        if (path == QLatin1String("/C:/Users/me/proj")) {
            // Python: base64.b64encode(script.encode("utf-16-le"))
            QCOMPARE(encoded, QStringLiteral("UwBlAHQALQBMAG8AYwBhAHQAaQBvAG4AIAAtAEwAaQB0AGUAcgBhAGwAUABhAHQAaAAgACcAQwA6AFwAVQBzAGUAcgBzAFwAbQBlAFwAcAByAG8AagAnAA=="));
        }
    }

    // The remote command as the remote login shell would run it: it reaches
    // the directory (whatever its name) and execs $SHELL.
    void sshTerminalRemoteCommand()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString target = dir.path() + QStringLiteral("/a b/it's $HOME `x` \\n");
        QVERIFY(QDir().mkpath(target));
        const QString shell = dir.path() + QStringLiteral("/shell");
        {
            QFile script(shell);
            QVERIFY(script.open(QIODevice::WriteOnly));
            script.write("#!/bin/sh\necho \"$1\"; pwd\n");
            script.setPermissions(script.permissions() | QFile::ExeOwner);
        }
        const QString remote = sshTerminalCommand(Location{QStringLiteral("sftp"), QStringLiteral("h"), target}, {QStringLiteral("ssh")}).last();
        // ssh runs it as "<login shell> -c <command>".
        for (const QString &loginShell : {QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("zsh")}) {
            const QString program = QStandardPaths::findExecutable(loginShell);
            if (program.isEmpty()) {
                continue;
            }
            QProcess process;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("SHELL"), shell);
            process.setProcessEnvironment(env);
            process.start(program, {QStringLiteral("-c"), remote});
            QVERIFY(process.waitForFinished());
            QCOMPARE(QString::fromLocal8Bit(process.readAllStandardOutput()), QStringLiteral("-l\n") + target + QLatin1Char('\n'));
        }
    }

    void sshAgent_data()
    {
        QTest::addColumn<QString>("authority");
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("program");
        QTest::addColumn<QStringList>("args");
        QTest::addColumn<QStringList>("expected"); // after the ssh options, empty: refused

        const QString env = QStringLiteral("exec env PATH=\"$PATH:$HOME/.local/bin:$HOME/bin:$HOME/.cargo/bin:$HOME/.local/share/kate-code/bin\" ");
        const auto remote = [&](const QString &inner) {
            return QStringLiteral("exec \"$SHELL\" -l -c %1").arg(shellQuote(inner));
        };
        QTest::newRow("plain") << "host" << "/home/u/proj" << "claude-agent-acp" << QStringList{}
                               << QStringList{QStringLiteral("--"), QStringLiteral("host"), remote(QStringLiteral("cd '/home/u/proj' && ") + env + QStringLiteral("'claude-agent-acp'"))};
        QTest::newRow("user, port, args") << "alice@host:2222" << "/srv" << "vibe-acp" << QStringList{QStringLiteral("--x"), QStringLiteral("a b")}
                                          << QStringList{QStringLiteral("-l"), QStringLiteral("alice"), QStringLiteral("-p"), QStringLiteral("2222"), QStringLiteral("--"),
                                                         QStringLiteral("host"), remote(QStringLiteral("cd '/srv' && ") + env + QStringLiteral("'vibe-acp' '--x' 'a b'"))};
        QTest::newRow("local absolute program") << "host" << "/p" << "/home/me/.local/bin/claude-agent-acp" << QStringList{}
                                                << QStringList{QStringLiteral("--"), QStringLiteral("host"), remote(QStringLiteral("cd '/p' && ") + env + QStringLiteral("'claude-agent-acp'"))};
        QTest::newRow("fish domain user") << "DOM;alice@host" << "/x" << "a" << QStringList{} << QStringList{};
        QTest::newRow("option-like host") << "-oProxyCommand=x" << "/x" << "a" << QStringList{} << QStringList{};
    }

    void sshAgent()
    {
        QFETCH(QString, authority);
        QFETCH(QString, path);
        QFETCH(QString, program);
        QFETCH(QStringList, args);
        QFETCH(QStringList, expected);
        const QStringList ssh{QStringLiteral("ssh"), QStringLiteral("-F"), QStringLiteral("/cfg")};
        const QStringList command = sshAgentCommand(Location{QStringLiteral("sftp"), authority, path}, ssh, program, args);
        if (expected.isEmpty()) {
            QVERIFY(command.isEmpty());
            return;
        }
        const QStringList options{QStringLiteral("-T"),
                                  QStringLiteral("-o"),
                                  QStringLiteral("ServerAliveInterval=15"),
                                  QStringLiteral("-o"),
                                  QStringLiteral("ServerAliveCountMax=3"),
                                  QStringLiteral("-o"),
                                  QStringLiteral("ConnectTimeout=10")};
        QCOMPARE(command, ssh + options + expected);
        QVERIFY(sshAgentCommand(Location{QStringLiteral("smb"), authority, path}, ssh, program, args).isEmpty());
    }

    // The agent's remote command as each login shell runs it ("<shell> -c
    // <command>", $SHELL the same shell): the agent from ~/.local/bin, in
    // the directory, with its arguments; exit 127 without it.
    void sshAgentRemoteCommand()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString home = dir.path() + QStringLiteral("/home");
        const QString target = dir.path() + QStringLiteral("/a b/it's $HOME `x`");
        QVERIFY(QDir().mkpath(target));
        QVERIFY(QDir().mkpath(home + QStringLiteral("/.local/bin")));
        {
            QFile agent(home + QStringLiteral("/.local/bin/agent"));
            QVERIFY(agent.open(QIODevice::WriteOnly));
            agent.write("#!/bin/sh\nfor a in \"$@\"; do echo \"[$a]\"; done; pwd\n");
            agent.setPermissions(agent.permissions() | QFile::ExeOwner);
        }
        const QStringList args{QStringLiteral("--x"), QStringLiteral("$HOME 'q' \"d\"")};
        const QString remote = sshAgentCommand(Location{QStringLiteral("sftp"), QStringLiteral("h"), target}, {QStringLiteral("ssh")}, QStringLiteral("agent"), args).last();
        const QString missing =
            sshAgentCommand(Location{QStringLiteral("sftp"), QStringLiteral("h"), target}, {QStringLiteral("ssh")}, QStringLiteral("no-such-agent"), {}).last();
        int shells = 0;
        for (const QString &loginShell : {QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("zsh"), QStringLiteral("fish")}) {
            const QString program = QStandardPaths::findExecutable(loginShell);
            if (program.isEmpty()) {
                continue;
            }
            ++shells;
            QProcessEnvironment env;
            env.insert(QStringLiteral("SHELL"), program);
            env.insert(QStringLiteral("HOME"), home);
            env.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
            QProcess process;
            process.setProcessEnvironment(env);
            process.start(program, {QStringLiteral("-c"), remote});
            QVERIFY(process.waitForFinished());
            QCOMPARE(QString::fromLocal8Bit(process.readAllStandardOutput()),
                     QStringLiteral("[--x]\n[$HOME 'q' \"d\"]\n") + target + QLatin1Char('\n'));
            process.start(program, {QStringLiteral("-c"), missing});
            QVERIFY(process.waitForFinished());
            QCOMPARE(process.exitCode(), 127);
        }
        QVERIFY(shells > 0);
    }

    void remotePaths()
    {
        const QString hostDir = QStringLiteral("/home/me/.kate-remote/sftp/u@h:22");
        const RemotePathMap map(hostDir);
        QCOMPARE(map.toLocal(QStringLiteral("/home/u/a.cpp")), hostDir + QStringLiteral("/home/u/a.cpp"));
        QCOMPARE(map.toLocal(QStringLiteral("/")), hostDir);
        // Never above the host directory.
        QCOMPARE(map.toLocal(QStringLiteral("/..")), hostDir);
        QCOMPARE(map.toLocal(QStringLiteral("/../../../home/me/.ssh/id_ed25519")), hostDir + QStringLiteral("/home/me/.ssh/id_ed25519"));
        QCOMPARE(map.toLocal(QStringLiteral("/a/./b/../../../x")), hostDir + QStringLiteral("/x"));
        QCOMPARE(map.toLocal(QStringLiteral("//a//b/")), hostDir + QStringLiteral("/a/b"));
        QCOMPARE(map.toLocal(QStringLiteral("file:///../../etc/passwd")), QStringLiteral("file://") + hostDir + QStringLiteral("/etc/passwd"));
        QCOMPARE(map.toLocal(QStringLiteral("file:///a/%2E%2E/%2E%2E/x")), QStringLiteral("file://") + hostDir + QStringLiteral("/x"));
        QCOMPARE(map.toLocal(QStringLiteral("src/a.cpp")), QStringLiteral("src/a.cpp"));
        QCOMPARE(map.toLocal(QStringLiteral("file:///home/u/a%20b.cpp")), QStringLiteral("file://") + hostDir + QStringLiteral("/home/u/a b.cpp"));
        QCOMPARE(map.toLocal(QStringLiteral("https://example.org/x")), QStringLiteral("https://example.org/x"));

        QCOMPARE(map.toRemote(hostDir + QStringLiteral("/home/u/a.cpp")), QStringLiteral("/home/u/a.cpp"));
        QCOMPARE(map.toRemote(hostDir), QStringLiteral("/"));
        QCOMPARE(map.toRemote(QStringLiteral("see ") + hostDir + QStringLiteral("/x and ") + hostDir + QStringLiteral(".")), QStringLiteral("see /x and /."));
        QCOMPARE(map.toRemote(QStringLiteral("file://") + hostDir + QStringLiteral("/a")), QStringLiteral("file:///a"));
        // Other hosts and other places stay.
        QCOMPARE(map.toRemote(hostDir + QStringLiteral("2/x")), hostDir + QStringLiteral("2/x"));
        QCOMPARE(map.toRemote(QStringLiteral("/backup") + hostDir + QStringLiteral("/x")), QStringLiteral("/backup") + hostDir + QStringLiteral("/x"));
        QCOMPARE(map.toRemote(QStringLiteral("/home/me/other")), QStringLiteral("/home/me/other"));

        // A host directory with characters URIs encode.
        const RemotePathMap spaced(QStringLiteral("/home/my dir/r/sftp/h"));
        QCOMPARE(spaced.toRemote(QUrl::fromLocalFile(QStringLiteral("/home/my dir/r/sftp/h/a b")).toString(QUrl::FullyEncoded)), QStringLiteral("file:///a%20b"));
        QCOMPARE(spaced.toRemote(QUrl::fromLocalFile(QStringLiteral("/home/my dir/r/sftp/h/a b")).toString()), QStringLiteral("file:///a b"));

        const RemotePathMap identity;
        QVERIFY(identity.isIdentity());
        QCOMPARE(identity.toLocal(QStringLiteral("/x")), QStringLiteral("/x"));
        QCOMPARE(identity.toRemote(hostDir + QStringLiteral("/x")), hostDir + QStringLiteral("/x"));
    }

    void windowsRemotePaths()
    {
        const QString hostDir = QStringLiteral("/r/sftp/me@win");
        const RemotePathMap map(hostDir, RemotePathMap::Style::Windows, QLatin1Char('C'));
        QCOMPARE(map.toLocal(QStringLiteral("C:\\Users\\me\\proj\\a.cpp")), hostDir + QStringLiteral("/C:/Users/me/proj/a.cpp"));
        QCOMPARE(map.toLocal(QStringLiteral("c:/Users/me/proj/a.cpp")), hostDir + QStringLiteral("/C:/Users/me/proj/a.cpp"));
        QCOMPARE(map.toLocal(QStringLiteral("/C:/Users/me")), hostDir + QStringLiteral("/C:/Users/me"));
        QCOMPARE(map.toLocal(QStringLiteral("d:\\x")), hostDir + QStringLiteral("/D:/x"));
        QCOMPARE(map.toLocal(QStringLiteral("C:\\")), hostDir + QStringLiteral("/C:"));
        QCOMPARE(map.toLocal(QStringLiteral("C:\\a\\..\\..\\..\\x")), hostDir + QStringLiteral("/C:/x"));
        QCOMPARE(map.toLocal(QStringLiteral("file:///C:/Users/me/a%20b.cpp")), QStringLiteral("file://") + hostDir + QStringLiteral("/C:/Users/me/a b.cpp"));
        QCOMPARE(map.toLocal(QStringLiteral("src\\a.cpp")), QStringLiteral("src\\a.cpp"));
        QCOMPARE(map.toLocal(QStringLiteral("\\\\server\\share\\x")), QStringLiteral("\\\\server\\share\\x"));

        QCOMPARE(map.toRemote(hostDir + QStringLiteral("/C:/Users/me/my proj/a.cpp"), true), QStringLiteral("C:\\Users\\me\\my proj\\a.cpp"));
        // In text, a path ends at a space.
        QCOMPARE(map.toRemote(hostDir + QStringLiteral("/C:/a.cpp has a read/write bug")), QStringLiteral("C:\\a.cpp has a read/write bug"));
        // As JSON: path keys whole, other strings as text.
        const QJsonObject message = map.outgoing(QJsonObject{{QStringLiteral("cwd"), QString(hostDir + QStringLiteral("/C:/my proj"))},
                                                             {QStringLiteral("text"), QString(hostDir + QStringLiteral("/C:/my proj/a b"))}});
        QCOMPARE(message[QStringLiteral("cwd")].toString(), QStringLiteral("C:\\my proj"));
        QCOMPARE(message[QStringLiteral("text")].toString(), QStringLiteral("C:\\my proj/a b"));
        QCOMPARE(map.toRemote(hostDir + QStringLiteral("/C:"), true), QStringLiteral("C:\\"));
        QCOMPARE(map.toRemote(QStringLiteral("Look at ") + hostDir + QStringLiteral("/C:/Users/me/a.cpp, please")),
                 QStringLiteral("Look at C:\\Users\\me\\a.cpp, please"));
        QCOMPARE(map.toRemote(QStringLiteral("file://") + hostDir + QStringLiteral("/C:/Users/me/a.cpp")), QStringLiteral("file:///C:/Users/me/a.cpp"));
        // Not a drive: as on POSIX hosts.
        QCOMPARE(map.toRemote(hostDir + QStringLiteral("/x")), QStringLiteral("/x"));
    }

    void windowsAgent()
    {
        const QStringList ssh{QStringLiteral("ssh")};
        const QStringList command = sshAgentCommand(Location{QStringLiteral("sftp"), QStringLiteral("me@win"), QStringLiteral("/C:/Users/me/it's proj")}, ssh,
                                                    QStringLiteral("/usr/bin/claude-agent-acp"), {QStringLiteral("--x"), QStringLiteral("a \"b\" c\\")});
        QVERIFY(!command.isEmpty());
        QCOMPARE(command.mid(command.size() - 4, 3), (QStringList{QStringLiteral("me"), QStringLiteral("--"), QStringLiteral("win")}));
        const QString prefix = QStringLiteral("powershell -NoLogo -NoProfile -NonInteractive -EncodedCommand ");
        QVERIFY(command.last().startsWith(prefix));
        // Windows' cmd takes command lines up to 8191 characters.
        QVERIFY2(command.last().size() < 7000, qPrintable(QString::number(command.last().size())));
        const QByteArray utf16 = QByteArray::fromBase64(command.last().mid(prefix.size()).toLatin1());
        QString script;
        for (qsizetype i = 0; i + 1 < utf16.size(); i += 2) {
            script += QChar(char16_t(uchar(utf16.at(i)) | (uchar(utf16.at(i + 1)) << 8)));
        }
        QCOMPARE(script, windowsAgentScript(QStringLiteral("C:\\Users\\me\\it's proj"), QStringLiteral("claude-agent-acp"), {QStringLiteral("--x"), QStringLiteral("a \"b\" c\\")}));
        QVERIFY(script.contains(QStringLiteral("$d='C:\\Users\\me\\it''s proj'\n")));
        QVERIFY(script.contains(QStringLiteral("Get-Command -Name 'claude-agent-acp' ")));
        QVERIFY(script.contains(QStringLiteral("Arguments='--x \"a \\\"b\\\" c\\\\\"';")));
        // The checks come before the tunnel, the program after it.
        QVERIFY(script.indexOf(QStringLiteral("exit 127")) < script.indexOf(QLatin1String(TunnelLinePrefix)));
        QVERIFY(script.indexOf(QLatin1String(TunnelLinePrefix)) < script.indexOf(QStringLiteral("[Diagnostics.Process]::Start(")));
        QVERIFY(script.contains(QStringLiteral("CopyToAsync($n)")));
        QVERIFY(!script.contains(QStringLiteral("OpenStandardInput")));
        QVERIFY(usesTunnel(Location{QStringLiteral("sftp"), QStringLiteral("me@win"), QStringLiteral("/C:/x")}));
        QVERIFY(!usesTunnel(Location{QStringLiteral("sftp"), QStringLiteral("me@host"), QStringLiteral("/home/me")}));
        // fish has no Windows hosts.
        QVERIFY(sshAgentCommand(Location{QStringLiteral("fish"), QStringLiteral("win"), QStringLiteral("/C:/x")}, ssh, QStringLiteral("a"), {}).isEmpty());
    }

    void remoteAgentPackages_data()
    {
        QTest::addColumn<QString>("id");
        QTest::addColumn<QString>("version");
        QTest::addColumn<QString>("cmd");
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("valid");
        QTest::newRow("claude") << "claude-acp" << "0.106.0" << "./claude-agent-acp" << "claude-agent-acp" << true;
        QTest::newRow("windows subdir") << "goose" << "1.59.0" << "./goose-package\\goose.exe" << "goose" << true;
        QTest::newRow("subdir") << "a" << "2026.11.11" << "bin/a+b_c" << "a" << true;
        QTest::newRow("dotdot cmd") << "a" << "1" << "./../x" << "x" << false;
        QTest::newRow("dotdot inside") << "a" << "1" << "./d/../../x" << "x" << false;
        QTest::newRow("absolute cmd") << "a" << "1" << "/bin/sh" << "sh" << false;
        QTest::newRow("quote in version") << "a" << "1\"; rm -rf ~" << "./a" << "a" << false;
        QTest::newRow("dollar in id") << "$(x)" << "1" << "./a" << "a" << false;
        QTest::newRow("dot version") << "a" << ".." << "./a" << "a" << false;
        QTest::newRow("space in name") << "a" << "1" << "./a" << "a b" << false;
        QTest::newRow("dash name") << "a" << "1" << "./a" << "-a" << false;
        QTest::newRow("empty cmd") << "a" << "1" << "./" << "a" << false;
    }

    void remoteAgentPackages()
    {
        QFETCH(QString, id);
        QFETCH(QString, version);
        QFETCH(QString, cmd);
        QFETCH(QString, name);
        QFETCH(bool, valid);
        const RemoteAgentPackage package{id, version, cmd, name};
        QCOMPARE(isValidRemoteAgentPackage(package), valid);
        QCOMPARE(remoteAgentInstallScript(false, package).isEmpty(), !valid);
        QCOMPARE(remoteAgentInstallScript(true, package).isEmpty(), !valid);
        // The probe takes the name only when it is valid.
        const bool nameValid = QRegularExpression(QStringLiteral("^[A-Za-z0-9_][A-Za-z0-9._+-]*$")).match(name).hasMatch();
        QCOMPARE(remotePlatformScript(false, name).isEmpty(), !nameValid);
    }

    void remotePlatforms_data()
    {
        QTest::addColumn<QString>("output");
        QTest::addColumn<QString>("key");
        QTest::addColumn<bool>("installed");
        QTest::newRow("linux glibc") << "Linux\nx86_64\nldd: ldd (GNU libc) 2.42\nloader: \n" << "linux-x86_64" << false;
        QTest::newRow("linux arm64") << "Linux\naarch64\nldd: ldd (Ubuntu GLIBC 2.39-0ubuntu8) 2.39\nloader:\ninstalled\n" << "linux-aarch64" << true;
        QTest::newRow("glibc with musl installed") << "Linux\nx86_64\nldd: ldd (GNU libc) 2.42\nloader: /lib/ld-musl-x86_64.so.1\n" << "linux-x86_64" << false;
        QTest::newRow("alpine") << "Linux\nx86_64\nldd: musl libc (x86_64)\nloader: /lib/ld-musl-x86_64.so.1\n" << "linux-x86_64-musl" << false;
        QTest::newRow("musl without ldd") << "Linux\naarch64\nldd: sh: ldd: not found\nloader: /lib/ld-musl-aarch64.so.1\n" << "linux-aarch64-musl" << false;
        QTest::newRow("macos arm") << "Darwin\narm64\nldd: sh: ldd: command not found\nloader:\n" << "darwin-aarch64" << false;
        QTest::newRow("macos intel") << "Darwin\nx86_64\n" << "darwin-x86_64" << false;
        QTest::newRow("freebsd amd64") << "FreeBSD\namd64\n" << "" << false;
        QTest::newRow("windows") << "Windows AMD64\r\n" << "windows-x86_64" << false;
        QTest::newRow("windows arm") << "Windows ARM64\r\ninstalled\r\n" << "windows-aarch64" << true;
        QTest::newRow("windows x86") << "Windows x86\r\n" << "" << false;
        QTest::newRow("riscv") << "Linux\nriscv64\n" << "" << false;
        QTest::newRow("empty") << "" << "" << false;
    }

    void remotePlatforms()
    {
        QFETCH(QString, output);
        QFETCH(QString, key);
        QFETCH(bool, installed);
        const auto platform = parseRemotePlatform(output);
        QCOMPARE(platform ? platform->key : QString(), key);
        if (platform) {
            QCOMPARE(platform->installed, installed);
        }
    }

    // The install script as each login shell runs it (sshScriptCommand()'s
    // remote command), with a .tar.gz on stdin: unpacked, the launcher
    // runs the program; a reinstall replaces it; an archive without the
    // program fails and changes nothing.
    void remoteAgentInstall()
    {
        const QString tar = QStandardPaths::findExecutable(QStringLiteral("tar"));
        if (tar.isEmpty()) {
            QSKIP("tar not found");
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString home = dir.path() + QStringLiteral("/my home");
        QVERIFY(QDir().mkpath(home));
        const auto makeArchive = [&](const QString &name, const QByteArray &output) {
            const QString src = dir.path() + QStringLiteral("/src-") + name;
            QDir().mkpath(src + QStringLiteral("/sub"));
            QFile program(src + QStringLiteral("/sub/agent-bin"));
            if (!program.open(QIODevice::WriteOnly)) {
                return QString();
            }
            program.write("#!/bin/sh\necho " + output + "\n");
            program.close();
            // Not executable in the archive: the script sets it.
            program.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
            const QString archive = dir.path() + QLatin1Char('/') + name + QStringLiteral(".tar.gz");
            QProcess::execute(tar, {QStringLiteral("-czf"), archive, QStringLiteral("-C"), src, QStringLiteral(".")});
            return archive;
        };
        const QString v1 = makeArchive(QStringLiteral("v1"), "one");
        const QString v2 = makeArchive(QStringLiteral("v2"), "two");
        const RemoteAgentPackage package{QStringLiteral("my-agent"), QStringLiteral("1.0"), QStringLiteral("./sub/agent-bin"), QStringLiteral("agent")};
        const QString folder = dir.path() + QStringLiteral("/project");
        QVERIFY(QDir().mkpath(folder));
        const Location location{QStringLiteral("sftp"), QStringLiteral("h"), folder};
        const QString install = sshScriptCommand(location, {QStringLiteral("ssh")}, remoteAgentInstallScript(false, package)).last();
        RemoteAgentPackage wrong = package;
        wrong.cmd = QStringLiteral("./other");
        const QString installWrong = sshScriptCommand(location, {QStringLiteral("ssh")}, remoteAgentInstallScript(false, wrong)).last();
        const QString probe = sshScriptCommand(location, {QStringLiteral("ssh")}, remotePlatformScript(false, QStringLiteral("agent"))).last();
        // As sshAgentCommand() finds it.
        const QString run = sshAgentCommand(location, {QStringLiteral("ssh")}, QStringLiteral("agent"), {}).last();
        const auto shellRun = [&](const QString &shell, const QString &command, const QString &stdinFile, QByteArray *out) {
            QProcessEnvironment env;
            env.insert(QStringLiteral("SHELL"), shell);
            env.insert(QStringLiteral("HOME"), home);
            env.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
            QProcess process;
            process.setProcessEnvironment(env);
            process.setStandardInputFile(stdinFile.isEmpty() ? QProcess::nullDevice() : stdinFile);
            process.start(shell, {QStringLiteral("-c"), command});
            process.waitForFinished();
            *out = process.readAllStandardOutput();
            return process.exitCode();
        };
        int shells = 0;
        for (const QString &loginShell : {QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("zsh"), QStringLiteral("fish")}) {
            const QString shell = QStandardPaths::findExecutable(loginShell);
            if (shell.isEmpty()) {
                continue;
            }
            ++shells;
            QDir(home + QStringLiteral("/.local")).removeRecursively();
            QByteArray out;
            QCOMPARE(shellRun(shell, probe, {}, &out), 0);
            QVERIFY2(parseRemotePlatform(QString::fromLocal8Bit(out)).has_value(), out.constData());
            QVERIFY(!parseRemotePlatform(QString::fromLocal8Bit(out))->installed);
            QCOMPARE(shellRun(shell, install, v1, &out), 0);
            QVERIFY(out.contains("installed"));
            QCOMPARE(shellRun(shell, probe, {}, &out), 0);
            QVERIFY(parseRemotePlatform(QString::fromLocal8Bit(out))->installed);
            // The agent's own command finds it (sshAgentCommand() runs
            // "$SHELL" -l: a login shell, as sshd would).
            QCOMPARE(shellRun(shell, run, {}, &out), 0);
            QCOMPARE(out, QByteArray("one\n"));
            QCOMPARE(shellRun(shell, install, v2, &out), 0);
            QCOMPARE(shellRun(shell, run, {}, &out), 0);
            QCOMPARE(out, QByteArray("two\n"));
            QCOMPARE(shellRun(shell, installWrong, v1, &out), 3);
            QCOMPARE(shellRun(shell, run, {}, &out), 0);
            QCOMPARE(out, QByteArray("two\n"));
            // No temporary directories left.
            const QStringList left = QDir(home + QStringLiteral("/.local/share/kate-code")).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
            QCOMPARE(left, (QStringList{QStringLiteral("agents"), QStringLiteral("bin")}));
        }
        QVERIFY(shells > 0);
    }

    void npmPackageSpecs()
    {
        QCOMPARE(parseNpmPackageSpec(QStringLiteral("@google/gemini-cli@0.68.0")), std::make_optional(std::make_pair(QStringLiteral("@google/gemini-cli"), QStringLiteral("0.68.0"))));
        QCOMPARE(parseNpmPackageSpec(QStringLiteral("pi-acp@0.0.34")), std::make_optional(std::make_pair(QStringLiteral("pi-acp"), QStringLiteral("0.0.34"))));
        QCOMPARE(parseNpmPackageSpec(QStringLiteral("a@1.0.0-beta.2+b")), std::make_optional(std::make_pair(QStringLiteral("a"), QStringLiteral("1.0.0-beta.2+b"))));
        for (const char *bad : {"pi-acp", "pi-acp@latest", "pi-acp@^1.0.0", "pi-acp@1.0", "@scope@1.0.0", "Upper@1.0.0", "-x@1.0.0",
                                "a/b@1.0.0", "git+https://x/y@1.0.0", "a@1.0.0 && rm", "a@1.0.0\"", "../a@1.0.0", "@s/../a@1.0.0"}) {
            QVERIFY2(!parseNpmPackageSpec(QString::fromLatin1(bad)), bad);
        }
        const NpmAgentPackage good{QStringLiteral("gemini"), QStringLiteral("0.68.0"), QStringLiteral("@google/gemini-cli"), QStringLiteral("0.68.0"), QStringLiteral("gemini")};
        QVERIFY(isValidNpmAgentPackage(good));
        QVERIFY(!npmAgentInstallScript(good).contains(QLatin1Char('\'')));
        QVERIFY(!npmAgentInstallScript(good).contains(QLatin1Char('\\')));
        NpmAgentPackage bad = good;
        bad.name = QStringLiteral("a b");
        QVERIFY(!isValidNpmAgentPackage(bad));
        QVERIFY(npmAgentInstallScript(bad).isEmpty());
        bad = good;
        bad.package = QStringLiteral("@google/gemini-cli\"");
        QVERIFY(!isValidNpmAgentPackage(bad));
    }

    void npmAgentInstall()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString home = dir.path() + QStringLiteral("/my home");
        const QString bin = dir.path() + QStringLiteral("/fake-bin");
        QVERIFY(QDir().mkpath(home));
        QVERIFY(QDir().mkpath(bin));
        // npm as far as the script uses it: install --prefix <dir> ...
        // <package>@<version> puts the package and its program (a relative
        // link in node_modules/.bin, as npm makes it) there.
        QFile npm(bin + QStringLiteral("/npm"));
        QVERIFY(npm.open(QIODevice::WriteOnly));
        npm.write("#!/bin/sh\n"
                  "test \"$1\" = install || exit 9\n"
                  "test \"$2\" = --prefix || exit 9\n"
                  "prefix=$3; for a in \"$@\"; do spec=$a; done\n"
                  "test -n \"$FAKE_NPM_FAIL\" && { echo \"npm ERR! 404\" >&2; exit 1; }\n"
                  "pkg=${spec%@*}; ver=${spec##*@}\n"
                  "echo \"added 1 package\"\n"
                  "mkdir -p \"$prefix/node_modules/$pkg\" \"$prefix/node_modules/.bin\"\n"
                  "printf \"#!/bin/sh\\necho $pkg $ver\\n\" > \"$prefix/node_modules/$pkg/cli.js\"\n"
                  "chmod +x \"$prefix/node_modules/$pkg/cli.js\"\n"
                  "ln -s \"../$pkg/cli.js\" \"$prefix/node_modules/.bin/${FAKE_NPM_BIN:-gemini}\"\n");
        npm.close();
        npm.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        const auto run = [&](const QString &script, QByteArray *out, const QString &fail = QString()) {
            QProcessEnvironment env;
            env.insert(QStringLiteral("HOME"), home);
            env.insert(QStringLiteral("PATH"), bin + QStringLiteral(":/usr/bin:/bin"));
            if (!fail.isEmpty()) {
                env.insert(QStringLiteral("FAKE_NPM_FAIL"), fail);
            }
            QProcess process;
            process.setProcessEnvironment(env);
            process.setStandardInputFile(QProcess::nullDevice());
            // As AgentInstaller runs it on this computer.
            process.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), script});
            process.waitForFinished();
            *out = process.readAllStandardOutput();
            return process.exitCode();
        };
        const QString base = home + QStringLiteral("/.local/share/kate-code");
        NpmAgentPackage package{QStringLiteral("gemini"), QStringLiteral("0.68.0"), QStringLiteral("@google/gemini-cli"), QStringLiteral("0.68.0"), QStringLiteral("gemini")};
        QByteArray out;
        QCOMPARE(run(npmAgentInstallScript(package), &out), 0);
        // npm's own output is not on stdout.
        QCOMPARE(out, QByteArray("installed\n"));
        QCOMPARE(QFile::symLinkTarget(base + QStringLiteral("/bin/gemini")).isEmpty(), false);
        QCOMPARE(QFileInfo(base + QStringLiteral("/bin/gemini")).symLinkTarget(),
                 base + QStringLiteral("/agents/gemini/0.68.0/node_modules/.bin/gemini"));
        QProcess program;
        program.start(base + QStringLiteral("/bin/gemini"), {});
        QVERIFY(program.waitForFinished());
        QCOMPARE(program.readAllStandardOutput(), QByteArray("@google/gemini-cli 0.68.0\n"));

        // A newer version replaces it; a package without the program and a
        // failing npm leave the installed one.
        package.version = package.packageVersion = QStringLiteral("0.69.0");
        QCOMPARE(run(npmAgentInstallScript(package), &out), 0);
        program.start(base + QStringLiteral("/bin/gemini"), {});
        QVERIFY(program.waitForFinished());
        QCOMPARE(program.readAllStandardOutput(), QByteArray("@google/gemini-cli 0.69.0\n"));
        NpmAgentPackage other = package;
        other.version = other.packageVersion = QStringLiteral("0.70.0");
        other.name = QStringLiteral("gemini-cli");
        QCOMPARE(run(npmAgentInstallScript(other), &out), 3);
        QCOMPARE(run(npmAgentInstallScript(other), &out, QStringLiteral("1")), 1);
        QVERIFY(!QFileInfo::exists(base + QStringLiteral("/bin/gemini-cli")));
        QVERIFY(!QFileInfo::exists(base + QStringLiteral("/agents/gemini/0.70.0")));
        program.start(base + QStringLiteral("/bin/gemini"), {});
        QVERIFY(program.waitForFinished());
        QCOMPARE(program.readAllStandardOutput(), QByteArray("@google/gemini-cli 0.69.0\n"));
        // No temporary directories left.
        QCOMPARE(QDir(base).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), (QStringList{QStringLiteral("agents"), QStringLiteral("bin")}));
    }

    void windowsAgentInstall()
    {
        const RemoteAgentPackage package{QStringLiteral("goose"), QStringLiteral("1.59.0"), QStringLiteral("./goose-package\\goose.exe"), QStringLiteral("goose")};
        const QStringList command = sshScriptCommand(Location{QStringLiteral("sftp"), QStringLiteral("me@win"), QStringLiteral("/C:/Users/me/proj")},
                                                     {QStringLiteral("ssh")},
                                                     remoteAgentInstallScript(true, package));
        QVERIFY(!command.isEmpty());
        const QString prefix = QStringLiteral("powershell -NoLogo -NoProfile -NonInteractive -EncodedCommand ");
        QVERIFY(command.last().startsWith(prefix));
        QVERIFY2(command.last().size() < 7000, qPrintable(QString::number(command.last().size())));
        const QByteArray utf16 = QByteArray::fromBase64(command.last().mid(prefix.size()).toLatin1());
        QString script;
        for (qsizetype i = 0; i + 1 < utf16.size(); i += 2) {
            script += QChar(char16_t(uchar(utf16.at(i)) | (uchar(utf16.at(i + 1)) << 8)));
        }
        QCOMPARE(script, remoteAgentInstallScript(true, package));
        QVERIFY(script.contains(QStringLiteral("(Join-Path $x 'goose-package\\goose.exe')")));
        QVERIFY(script.contains(QStringLiteral("-Value '@\"%~dp0..\\agents\\goose\\1.59.0\\goose-package\\goose.exe\" %*'")));
        QVERIFY(script.contains(QStringLiteral("(Join-Path $n 'goose.cmd')")));
        QVERIFY(script.startsWith(QStringLiteral("$l=New-Object Net.Sockets.TcpListener")));
        QVERIFY(script.contains(QStringLiteral("$n.CopyTo($f)")));
        QVERIFY(!script.contains(QStringLiteral("OpenStandardInput")));
        // The agent's script looks there last.
        QVERIFY(windowsAgentScript(QStringLiteral("C:\\x"), QStringLiteral("goose"), {}).contains(QStringLiteral(";$env:LOCALAPPDATA\\kate-code\\bin\"")));
        const QString probe = remotePlatformScript(true, QStringLiteral("goose"));
        QVERIFY(probe.contains(QStringLiteral("'kate-code\\bin\\goose.cmd'")));
    }

    // The pin file (pin-release.py's output): validated before anything of
    // it reaches a script or a download.
    void remoteHelperPin()
    {
        const auto entry = [](const QString &archive, const QString &program) {
            return QJsonObject{{QStringLiteral("archive"), archive},
                               {QStringLiteral("sha256"), QString(64, QLatin1Char('a'))},
                               {QStringLiteral("size"), 100},
                               {QStringLiteral("program"), program},
                               {QStringLiteral("programSha256"), QString(64, QLatin1Char('b'))},
                               {QStringLiteral("programSize"), 50}};
        };
        QJsonObject pin{{QStringLiteral("name"), QStringLiteral("kate-code-helper")},
                        {QStringLiteral("version"), QStringLiteral("0.1.0")},
                        {QStringLiteral("protocol"), 1},
                        {QStringLiteral("baseUrl"), QStringLiteral("https://github.com/o/r/releases/download/remote-helper-v0.1.0/")},
                        {QStringLiteral("platforms"),
                         QJsonObject{{QStringLiteral("linux-x86_64"), entry(QStringLiteral("kate-code-helper-linux-x86_64.tar.gz"), QStringLiteral("kate-code-helper"))},
                                     {QStringLiteral("linux-x86_64-musl"), entry(QStringLiteral("kate-code-helper-linux-x86_64.tar.gz"), QStringLiteral("kate-code-helper"))},
                                     {QStringLiteral("windows-x86_64"),
                                      entry(QStringLiteral("kate-code-helper-windows-x86_64.tar.gz"), QStringLiteral("kate-code-helper.exe"))}}}};
        const auto parse = [](const QJsonObject &o, bool allowFile = false) {
            return parseRemoteHelperPin(QJsonDocument(o).toJson(), allowFile);
        };
        const auto parsed = parse(pin);
        QVERIFY(parsed);
        QCOMPARE(parsed->version, QStringLiteral("0.1.0"));
        QCOMPARE(parsed->builds.size(), 2); // the -musl alias dropped
        QVERIFY(parsed->build(QStringLiteral("linux-x86_64")));
        QCOMPARE(parsed->archiveUrl(*parsed->build(QStringLiteral("linux-x86_64"))).toString(),
                 QStringLiteral("https://github.com/o/r/releases/download/remote-helper-v0.1.0/kate-code-helper-linux-x86_64.tar.gz"));
        QVERIFY(!remoteHelperLaunchScript(false, *parsed).isEmpty());
        QVERIFY(!remoteHelperLaunchScript(true, *parsed).isEmpty());

        // The placeholder (no release pinned yet), and invalid fields.
        QVERIFY(!parseRemoteHelperPin(QByteArray()));
        QVERIFY(!parse(QJsonObject{{QStringLiteral("name"), QStringLiteral("kate-code-helper")}, {QStringLiteral("version"), QJsonValue()}}));
        const auto with = [&](const QString &key, const QJsonValue &value) {
            QJsonObject o = pin;
            o[key] = value;
            return o;
        };
        const auto withEntry = [&](const QString &key, const QJsonValue &value) {
            QJsonObject platforms = pin[QStringLiteral("platforms")].toObject();
            QJsonObject e = platforms[QStringLiteral("linux-x86_64")].toObject();
            e[key] = value;
            platforms[QStringLiteral("linux-x86_64")] = e;
            return with(QStringLiteral("platforms"), platforms);
        };
        QVERIFY(!parse(with(QStringLiteral("version"), QStringLiteral("0.1.0; rm -rf ~"))));
        QVERIFY(!parse(with(QStringLiteral("protocol"), 0)));
        QVERIFY(!parse(with(QStringLiteral("baseUrl"), QStringLiteral("http://example.org/x/"))));
        QVERIFY(!parse(with(QStringLiteral("baseUrl"), QStringLiteral("https://example.org/x"))));
        QVERIFY(!parse(with(QStringLiteral("baseUrl"), QStringLiteral("file:///tmp/x/"))));
        QVERIFY(parse(with(QStringLiteral("baseUrl"), QStringLiteral("file:///tmp/x/")), true));
        QVERIFY(!parse(withEntry(QStringLiteral("sha256"), QStringLiteral("ABC"))));
        QVERIFY(!parse(withEntry(QStringLiteral("programSha256"), QString(64, QLatin1Char('A')))));
        QVERIFY(!parse(withEntry(QStringLiteral("archive"), QStringLiteral("../evil.tar.gz"))));
        QVERIFY(!parse(withEntry(QStringLiteral("program"), QStringLiteral("sh"))));
        QVERIFY(!parse(withEntry(QStringLiteral("programSize"), 0)));
        QJsonObject badPlatform = pin;
        badPlatform[QStringLiteral("platforms")] = QJsonObject{{QStringLiteral("linux-x86_64;id"), entry(QStringLiteral("kate-code-helper-linux-x86_64.tar.gz"), QStringLiteral("kate-code-helper"))}};
        QVERIFY(!parse(badPlatform));
        // Kate Code's own pin parses, or is the placeholder.
        QFile own(QStringLiteral(KATECODE_SOURCE_DIR "/src/acp/remote-helper-pin.json"));
        QVERIFY(own.open(QIODevice::ReadOnly));
        const QJsonObject ownPin = QJsonDocument::fromJson(own.readAll()).object();
        QCOMPARE(ownPin[QStringLiteral("name")].toString(), QStringLiteral("kate-code-helper"));
        QCOMPARE(parse(ownPin).has_value(), !ownPin[QStringLiteral("platforms")].toObject().isEmpty());
    }

    // The helper's launch and install scripts, run as ssh would run them
    // (the login shell gets sshHelperCommand()'s remote command): a missing
    // copy asks for an install (exit 97), the installed one is exec'd with
    // stdin/stdout passed on, a tampered one is reinstalled; nothing else
    // is left in the helper's directory.
    void remoteHelperLaunch()
    {
        const QString tar = QStandardPaths::findExecutable(QStringLiteral("tar"));
        if (tar.isEmpty()) {
            QSKIP("tar not found");
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString home = dir.path() + QStringLiteral("/my home");
        QVERIFY(QDir().mkpath(home));
        // A stand-in helper: echoes its input.
        const QByteArray program = "#!/bin/sh\necho hello\ncat\n";
        const QString src = dir.path() + QStringLiteral("/src");
        QVERIFY(QDir().mkpath(src));
        QFile file(src + QStringLiteral("/kate-code-helper"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(program);
        file.close();
        const QString archive = dir.path() + QStringLiteral("/helper.tar.gz");
        QCOMPARE(QProcess::execute(tar, {QStringLiteral("-czf"), archive, QStringLiteral("-C"), src, QStringLiteral("kate-code-helper")}), 0);
        const QString emptyArchive = dir.path() + QStringLiteral("/empty.tar.gz");
        QCOMPARE(QProcess::execute(tar, {QStringLiteral("-czf"), emptyArchive, QStringLiteral("-C"), dir.path(), QStringLiteral("my home")}), 0);

        // Every POSIX platform the script knows points at the stand-in, so
        // the test runs on any host.
        RemoteHelperPin pin;
        pin.version = QStringLiteral("0.1.0");
        pin.protocol = 1;
        const QString digest = QString::fromLatin1(QCryptographicHash::hash(program, QCryptographicHash::Sha256).toHex());
        for (const QString &platform : {QStringLiteral("linux-x86_64"), QStringLiteral("linux-aarch64"), QStringLiteral("linux-x86"), QStringLiteral("linux-arm"),
                                        QStringLiteral("darwin-x86_64"), QStringLiteral("darwin-aarch64")}) {
            pin.builds << RemoteHelperBuild{platform, QString(), QString(), 0, QStringLiteral("kate-code-helper"), digest, program.size()};
        }
        const Location location{QStringLiteral("sftp"), QStringLiteral("h"), QStringLiteral("/home/me/proj")};
        const QStringList launchCommand = sshHelperCommand(location, {QStringLiteral("ssh")}, pin);
        QVERIFY(launchCommand.contains(QStringLiteral("ServerAliveInterval=15")));
        const QString launch = launchCommand.last();
        QVERIFY(!launch.mid(18, launch.size() - 19).contains(QLatin1Char('\''))); // inside "exec /bin/sh -c '...'"

        const auto shellRun = [&](const QString &shell, const QString &command, const QString &stdinFile, QByteArray *out) {
            QProcessEnvironment env;
            env.insert(QStringLiteral("SHELL"), shell);
            env.insert(QStringLiteral("HOME"), home);
            env.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
            QProcess process;
            process.setProcessEnvironment(env);
            process.setStandardInputFile(stdinFile.isEmpty() ? QProcess::nullDevice() : stdinFile);
            process.start(shell, {QStringLiteral("-c"), command});
            process.waitForFinished();
            *out = process.readAllStandardOutput();
            return process.exitCode();
        };
        const QString input = dir.path() + QStringLiteral("/input");
        QFile inputFile(input);
        QVERIFY(inputFile.open(QIODevice::WriteOnly));
        inputFile.write("request\n");
        inputFile.close();

        int shells = 0;
        for (const QString &loginShell : {QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("zsh"), QStringLiteral("fish")}) {
            const QString shell = QStandardPaths::findExecutable(loginShell);
            if (shell.isEmpty()) {
                continue;
            }
            ++shells;
            QDir(home + QStringLiteral("/.local")).removeRecursively();
            QByteArray out;
            QCOMPARE(shellRun(shell, launch, {}, &out), RemoteHelperInstallNeeded);
            const QRegularExpression installLine(QStringLiteral("^kate-code-helper: install (linux|darwin)-[a-z0-9_]+\\n$"));
            QVERIFY2(installLine.match(QString::fromLatin1(out)).hasMatch(), out.constData());
            const QString platform = QString::fromLatin1(out).trimmed().section(QLatin1Char(' '), 2);
            const RemoteHelperBuild *build = pin.build(platform);
            QVERIFY(build);
            const QString install = sshScriptCommand(location, {QStringLiteral("ssh")}, remoteHelperInstallScript(false, pin.version, *build)).last();
            QCOMPARE(shellRun(shell, install, emptyArchive, &out), 3);
            QCOMPARE(shellRun(shell, launch, {}, &out), RemoteHelperInstallNeeded);
            QCOMPARE(shellRun(shell, install, archive, &out), 0);
            QCOMPARE(out, QByteArray("installed\n"));
            const QString installed = home + QStringLiteral("/.local/share/kate-code/helper/0.1.0/") + platform + QStringLiteral("/kate-code-helper");
            QVERIFY(QFileInfo(installed).isExecutable());
            // Run, with stdin and stdout passed through.
            QCOMPARE(shellRun(shell, launch, input, &out), 0);
            QCOMPARE(out, QByteArray("hello\nrequest\n"));
            // Same size, other contents: reinstalled, unless the host has no
            // SHA-256 tool (here it has).
            QFile tampered(installed);
            QVERIFY(tampered.open(QIODevice::ReadWrite));
            tampered.write("#!/bin/sh\necho owned\n#");
            tampered.close();
            QCOMPARE(QFileInfo(installed).size(), qint64(program.size()));
            QCOMPARE(shellRun(shell, launch, input, &out), RemoteHelperInstallNeeded);
            QCOMPARE(shellRun(shell, install, archive, &out), 0);
            QCOMPARE(shellRun(shell, launch, input, &out), 0);
            QCOMPARE(out, QByteArray("hello\nrequest\n"));
            const QStringList left = QDir(home + QStringLiteral("/.local/share/kate-code/helper/0.1.0/") + platform)
                                         .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
            QCOMPARE(left, QStringList{QStringLiteral("kate-code-helper")});
        }
        QVERIFY(shells > 0);

        // A platform not pinned: unsupported, not an install.
        RemoteHelperPin windowsOnly = pin;
        windowsOnly.builds = {RemoteHelperBuild{QStringLiteral("windows-x86_64"), QString(), QString(), 0, QStringLiteral("kate-code-helper.exe"), digest, 10}};
        QByteArray out;
        QCOMPARE(shellRun(QStringLiteral("/bin/sh"), sshHelperCommand(location, {QStringLiteral("ssh")}, windowsOnly).last(), {}, &out), RemoteHelperUnsupported);
        QVERIFY(out.startsWith("kate-code-helper: unsupported "));
        // Invalid input makes no script.
        QVERIFY(remoteHelperInstallScript(false, QStringLiteral("1.0\"; id"), pin.builds.first()).isEmpty());
        QVERIFY(remoteHelperInstallScript(true, pin.version, pin.builds.first()).isEmpty()); // a POSIX build on Windows
        RemoteHelperPin badPin = pin;
        badPin.builds.first().platform = QStringLiteral("linux-$(id)");
        QVERIFY(remoteHelperLaunchScript(false, badPin).isEmpty());
    }

    void windowsHelperScripts()
    {
        RemoteHelperPin pin;
        pin.version = QStringLiteral("0.1.0");
        pin.protocol = 1;
        pin.builds << RemoteHelperBuild{QStringLiteral("windows-x86_64"), QString(), QString(), 0, QStringLiteral("kate-code-helper.exe"), QString(64, QLatin1Char('c')), 1234}
                   << RemoteHelperBuild{QStringLiteral("linux-x86_64"), QString(), QString(), 0, QStringLiteral("kate-code-helper"), QString(64, QLatin1Char('d')), 99};
        const QStringList command = sshHelperCommand(Location{QStringLiteral("sftp"), QStringLiteral("me@win"), QStringLiteral("/C:/Users/me/proj")}, {QStringLiteral("ssh")}, pin);
        const QString prefix = QStringLiteral("powershell -NoLogo -NoProfile -NonInteractive -EncodedCommand ");
        QVERIFY(command.last().startsWith(prefix));
        QVERIFY2(command.last().size() < 7000, qPrintable(QString::number(command.last().size())));
        const QByteArray utf16 = QByteArray::fromBase64(command.last().mid(prefix.size()).toLatin1());
        QString script;
        for (qsizetype i = 0; i + 1 < utf16.size(); i += 2) {
            script += QChar(char16_t(uchar(utf16.at(i)) | (uchar(utf16.at(i + 1)) << 8)));
        }
        QCOMPARE(script, remoteHelperLaunchScript(true, pin));
        QVERIFY(script.contains(QStringLiteral("$t=@{'windows-x86_64'=@('%1',1234)}").arg(QString(64, QLatin1Char('c')))));
        QVERIFY(script.contains(QStringLiteral("('kate-code\\helper\\0.1.0\\'+$q+'\\kate-code-helper.exe')")));
        QVERIFY(script.contains(QStringLiteral("FileName=$f;")));
        QVERIFY(script.indexOf(QStringLiteral("exit %1").arg(RemoteHelperInstallNeeded)) < script.indexOf(QLatin1String(TunnelLinePrefix)));
        // Two Windows builds still fit.
        RemoteHelperPin two = pin;
        two.builds << RemoteHelperBuild{QStringLiteral("windows-aarch64"), QString(), QString(), 0, QStringLiteral("kate-code-helper.exe"), QString(64, QLatin1Char('e')), 123456789};
        const QStringList twoCommand = sshHelperCommand(Location{QStringLiteral("sftp"), QStringLiteral("me@win"), QStringLiteral("/C:/Users/me/proj")}, {QStringLiteral("ssh")}, two);
        QVERIFY2(twoCommand.last().size() < 7000, qPrintable(QString::number(twoCommand.last().size())));
        QVERIFY(!script.contains(QStringLiteral("linux-x86_64")));
        const QString install = remoteHelperInstallScript(true, pin.version, pin.builds.first());
        QVERIFY(install.contains(QStringLiteral("'kate-code\\helper\\0.1.0\\windows-x86_64'")));
        QVERIFY(install.contains(QStringLiteral("Move-Item -LiteralPath $s -Destination (Join-Path $d 'kate-code-helper.exe') -Force")));
        QVERIFY(install.contains(QStringLiteral("$n.CopyTo($f)")));
        QVERIFY(!install.contains(QStringLiteral("OpenStandardInput")));
    }

    void tunnel()
    {
        const auto endpoint = parseTunnelLine("KATECODE-TUNNEL 50123 0123456789abcdef0123456789abcdef\r");
        QVERIFY(endpoint);
        QCOMPARE(endpoint->port, quint16(50123));
        QCOMPARE(endpoint->token, QByteArray("0123456789abcdef0123456789abcdef"));
        QVERIFY(parseTunnelLine("KATECODE-TUNNEL 50123 0123456789abcdef0123456789abcdef"));
        QVERIFY(!parseTunnelLine("KATECODE-TUNNEL 0 0123456789abcdef0123456789abcdef"));
        QVERIFY(!parseTunnelLine("KATECODE-TUNNEL 70000 0123456789abcdef0123456789abcdef"));
        QVERIFY(!parseTunnelLine("KATECODE-TUNNEL 50123 0123456789abcdef"));
        QVERIFY(!parseTunnelLine("KATECODE-TUNNEL 50123 0123456789ABCDEF0123456789abcdef"));
        QVERIFY(!parseTunnelLine("x KATECODE-TUNNEL 50123 0123456789abcdef0123456789abcdef"));
        QVERIFY(!parseTunnelLine(""));

        const Location win{QStringLiteral("sftp"), QStringLiteral("me@win:2222"), QStringLiteral("/C:/Users/me")};
        QCOMPARE(sshTunnelCommand(win, {QStringLiteral("ssh"), QStringLiteral("-F"), QStringLiteral("cfg")}, QStringLiteral("/run/user/1/kc"), 50123),
                 (QStringList{QStringLiteral("ssh"), QStringLiteral("-F"), QStringLiteral("cfg"),
                              QStringLiteral("-o"), QStringLiteral("ControlMaster=no"), QStringLiteral("-o"), QStringLiteral("ControlPath=/run/user/1/kc"),
                              QStringLiteral("-o"), QStringLiteral("ServerAliveInterval=15"), QStringLiteral("-o"), QStringLiteral("ServerAliveCountMax=3"),
                              QStringLiteral("-o"), QStringLiteral("ConnectTimeout=10"), QStringLiteral("-W"), QStringLiteral("127.0.0.1:50123"),
                              QStringLiteral("-l"), QStringLiteral("me"), QStringLiteral("-p"), QStringLiteral("2222"), QStringLiteral("--"), QStringLiteral("win")}));
        QVERIFY(!sshTunnelCommand(win, {QStringLiteral("ssh")}, QString(), 1).contains(QStringLiteral("ControlMaster=no")));
        QVERIFY(sshTunnelCommand(win, {QStringLiteral("ssh")}, QString(), 0).isEmpty());
        QVERIFY(sshTunnelCommand(Location{QStringLiteral("sftp"), QStringLiteral("-oProxyCommand=x"), QStringLiteral("/C:/")}, {QStringLiteral("ssh")}, QString(), 1).isEmpty());
    }

    void windowsArguments_data()
    {
        QTest::addColumn<QString>("arg");
        QTest::addColumn<QString>("quoted");
        QTest::newRow("plain") << "--x" << "--x";
        QTest::newRow("empty") << "" << "\"\"";
        QTest::newRow("space") << "a b" << "\"a b\"";
        QTest::newRow("quote") << "a\"b" << "\"a\\\"b\"";
        QTest::newRow("backslashes") << "C:\\a b\\" << "\"C:\\a b\\\\\"";
        QTest::newRow("backslash quote") << "a\\\"b" << "\"a\\\\\\\"b\"";
        QTest::newRow("plain backslash") << "C:\\x" << "C:\\x";
    }

    void windowsArguments()
    {
        QFETCH(QString, arg);
        QFETCH(QString, quoted);
        QCOMPARE(windowsArgument(arg), quoted);
    }

    void remoteMessages()
    {
        const QString hostDir = QStringLiteral("/r/sftp/h");
        const RemotePathMap map(hostDir);
        const auto json = [](const char *text) {
            return QJsonDocument::fromJson(text).object();
        };
        // From the agent: path-like keys only; titles, commands and text stay.
        const QJsonObject update = json("{\"sessionUpdate\":\"tool_call\",\"title\":\"Read /srv/a.cpp\",\"kind\":\"read\","
            "\"locations\":[{\"path\":\"/srv/a.cpp\",\"line\":3}],"
            "\"rawInput\":{\"file_path\":\"/srv/a.cpp\",\"command\":\"cat /srv/a.cpp\",\"paths\":[\"/x\",\"rel\"]},"
            "\"content\":[{\"type\":\"diff\",\"path\":\"/srv/a.cpp\",\"oldText\":\"/srv\",\"newText\":\"x\"},"
            "{\"type\":\"content\",\"content\":{\"type\":\"resource_link\",\"uri\":\"file:///srv/b\"}}]}");
        const QJsonObject local = map.incoming(update);
        QCOMPARE(local[QStringLiteral("title")].toString(), QStringLiteral("Read /srv/a.cpp"));
        QCOMPARE(local[QStringLiteral("locations")].toArray().at(0)[QStringLiteral("path")].toString(), hostDir + QStringLiteral("/srv/a.cpp"));
        QCOMPARE(local[QStringLiteral("locations")].toArray().at(0)[QStringLiteral("line")].toInt(), 3);
        const QJsonObject input = local[QStringLiteral("rawInput")].toObject();
        QCOMPARE(input[QStringLiteral("file_path")].toString(), hostDir + QStringLiteral("/srv/a.cpp"));
        QCOMPARE(input[QStringLiteral("command")].toString(), QStringLiteral("cat /srv/a.cpp"));
        QCOMPARE(input[QStringLiteral("paths")].toArray().at(0).toString(), hostDir + QStringLiteral("/x"));
        QCOMPARE(input[QStringLiteral("paths")].toArray().at(1).toString(), QStringLiteral("rel"));
        const QJsonArray content = local[QStringLiteral("content")].toArray();
        QCOMPARE(content.at(0)[QStringLiteral("path")].toString(), hostDir + QStringLiteral("/srv/a.cpp"));
        QCOMPARE(content.at(0)[QStringLiteral("oldText")].toString(), QStringLiteral("/srv"));
        QCOMPARE(content.at(1)[QStringLiteral("content")][QStringLiteral("uri")].toString(), QStringLiteral("file://") + hostDir + QStringLiteral("/srv/b"));

        // To the agent: every string.
        const QJsonObject prompt = json("{\"cwd\":\"/r/sftp/h/srv\",\"prompt\":[{\"type\":\"resource\",\"resource\":{\"uri\":\"file:///r/sftp/h/srv/a.cpp\",\"text\":\"int x;\"}},"
            "{\"type\":\"text\",\"text\":\"Look at /r/sftp/h/srv/a.cpp\"}],\"n\":1}");
        const QJsonObject remote = map.outgoing(prompt);
        QCOMPARE(remote[QStringLiteral("cwd")].toString(), QStringLiteral("/srv"));
        QCOMPARE(remote[QStringLiteral("n")].toInt(), 1);
        const QJsonArray blocks = remote[QStringLiteral("prompt")].toArray();
        QCOMPARE(blocks.at(0)[QStringLiteral("resource")][QStringLiteral("uri")].toString(), QStringLiteral("file:///srv/a.cpp"));
        QCOMPARE(blocks.at(0)[QStringLiteral("resource")][QStringLiteral("text")].toString(), QStringLiteral("int x;"));
        QCOMPARE(blocks.at(1)[QStringLiteral("text")].toString(), QStringLiteral("Look at /srv/a.cpp"));
        QCOMPARE(RemotePathMap().incoming(update), update);
    }

    void askpassEnvironment()
    {
        QProcessEnvironment env;
        env.insert(QStringLiteral("SSH_ASKPASS"), QStringLiteral("/usr/bin/other-askpass"));
        QVERIFY(setSshAskpassEnvironment(env, QStringLiteral("/x/kate-remote-fs-askpass"), QStringLiteral("alice@h"), QStringLiteral("/run/state")));
        QCOMPARE(env.value(QStringLiteral("SSH_ASKPASS")), QStringLiteral("/x/kate-remote-fs-askpass"));
        QCOMPARE(env.value(QStringLiteral("SSH_ASKPASS_REQUIRE")), QStringLiteral("force"));
        QCOMPARE(env.value(QStringLiteral("KATE_REMOTE_AUTHORITY")), QStringLiteral("alice@h"));
        QCOMPARE(env.value(QStringLiteral("KATE_REMOTE_FALLBACK_ASKPASS")), QStringLiteral("/usr/bin/other-askpass"));
        QCOMPARE(env.value(QStringLiteral("KATE_REMOTE_ASKPASS_STATE")), QStringLiteral("/run/state"));

        // KdePasswords off: only the usual askpass.
        QProcessEnvironment plain;
        plain.insert(QStringLiteral("SSH_ASKPASS"), QStringLiteral("/usr/bin/other-askpass"));
        QVERIFY(!setSshAskpassEnvironment(plain, QString(), QStringLiteral("h"), QStringLiteral("/run/state")));
        QCOMPARE(plain.value(QStringLiteral("SSH_ASKPASS_REQUIRE")), QStringLiteral("prefer"));
        QVERIFY(!plain.contains(QStringLiteral("KATE_REMOTE_AUTHORITY")));
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
