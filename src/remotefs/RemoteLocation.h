#pragma once

#include <QString>
#include <QUrl>

class QProcessEnvironment;

#include <optional>
#include <string>

/**
 * Mapping between remote URLs and paths under the kate-remote-fs mount root.
 *
 * The mount root (default ~/.kate-remote, configurable as
 * [General] MountRoot in kate-remoterc) mirrors URLs:
 *
 *     <root>/sftp/user@host:2222/home/user/proj  <->  sftp://user@host:2222/home/user/proj
 *
 * Shared by the kate-remote-fs daemon, the Remote Projects plugin and Kate
 * Code, so all of them agree on the layout.
 */
namespace KateRemote
{

// Name of the shared config file (KConfig, in $XDG_CONFIG_HOME).
QString configFileName();

// ~/.kate-remote
QString defaultMountRoot();

// The configured mount root: an absolute, clean path. "~/" is expanded;
// an empty or relative setting falls back to the default.
QString configuredMountRoot();
QString cleanMountRoot(const QString &setting);

// Schemes the daemon serves: sftp (through sshfs) and the KIO schemes.
bool isSupportedScheme(const QString &scheme);
// Schemes mounted through kio-fuse: [General] KioSchemes in kate-remoterc
// (default smb, webdav, webdavs, ftp, fish, nfs); read once per process.
QStringList kioSchemes();
bool isKioScheme(const QString &scheme);

// user@host:port as accepted for ssh. Strict on purpose: these names come
// from file system lookups (anything stat-ing <root>/sftp/<name>) and end up
// on the ssh command line.
struct SshTarget {
    QString user; // may be empty (ssh config / current user)
    QString host; // host name, ssh config alias or IP address (IPv6 without brackets)
    int port = -1; // -1 = default / ssh config

    // user@host (IPv6 in brackets), for sshfs "<destination>:/path".
    QString destination() const;
};
std::optional<SshTarget> parseSshAuthority(const QString &authority);

// The URL KIO's sftp worker files the host's password under in KDE's
// password cache (kpasswdserver): sftp://[user@]host[:port], without the
// port when it is the default one.
QUrl kioAuthUrl(const SshTarget &target);

// ssh's prompts through SSH_ASKPASS: a login password ("user@host's
// password: ", "(user@host) Password: ", "Password: "), or anything else
// (key passphrases, host key confirmation, one-time codes, new passwords).
bool isLoginPasswordPrompt(const QString &prompt);
// The user ssh logs in as, from such a prompt (empty if it does not say).
QString userFromPasswordPrompt(const QString &prompt);

// Whether a path component under <root>/<scheme>/ names a host the daemon
// may connect to: [user@]host[:port], strictly (for sftp it ends up on the
// ssh command line); for KIO schemes the user may contain ';' (an smb
// DOMAIN;user).
bool isValidAuthority(const QString &scheme, const QString &authority);

struct Location {
    QString scheme;
    QString authority;
    QString path; // absolute remote path, starts with '/'

    QUrl url() const;
    bool operator==(const Location &other) const
    {
        return scheme == other.scheme && authority == other.authority && path == other.path;
    }
};

// The remote location for a local path under the mount root, if any.
// A path naming the host directory itself maps to remote path "/".
std::optional<Location> locationForPath(const QString &localPath, const QString &mountRoot);

// The local path for a remote location (no validation of the host).
QString pathForLocation(const Location &location, const QString &mountRoot);

// sftp://authority/path -> Location, if the scheme is supported and the
// authority valid.
std::optional<Location> locationForUrl(const QUrl &url);

// Schemes whose hosts are ssh servers: sftp and fish (KIO's shell over ssh).
bool isSshScheme(const QString &scheme);

// The ssh command: [General] SshCommand in kate-remoterc (default "ssh";
// split like a shell command line, may hold options).
QStringList configuredSshCommand();

// The command line (program first) of an interactive login shell in
// location's directory on its host, through `ssh`: for sftp and fish
// locations whose host ssh accepts (parseSshAuthority()), else empty.
// The remote command is for a POSIX-like login shell (tested with sh, bash
// and zsh): it changes to the directory (a missing one leaves the home
// directory) and execs "$SHELL" -l. On a Windows host (a drive path, see
// windowsPathForSftpPath()) it starts PowerShell in the folder instead,
// whether the server's default shell is cmd or PowerShell.
QStringList sshTerminalCommand(const Location &location, const QStringList &ssh);

// The command line (program first) running an ACP agent (program, args)
// in location's directory on its host through `ssh -T`, stdin/stdout being
// the protocol: for sftp and fish locations whose host ssh accepts
// (parseSshAuthority()), else empty. A local absolute program path becomes
// its file name.
// POSIX hosts: the agent runs from a login shell (the user's PATH), with
// ~/.local/bin, ~/bin and ~/.cargo/bin added; a missing directory fails, a
// missing agent exits with 127 (env).
// Windows hosts (sftp drive paths, windowsPathForSftpPath()): PowerShell
// runs windowsAgentScript() (-EncodedCommand, so either default shell of
// Windows' sshd passes it on unchanged).
QStringList sshAgentCommand(const Location &location, const QStringList &ssh, const QString &program, const QStringList &args);

// SSH_ASKPASS for an ssh started without a terminal (env: its environment).
// With askpass (kate-remote-fs-askpass), login passwords come from KDE's
// password cache, other prompts go to the previous SSH_ASKPASS or
// ksshaskpass (KATE_REMOTE_FALLBACK_ASKPASS); stateFile (per connection
// attempt, removed by the caller before and after) tells a repeated prompt
// (wrong password). Returns whether askpass is used. Without it, only the
// fallback is set up.
bool setSshAskpassEnvironment(QProcessEnvironment &env, const QString &askpass, const QString &authority, const QString &stateFile);

// The PowerShell script starting an agent on a Windows host in folder
// (a Windows path): the program from PATH plus %USERPROFILE%\.local\bin,
// \bin, \.cargo\bin and %APPDATA%\npm (exit 127 if missing), its stdio
// copied as bytes.
QString windowsAgentScript(const QString &folder, const QString &program, const QStringList &args);
// 'arg' quoted for a Windows command line (CommandLineToArgvW rules).
QString windowsArgument(const QString &arg);

// Windows' OpenSSH sftp server shows drives as "/C:/...": the Windows path
// of such a path ("/C:/Users/me" -> "C:\Users\me", "/d:" -> "D:\"), empty
// for any other path.
QString windowsPathForSftpPath(const QString &path);

// 'text' quoted for a POSIX shell.
QString shellQuote(const QString &text);

enum class InputError {
    None,
    Empty,
    LocalPath, // a local path that is not below the mount root
    Syntax,
    RelativePath, // host:path without a leading '/'
    Unsupported, // not sftp, or an invalid host
    RemoteRoot, // the remote "/": as a project it would index the whole file system
};

// A remote folder as typed by the user: "sftp://host/path",
// "[user@]host:/path" or a local path below the mount root.
std::optional<QUrl> urlForInput(const QString &text, const QString &mountRoot, InputError *error);

/**
 * The target to report for a remote symlink so that following it under the
 * host directory reaches what it reaches on the remote, and never anything
 * above the host directory (local files).
 *
 * linkDir: directory of the link relative to the remote root ("" for "/").
 * Targets that stay inside are returned unchanged; absolute targets and
 * relative ones climbing above the remote root ("/.." is "/" there) are
 * rewritten relative to linkDir.
 */
std::string hostSymlinkTarget(const std::string &linkDir, const std::string &target);

}
