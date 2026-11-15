#!/usr/bin/env python3
"""Integration test for kate-remote-fs.

Runs the daemon on a private session bus (run under dbus-run-session) with a
private XDG_RUNTIME_DIR / XDG_CONFIG_HOME and a temporary mount root. sshfs
talks to a local sftp-server confined by bwrap to a temporary tree, which the
"remote" sees as /data.

Cleanup only ever unmounts; the temporary directory is removed only once
nothing below it is mounted any more (the mounts expose files).
"""

import errno
import json
import os
import pty
import re
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

SKIP = 77
DAEMON = os.path.abspath(sys.argv[1])
KPASSWDTOOL = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
ACPDRIVER = os.path.abspath(sys.argv[3]) if len(sys.argv) > 3 else None

SFTP_SERVER = next((p for p in ("/usr/lib/ssh/sftp-server", "/usr/libexec/openssh/sftp-server",
                                "/usr/lib/openssh/sftp-server") if os.path.exists(p)), None)
for tool in ("sshfs", "bwrap", "fusermount3", "dbus-send", "dbus-update-activation-environment"):
    if not shutil.which(tool):
        print(f"SKIP: {tool} not found")
        sys.exit(SKIP)
if not SFTP_SERVER:
    print("SKIP: sftp-server not found")
    sys.exit(SKIP)
if not os.environ.get("DBUS_SESSION_BUS_ADDRESS"):
    print("SKIP: no session bus (run under dbus-run-session)")
    sys.exit(SKIP)

TMP = tempfile.mkdtemp(prefix="krfs-test.")
TREE = os.path.join(TMP, "tree")          # the remote's /data
ROOT = os.path.join(TMP, "root")          # MountRoot
RUNTIME = os.path.join(TMP, "runtime")    # XDG_RUNTIME_DIR
CONFIG = os.path.join(TMP, "config")      # XDG_CONFIG_HOME
STATE = os.path.join(TMP, "state")        # XDG_STATE_HOME
DATA = os.path.join(TMP, "data")          # XDG_DATA_HOME
FALLBACK_ASKPASS = os.path.join(TMP, "fallback-askpass")
KIO_BIN = os.path.join(TMP, "kio-bin")    # fake ssh for KIO's fish worker (through kio-fuse)
KIO_SSH_LOG = os.path.join(TMP, "kio-ssh.log")
FALLBACK_LOG = os.path.join(TMP, "fallback.log")
AGENT_HOME = os.path.join(TMP, "agent-home")  # the remote's home for agents
AGENT_LOG = os.path.join(AGENT_HOME, "agent.log")
HOSTS_FILE = os.path.join(STATE, "kate-remote-fs", "hosts")
SSH_LOG = os.path.join(TMP, "ssh.log")
FAKE_SSH = os.path.join(TMP, "fake-ssh")
DAEMON_LOG = os.path.join(TMP, "daemon.log")
DAV_TREE = os.path.join(TMP, "dav")       # a WebDAV share (served at /dav/; "/" is 404)
DAV_DEEP = os.path.join(TMP, "dav-deep")  # one at /deep/er/dav/ (/deep/ and /deep/er/ are 404)
BACKEND = os.path.join(RUNTIME, "kate-remote-fs")

failures = []
daemon = None


def check(condition, message):
    if condition:
        print(f"  ok: {message}")
    else:
        print(f"  FAIL: {message}")
        failures.append(message)


def unescape(field):
    out, i = bytearray(), 0
    raw = field.encode()
    while i < len(raw):
        if raw[i:i + 1] == b"\\" and i + 3 < len(raw):
            try:
                out.append(int(raw[i + 1:i + 4], 8))
                i += 4
                continue
            except ValueError:
                pass
        out.append(raw[i])
        i += 1
    return out.decode(errors="replace")


def mounts_under(prefix):
    result = []
    with open("/proc/self/mountinfo") as f:
        for line in f:
            fields = line.split()
            sep = fields.index("-")
            point = unescape(fields[4])
            if point == prefix or point.startswith(prefix + "/"):
                result.append((point, fields[sep + 1]))
    return result


def backend_mounted(host):
    return any(p == os.path.join(BACKEND, "sftp", host) for p, _ in mounts_under(BACKEND))


def ssh_calls(host):
    if not os.path.exists(SSH_LOG):
        return 0
    with open(SSH_LOG) as f:
        return sum(1 for line in f if f" {host} " in f" {line.strip()} ")


def dbus(method, *args):
    cmd = ["dbus-send", "--session", "--print-reply", "--dest=org.kde.KateRemoteFS",
           "/org/kde/KateRemoteFS", f"org.kde.KateRemoteFS.{method}"] + [f"string:{a}" for a in args]
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=30).stdout
    strings = [line.split('"', 1)[1].rsplit('"', 1)[0] for line in out.splitlines() if 'string "' in line]
    if "boolean true" in out:
        return True
    if "boolean false" in out:
        return False
    if "array [" in out:
        return strings
    return strings[0] if strings else ""


def wait_for(predicate, timeout=10.0, interval=0.1):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return True
        time.sleep(interval)
    return predicate()


def env():
    e = dict(os.environ)
    e.update(XDG_RUNTIME_DIR=RUNTIME, XDG_CONFIG_HOME=CONFIG, XDG_STATE_HOME=STATE, XDG_DATA_HOME=DATA,
             QT_FORCE_STDERR_LOGGING="1", QT_QPA_PLATFORM="offscreen")
    # No dialogs from the test: kate-remote-fs-askpass fails instead of
    # asking, other prompts go to a fake askpass.
    for key in ("DISPLAY", "WAYLAND_DISPLAY", "SSH_AUTH_SOCK"):
        e.pop(key, None)
    e.update(SSH_ASKPASS=FALLBACK_ASKPASS, KATE_REMOTE_ASKPASS_NO_DIALOG="1")
    return e


def start_daemon():
    global daemon
    log = open(DAEMON_LOG, "a")
    daemon = subprocess.Popen([DAEMON], env=env(), stdout=log, stderr=subprocess.STDOUT)
    def ready():
        # A crashed instance leaves a dead mount behind; wait for a live one.
        try:
            return "sftp" in os.listdir(ROOT)
        except OSError:
            return False
    if not wait_for(lambda: ready() or daemon.poll() is not None):
        raise RuntimeError("daemon did not mount")
    if daemon.poll() is not None:
        raise RuntimeError(f"daemon exited with {daemon.returncode}")


def stop_daemon(sig=signal.SIGTERM):
    if daemon and daemon.poll() is None:
        daemon.send_signal(sig)
        try:
            return daemon.wait(15)
        except subprocess.TimeoutExpired:
            daemon.kill()
            return daemon.wait()
    return daemon.returncode if daemon else None


def stop_bus_services():
    # Services DBus started for the test (kpasswdserver in kiod6, ...)
    # outlive dbus-run-session; stop those of our runtime directory.
    for pid in os.listdir("/proc"):
        if not pid.isdigit() or int(pid) == os.getpid():
            continue
        try:
            with open(f"/proc/{pid}/environ", "rb") as f:
                environ = f.read().split(b"\0")
        except OSError:
            continue
        if f"XDG_RUNTIME_DIR={RUNTIME}".encode() in environ:
            try:
                os.kill(int(pid), signal.SIGTERM)
            except OSError:
                pass


def cleanup():
    stop_daemon()
    stop_bus_services()
    # Deepest first; only unmount, never delete through a mount.
    for point, _ in sorted(mounts_under(TMP), key=lambda m: -len(m[0])):
        subprocess.run(["fusermount3", "-u", "-z", point], capture_output=True)
    remaining = mounts_under(TMP)
    if remaining:
        print(f"WARNING: still mounted, leaving {TMP}: {remaining}")
        return
    if failures:
        print(f"Keeping {TMP} for inspection (daemon log: {DAEMON_LOG})")
        return
    shutil.rmtree(TMP)


