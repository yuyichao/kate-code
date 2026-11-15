#include "RemoteLocation.h"

#include <KConfig>
#include <KConfigGroup>

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QProcess>
#include <QRegularExpression>

#include <vector>

namespace KateRemote
{

QString configFileName()
{
    return QStringLiteral("kate-remoterc");
}

QString defaultMountRoot()
{
    return QDir::homePath() + QStringLiteral("/.kate-remote");
}

QString cleanMountRoot(const QString &setting)
{
    QString path = setting.trimmed();
    if (path == QLatin1String("~")) {
        path = QDir::homePath();
    } else if (path.startsWith(QLatin1String("~/"))) {
        path = QDir::homePath() + path.mid(1);
    }
    if (path.isEmpty() || QDir::isRelativePath(path)) {
        return defaultMountRoot();
    }
    path = QDir::cleanPath(path);
    // The root itself or the home directory would shadow everything below it.
    if (path == QLatin1String("/") || path == QDir::cleanPath(QDir::homePath())) {
        return defaultMountRoot();
    }
    return path;
}

QString configuredMountRoot()
{
    KConfig config(configFileName(), KConfig::SimpleConfig);
    return cleanMountRoot(KConfigGroup(&config, QStringLiteral("General")).readEntry("MountRoot", QString()));
}

QStringList kioSchemes()
{
    static const QStringList schemes = [] {
        KConfig config(configFileName(), KConfig::SimpleConfig);
        const QStringList defaults{QStringLiteral("smb"),
                                   QStringLiteral("webdav"),
                                   QStringLiteral("webdavs"),
                                   QStringLiteral("ftp"),
                                   QStringLiteral("fish"),
                                   QStringLiteral("nfs")};
        static const QRegularExpression name(QStringLiteral("^[a-z][a-z0-9+.-]*$"));
        // sftp is mounted with sshfs; local and virtual KIO schemes are not
        // remote folders.
        static const QStringList excluded{QStringLiteral("sftp"),
                                          QStringLiteral("file"),
                                          QStringLiteral("remote"),
                                          QStringLiteral("trash"),
                                          QStringLiteral("desktop"),
                                          QStringLiteral("recentlyused"),
                                          QStringLiteral("kateexec")};
        QStringList result;
        for (const QString &scheme : KConfigGroup(&config, QStringLiteral("General")).readEntry("KioSchemes", defaults)) {
            const QString clean = scheme.trimmed().toLower();
            if (name.match(clean).hasMatch() && !excluded.contains(clean) && !result.contains(clean)) {
                result << clean;
            }
        }
        return result;
    }();
    return schemes;
}

bool isKioScheme(const QString &scheme)
{
    return kioSchemes().contains(scheme);
}

bool isSupportedScheme(const QString &scheme)
{
    return scheme == QLatin1String("sftp") || isKioScheme(scheme);
}

QString SshTarget::destination() const
{
    QString result;
    if (!user.isEmpty()) {
        result = user + QLatin1Char('@');
    }
    if (host.contains(QLatin1Char(':'))) {
        result += QLatin1Char('[') + host + QLatin1Char(']');
    } else {
        result += host;
    }
    return result;
}

static std::optional<SshTarget> parseAuthority(const QString &authority, bool allowDomainUser)
{
    // user: no leading '-' (it would read as an option)
    // host: name/alias with no leading '-' or '.', or a bracketed IPv6 address
    static const QRegularExpression sshRe(QStringLiteral("^(?:([A-Za-z0-9_][A-Za-z0-9._-]*)@)?"
                                                         "(?:([A-Za-z0-9_][A-Za-z0-9._-]*)|\\[([0-9A-Fa-f:.]+)\\])"
                                                         "(?::([0-9]{1,5}))?$"));
    static const QRegularExpression kioRe(QStringLiteral("^(?:([A-Za-z0-9_][A-Za-z0-9._;-]*)@)?"
                                                         "(?:([A-Za-z0-9_][A-Za-z0-9._-]*)|\\[([0-9A-Fa-f:.]+)\\])"
                                                         "(?::([0-9]{1,5}))?$"));
    if (authority.isEmpty() || authority.size() > 255) {
        return std::nullopt;
    }
    const auto match = (allowDomainUser ? kioRe : sshRe).match(authority);
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    SshTarget target;
    target.user = match.captured(1);
    target.host = match.hasCaptured(2) ? match.captured(2) : match.captured(3);
    if (match.hasCaptured(3) && !target.host.contains(QLatin1Char(':'))) {
        return std::nullopt;
    }
    if (match.hasCaptured(4)) {
        const int port = match.captured(4).toInt();
        if (port < 1 || port > 65535) {
            return std::nullopt;
        }
        target.port = port;
    }
    return target;
}

std::optional<SshTarget> parseSshAuthority(const QString &authority)
{
    return parseAuthority(authority, false);
}

QUrl kioAuthUrl(const SshTarget &target)
{
    QUrl url;
    url.setScheme(QStringLiteral("sftp"));
    url.setHost(target.host);
    if (target.port > 0 && target.port != 22) {
        url.setPort(target.port);
    }
    if (!target.user.isEmpty()) {
        url.setUserName(target.user);
    }
    return url;
}

bool isLoginPasswordPrompt(const QString &prompt)
{
    const QString text = prompt.trimmed().toLower();
    if (text.contains(QLatin1String("passphrase")) || text.contains(QLatin1String("new password")) || text.contains(QLatin1String("retype"))
        || text.contains(QLatin1String("old password")) || text.contains(QLatin1String("current password"))) {
        return false;
    }
    static const QRegularExpression passwordPrompt(QStringLiteral("password( for [^:]*)?:$"));
    return passwordPrompt.match(text).hasMatch();
}

QString userFromPasswordPrompt(const QString &prompt)
{
    // "user@host's password: " or "(user@host) Password: "
    static const QRegularExpression userAtHost(QStringLiteral("^\\(?([^@()\\s]+)@[^\\s']+"));
    const auto match = userAtHost.match(prompt.trimmed());
    return match.hasMatch() ? match.captured(1) : QString();
}

bool isValidAuthority(const QString &scheme, const QString &authority)
{
    if (scheme == QLatin1String("sftp")) {
        return parseSshAuthority(authority).has_value();
    }
    if (isKioScheme(scheme)) {
        return parseAuthority(authority, true).has_value();
    }
    return false;
}

QUrl Location::url() const
{
    QUrl url;
    url.setScheme(scheme);
    url.setAuthority(authority);
    url.setPath(path.isEmpty() ? QStringLiteral("/") : path);
    return url;
}

std::optional<Location> locationForPath(const QString &localPath, const QString &mountRoot)
{
    if (localPath.isEmpty() || QDir::isRelativePath(localPath)) {
        return std::nullopt;
    }
    const QString root = QDir::cleanPath(mountRoot);
    const QString path = QDir::cleanPath(localPath);
    if (!path.startsWith(root + QLatin1Char('/'))) {
        return std::nullopt;
    }
    const QString rest = path.mid(root.size() + 1);
    const qsizetype schemeEnd = rest.indexOf(QLatin1Char('/'));
    if (schemeEnd <= 0) {
        return std::nullopt;
    }
    Location location;
    location.scheme = rest.left(schemeEnd);
    const qsizetype authorityEnd = rest.indexOf(QLatin1Char('/'), schemeEnd + 1);
    location.authority = rest.mid(schemeEnd + 1, authorityEnd < 0 ? -1 : authorityEnd - schemeEnd - 1);
    location.path = authorityEnd < 0 ? QStringLiteral("/") : rest.mid(authorityEnd);
    if (!isSupportedScheme(location.scheme) || !isValidAuthority(location.scheme, location.authority)) {
        return std::nullopt;
    }
    return location;
}

QString pathForLocation(const Location &location, const QString &mountRoot)
{
    QString path = QDir::cleanPath(mountRoot) + QLatin1Char('/') + location.scheme + QLatin1Char('/') + location.authority;
    const QString remote = QDir::cleanPath(location.path.isEmpty() ? QStringLiteral("/") : location.path);
    if (remote != QLatin1String("/")) {
        path += remote.startsWith(QLatin1Char('/')) ? remote : QLatin1Char('/') + remote;
    }
    return path;
}

std::optional<Location> locationForUrl(const QUrl &url)
{
    if (!url.isValid() || !isSupportedScheme(url.scheme()) || url.host().isEmpty()) {
        return std::nullopt;
    }
    // Rebuilt from the parts so a password never ends up in a path.
    QString authority;
    if (!url.userName().isEmpty()) {
        authority = url.userName() + QLatin1Char('@');
    }
    const QString host = url.host();
    authority += host.contains(QLatin1Char(':')) ? QLatin1Char('[') + host + QLatin1Char(']') : host;
    if (url.port() != -1) {
        authority += QLatin1Char(':') + QString::number(url.port());
    }
    if (!isValidAuthority(url.scheme(), authority)) {
        return std::nullopt;
    }
    Location location;
    location.scheme = url.scheme();
    location.authority = authority;
    location.path = QDir::cleanPath(url.path().isEmpty() ? QStringLiteral("/") : url.path());
    if (!location.path.startsWith(QLatin1Char('/'))) {
        location.path.prepend(QLatin1Char('/'));
    }
    return location;
}

bool isSshScheme(const QString &scheme)
{
    return scheme == QLatin1String("sftp") || (scheme == QLatin1String("fish") && isKioScheme(scheme));
}

QStringList configuredSshCommand()
{
    KConfig config(configFileName(), KConfig::SimpleConfig);
    const QStringList command = QProcess::splitCommand(KConfigGroup(&config, QStringLiteral("General")).readEntry("SshCommand", QStringLiteral("ssh")));
    return command.isEmpty() ? QStringList{QStringLiteral("ssh")} : command;
}

QString shellQuote(const QString &text)
{
    QString quoted = text;
    quoted.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

// PowerShell's -EncodedCommand argument: base64 of the UTF-16LE script,
// which neither cmd nor PowerShell (Windows sshd's shells) changes.
static QString powershellEncoded(const QString &script)
{
    QByteArray utf16;
    for (const QChar c : script) {
        utf16.append(char(c.unicode() & 0xff));
        utf16.append(char(c.unicode() >> 8));
    }
    return QString::fromLatin1(utf16.toBase64());
}

// 'text' in a PowerShell single-quoted string.
static QString powershellQuote(const QString &text)
{
    return QLatin1Char('\'') + QString(text).replace(QLatin1Char('\''), QLatin1String("''")) + QLatin1Char('\'');
}

QString windowsArgument(const QString &arg)
{
    static const QRegularExpression special(QStringLiteral("[\\s\"]"));
    if (!arg.isEmpty() && !arg.contains(special)) {
        return arg;
    }
    // CommandLineToArgvW: backslashes are literal except before a quote,
    // where 2n+1 of them make n and a literal quote.
    QString quoted = QStringLiteral("\"");
    int backslashes = 0;
    for (const QChar c : arg) {
        if (c == QLatin1Char('\\')) {
            ++backslashes;
            continue;
        }
        if (c == QLatin1Char('"')) {
            quoted += QString(backslashes * 2 + 1, QLatin1Char('\\')) + c;
        } else {
            quoted += QString(backslashes, QLatin1Char('\\')) + c;
        }
        backslashes = 0;
    }
    return quoted + QString(backslashes * 2, QLatin1Char('\\')) + QLatin1Char('"');
}

// [-l user] [-p port] -- host
static void appendSshDestination(QStringList &command, const SshTarget &target)
{
    if (!target.user.isEmpty()) {
        command << QStringLiteral("-l") << target.user;
    }
    if (target.port > 0) {
        command << QStringLiteral("-p") << QString::number(target.port);
    }
    command << QStringLiteral("--") << target.host;
}

QStringList sshTerminalCommand(const Location &location, const QStringList &ssh)
{
    if (!isSshScheme(location.scheme) || ssh.isEmpty() || !location.path.startsWith(QLatin1Char('/'))) {
        return {};
    }
    // Strict: a fish "DOMAIN;user" is no ssh user.
    const auto target = parseSshAuthority(location.authority);
    if (!target) {
        return {};
    }
    QStringList command = ssh;
    command << QStringLiteral("-t");
    appendSshDestination(command, *target);
    const QString windowsPath = windowsPathForSftpPath(location.path);
    if (!windowsPath.isEmpty()) {
        // Windows' sshd runs the command with its default shell, cmd or
        // PowerShell (cmd /c ..., powershell -c ...): start PowerShell in
        // the folder. The script is base64 (UTF-16LE), which neither shell
        // changes; -NoExit keeps the session after it.
        const QString script = QStringLiteral("Set-Location -LiteralPath %1").arg(powershellQuote(windowsPath));
        command << QStringLiteral("powershell -NoLogo -NoExit -EncodedCommand %1").arg(powershellEncoded(script));
        return command;
    }
    // ssh joins the remote command and hands it to the user's login shell;
    // ';' (not '&&') so a missing directory still gives a shell.
    command << QStringLiteral("cd %1; exec \"$SHELL\" -l").arg(shellQuote(location.path));
    return command;
}

QString windowsAgentScript(const QString &folder, const QString &program, const QStringList &args)
{
    QStringList quoted;
    for (const QString &arg : args) {
        quoted << windowsArgument(arg);
    }
    // Run by Windows PowerShell 5.1 or PowerShell 7, whichever shell sshd
    // uses. PowerShell re-encodes a native program's output it passes on,
    // so the agent's stdin/stdout/stderr are copied as bytes instead:
    // stdin through an unbuffered stream, closed at end of input (the agent
    // then exits); the script exits with the agent's code. Kept short: cmd
    // limits a command line to 8191 characters.
    return QStringLiteral(
               "$ProgressPreference='SilentlyContinue'\n"
               "$d=%1\n"
               "if(-not(Test-Path -LiteralPath $d -PathType Container)){[Console]::Error.WriteLine(\"No such folder: $d\");exit 1}\n"
               "$h=$env:USERPROFILE\n"
               "$env:PATH=\"$env:PATH;$h\\.local\\bin;$h\\bin;$h\\.cargo\\bin;$env:APPDATA\\npm\"\n"
               "$c=Get-Command -Name %2 -CommandType Application -ErrorAction SilentlyContinue|Select-Object -First 1\n"
               "if(-not $c){[Console]::Error.WriteLine(%3);exit 127}\n"
               "$i=New-Object Diagnostics.ProcessStartInfo\n"
               "$i.FileName=$c.Path\n"
               "$i.Arguments=%4\n"
               "$i.WorkingDirectory=$d\n"
               "$i.UseShellExecute=$false\n"
               "$i.RedirectStandardInput=$true\n"
               "$i.RedirectStandardOutput=$true\n"
               "$i.RedirectStandardError=$true\n"
               "$p=[Diagnostics.Process]::Start($i)\n"
               "$w=New-Object IO.FileStream($p.StandardInput.BaseStream.SafeFileHandle,[IO.FileAccess]::Write,1)\n"
               "$o=$p.StandardOutput.BaseStream.CopyToAsync([Console]::OpenStandardOutput())\n"
               "$e=$p.StandardError.BaseStream.CopyToAsync([Console]::OpenStandardError())\n"
               "$n=[Console]::OpenStandardInput().CopyToAsync($w)\n"
               "while(-not $p.HasExited){if($n -and $n.IsCompleted){$w.Close();$n=$null};Start-Sleep -Milliseconds 100}\n"
               "[void]$o.Wait(5000);[void]$e.Wait(5000)\n"
               "exit $p.ExitCode\n")
        .arg(powershellQuote(folder),
             powershellQuote(program),
             powershellQuote(program + QStringLiteral(": not found")),
             powershellQuote(quoted.join(QLatin1Char(' '))));
}

QStringList sshAgentCommand(const Location &location, const QStringList &ssh, const QString &program, const QStringList &args)
{
    if (!isSshScheme(location.scheme) || ssh.isEmpty() || program.isEmpty() || !location.path.startsWith(QLatin1Char('/'))) {
        return {};
    }
    const auto target = parseSshAuthority(location.authority);
    if (!target) {
        return {};
    }
    const QString windowsPath = windowsPathForSftpPath(location.path);
    if (!windowsPath.isEmpty() && location.scheme != QLatin1String("sftp")) {
        return {};
    }
    QStringList command = ssh;
    // No terminal: stdin/stdout carry the protocol. Notice a dead
    // connection like the mounts do.
    command << QStringLiteral("-T") << QStringLiteral("-o") << QStringLiteral("ServerAliveInterval=15") << QStringLiteral("-o")
            << QStringLiteral("ServerAliveCountMax=3") << QStringLiteral("-o") << QStringLiteral("ConnectTimeout=10");
    appendSshDestination(command, *target);
    // A local absolute path means nothing on the host: look the name up.
    const QString name = QFileInfo(program).isAbsolute() ? QFileInfo(program).fileName() : program;
    if (!windowsPath.isEmpty()) {
        command << QStringLiteral("powershell -NoLogo -NoProfile -NonInteractive -EncodedCommand %1").arg(powershellEncoded(windowsAgentScript(windowsPath, name, args)));
        return command;
    }
    QStringList quoted{shellQuote(name)};
    for (const QString &arg : args) {
        quoted << shellQuote(arg);
    }
    // ssh hands the command to the user's shell without a login profile:
    // run a login shell for the user's PATH, with the usual per-user
    // install directories added (as the local search does). Written for
    // POSIX shells (tested: sh, bash, zsh) and fish 3 ("$PATH" is ':'-joined
    // there too; not tested).
    const QString inner = QStringLiteral("cd %1 && exec env PATH=\"$PATH:$HOME/.local/bin:$HOME/bin:$HOME/.cargo/bin\" %2")
                              .arg(shellQuote(location.path), quoted.join(QLatin1Char(' ')));
    command << QStringLiteral("exec \"$SHELL\" -l -c %1").arg(shellQuote(inner));
    return command;
}

bool setSshAskpassEnvironment(QProcessEnvironment &env, const QString &askpass, const QString &authority, const QString &stateFile)
{
    QString fallbackAskpass = env.value(QStringLiteral("SSH_ASKPASS"));
    if (fallbackAskpass.isEmpty() && (env.contains(QStringLiteral("DISPLAY")) || env.contains(QStringLiteral("WAYLAND_DISPLAY")))) {
        fallbackAskpass = QStandardPaths::findExecutable(QStringLiteral("ksshaskpass"));
    }
    if (!askpass.isEmpty() && fallbackAskpass != askpass) {
        env.insert(QStringLiteral("SSH_ASKPASS"), askpass);
        env.insert(QStringLiteral("SSH_ASKPASS_REQUIRE"), QStringLiteral("force"));
        env.insert(QStringLiteral("KATE_REMOTE_AUTHORITY"), authority);
        env.insert(QStringLiteral("KATE_REMOTE_FALLBACK_ASKPASS"), fallbackAskpass);
        env.insert(QStringLiteral("KATE_REMOTE_ASKPASS_STATE"), stateFile);
        return true;
    }
    if (!fallbackAskpass.isEmpty()) {
        env.insert(QStringLiteral("SSH_ASKPASS"), fallbackAskpass);
        if (!env.contains(QStringLiteral("SSH_ASKPASS_REQUIRE"))) {
            env.insert(QStringLiteral("SSH_ASKPASS_REQUIRE"), QStringLiteral("prefer"));
        }
    }
    return false;
}

QString windowsPathForSftpPath(const QString &path)
{
    static const QRegularExpression drive(QStringLiteral("^/([A-Za-z]:)(/.*)?$"));
    const auto match = drive.match(path);
    if (!match.hasMatch()) {
        return {};
    }
    QString rest = match.captured(2);
    rest.replace(QLatin1Char('/'), QLatin1Char('\\'));
    return match.captured(1).toUpper() + (rest.isEmpty() ? QStringLiteral("\\") : rest);
}

std::optional<QUrl> urlForInput(const QString &text, const QString &mountRoot, InputError *error)
{
    const auto fail = [error](InputError e) -> std::optional<QUrl> {
        *error = e;
        return std::nullopt;
    };
    *error = InputError::None;
    const QString input = text.trimmed();
    if (input.isEmpty()) {
        return fail(InputError::Empty);
    }

    std::optional<Location> location;
    if (input.startsWith(QLatin1Char('/')) || input.startsWith(QLatin1Char('~'))) {
        const QString path = input.startsWith(QLatin1Char('~')) ? QDir::homePath() + input.mid(1) : input;
        location = locationForPath(path, mountRoot);
        if (!location) {
            return fail(InputError::LocalPath);
        }
    } else if (input.contains(QLatin1String("://"))) {
        location = locationForUrl(QUrl(input));
        if (!location) {
            return fail(InputError::Unsupported);
        }
    } else {
        // scp style: [user@]host:/path
        static const QRegularExpression scpStyle(QStringLiteral("^([^/:]+):(.*)$"));
        const auto match = scpStyle.match(input);
        if (!match.hasMatch()) {
            return fail(InputError::Syntax);
        }
        const QString path = match.captured(2);
        if (!path.isEmpty() && !path.startsWith(QLatin1Char('/'))) {
            return fail(InputError::RelativePath);
        }
        QUrl url(QStringLiteral("sftp://") + match.captured(1));
        url.setPath(path.isEmpty() ? QStringLiteral("/") : path);
        location = locationForUrl(url);
        if (!location) {
            return fail(InputError::Unsupported);
        }
    }
    if (location->path == QLatin1String("/")) {
        return fail(InputError::RemoteRoot);
    }
    return location->url();
}

static std::vector<std::string> splitPath(const std::string &path)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) {
            end = path.size();
        }
        std::string part = path.substr(start, end - start);
        if (!part.empty() && part != ".") {
            parts.push_back(std::move(part));
        }
        start = end + 1;
    }
    return parts;
}

std::string hostSymlinkTarget(const std::string &linkDir, const std::string &target)
{
    if (target.empty()) {
        return target;
    }
    const std::vector<std::string> dir = splitPath(linkDir);
    const std::vector<std::string> parts = splitPath(target);
    const bool absolute = target.front() == '/';
    if (!absolute) {
        size_t depth = dir.size();
        bool escapes = false;
        for (const auto &part : parts) {
            if (part == "..") {
                if (depth == 0) {
                    escapes = true;
                    break;
                }
                --depth;
            } else {
                ++depth;
            }
        }
        if (!escapes) {
            return target;
        }
    }

    // Resolve against the remote root, where ".." of "/" is "/".
    std::vector<std::string> resolved = absolute ? std::vector<std::string>() : dir;
    for (const auto &part : parts) {
        if (part == "..") {
            if (!resolved.empty()) {
                resolved.pop_back();
            }
        } else {
            resolved.push_back(part);
        }
    }

    std::string result;
    for (size_t i = 0; i < dir.size(); ++i) {
        result += "../";
    }
    for (const auto &part : resolved) {
        result += part;
        result += '/';
    }
    if (!result.empty()) {
        result.pop_back();
    }
    return result.empty() ? std::string(".") : result;
}

}
