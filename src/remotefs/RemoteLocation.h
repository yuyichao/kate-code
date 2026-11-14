#pragma once

#include <QString>
#include <QUrl>

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