def setup():
    os.makedirs(os.path.join(TREE, "sub"))
    os.makedirs(CONFIG)
    os.makedirs(DATA)
    # KWallet would wait for a "create a wallet" prompt nobody answers.
    with open(os.path.join(CONFIG, "kwalletrc"), "w") as f:
        f.write("[Wallet]\nEnabled=false\n")
    with open(FALLBACK_ASKPASS, "w") as f:
        f.write(f"#!/bin/sh\necho \"$1\" >> {FALLBACK_LOG}\necho fallback-answer\n")
    os.chmod(FALLBACK_ASKPASS, 0o755)
    os.makedirs(RUNTIME, mode=0o700)
    with open(os.path.join(TREE, "sub", "f.txt"), "w") as f:
        f.write("hello\n")
    # Local file the remote must never reach through a symlink.
    with open(os.path.join(TMP, "secret.txt"), "w") as f:
        f.write("local secret\n")

    with open(FAKE_SSH, "w") as f:
        f.write(f"""#!/bin/sh
# Stand-in for ssh as run by sshfs: logs the call and serves {TREE} as /data.
echo "$*" >> {SSH_LOG}
log=/dev/stderr
prev=
for arg in "$@"; do
    [ "$prev" = "-E" ] && log=$arg
    prev=$arg
done
# Password hosts: like ssh, ask through SSH_ASKPASS (3 tries); only
# alice/secret logs in. keyhost: a key passphrase.
prev=
for arg in "$@"; do
    case "$arg" in *pwhost*) login=$arg;; *keyhost*) key=1;; esac
    [ "$prev" = "-l" ] && user=$arg
    prev=$arg
done
if [ -n "$login" ]; then
    case "$login" in *@*) ;; *) login="${{user:-$(id -un)}}@$login";; esac
    ok=
    for i in 1 2 3; do
        echo "askpass-call $login" >> {SSH_LOG}
        answer=$("$SSH_ASKPASS" "$login's password: ") || break
        case "$login" in alice@*) [ "$answer" = secret ] && ok=1 && break;; esac
        echo "Permission denied, please try again." >> "$log"
    done
    [ -n "$ok" ] || {{ echo "$login: Permission denied (publickey,password)." >> "$log"; exit 255; }}
fi
if [ -n "$key" ]; then
    answer=$("$SSH_ASKPASS" "Enter passphrase for key '/home/alice/.ssh/id_ed25519': ") || exit 255
    [ "$answer" = fallback-answer ] || {{ echo "keyhost: Permission denied (publickey)." >> "$log"; exit 255; }}
fi
case " $* " in
    *" badhost "*) echo "ssh: connect to host badhost port 22: Connection refused" >> "$log"; exit 255;;
    *" badagent "*) echo "ssh: connect to host badagent port 22: Connection refused" >&2; exit 255;;
    *" flakyhost "*)
        if [ -e {TMP}/flaky-down ]; then
            sleep 1
            echo "ssh: connect to host flakyhost port 22: Connection timed out" >> "$log"
            exit 255
        fi
        echo $$ > {TMP}/flakyhost.pid;;
    *" slowhost "*) sleep 1.5;;
esac
case " $* " in
    *" -T "*)
        # A remote command (Kate Code's agent): run by the login shell
        # (sh), with a home of its own.
        # Remote forwards: the sandbox shares our network, so the host's
        # port is the local one.
        prev=
        for arg in "$@"; do
            [ "$prev" = "-R" ] && echo "Allocated port ${{arg##*:}} for remote forward to 127.0.0.1:${{arg##*:}}" >&2
            prev=$arg
            last=$arg
        done
        exec bwrap --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib /lib64 --symlink usr/bin /bin \\
            --ro-bind /etc/passwd /etc/passwd --ro-bind /etc/group /etc/group \\
            --bind {TREE} /data --bind {AGENT_HOME} /agent-home --dev /dev --proc /proc --tmpfs /tmp --chdir / \\
            --die-with-parent --setenv HOME /agent-home --setenv SHELL /bin/sh /bin/sh -c "$last";;
esac
exec bwrap --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib /lib64 --symlink usr/bin /bin \\
    --ro-bind /etc/passwd /etc/passwd --ro-bind /etc/group /etc/group \\
    --bind {TREE} /data --dev /dev --proc /proc --chdir / --die-with-parent {SFTP_SERVER}
""")
    os.chmod(FAKE_SSH, 0o755)

    # KIO's fish worker runs "ssh ... host <command>": run the command in the
    # same sandbox (the tree as /data).
    os.makedirs(KIO_BIN)
    with open(os.path.join(KIO_BIN, "ssh"), "w") as f:
        f.write(f"""#!/bin/sh
echo "$*" >> {KIO_SSH_LOG}
case " $* " in *" badfish "*) echo "ssh: connect to host badfish port 22: Connection refused" >&2; exit 255;; esac
for arg in "$@"; do last=$arg; done
exec bwrap --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib /lib64 --symlink usr/bin /bin \\
    --ro-bind /etc/passwd /etc/passwd --ro-bind /etc/group /etc/group \\
    --bind {TREE} /data --dev /dev --proc /proc --tmpfs /tmp --chdir / --die-with-parent /bin/sh -c "$last"
""")
    os.chmod(os.path.join(KIO_BIN, "ssh"), 0o755)

    # An ACP agent for the remote, in ~/.local/bin there: logs what it gets
    # in its home, reads a file through the client, reports a tool call
    # with paths.
    agent = os.path.join(AGENT_HOME, ".local", "bin", "fake-acp")
    os.makedirs(os.path.dirname(agent))
    with open(agent, "w") as f:
        f.write("""#!/usr/bin/python3
import json, os, sys, urllib.request, urllib.error
log = open("/agent-home/agent.log", "a")
def mcp(server, message, token=None):
    headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
    for header in server.get("headers", []):
        headers[header["name"]] = header["value"]
    if token is not None:
        headers["Authorization"] = token
    request = urllib.request.Request(server["url"], json.dumps(message).encode(), headers)
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            body = response.read()
            return response.status, json.loads(body) if body else None
    except urllib.error.HTTPError as e:
        return e.code, None
def send(message):
    sys.stdout.write(json.dumps(message) + "\\n")
    sys.stdout.flush()
print("noise from a login profile")
sys.stdout.flush()
print("agent stderr", file=sys.stderr)
cwd = None
for line in sys.stdin:
    message = json.loads(line)
    log.write(json.dumps(message) + "\\n")
    log.flush()
    method = message.get("method")
    if method == "initialize":
        send({"jsonrpc": "2.0", "id": message["id"], "result": {"protocolVersion": 1, "agentCapabilities": {}}})
    elif method == "session/new":
        cwd = message["params"]["cwd"]
        for server in message["params"]["mcpServers"]:
            calls = [mcp(server, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18"}}),
                     mcp(server, {"jsonrpc": "2.0", "method": "notifications/initialized"}),
                     mcp(server, {"jsonrpc": "2.0", "id": 2, "method": "tools/list"}),
                     mcp(server, {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "katecode_read", "arguments": {"file_path": "rel.txt"}}}),
                     mcp(server, {"jsonrpc": "2.0", "id": 4, "method": "ping"}, token="Bearer wrong")]
            log.write(json.dumps({"mcp": calls}) + "\\n")
            log.flush()
        send({"jsonrpc": "2.0", "id": message["id"], "result": {"sessionId": "s1"}})
    elif method == "session/prompt":
        send({"jsonrpc": "2.0", "id": 900, "method": "fs/read_text_file", "params": {"sessionId": "s1", "path": cwd + "/f.txt"}})
        reply = json.loads(sys.stdin.readline())
        log.write(json.dumps(reply) + "\\n")
        log.flush()
        send({"jsonrpc": "2.0", "method": "session/update", "params": {"sessionId": "s1", "update": {
            "sessionUpdate": "tool_call", "toolCallId": "t1", "title": "Read " + cwd + "/f.txt", "kind": "read",
            "locations": [{"path": cwd + "/f.txt"}], "rawInput": {"file_path": cwd + "/f.txt"}}}})
        send({"jsonrpc": "2.0", "method": "session/update", "params": {"sessionId": "s1", "update": {
            "sessionUpdate": "agent_message_chunk", "content": {"type": "text", "text": "pwd=" + os.getcwd()}}}})
        send({"jsonrpc": "2.0", "id": message["id"], "result": {"stopReason": "end_turn"}})
""")
    os.chmod(agent, 0o755)

    with open(os.path.join(CONFIG, "kate-remoterc"), "w") as f:
        f.write(f"""[General]
MountRoot={ROOT}
IdleTimeout=3
RetryAfterFailure=2
MountTimeout=20
SshCommand={FAKE_SSH}
""")


