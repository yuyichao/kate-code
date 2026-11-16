#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Kate Code contributors
"""Conformance test of a kate-code-helper (see PROTOCOL.md).

    test_protocol.py <helper command...>

Drives the helper only through its protocol and command line, so any
implementation can be checked with it. Runs on Linux, macOS and Windows.
"""

import base64
import hashlib
import json
import os
import queue
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time

WINDOWS = os.name == "nt"
NOTIFY_TIMEOUT = 5.0  # polling 0.5 s + batching 0.1 s, with room for slow CI
QUIET_TIME = 1.5

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)
        print("FAIL:", message, flush=True)
    return condition


def git_digest(data):
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def b64(data):
    return base64.b64encode(data).decode("ascii")


class Helper:
    def __init__(self, command, env=None):
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, env=env)
        self.messages = queue.Queue()
        self.notifications = []
        self.next_id = 1
        self.stderr = b""
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            self.messages.put(line)
        self.messages.put(None)

    def _read_stderr(self):
        self.stderr = self.proc.stderr.read()

    def next_message(self, timeout=10.0):
        line = self.messages.get(timeout=timeout)
        if line is None:
            raise EOFError("helper closed its output")
        if not line.endswith(b"\n"):
            raise ValueError("line without newline: %r" % line[:200])
        return json.loads(line)

    def send_line(self, line):
        self.proc.stdin.write(line if isinstance(line, bytes) else line.encode("utf-8"))
        self.proc.stdin.flush()

    def send(self, message):
        self.send_line(json.dumps(message, separators=(",", ":")) + "\n")

    def response(self, expected_id):
        while True:
            message = self.next_message()
            if "id" not in message:
                self.notifications.append(message)
                continue
            assert message["id"] == expected_id, "response %r for request %r" % (message, expected_id)
            return message

    def request(self, method, params=None, raw=False):
        request_id = self.next_id
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            message["params"] = params
        self.send(message)
        response = self.response(request_id)
        if raw:
            return response
        if "error" in response:
            raise RuntimeError("%s failed: %r" % (method, response["error"]))
        return response["result"]

    def error(self, method, params):
        response = self.request(method, params, raw=True)
        check("error" in response, "%s %r should fail, got %r" % (method, params, response))
        return response.get("error", {})

    def changed_paths(self, timeout, want=None):
        """Paths of 'changed' notifications within timeout (until 'want' were all seen)."""
        seen = set()
        for message in self.notifications:
            if message.get("method") == "changed":
                seen.update(message["params"]["paths"])
        self.notifications = [m for m in self.notifications if m.get("method") != "changed"]
        deadline = time.monotonic() + timeout
        while want is None or not want <= seen:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            try:
                message = self.next_message(timeout=remaining)
            except queue.Empty:
                break
            check("id" not in message, "unexpected response %r" % message)
            if message.get("method") == "changed":
                seen.update(message["params"]["paths"])
        return seen

    def close(self):
        self.proc.stdin.close()
        try:
            code = self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            code = None
        return code


def fs_error(error, name, path=None):
    check(error.get("code") == 1, "error code 1 for %s, got %r" % (name, error))
    data = error.get("data", {})
    check(data.get("error") == name, "error %s expected, got %r" % (name, error))
    if path is not None:
        check(data.get("path") == path, "error path %r expected, got %r" % (path, error))
    check(isinstance(error.get("message"), str) and error["message"], "error message in %r" % error)


def test_command_line(command):
    out = subprocess.run(command + ["--version"], capture_output=True, timeout=10)
    words = out.stdout.decode().split()
    check(out.returncode == 0 and len(words) == 4 and words[0] == "kate-code-helper" and words[2] == "protocol"
          and words[3] == "1", "--version: %r" % out.stdout)
    out = subprocess.run(command + ["--bogus"], capture_output=True, timeout=10)
    check(out.returncode == 2 and out.stderr, "unknown argument: exit 2 with usage, got %d" % out.returncode)
    return words[1] if len(words) == 4 else None


