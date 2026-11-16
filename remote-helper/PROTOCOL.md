# kate-code-helper protocol

`kate-code-helper` runs on the host of a remote folder, started by Kate Code
through `ssh -T` (no terminal). It gives Kate Code current access to the
host's files (the sshfs mount may show files up to ~20 s old) and change
notifications for the files open in Kate.

This document is the contract between Kate Code and every implementation of
the helper (the native one in this directory; possibly others later), which
are released separately from Kate Code. It is versioned: see
"Compatibility" below.

## Invocation

    kate-code-helper --version

prints one line and exits with 0, without starting a session:

    kate-code-helper <helper version> protocol <protocol version>

e.g. `kate-code-helper 0.1.0 protocol 1`. Kate Code uses it to check an
installed copy.

    kate-code-helper

(no arguments) serves the protocol on stdin/stdout until stdin reaches end
of file, then exits with 0. stderr carries diagnostics only (free text, for
logs). Any other arguments: a usage message on stderr, exit code 2.

## Framing

Both directions are newline-delimited JSON (UTF-8): one JSON value per
line, no newline inside it. A `\r` before the `\n` is ignored. Lines may be
long (file contents); an implementation must accept lines of at least
128 MiB. A line it cannot take (longer than its limit, or more than its
memory holds) is answered with an error whose `id` is `null`: a client
then fails the requests it was waiting for that are not answered.

Messages follow JSON-RPC 2.0: requests (`jsonrpc`, `id`, `method`,
`params`), responses (`jsonrpc`, `id`, and `result` or `error`) and
notifications (no `id`). Only Kate Code sends requests; the helper sends
responses and notifications. Clients match responses to requests by `id`
and must not rely on their order (this helper answers in order; later
methods may not). Notifications may come between responses at any time.

Binary data (file contents) is base64 (RFC 4648, with padding) in JSON
strings. Paths are UTF-8 strings and must be absolute host paths: POSIX
`/a/b`; Windows `C:\a\b` (or `C:/a/b`), or UNC `\\server\share\a`.
Paths in results and notifications are the request's strings, unchanged.

Fields not listed here must be ignored by both sides, and notifications
with an unknown method too (later versions add some).

## Handshake

The helper's first line is a notification (clients skip any lines before
it: shell startup files may print over ssh):

    {"jsonrpc":"2.0","method":"hello","params":{
      "protocol":1,
      "version":"0.1.0",
      "platform":"linux-x86_64",
      "capabilities":["stat","read","write","digest","watch"]}}

- `protocol`: the protocol's major version (an integer).
- `version`: the helper's own version (informational).
- `platform`: what the helper was built for: `linux-x86_64`,
  `linux-aarch64`, `linux-x86`, `linux-arm`, `darwin-aarch64`,
  `darwin-x86_64`, `windows-x86_64`, `windows-aarch64`, ...
  (informational).
- `capabilities`: the method groups this helper supports. A client must not
  call a method whose capability is missing (it would get "method not
  found").

## Compatibility

- A change that an older client or helper would misunderstand increments
  `protocol`. A client refuses (and does not use) a helper with a protocol
  it does not know; Kate Code then installs a matching helper or does
  without.
- Additions (new methods, new optional fields, new error names, new
  capabilities) keep `protocol` and appear as capabilities, so clients can
  tell what a helper supports without comparing versions.

## Errors

Error responses use JSON-RPC's codes for protocol errors:

| code | meaning |
|---|---|
| -32700 | parse error (the line is not JSON); `id` is `null` |
| -32600 | invalid request (not a request object) |
| -32601 | method not found |
| -32602 | invalid params (missing/mistyped field, relative path, bad base64) |

and code `1` for an operation that failed. Then `error.data.error` names
the reason (clients must accept names not listed here, as "failed"):

| `error` | meaning |
|---|---|
| `notFound` | the path (or a directory on the way) does not exist |
| `permissionDenied` | access refused by the system |
| `isDirectory` | a file operation on a directory |
| `notDirectory` | a path component is not a directory |
| `tooLarge` | the file is larger than `maxSize` |
| `changed` | `write`'s precondition does not hold (see `write`) |
| `busy` | the file is locked by another program (Windows) |
| `io` | anything else |

`error.message` is a human-readable description (for logs and the user).
`error.data.path` repeats the path when there is one.

## Methods

### `stat` (capability `stat`)

    params: {"path": "/a/b"}
    result: {"type": "file", "size": 123, "mtimeNs": "1731600000123456789",
             "mode": 420}

Follows symlinks. `type`: `file`, `directory` or `other`. `size` in bytes
(files). `mtimeNs`: modification time in nanoseconds since the Unix epoch,
as a decimal string (exact; a JSON number cannot hold it). `mode`: the
POSIX permission bits (POSIX hosts only).

### `read` (capability `read`)

    params: {"path": "/a/b", "maxSize": 67108864}
    result: {"data": "<base64>", "size": 123, "digest": "<hex>"}

The whole file. `maxSize` (optional, default 64 MiB): larger files fail
with `tooLarge`. `digest`: the file's digest (see `digest`) of exactly the
bytes returned, for a later `write` with `expectDigest`.

### `write` (capability `write`)

    params: {"path": "/a/b", "data": "<base64>",
             "expectDigest": "<hex>" | null}
    result: {"digest": "<hex>"}

Replaces the file's contents. `expectDigest` (optional): when a string, the
file must exist with that digest; when `null`, the file must not exist;
otherwise the write fails with `changed` and nothing is written (checked
just before writing: not atomic with the write itself). Without it the file
is written in any case (created if missing; the directory must exist).

The new contents are written to a temporary file in the same directory and
renamed over the file, so readers never see a partial file. The file keeps
its permission bits (POSIX). A symlink is followed (the target is
replaced, the link kept). When the file cannot be replaced that way (it has
other hard links, the directory is not writable, or (Windows) the file is
open without delete sharing), it is overwritten in place.

`digest`: the digest of the written contents.

### `digest` (capability `digest`)

    params: {"paths": ["/a/b", "/a/c"]}
    result: {"digests": [{"path": "/a/b", "digest": "<hex>"},
                         {"path": "/a/c", "error": "notFound"}]}

The digest of each file, in the order given: the hex SHA-1 of the file as
a git blob (`git hash-object`: SHA-1 of `"blob <size>\0"` followed by the
contents). Kate's `KTextEditor::Document::checksum()` is the same digest of
a document's saved text. A file that cannot be read has `error` (a name from
the table above) instead of `digest`. (This helper reads each file whole,
without a size limit, as `write` does for `expectDigest`: meant for
documents, not huge files.)

### `watch`, `unwatch` (capability `watch`)

    watch params:   {"paths": ["/a/b", "/a/c"]}
    watch result:   {"failed": [{"path": "/a/c", "error": "notFound"}]}
    unwatch params: {"paths": ["/a/b"]}
    unwatch result: {}

`watch` adds files to the watched set; the helper then sends

    {"jsonrpc":"2.0","method":"changed","params":{"paths":["/a/b"]}}

when a watched file may have changed: its contents written, the file
created, deleted, renamed or replaced (an editor's atomic save). Changes
within a short time (about 100 ms) are reported together. A notification
can be spurious (nothing changed in the end); clients compare digests. A
file may not exist yet, but its directory must (else it is in `failed`,
with an error name and `message`). Watching a path already watched, or
unwatching one not watched, does nothing. `unwatch` stops notifications for
the paths (one already on its way may still arrive).

How changes are detected is up to the helper (the native one: inotify on
Linux, `ReadDirectoryChangesW` on Windows, polling every 0.5 s elsewhere or
when the system's watches run out).