def register_hosts():
    print("registering hosts")
    check(os.path.exists(HOSTS_FILE) is False, "no hosts before registering")
    for host in ("goodhost", "badhost", "slowhost", "flakyhost"):
        check(dbus("addHost", "sftp", host) is True, f"addHost {host}")


def test_listing_and_read():
    print("listing and reading")
    check({"sftp", "fish", "smb"} <= set(os.listdir(ROOT)), "root lists the schemes")
    check(os.listdir(os.path.join(ROOT, "sftp")) == [], "no hosts listed before use")
    check(dbus("mountRoot") == ROOT, "DBus mountRoot")
    check(re.fullmatch(r"\d+\.\d+\.\d+(-[0-9a-f]+)?", dbus("version")) is not None, "DBus version")
    host = os.path.join(ROOT, "sftp", "goodhost")
    with open(os.path.join(host, "data", "sub", "f.txt")) as f:
        check(f.read() == "hello\n", "file read through the mount")
    check(backend_mounted("goodhost"), "sshfs backend mounted")
    check(os.listdir(os.path.join(ROOT, "sftp")) == ["goodhost"], "connected host listed")
    check(dbus("mounts") == ["sftp/goodhost"], "DBus mounts")
    check(os.stat(host).st_uid == os.getuid(), "host root owned by us (idmap=user)")


def test_write_operations():
    print("write operations")
    data = os.path.join(ROOT, "sftp", "goodhost", "data")
    path = os.path.join(data, "new.txt")
    with open(path, "w") as f:
        f.write("abc")
    with open(path, "a") as f:
        f.write("def")
    check(open(os.path.join(TREE, "new.txt")).read() == "abcdef", "create and append reach the remote")
    os.truncate(path, 2)
    check(open(os.path.join(TREE, "new.txt")).read() == "ab", "truncate")
    os.chmod(path, 0o640)
    check(os.stat(os.path.join(TREE, "new.txt")).st_mode & 0o777 == 0o640, "chmod")
    os.utime(path, (1000000000, 1000000000))
    check(int(os.stat(os.path.join(TREE, "new.txt")).st_mtime) == 1000000000, "utime")
    os.rename(path, os.path.join(data, "sub", "moved.txt"))
    check(os.path.exists(os.path.join(TREE, "sub", "moved.txt")) and not os.path.exists(os.path.join(TREE, "new.txt")),
          "rename")
    os.mkdir(os.path.join(data, "dir"))
    check(os.path.isdir(os.path.join(TREE, "dir")), "mkdir")
    os.rmdir(os.path.join(data, "dir"))
    check(not os.path.exists(os.path.join(TREE, "dir")), "rmdir")
    os.symlink("sub/f.txt", os.path.join(data, "made-link"))
    check(os.readlink(os.path.join(TREE, "made-link")) == "sub/f.txt", "symlink created on the remote")
    os.unlink(os.path.join(data, "made-link"))
    os.unlink(os.path.join(data, "sub", "moved.txt"))
    check(sorted(os.listdir(data)) == ["sub"], "unlink")
    # Writes keep working on an open file after unlinking it.
    keep = os.path.join(data, "keep.txt")
    with open(keep, "w") as f:
        os.unlink(keep)
        f.write("x")
        f.flush()
    check(not os.path.exists(os.path.join(TREE, "keep.txt")), "unlinked open file")
    # Names that cannot be hosts: refused without connecting.
    for path in (os.path.join(ROOT, "newscheme"), os.path.join(ROOT, "sftp", ".hidden")):
        try:
            os.mkdir(path)
            check(False, f"mkdir {os.path.relpath(path, ROOT)} refused")
        except OSError as e:
            check(e.errno == errno.EACCES, f"mkdir {os.path.relpath(path, ROOT)} refused")
    try:
        open(os.path.join(ROOT, "sftp", ".newfile"), "w").close()
        check(False, "creating a file in sftp/ refused")
    except OSError as e:
        check(e.errno == errno.EACCES, "creating a file in sftp/ refused")
    # A host name is the host's directory (looking it up connects).
    try:
        os.mkdir(os.path.join(ROOT, "sftp", "goodhost"))
        check(False, "mkdir of a host directory refused")
    except OSError as e:
        check(e.errno in (errno.EEXIST, errno.EACCES), "mkdir of a host directory refused")


def test_symlinks():
    print("symlinks")
    data = os.path.join(ROOT, "sftp", "goodhost", "data")
    host = os.path.join(ROOT, "sftp", "goodhost")
    os.symlink("sub/f.txt", os.path.join(TREE, "rel"))
    os.symlink("/data/sub/f.txt", os.path.join(TREE, "abs"))
    os.symlink("../" * 12 + "data/sub/f.txt", os.path.join(TREE, "sub", "escape"))
    os.symlink("../" * 20 + TMP.lstrip("/") + "/secret.txt", os.path.join(TREE, "evil"))
    check(os.readlink(os.path.join(data, "rel")) == "sub/f.txt", "inside link unchanged")
    check(os.readlink(os.path.join(data, "abs")) == "../data/sub/f.txt", "absolute link made relative to the host")
    check(open(os.path.join(data, "abs")).read() == "hello\n", "absolute link resolves on the remote")
    check(open(os.path.join(data, "sub", "escape")).read() == "hello\n", "'..' above the remote root is clamped")
    check(os.path.realpath(os.path.join(data, "sub", "escape")).startswith(host + "/"), "link stays in the host")
    try:
        content = open(os.path.join(data, "evil")).read()
        check("local secret" not in content, "no local file through a link")
    except OSError as e:
        check(e.errno == errno.ENOENT, "no local file through a link")
    for name in ("rel", "abs", "evil"):
        os.unlink(os.path.join(TREE, name))
    os.unlink(os.path.join(TREE, "sub", "escape"))


def test_invalid_names():
    print("invalid names")
    for path in (os.path.join(ROOT, ".git"), os.path.join(ROOT, ".kateproject"), os.path.join(ROOT, "gopher"),
                 os.path.join(ROOT, "sftp", ".git"), os.path.join(ROOT, "sftp", "-oProxyCommand=true"),
                 os.path.join(ROOT, "sftp", "a b"), os.path.join(ROOT, "sftp", "host;true")):
        try:
            os.stat(path)
            check(False, f"{os.path.relpath(path, ROOT)} does not exist")
        except OSError as e:
            check(e.errno == errno.ENOENT, f"{os.path.relpath(path, ROOT)} does not exist")
    # Valid host names that are not registered: names tools probe for in
    # every parent directory.
    for name in ("HEAD", "Cargo.toml", "README.md", "setup.py", "newhost"):
        try:
            os.stat(os.path.join(ROOT, "sftp", name))
            check(False, f"unregistered sftp/{name} does not exist")
        except OSError as e:
            check(e.errno == errno.ENOENT, f"unregistered sftp/{name} does not exist")
    os.makedirs(os.path.join(TREE, "nogit", "a"))
    subprocess.run(["git", "status"], cwd=os.path.join(ROOT, "sftp", "goodhost", "data", "nogit", "a"),
                   capture_output=True, env=dict(os.environ, GIT_OPTIONAL_LOCKS="0"))
    shutil.rmtree(os.path.join(TREE, "nogit"))
    log = open(SSH_LOG).read() if os.path.exists(SSH_LOG) else ""
    check("ProxyCommand" not in log and ".git" not in log and "a b" not in log, "no ssh for invalid names")
    check(not any(f" {n} " in f" {line} " for line in log.splitlines()
                  for n in ("HEAD", "Cargo.toml", "README.md", "setup.py", "newhost")),
          "no ssh for unregistered names (incl. git status below a host)")
    try:
        os.mkdir(os.path.join(ROOT, "sftp", "newdir"))
        check(False, "mkdir of an unregistered host refused")
    except OSError as e:
        check(e.errno == errno.EACCES, "mkdir of an unregistered host refused")
    check(ssh_calls("newdir") == 0, "no connection for mkdir")