def test_handshake(helper, version):
    hello = helper.next_message()
    check(hello.get("method") == "hello" and "id" not in hello, "first line is the hello notification: %r" % hello)
    params = hello.get("params", {})
    check(params.get("protocol") == 1, "protocol 1: %r" % params)
    check(params.get("version") == version, "hello version matches --version: %r" % params)
    check(isinstance(params.get("platform"), str) and "-" in params["platform"], "platform: %r" % params)
    check(set(params.get("capabilities", [])) >= {"stat", "read", "write", "digest", "watch"},
          "capabilities: %r" % params)


def test_protocol_errors(helper, tmp):
    helper.send_line("this is not json\n")
    response = helper.response(None)
    check(response.get("error", {}).get("code") == -32700, "parse error: %r" % response)
    helper.send_line("[1,2]\n")
    response = helper.response(None)
    check(response.get("error", {}).get("code") == -32600, "invalid request: %r" % response)
    # A notification gets no answer; blank lines and CRLF are fine.
    helper.send({"jsonrpc": "2.0", "method": "whatever", "params": {}})
    helper.send_line("\n   \n")
    helper.send_line(json.dumps({"jsonrpc": "2.0", "id": "s1", "method": "stat",
                                 "params": {"path": tmp}}) + "\r\n")
    response = helper.response("s1")
    check(response.get("result", {}).get("type") == "directory", "string id, CRLF: %r" % response)

    error = helper.error("bogus", {})
    check(error.get("code") == -32601, "method not found: %r" % error)
    error = helper.error("stat", [])
    check(error.get("code") == -32602, "params not an object: %r" % error)
    error = helper.error("stat", {"path": "relative/path"})
    check(error.get("code") == -32602, "relative path: %r" % error)
    error = helper.error("stat", {"path": 5})
    check(error.get("code") == -32602, "path not a string: %r" % error)
    error = helper.error("write", {"path": os.path.join(tmp, "x"), "data": "not base64!"})
    check(error.get("code") == -32602, "bad base64: %r" % error)
    check(not os.path.exists(os.path.join(tmp, "x")), "nothing written for bad base64")
    error = helper.error("digest", {"paths": "x"})
    check(error.get("code") == -32602, "paths not an array: %r" % error)
    # Unknown fields are ignored.
    result = helper.request("stat", {"path": tmp, "someFutureField": [1, {"a": None}]})
    check(result.get("type") == "directory", "unknown field ignored: %r" % result)


def test_stat(helper, tmp):
    path = os.path.join(tmp, "stat.txt")
    with open(path, "wb") as f:
        f.write(b"hello")
    if not WINDOWS:
        os.chmod(path, 0o640)
    st = os.stat(path)
    result = helper.request("stat", {"path": path})
    check(result.get("type") == "file" and result.get("size") == 5, "stat file: %r" % result)
    check(result.get("mtimeNs") == str(st.st_mtime_ns), "mtimeNs %r vs %d" % (result.get("mtimeNs"), st.st_mtime_ns))
    if WINDOWS:
        check("mode" not in result, "no mode on Windows: %r" % result)
    else:
        check(result.get("mode") == 0o640, "mode: %r" % result)
    result = helper.request("stat", {"path": tmp})
    check(result.get("type") == "directory", "stat dir: %r" % result)
    missing = os.path.join(tmp, "missing")
    fs_error(helper.error("stat", {"path": missing}), "notFound", missing)
    below = os.path.join(path, "below")
    error = helper.error("stat", {"path": below})
    if WINDOWS:  # ERROR_PATH_NOT_FOUND or ERROR_DIRECTORY
        check(error.get("data", {}).get("error") in ("notFound", "notDirectory"), "stat below a file: %r" % error)
    else:
        fs_error(error, "notDirectory", below)
    if WINDOWS:
        result = helper.request("stat", {"path": path.replace("\\", "/")})
        check(result.get("type") == "file", "Windows path with '/': %r" % result)


