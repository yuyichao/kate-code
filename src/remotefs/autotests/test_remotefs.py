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
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

SKIP = 77
DAEMON = os.path.abspath(sys.argv[1])

SFTP_SERVER = next((p for p in ("/usr/lib/ssh/sftp-server", "/usr/libexec/openssh/sftp-server",
                                "/usr/lib/openssh/sftp-server") if os.path.exists(p)), None)
for tool in ("sshfs", "bwrap", "fusermount3", "dbus-send"):
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
HOSTS_FILE = os.path.join(STATE, "kate-remote-fs", "hosts")
SSH_LOG = os.path.join(TMP, "ssh.log")
FAKE_SSH = os.path.join(TMP, "fake-ssh")
DAEMON_LOG = os.path.join(TMP, "daemon.log")
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
    e.update(XDG_RUNTIME_DIR=RUNTIME, XDG_CONFIG_HOME=CONFIG, XDG_STATE_HOME=STATE, QT_FORCE_STDERR_LOGGING="1")
    # No askpass helper popping up from the test.
    for key in ("DISPLAY", "WAYLAND_DISPLAY", "SSH_ASKPASS", "SSH_AUTH_SOCK"):
        e.pop(key, None)
    return e


def start_daemon():
    global daemon
    log = open(DAEMON_LOG, "a")
    daemon = subprocess.Popen([DAEMON], env=env(), stdout=log, stderr=subprocess.STDOUT)
    def ready():
        # A crashed instance leaves a dead mount behind; wait for a live one.
        try:
            return os.listdir(ROOT) == ["sftp"]
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


def cleanup():
    stop_daemon()
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
case " $* " in
    *" badhost "*) echo "ssh: connect to host badhost port 22: Connection refused" >> "$log"; exit 255;;
    *" flakyhost "*)
        if [ -e {TMP}/flaky-down ]; then
            sleep 1
            echo "ssh: connect to host flakyhost port 22: Connection timed out" >> "$log"
            exit 255
        fi
        echo $$ > {TMP}/flakyhost.pid;;
    *" slowhost "*) sleep 1.5;;
esac
exec bwrap --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib /lib64 --symlink usr/bin /bin \\
    --ro-bind /etc/passwd /etc/passwd --ro-bind /etc/group /etc/group \\
    --bind {TREE} /data --dev /dev --proc /proc --chdir / --die-with-parent {SFTP_SERVER}
""")
    os.chmod(FAKE_SSH, 0o755)

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
    check(sorted(os.listdir(ROOT)) == ["sftp"], "root lists sftp")
    check(os.listdir(os.path.join(ROOT, "sftp")) == [], "no hosts listed before use")
    check(dbus("mountRoot") == ROOT, "DBus mountRoot")
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
    for path in (os.path.join(ROOT, ".git"), os.path.join(ROOT, ".kateproject"), os.path.join(ROOT, "smb"),
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
    check(mounts_under(TMP) == [], "everything unmounted on exit")

    start_daemon()
    open(os.path.join(ROOT, "sftp", "goodhost", "data", "sub", "f.txt")).read()
    stop_daemon(signal.SIGKILL)
    check(backend_mounted("goodhost"), "crash leaves the backend mounted")
    start_daemon()
    check(not backend_mounted("goodhost"), "stale backend cleaned up at start")
    check("sftp/goodhost" in dbus("hosts"), "registered hosts kept across restarts")
    with open(os.path.join(ROOT, "sftp", "goodhost", "data", "sub", "f.txt")) as f:
        check(f.read() == "hello\n", "works after restarting over a stale mount")


def main():
    try:
        setup()
        try:
            start_daemon()
        except Exception as e:
            check(False, f"daemon start: {e!r}")
            return 1
        for test in (register_hosts, test_listing_and_read, test_write_operations, test_symlinks, test_invalid_names,
                     test_host_registry, test_network_drop, test_failure_cache, test_concurrent_connect, test_idle_unmount, test_backend_crash,
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