def test_host_registry():
    print("host registry")
    check(sorted(dbus("hosts")) == ["sftp/badhost", "sftp/flakyhost", "sftp/goodhost", "sftp/slowhost"], "DBus hosts")
    check(dbus("addHost", "sftp", "-bad") is False, "invalid host refused")
    check(dbus("addHost", "sftp", "newhost") is True, "addHost")
    check("sftp/newhost" in open(HOSTS_FILE).read().split(), "added host saved in the state file")
    check("Hosts" not in open(os.path.join(CONFIG, "kate-remoterc")).read(), "user config left alone")
    with open(os.path.join(ROOT, "sftp", "newhost", "data", "sub", "f.txt")) as f:
        check(f.read() == "hello\n", "added host reachable")
    check(dbus("removeHost", "sftp", "newhost") is True, "removeHost")
    check(not os.path.exists(os.path.join(ROOT, "sftp", "newhost")), "removed host gone")
    check(not backend_mounted("newhost"), "removed host disconnected")
    check("sftp/newhost" not in open(HOSTS_FILE).read().split(), "removed host dropped from the state file")


def kpasswd(*args):
    return subprocess.run([KPASSWDTOOL, *args], env=env(), capture_output=True, text=True, timeout=60).returncode


def askpass_calls(login):
    if not os.path.exists(SSH_LOG):
        return 0
    return sum(1 for line in open(SSH_LOG) if line.strip() == f"askpass-call {login}")


def read_host(authority):
    try:
        with open(os.path.join(ROOT, "sftp", authority, "data", "sub", "f.txt")) as f:
            return f.read()
    except OSError as e:
        return errno.errorcode.get(e.errno, str(e.errno))


def test_kde_passwords():
    print("passwords from KDE's password cache")
    if not KPASSWDTOOL:
        check(False, "kpasswdtool given")
        return
    me = subprocess.run(["id", "-un"], capture_output=True, text=True).stdout.strip()
    for host in ("alice@pwhost", "pwhost", "alice@pwhost2", "alice@pwhost3", "keyhost"):
        dbus("addHost", "sftp", host)

    check(read_host("alice@pwhost3") == "EHOSTUNREACH", "no cached password: login fails (no dialog in tests)")
    check("Permission denied" in dbus("lastError", "sftp", "alice@pwhost3"), "lastError has ssh's message")
    check(askpass_calls("alice@pwhost3") == 1, "ssh stops asking once askpass gives up")

    # As KIO stores it when the username was typed into its login dialog:
    # under the URL without user.
    check(kpasswd("add", "sftp://pwhost", "alice", "secret") == 0, "password cached as KIO would")
    check(read_host("alice@pwhost") == "hello\n", "cached password used for the mount")
    check(askpass_calls("alice@pwhost") == 1, "answered on the first prompt")
    # The same entry is alice's: not used when ssh logs in as another user.
    check(read_host("pwhost") == "EHOSTUNREACH", "another user's cached password not used")
    check(askpass_calls(f"{me}@pwhost") == 1, "asked once for the local user")

    # A wrong cached password is used once; asked again, the cache is skipped.
    check(kpasswd("add", "sftp://alice@pwhost2", "alice", "wrong") == 0, "wrong password cached")
    check(read_host("alice@pwhost2") == "EHOSTUNREACH", "wrong cached password fails")
    check(askpass_calls("alice@pwhost2") == 2, "cache skipped when ssh asks again")
    check(kpasswd("add", "sftp://alice@pwhost2", "alice", "secret") == 0, "password corrected")
    dbus("unmount", "sftp", "alice@pwhost2")
    check(read_host("alice@pwhost2") == "hello\n", "next attempt uses the cache again")

    # Asked again after a wrong cached password: the entry is dropped and
    # KDE's dialog asks (a new client's query would otherwise get the cached
    # entry back without a dialog). Run the helper directly, with dialogs.
    check(kpasswd("add", "sftp://alice@pwhost9", "alice", "wrong") == 0, "wrong password cached")
    helper = os.path.join(os.path.dirname(DAEMON), "kate-remote-fs-askpass")
    state = os.path.join(TMP, "askpass-state")
    e = env()
    del e["KATE_REMOTE_ASKPASS_NO_DIALOG"]
    e.update(KATE_REMOTE_AUTHORITY="alice@pwhost9", KATE_REMOTE_ASKPASS_STATE=state, KATE_REMOTE_FALLBACK_ASKPASS="")
    first = subprocess.run([helper, "alice@pwhost9's password: "], env=e, capture_output=True, text=True, timeout=30)
    check(first.returncode == 0 and first.stdout.strip() == "wrong", "first prompt answered from the cache")
    try:
        second = subprocess.run([helper, "alice@pwhost9's password: "], env=e, capture_output=True, text=True, timeout=5)
        check(False, f"asked again: dialog instead of the cached password (got {second.returncode} {second.stdout.strip()!r})")
    except subprocess.TimeoutExpired:
        check(True, "asked again: dialog instead of the cached password")
    check(kpasswd("check", "sftp://alice@pwhost9", "alice") != 0, "wrong password removed from the cache")

    # Other prompts go to the usual askpass.
    check(read_host("keyhost") == "hello\n", "key passphrase from the fallback askpass")
    check("Enter passphrase" in (open(FALLBACK_LOG).read() if os.path.exists(FALLBACK_LOG) else ""),
          "passphrase prompt passed to the fallback")


def on_terminal(argv, env, typed=None, timeout=20):
    """Runs argv on a new terminal; types `typed` (a line) once the prompt
    (argv[1]) shows. Returns (exit code, terminal output)."""
    pid, fd = pty.fork()
    if pid == 0:
        try:
            os.execve(argv[0], argv, env)
        finally:
            os._exit(127)
    out = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.2)
        if not ready:
            continue
        try:
            data = os.read(fd, 1024)
        except OSError:
            break
        if not data:
            break
        out += data
        if typed is not None and argv[1].encode() in out:
            os.write(fd, typed.encode() + b"\n")
            typed = None
    else:
        os.kill(pid, signal.SIGKILL)
    os.close(fd)
    _, status = os.waitpid(pid, 0)
    return os.waitstatus_to_exitcode(status), out.decode(errors="replace")