def test_read(helper, tmp):
    data = bytes(range(256)) * 3 + b"\0\xff\xfe not utf-8 \x80"
    path = os.path.join(tmp, "binary.bin")
    with open(path, "wb") as f:
        f.write(data)
    result = helper.request("read", {"path": path})
    check(base64.b64decode(result.get("data", "")) == data, "read returns the bytes")
    check(result.get("size") == len(data), "read size")
    check(result.get("digest") == git_digest(data), "read digest")

    empty = os.path.join(tmp, "empty")
    open(empty, "wb").close()
    result = helper.request("read", {"path": empty})
    check(result == {"data": "", "size": 0, "digest": git_digest(b"")}, "empty file: %r" % result)

    fs_error(helper.error("read", {"path": path, "maxSize": 10}), "tooLarge", path)
    result = helper.request("read", {"path": path, "maxSize": len(data)})
    check(result.get("size") == len(data), "maxSize = size is fine")
    error = helper.error("read", {"path": path, "maxSize": -1})
    check(error.get("code") == -32602, "negative maxSize: %r" % error)
    fs_error(helper.error("read", {"path": tmp}), "isDirectory", tmp)
    fs_error(helper.error("read", {"path": os.path.join(tmp, "missing")}), "notFound")

    big = os.urandom(5 << 20)
    big_path = os.path.join(tmp, "big.bin")
    with open(big_path, "wb") as f:
        f.write(big)
    result = helper.request("read", {"path": big_path})
    check(base64.b64decode(result["data"]) == big and result["digest"] == git_digest(big), "5 MiB read")

    if not WINDOWS and hasattr(os, "mkfifo"):
        fifo = os.path.join(tmp, "fifo")
        os.mkfifo(fifo)
        fs_error(helper.error("read", {"path": fifo}), "io", fifo)  # must not hang
        os.unlink(fifo)


def leftovers(directory, known):
    return sorted(set(os.listdir(directory)) - set(known))


def test_write(helper, tmp):
    wdir = os.path.join(tmp, "write")
    os.mkdir(wdir)
    path = os.path.join(wdir, "new.txt")
    data = "héllo wörld\n文件\n".encode("utf-8")
    result = helper.request("write", {"path": path, "data": b64(data)})
    check(result.get("digest") == git_digest(data), "write digest")
    with open(path, "rb") as f:
        check(f.read() == data, "written bytes")
    if not WINDOWS:
        mask = os.umask(0)
        os.umask(mask)
        check(stat.S_IMODE(os.stat(path).st_mode) == 0o666 & ~mask, "new file mode follows the umask")
        os.chmod(path, 0o640)

    data2 = b"second\r\nversion\0"
    helper.request("write", {"path": path, "data": b64(data2)})
    with open(path, "rb") as f:
        check(f.read() == data2, "overwrite")
    if not WINDOWS:
        check(stat.S_IMODE(os.stat(path).st_mode) == 0o640, "overwrite keeps the mode")
    check(leftovers(wdir, ["new.txt"]) == [], "no temporary files left: %r" % leftovers(wdir, ["new.txt"]))

    # expectDigest
    data3 = b"third"
    result = helper.request("write", {"path": path, "data": b64(data3), "expectDigest": git_digest(data2)})
    check(result.get("digest") == git_digest(data3), "write with the right digest")
    fs_error(helper.error("write", {"path": path, "data": b64(b"lost"), "expectDigest": git_digest(data2)}),
             "changed", path)
    fs_error(helper.error("write", {"path": path, "data": b64(b"lost"), "expectDigest": None}), "changed", path)
    with open(path, "rb") as f:
        check(f.read() == data3, "refused writes change nothing")
    upper = git_digest(data3).upper()
    helper.request("write", {"path": path, "data": b64(b"fourth"), "expectDigest": upper})
    created = os.path.join(wdir, "created.txt")
    fs_error(helper.error("write", {"path": created, "data": b64(b"x"), "expectDigest": git_digest(b"x")}),
             "changed", created)
    check(not os.path.exists(created), "not created when a digest was expected")
    helper.request("write", {"path": created, "data": b64(b"x"), "expectDigest": None})
    check(os.path.exists(created), "created with expectDigest null")

    # Errors
    missing_dir = os.path.join(wdir, "no", "such.txt")
    fs_error(helper.error("write", {"path": missing_dir, "data": b64(b"x")}), "notFound", missing_dir)
    fs_error(helper.error("write", {"path": wdir, "data": b64(b"x")}), "isDirectory", wdir)

    # Non-ASCII names
    name = os.path.join(wdir, "файл-文件 é.txt")
    helper.request("write", {"path": name, "data": b64(b"named")})
    with open(name, "rb") as f:
        check(f.read() == b"named", "non-ASCII file name")
    result = helper.request("digest", {"paths": [name]})
    check(result["digests"][0].get("digest") == git_digest(b"named"), "digest of a non-ASCII name")

    if not WINDOWS:
        # A symlink: the target is replaced, the link kept.
        target = os.path.join(wdir, "target.txt")
        link = os.path.join(wdir, "link.txt")
        with open(target, "wb") as f:
            f.write(b"old")
        os.symlink("target.txt", link)
        helper.request("write", {"path": link, "data": b64(b"through the link")})
        check(os.path.islink(link), "the symlink stays a symlink")
        with open(target, "rb") as f:
            check(f.read() == b"through the link", "the link's target is written")

        # Hard links keep sharing the contents (written in place).
        first = os.path.join(wdir, "hard1.txt")
        second = os.path.join(wdir, "hard2.txt")
        with open(first, "wb") as f:
            f.write(b"shared")
        os.link(first, second)
        helper.request("write", {"path": first, "data": b64(b"both names")})
        with open(second, "rb") as f:
            check(f.read() == b"both names", "hard link written in place")
        check(os.stat(first).st_ino == os.stat(second).st_ino, "hard link kept")

        # A read-only directory with a writable file: written in place.
        if os.geteuid() != 0:
            ro = os.path.join(tmp, "readonly")
            os.mkdir(ro)
            ro_file = os.path.join(ro, "file.txt")
            with open(ro_file, "wb") as f:
                f.write(b"before")
            os.chmod(ro, 0o555)
            try:
                helper.request("write", {"path": ro_file, "data": b64(b"after")})
                with open(ro_file, "rb") as f:
                    check(f.read() == b"after", "file in a read-only directory written in place")
                ro_new = os.path.join(ro, "new.txt")
                fs_error(helper.error("write", {"path": ro_new, "data": b64(b"x")}), "permissionDenied", ro_new)
            finally:
                os.chmod(ro, 0o755)


def test_digest(helper, tmp):
    a = os.path.join(tmp, "digest-a")
    b = os.path.join(tmp, "digest-b")
    with open(a, "wb") as f:
        f.write(b"aaa")
    with open(b, "wb") as f:
        f.write(b"")
    missing = os.path.join(tmp, "digest-missing")
    result = helper.request("digest", {"paths": [a, missing, b, tmp]})
    digests = result.get("digests", [])
    check([d.get("path") for d in digests] == [a, missing, b, tmp], "digest order: %r" % digests)
    check(digests[0].get("digest") == git_digest(b"aaa"), "digest a")
    check(digests[1].get("error") == "notFound" and "digest" not in digests[1], "digest missing: %r" % digests[1])
    check(digests[2].get("digest") == git_digest(b""), "digest empty")
    check(digests[3].get("error") == "isDirectory", "digest of a directory: %r" % digests[3])
    check(helper.request("digest", {"paths": []}) == {"digests": []}, "digest of nothing")


def test_watch(helper, tmp, label):
    wdir = os.path.join(tmp, "watch-" + label)
    os.mkdir(wdir)
    a = os.path.join(wdir, "a.txt")
    b = os.path.join(wdir, "b.txt")
    other = os.path.join(wdir, "other.txt")
    with open(a, "wb") as f:
        f.write(b"a")
    missing = os.path.join(wdir, "nodir", "c.txt")
    result = helper.request("watch", {"paths": [a, b, missing]})
    failed = result.get("failed", [])
    check(len(failed) == 1 and failed[0].get("path") == missing and failed[0].get("error") == "notFound",
          "[%s] watch of a file in a missing directory fails: %r" % (label, failed))
    helper.request("watch", {"paths": [a]})  # again: nothing changes
    # Some systems report the watch's own setup late; drain first.
    helper.changed_paths(0.5)

    with open(other, "wb") as f:
        f.write(b"unwatched")
    seen = helper.changed_paths(QUIET_TIME)
    check(not seen, "[%s] no notification for unwatched files: %r" % (label, seen))

    time.sleep(0.05)  # mtime granularity for the polling watcher
    with open(a, "ab") as f:
        f.write(b" appended")
    seen = helper.changed_paths(NOTIFY_TIMEOUT, {a})
    check(a in seen, "[%s] in-place write reported: %r" % (label, seen))

    replacement = os.path.join(wdir, "a.tmp")
    with open(replacement, "wb") as f:
        f.write(b"replaced atomically")
    os.replace(replacement, a)
    seen = helper.changed_paths(NOTIFY_TIMEOUT, {a})
    check(a in seen and b not in seen, "[%s] atomic replace reported: %r" % (label, seen))

    with open(b, "wb") as f:
        f.write(b"b")
    seen = helper.changed_paths(NOTIFY_TIMEOUT, {b})
    check(b in seen, "[%s] creation reported: %r" % (label, seen))

    helper.request("write", {"path": b, "data": b64(b"by the helper")})
    seen = helper.changed_paths(NOTIFY_TIMEOUT, {b})
    check(b in seen, "[%s] the helper's own write reported: %r" % (label, seen))

    os.unlink(a)
    seen = helper.changed_paths(NOTIFY_TIMEOUT, {a})
    check(a in seen, "[%s] deletion reported: %r" % (label, seen))

    # Notifications may be spurious or late (PROTOCOL.md): let them pass
    # before checking that none follow.
    helper.changed_paths(0.5)
    helper.request("unwatch", {"paths": [a, os.path.join(wdir, "never-watched")]})
    with open(a, "wb") as f:
        f.write(b"back")
    seen = helper.changed_paths(QUIET_TIME)
    check(a not in seen, "[%s] no notification after unwatch: %r" % (label, seen))

    # Several quick writes: reported, possibly together.
    for i in range(5):
        with open(b, "ab") as f:
            f.write(b"%d" % i)
    seen = helper.changed_paths(NOTIFY_TIMEOUT, {b})
    check(b in seen, "[%s] burst reported: %r" % (label, seen))
    helper.changed_paths(0.5)
    helper.request("unwatch", {"paths": [b]})
    seen = helper.changed_paths(QUIET_TIME)
    check(not seen, "[%s] quiet after unwatching everything: %r" % (label, seen))