def test_terminal_askpass():
    print("askpass for the Remote Terminal (KATE_REMOTE_ASKPASS_TTY)")
    helper = os.path.join(os.path.dirname(DAEMON), "kate-remote-fs-askpass")
    state = os.path.join(TMP, "terminal-askpass-state")
    e = env()
    del e["KATE_REMOTE_ASKPASS_NO_DIALOG"]  # never a dialog in this mode
    e.update(KATE_REMOTE_ASKPASS_TTY="1", KATE_REMOTE_ASKPASS_STATE=state, KATE_REMOTE_FALLBACK_ASKPASS="")

    # Cached: answered without a prompt.
    check(kpasswd("add", "sftp://alice@termhost", "alice", "cached-pw") == 0, "password cached")
    e["KATE_REMOTE_AUTHORITY"] = "alice@termhost"
    code, out = on_terminal([helper, "alice@termhost's password: "], e)
    check(code == 0 and out.strip() == "cached-pw", f"cached password answered silently ({code} {out!r})")
    check(os.path.exists(state), "cached answer marked for this attempt")

    # Asked again in the same attempt (refused): asked on the terminal,
    # without echo; the cache is left alone.
    code, out = on_terminal([helper, "alice@termhost's password: "], e, typed="typed-pw")
    check(code == 0, f"refused password: asked on the terminal ({code} {out!r})")
    check("not accepted" in out, "says the saved password was refused")
    check(out.count("typed-pw") == 1 and out.rstrip().endswith("typed-pw"), f"typed password not echoed, then answered ({out!r})")
    check(kpasswd("check", "sftp://alice@termhost", "alice") == 0, "cache left alone")
    os.remove(state)

    # Nothing cached: asked on the terminal, no dialog.
    e["KATE_REMOTE_AUTHORITY"] = "alice@termhost2"
    code, out = on_terminal([helper, "alice@termhost2's password: "], e, typed="typed2", timeout=10)
    check(code == 0 and out.count("typed2") == 1 and "not accepted" not in out, f"no cached password: asked on the terminal ({code} {out!r})")
    check(not os.path.exists(state), "typed answers are not marked")

    # Host keys: on the terminal, echoed like ssh's own question.
    code, out = on_terminal([helper, "Are you sure you want to continue connecting (yes/no/[fingerprint])? "], e, typed="yes")
    check(code == 0 and out.count("yes\r\n") == 2, f"yes/no asked with echo ({code} {out!r})")
    # Passphrases: without echo; end of input fails.
    code, out = on_terminal([helper, "Enter passphrase for key '/home/alice/.ssh/id_ed25519': "], e, typed="phrase")
    check(code == 0 and out.count("phrase\r\n") == 1, f"passphrase asked without echo ({code} {out!r})")
    code, out = on_terminal([helper, "Verification code: "], e, typed="\x04")
    check(code == 1, f"end of input: no answer ({code} {out!r})")


def run_agent(authority, path, program="fake-acp", mcp_server=None):
    """Kate Code's ACPService for <root>/sftp/<authority><path>; returns
    (exit code, [messages], [errors], output)."""
    folder = os.path.join(ROOT, "sftp", authority) + path
    e = env()
    if mcp_server:
        e["ACPDRIVER_MCP_SERVER"] = mcp_server
    result = subprocess.run([ACPDRIVER, folder, program], env=e, capture_output=True, text=True, timeout=60)
    messages, errors = [], []
    for line in result.stdout.splitlines():
        if line.startswith("MSG "):
            messages.append(__import__("json").loads(line[4:]))
        elif line.startswith("ERROR "):
            errors.append(line[6:])
    return result.returncode, messages, errors, result.stdout + result.stderr


def agent_log():
    import json
    if not os.path.exists(AGENT_LOG):
        return []
    with open(AGENT_LOG) as f:
        return [json.loads(line) for line in f]


def test_remote_agent():
    print("Kate Code agents on the remote host (ACPService over ssh)")
    if not ACPDRIVER:
        check(False, "acpdriver given")
        return
    local = os.path.join(ROOT, "sftp", "goodhost") + "/data/sub"
    code, messages, errors, output = run_agent("goodhost", "/data/sub")
    check(code == 0 and "DONE" in output, f"agent ran on the host ({code}): {output}")
    check("REMOTE goodhost" in output, "agent marked remote")
    received = agent_log()
    new = next((m for m in received if m.get("method") == "session/new"), {})
    check(new.get("params", {}).get("cwd") == "/data/sub", f"agent got the remote cwd: {new}")
    prompt = next((m for m in received if m.get("method") == "session/prompt"), {})
    text = prompt.get("params", {}).get("prompt", [{}])[0].get("text")
    check(text == "Look at /data/sub/f.txt", f"paths in the prompt are remote: {text!r}")
    reply = next((m for m in received if m.get("id") == 900), {})
    check(reply.get("result", {}).get("content") == f"content mentions {local}/f.txt", f"file contents not translated: {reply}")
    read = next((m for m in messages if m.get("method") == "fs/read_text_file"), {})
    check(read.get("params", {}).get("path") == local + "/f.txt", f"fs request path is local: {read}")
    tool = next((m["params"]["update"] for m in messages if m.get("method") == "session/update"
                 and m["params"]["update"].get("sessionUpdate") == "tool_call"), {})
    check(tool.get("locations") == [{"path": local + "/f.txt"}], f"tool location is local: {tool}")
    check(tool.get("rawInput", {}).get("file_path") == local + "/f.txt", "tool input path is local")
    check(tool.get("title") == "Read /data/sub/f.txt", f"title left as the agent wrote it: {tool.get('title')!r}")
    chunk = next((m["params"]["update"]["content"]["text"] for m in messages if m.get("method") == "session/update"
                  and m["params"]["update"].get("sessionUpdate") == "agent_message_chunk"), "")
    check(chunk == "pwd=/data/sub", f"agent runs in the folder: {chunk!r}")
    check(not errors, f"no errors: {errors}")

    # Login password from KDE's password cache (cached for alice in
    # test_kde_passwords), as for the mounts.
    before = askpass_calls("alice@pwhost")
    code, _, errors, output = run_agent("alice@pwhost", "/data")
    check(code == 0 and "DONE" in output, f"agent on a password host with the cached password ({code}): {output}")
    check(askpass_calls("alice@pwhost") == before + 1, "ssh asked once, answered from the cache")

    # Kate's MCP server, through the (fake) ssh remote forward.
    os.remove(AGENT_LOG)
    mcp_server = os.path.join(os.path.dirname(ACPDRIVER), "kate-mcp-server")
    code, _, errors, output = run_agent("goodhost", "/data/sub", mcp_server=mcp_server)
    check(code == 0 and "MCP http://127.0.0.1:" in output, f"MCP server forwarded ({code}): {output}")
    calls = next((m["mcp"] for m in agent_log() if "mcp" in m), None)
    check(calls is not None, "agent got Kate's MCP server")
    if calls:
        check(calls[0][0] == 200 and calls[0][1]["result"]["protocolVersion"] == "2025-06-18", f"MCP initialize over HTTP: {calls[0]}")
        check(calls[1][0] == 202, "MCP notification accepted")
        tools = [t["name"] for t in calls[2][1]["result"]["tools"]] if calls[2][1] else []
        check("katecode_read" in tools and "katecode_paste_to_terminal" not in tools, f"remote tool list: {tools}")
        check("absolute path on the agent's host" in json.dumps(calls[3][1]), f"relative paths refused: {calls[3]}")
        check(calls[4][0] == 401, f"wrong token refused: {calls[4][0]}")

    code, _, errors, output = run_agent("goodhost", "/data", "no-such-agent")
    check(code != 0 and any("no-such-agent was not found on goodhost" in e for e in errors), f"missing agent reported: {errors} {output}")
    code, _, errors, output = run_agent("badagent", "/data")
    check(code != 0 and any("ssh to badagent failed" in e and "Connection refused" in e for e in errors), f"ssh failure reported: {errors}")
    code, _, errors, output = run_agent("goodhost", "/data/missing")
    check(code != 0 and any("exited" in e for e in errors), f"missing folder reported: {errors}")


def run_install(entry, program, authority="goodhost", path="/data/sub"):
    """acpdriver --install (RemoteAgentInstaller) with the registry entry;
    returns (exit code, output)."""
    entry_file = os.path.join(TMP, "registry-entry.json")
    with open(entry_file, "w") as f:
        json.dump(entry, f)
    e = env()
    e["ACPDRIVER_DOWNLOADS"] = os.path.join(TMP, "agent-downloads")
    folder = os.path.join(ROOT, "sftp", authority) + path
    result = subprocess.run([ACPDRIVER, "--install", folder, entry_file, program], env=e, capture_output=True, text=True, timeout=60)
    return result.returncode, result.stdout + result.stderr