def run_session(command, tmp, version, env=None, label="native", full=True):
    helper = Helper(command, env)
    try:
        test_handshake(helper, version)
        if full:
            test_protocol_errors(helper, tmp)
            test_stat(helper, tmp)
            test_read(helper, tmp)
            test_write(helper, tmp)
            test_digest(helper, tmp)
        test_watch(helper, tmp, label)
        # End of input ends the session, watches and all.
        helper.request("watch", {"paths": [os.path.join(tmp, "stat.txt")]})
        start = time.monotonic()
        code = helper.close()
        check(code == 0, "[%s] exit code 0 at end of input, got %r" % (label, code))
        check(time.monotonic() - start < 5, "[%s] exits promptly" % label)
    finally:
        if helper.proc.poll() is None:
            helper.proc.kill()
            helper.proc.wait()
        if helper.stderr:
            print("[%s] helper stderr:" % label, helper.stderr.decode(errors="replace"))


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    command = sys.argv[1:]
    version = test_command_line(command)
    tmp = tempfile.mkdtemp(prefix="kate-code-helper-test-")
    try:
        run_session(command, tmp, version)
        env = dict(os.environ, KATE_CODE_HELPER_WATCH="poll")
        run_session(command, tmp, version, env, label="polling", full=False)
    finally:
        def writable(func, path, _error):
            os.chmod(path, 0o755)
            func(path)
        if sys.version_info >= (3, 12):
            shutil.rmtree(tmp, onexc=writable)
        else:
            shutil.rmtree(tmp, onerror=writable)
    if failures:
        print("%d check(s) failed" % len(failures))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