def test_remote_agent_install():
    print("installing an agent from the ACP registry on the remote host")
    import hashlib
    import tarfile
    import zipfile
    machine = {"x86_64": "x86_64", "amd64": "x86_64", "aarch64": "aarch64", "arm64": "aarch64"}.get(os.uname().machine)
    if not machine:
        print("  (skipped: unknown machine)")
        return
    musl = any(os.path.exists(p) for p in __import__("glob").glob("/lib/ld-musl-*"))
    platform = f"linux-{machine}" + ("-musl" if musl else "")
    with open(os.path.join(AGENT_HOME, ".local", "bin", "fake-acp")) as f:
        agent = f.read()
    work = os.path.join(TMP, "agent-archives")
    os.makedirs(work, exist_ok=True)
    # A zip (re-packed as .tar.gz before it is sent) and a .tar.gz (sent as is).
    zip_path = os.path.join(work, "fake-installed.zip")
    with zipfile.ZipFile(zip_path, "w") as z:
        info = zipfile.ZipInfo("pkg/fake-installed")
        info.external_attr = 0o755 << 16
        z.writestr(info, agent)
        z.writestr("pkg/README", "fake\n")
    tgz_path = os.path.join(work, "fake-installed-2.tar.gz")
    src = os.path.join(work, "src")
    os.makedirs(os.path.join(src, "pkg"), exist_ok=True)
    with open(os.path.join(src, "pkg", "fake-installed"), "w") as f:
        f.write(agent.replace("pwd=", "v2 pwd="))
    os.chmod(os.path.join(src, "pkg", "fake-installed"), 0o755)
    with tarfile.open(tgz_path, "w:gz") as t:
        t.add(os.path.join(src, "pkg"), arcname="pkg")

    def entry(archive, version, sha=None, plat=platform):
        with open(archive, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        return {"id": "fake-acp-agent", "name": "Fake Agent", "version": version,
                "distribution": {"binary": {plat: {"archive": "file://" + archive, "cmd": "./pkg/fake-installed",
                                                   "sha256": sha or digest}},
                                 "npx": {"package": "fake"}}}

    installed = os.path.join(AGENT_HOME, ".local", "share", "kate-code")
    code, _, errors, output = run_agent("goodhost", "/data/sub", "fake-installed")
    check(code != 0 and "MISSING" in output, f"a missing agent is reported as such: {output}")

    code, output = run_install(entry(zip_path, "1.0"), "npx")
    check("CANINSTALL 0" in output, "no offer when the provider runs another program (npx)")
    code, output = run_install(entry(zip_path, "1.0", sha="0" * 64), "fake-installed")
    check(code != 0 and "SHA-256" in output, f"a wrong SHA-256 is refused: {output}")
    check(not os.path.exists(os.path.join(installed, "agents")), "nothing reaches the host then")
    code, output = run_install(entry(zip_path, "1.0", plat="linux-riscv64"), "fake-installed")
    check(code != 0 and "no Fake Agent build for goodhost" in output, f"no build for the host's platform: {output}")

    code, output = run_install(entry(zip_path, "1.0"), "fake-installed")
    check(code == 0 and "CANINSTALL 1" in output and "Re-packing" in output and "FINISHED 1 Installed Fake Agent 1.0 on goodhost" in output,
          f"installed from a zip: {output}")
    code, _, errors, output = run_agent("goodhost", "/data/sub", "fake-installed")
    check(code == 0 and "DONE" in output and not errors, f"the installed agent runs ({code}): {errors} {output}")

    code, output = run_install(entry(tgz_path, "2.0"), "fake-installed")
    check(code == 0 and "Re-packing" not in output and "FINISHED 1" in output, f"reinstalled from a .tar.gz: {output}")
    check(os.readlink(os.path.join(installed, "bin", "fake-installed")) == "../agents/fake-acp-agent/2.0/pkg/fake-installed",
          "the launcher runs the new version")
    os.remove(AGENT_LOG)
    code, messages, errors, output = run_agent("goodhost", "/data/sub", "fake-installed")
    chunk = next((m["params"]["update"]["content"]["text"] for m in messages if m.get("method") == "session/update"
                  and m["params"]["update"].get("sessionUpdate") == "agent_message_chunk"), "")
    check(code == 0 and chunk == "v2 pwd=/data/sub", f"the new version runs: {chunk!r}")
    check(sorted(os.listdir(installed)) == ["agents", "bin"], f"no temporary files left: {os.listdir(installed)}")
    code, output = run_install(entry(tgz_path, "2.0"), "fake-installed")
    check(code == 0 and "Downloading" not in output, f"a verified download is reused: {output}")

    code, output = run_install(entry(tgz_path, "3.0"), "fake-installed", authority="badagent")
    check(code != 0 and "ssh to badagent failed" in output, f"ssh failures reported: {output}")


def kio_fuse_pids():
    pids = []
    for pid in os.listdir("/proc"):
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                cmd = f.read()
            with open(f"/proc/{pid}/environ", "rb") as f:
                environ = f.read().split(b"\0")
        except OSError:
            continue
        if b"kio-fuse" in cmd and f"XDG_RUNTIME_DIR={RUNTIME}".encode() in environ:
            pids.append(int(pid))
    return pids


def test_kio_backend():
    print("KIO schemes through kio-fuse (fish)")
    check(dbus("addHost", "sftp", "-x") is False, "still validated")
    for host in ("fakehost", "badfish"):
        check(dbus("addHost", "fish", host) is True, f"addHost fish {host}")
    host = os.path.join(ROOT, "fish", "fakehost")
    data = os.path.join(host, "data")
    check(read_host_path(os.path.join(data, "sub", "f.txt")) == "hello\n", "file read through kio-fuse")
    check("fish/fakehost" in dbus("mounts"), "fish host connected")
    check(os.listdir(os.path.join(ROOT, "fish")) == ["fakehost"], "fish/ lists the connected host")
    with open(os.path.join(data, "kio-new.txt"), "w") as f:
        f.write("via kio")
    check(wait_for(lambda: os.path.exists(os.path.join(TREE, "kio-new.txt"))
                   and open(os.path.join(TREE, "kio-new.txt")).read() == "via kio"), "write reaches the remote")
    os.mkdir(os.path.join(data, "kio-dir"))
    check(os.path.isdir(os.path.join(TREE, "kio-dir")), "mkdir")
    os.rename(os.path.join(data, "kio-new.txt"), os.path.join(data, "kio-dir", "moved.txt"))
    check(os.path.exists(os.path.join(TREE, "kio-dir", "moved.txt")), "rename")
    os.unlink(os.path.join(data, "kio-dir", "moved.txt"))
    os.rmdir(os.path.join(data, "kio-dir"))
    check(not os.path.exists(os.path.join(TREE, "kio-dir")), "unlink and rmdir")
    check(read_host_path(os.path.join(ROOT, "fish", "other", "data")) == "ENOENT", "unregistered fish host does not exist")

    check(read_host_path(os.path.join(ROOT, "fish", "badfish", "data")) == "EHOSTUNREACH", "unreachable fish host fails")
    check(dbus("lastError", "fish", "badfish") != "", "lastError explains")

    check(dbus("unmount", "fish", "fakehost") is True, "unmount (releases kio-fuse)")
    check(read_host_path(os.path.join(data, "sub", "f.txt")) == "hello\n", "mounted again on access")

    pids = kio_fuse_pids()
    check(len(pids) == 1, "kio-fuse runs (started over DBus)")
    for pid in pids:
        os.kill(pid, signal.SIGKILL)
    time.sleep(0.5)
    check(read_host_path(os.path.join(data, "sub", "f.txt")) == "hello\n", "kio-fuse restarted, host remounted")


def start_webdav_server():
    """A minimal WebDAV server on 127.0.0.1 with shares at /dav/ and
    /deep/er/dav/ while "/" and anything else is 404, like Seafile's
    /seafdav or Nextcloud's /remote.php/dav/files/<user>/. Enough for KIO's
    webdav worker. server.requests counts the requests."""
    import http.server
    import email.utils
    import urllib.parse
    from xml.sax.saxutils import escape

    for tree in (DAV_TREE, DAV_DEEP):
        os.makedirs(os.path.join(tree, "sub"), exist_ok=True)
        with open(os.path.join(tree, "sub", "f.txt"), "w") as f:
            f.write("dav hello\n")
    shares = {"/deep/er/dav": DAV_DEEP, "/dav": DAV_TREE}

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def local(self):
            path = urllib.parse.unquote(urllib.parse.urlsplit(self.path).path)
            for prefix, tree in shares.items():
                if path == prefix or path.startswith(prefix + "/"):
                    parts = [p for p in path[len(prefix):].split("/") if p]
                    return None if ".." in parts else os.path.join(tree, *parts)
            return None

        def parse_request(self):
            server.requests += 1
            return super().parse_request()

        def send(self, code, body=b"", headers=()):
            self.send_response(code)
            for name, value in headers:
                self.send_header(name, value)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def body(self):
            length = int(self.headers.get("Content-Length") or 0)
            return self.rfile.read(length) if length else b""

        def do_OPTIONS(self):
            self.send(200, headers=[("DAV", "1, 2"), ("Allow", "OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, MOVE, PROPFIND")])

        def entry(self, href, path):
            st = os.stat(path)
            is_dir = os.path.isdir(path)
            modified = email.utils.formatdate(st.st_mtime, usegmt=True)
            props = f"<D:resourcetype>{'<D:collection/>' if is_dir else ''}</D:resourcetype><D:getlastmodified>{modified}</D:getlastmodified>"
            if not is_dir:
                props += f"<D:getcontentlength>{st.st_size}</D:getcontentlength><D:getcontenttype>text/plain</D:getcontenttype>"
            return (f"<D:response><D:href>{escape(urllib.parse.quote(href))}</D:href><D:propstat><D:prop>{props}</D:prop>"
                    f"<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>")

        def do_PROPFIND(self):
            self.body()
            path = self.local()
            if path is None or not os.path.exists(path):
                return self.send(404)
            href = urllib.parse.unquote(urllib.parse.urlsplit(self.path).path)
            if os.path.isdir(path) and not href.endswith("/"):
                href += "/"
            responses = [self.entry(href, path)]
            if os.path.isdir(path) and self.headers.get("Depth", "infinity") != "0":
                for name in sorted(os.listdir(path)):
                    child = os.path.join(path, name)
                    responses.append(self.entry(href + name + ("/" if os.path.isdir(child) else ""), child))
            xml = '<?xml version="1.0" encoding="utf-8"?><D:multistatus xmlns:D="DAV:">' + "".join(responses) + "</D:multistatus>"
            self.send(207, xml.encode(), [("Content-Type", 'application/xml; charset="utf-8"')])

        def do_GET(self):
            path = self.local()
            if path is None or not os.path.isfile(path):
                return self.send(404)
            with open(path, "rb") as f:
                self.send(200, f.read(), [("Content-Type", "text/plain")])

        do_HEAD = do_GET

        def do_PUT(self):
            data = self.body()
            path = self.local()
            if path is None or not os.path.isdir(os.path.dirname(path)):
                return self.send(409 if path else 404)
            existed = os.path.exists(path)
            with open(path, "wb") as f:
                f.write(data)
            self.send(204 if existed else 201)

        def do_DELETE(self):
            path = self.local()
            if path is None or not os.path.exists(path):
                return self.send(404)
            shutil.rmtree(path) if os.path.isdir(path) else os.unlink(path)
            self.send(204)

        def do_MKCOL(self):
            self.body()
            path = self.local()
            if path is None:
                return self.send(403)
            if os.path.exists(path):
                return self.send(405)
            os.mkdir(path)
            self.send(201)

        def do_MOVE(self):
            path = self.local()
            destination = self.headers.get("Destination", "")
            self.path = urllib.parse.urlsplit(destination).path
            target = self.local()
            if path is None or target is None or not os.path.exists(path):
                return self.send(404)
            existed = os.path.exists(target)
            os.replace(path, target)
            self.send(204 if existed else 201)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.requests = 0
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def test_kio_folders():
    print("KIO hosts mounted from their folders (WebDAV shares below an unreadable /)")
    server = start_webdav_server()
    try:
        authority = f"127.0.0.1:{server.server_address[1]}"
        check(dbus("addHost", "webdav", authority) is True, "addHost webdav")
        host = os.path.join(ROOT, "webdav", authority)
        share = os.path.join(host, "dav")
        check(read_host_path(os.path.join(share, "sub", "f.txt")) == "EHOSTUNREACH", "without folders the host is mounted from /, which is 404")
        check(dbus("lastError", "webdav", authority) != "", "lastError explains")

        check(dbus("addFolder", "webdav", authority, "/dav/missing") != "", "addFolder: a missing folder fails")
        check(dbus("addFolder", "webdav", authority, "/dav") == "", "addFolder")
        check(read_host_path(os.path.join(share, "sub", "f.txt")) == "dav hello\n", "file read in the share")
        check(sorted(os.listdir(host)) == ["dav"], "the host lists only the share")
        check(sorted(os.listdir(share)) == ["sub"], "the share lists its files")
        with open(os.path.join(share, "sub", "new.txt"), "w") as f:
            f.write("via dav")
        check(wait_for(lambda: os.path.exists(os.path.join(DAV_TREE, "sub", "new.txt"))
                       and open(os.path.join(DAV_TREE, "sub", "new.txt")).read() == "via dav"), "write reaches the share")
        before = server.requests
        check(read_host_path(os.path.join(host, "HEAD")) == "ENOENT", "other names on the host do not exist")
        check(server.requests == before, "without asking the server")

        # Two unreadable folders above the share: kio-fuse walks to it.
        deep = os.path.join(host, "deep", "er", "dav", "sub")
        check(read_host_path(os.path.join(deep, "f.txt")) == "ENOENT", "an unregistered share is not there")
        check(dbus("addFolder", "webdav", authority, "/deep/er/dav/sub") == "", "addFolder while connected")
        check(read_host_path(os.path.join(deep, "f.txt")) == "dav hello\n", "file read in the deeper share")
        check(sorted(os.listdir(host)) == ["dav", "deep"], "the host lists both shares")
        check(sorted(os.listdir(os.path.join(host, "deep"))) == ["er"], "folders above a share list the way to it")
        check(dbus("addFolder", "webdav", authority, "/dav/sub") == "", "addFolder inside a registered folder")
        with open(HOSTS_FILE) as f:
            entries = f.read().split()
        check(sorted(e for e in entries if e.startswith("webdav/")) ==
              [f"webdav/{authority}", f"webdav/{authority}/dav", f"webdav/{authority}/deep/er/dav/sub"],
              "registered folders are kept in the hosts file (not missing or nested ones)")

        check(dbus("unmount", "webdav", authority) is True, "unmount")
        check(read_host_path(os.path.join(deep, "f.txt")) == "dav hello\n", "connecting again mounts the registered folders")
        check(stop_daemon() == 0, "daemon stopped")
        start_daemon()
        check(read_host_path(os.path.join(share, "sub", "f.txt")) == "dav hello\n"
              and read_host_path(os.path.join(deep, "f.txt")) == "dav hello\n", "registered folders kept across restarts")
        check(dbus("removeHost", "webdav", authority) is True, "removeHost")
        with open(HOSTS_FILE) as f:
            check("webdav/" not in f.read(), "removeHost forgets its folders")
    finally:
        server.shutdown()
        server.server_close()

def read_host_path(path):
    try:
        if os.path.isdir(path):
            os.listdir(path)
            return "dir"
        with open(path) as f:
            return f.read()
    except OSError as e:
        return errno.errorcode.get(e.errno, str(e.errno))


def test_network_drop():
    print("network drop under a live mount")
    path = os.path.join(ROOT, "sftp", "flakyhost", "data", "sub", "f.txt")
    open(path).read()
    pid = int(open(os.path.join(TMP, "flakyhost.pid")).read())
    open(os.path.join(TMP, "flaky-down"), "w").close()
    os.kill(pid, signal.SIGKILL)  # the connection drops; reconnecting fails slowly
    time.sleep(0.5)
    times = []
    for _ in range(5):
        start = time.monotonic()
        try:
            open(path).read()
            times.append(("ok", time.monotonic() - start))
        except OSError as e:
            times.append((errno.errorcode.get(e.errno, e.errno), time.monotonic() - start))
    print(f"    {times}")
    check(all(r != "ok" for r, _ in times), "accesses fail while the host is down")
    check(sum(t for _, t in times[1:]) < 0.5, "only the first access waits for the failing reconnect")
    check("timed out" in dbus("lastError", "sftp", "flakyhost"), "lastError explains")
    os.unlink(os.path.join(TMP, "flaky-down"))
    time.sleep(2.5)
    with open(path) as f:
        check(f.read() == "hello\n", "back once the host is reachable")


def test_failure_cache():
    print("connection failure")
    path = os.path.join(ROOT, "sftp", "badhost", "data")
    try:
        os.stat(path)
        check(False, "unreachable host fails")
    except OSError as e:
        check(e.errno == errno.EHOSTUNREACH, "unreachable host fails with EHOSTUNREACH")
    check("Connection refused" in dbus("lastError", "sftp", "badhost"), "DBus lastError has ssh's message")
    start = time.monotonic()
    try:
        os.stat(path)
    except OSError:
        pass
    check(ssh_calls("badhost") == 1 and time.monotonic() - start < 0.5, "failure cached (no new attempt)")
    time.sleep(2.5)
    try:
        os.stat(path)
    except OSError:
        pass
    check(ssh_calls("badhost") == 2, "retried after RetryAfterFailure")
    check(dbus("unmount", "sftp", "badhost") is True, "DBus unmount forgets the failure")
    try:
        os.stat(path)
    except OSError:
        pass
    check(ssh_calls("badhost") == 3, "retried at once after unmount")


def test_concurrent_connect():
    print("concurrent connect")
    path = os.path.join(ROOT, "sftp", "slowhost", "data", "sub", "f.txt")
    results = []

    def reader():
        try:
            with open(path) as f:
                results.append(f.read())
        except OSError as e:
            results.append(e)

    # goodhost connected now (it may have been unmounted as idle).
    open(os.path.join(ROOT, "sftp", "goodhost", "data", "sub", "f.txt")).read()
    threads = [threading.Thread(target=reader) for _ in range(8)]
    for t in threads:
        t.start()
    time.sleep(0.4)
    # Other hosts and the virtual directories do not wait for slowhost.
    start = time.monotonic()
    os.listdir(os.path.join(ROOT, "sftp"))
    listed = time.monotonic() - start
    open(os.path.join(ROOT, "sftp", "goodhost", "data", "sub", "f.txt")).read()
    read = time.monotonic() - start - listed
    check(listed < 0.5 and read < 0.5, f"other hosts not blocked by a connecting host (list {listed:.2f}s, read {read:.2f}s)")
    for t in threads:
        t.join()
    check(results == ["hello\n"] * 8, "all concurrent readers succeed")
    check(ssh_calls("slowhost") == 1, "one connection for concurrent accesses")


def test_idle_unmount():
    print("idle unmount")
    held = open(os.path.join(ROOT, "sftp", "slowhost", "data", "sub", "f.txt"))
    calls = ssh_calls("goodhost")
    check(wait_for(lambda: not backend_mounted("goodhost"), timeout=10), "idle host unmounted")
    check("sftp/goodhost" not in dbus("mounts"), "idle host not listed")
    check(backend_mounted("slowhost"), "host with an open file stays mounted")
    held.close()
    check(wait_for(lambda: not backend_mounted("slowhost"), timeout=10), "unmounted once the file is closed")
    with open(os.path.join(ROOT, "sftp", "goodhost", "data", "sub", "f.txt")) as f:
        check(f.read() == "hello\n", "reconnects on access")
    check(ssh_calls("goodhost") == calls + 1, "one new connection")


def test_backend_crash():
    print("sshfs crash")
    point = os.path.join(BACKEND, "sftp", "goodhost")
    open(os.path.join(ROOT, "sftp", "goodhost", "data", "sub", "f.txt")).read()
    pids = subprocess.run(["pgrep", "-f", f"sshfs .*{point}"], capture_output=True, text=True).stdout.split()
    check(len(pids) == 1, "found the sshfs process")
    calls = ssh_calls("goodhost")
    for pid in pids:
        os.kill(int(pid), signal.SIGKILL)
    time.sleep(0.5)
    with open(os.path.join(ROOT, "sftp", "goodhost", "data", "sub", "f.txt")) as f:
        check(f.read() == "hello\n", "transparently reconnected")
    check(ssh_calls("goodhost") == calls + 1, "one new connection after the crash")


def test_dbus_unmount():
    print("DBus unmount")
    open(os.path.join(ROOT, "sftp", "goodhost", "data", "sub", "f.txt")).read()
    check(dbus("unmount", "sftp", "goodhost") is True, "unmount")
    check(not backend_mounted("goodhost"), "backend gone")
    held = open(os.path.join(ROOT, "sftp", "goodhost", "data", "sub", "f.txt"))
    check(dbus("unmount", "sftp", "goodhost") is False, "refused while a file is open")
    held.close()


def test_shutdown_and_restart():
    print("shutdown and restart")
    open(os.path.join(ROOT, "sftp", "goodhost", "data", "sub", "f.txt")).read()
    check(stop_daemon() == 0, "SIGTERM exits cleanly")
    # (kio-fuse keeps its own mount; it is not the daemon's)
    check(mounts_under(ROOT) == [] and mounts_under(BACKEND) == [], "everything unmounted on exit")

    start_daemon()
    open(os.path.join(ROOT, "sftp", "goodhost", "data", "sub", "f.txt")).read()
    stop_daemon(signal.SIGKILL)
    check(backend_mounted("goodhost"), "crash leaves the backend mounted")
    start_daemon()
    check(not backend_mounted("goodhost"), "stale backend cleaned up at start")
    check("sftp/goodhost" in dbus("hosts"), "registered hosts kept across restarts")
    with open(os.path.join(ROOT, "sftp", "goodhost", "data", "sub", "f.txt")) as f:
        check(f.read() == "hello\n", "works after restarting over a stale mount")


def isolate_bus_services():
    # Services the bus starts (kpasswdserver in kiod6, KWallet) get the bus's
    # environment, i.e. the user's real config: give them the test's.
    e = env()
    names = ("XDG_RUNTIME_DIR", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "QT_QPA_PLATFORM")
    subprocess.run(["dbus-update-activation-environment"] + [f"{n}={e[n]}" for n in names]
                   + ["DISPLAY=", "WAYLAND_DISPLAY=", f"PATH={KIO_BIN}:/usr/bin:/bin"],
                   check=True, env=e, timeout=30)


def main():
    try:
        setup()
        isolate_bus_services()
        try:
            start_daemon()
        except Exception as e:
            check(False, f"daemon start: {e!r}")
            return 1
        for test in (register_hosts, test_listing_and_read, test_write_operations, test_symlinks, test_invalid_names,
                     test_host_registry, test_kde_passwords, test_remote_agent, test_remote_agent_install, test_terminal_askpass, test_kio_backend, test_kio_folders, test_network_drop, test_failure_cache, test_concurrent_connect, test_idle_unmount, test_backend_crash,
                     test_dbus_unmount, test_shutdown_and_restart):
            try:
                test()
            except Exception as e:  # report and continue with the next test
                check(False, f"{test.__name__} raised {e!r}")
    finally:
        cleanup()
    if failures:
        print(f"{len(failures)} failure(s)")
        return 1
    print("all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
