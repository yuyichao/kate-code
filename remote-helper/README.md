# kate-code-helper

Kate Code's optional helper on the hosts of remote folders: current file
access (the sshfs mount may lag ~20 s behind) and change notifications for
the files open in Kate, over the ssh connection's stdin/stdout. The
protocol, which every implementation follows and which is versioned
separately from Kate Code, is in [PROTOCOL.md](PROTOCOL.md).

This is a project of its own: the plugin's build does not include it. It
uses only the C++17 standard library and the system's APIs, so release
builds are static and run on any host of their platform.

## Building and testing

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure

`tests/test_protocol.py <helper command...>` checks a helper through the
protocol only (any implementation), on Linux, macOS and Windows; it runs
each session once with the native change notifications and once with
`KATE_CODE_HELPER_WATCH=poll` (polling only, as on macOS).

Source layout: `Server` (the protocol), `Json`, `Codec` (SHA-1, base64),
`Files*` (file access: `FilesPosix.cpp`, `FilesWindows.cpp`), `Watcher*`
(batching and polling in `Watcher.cpp`; directory notifications:
`WatcherInotify.cpp` on Linux, `WatcherWindows.cpp`, none elsewhere).

## Releases

`.github/workflows/remote-helper.yml` builds and tests on each platform's
runner:

- Linux x86_64, aarch64, x86 (32-bit) and arm (32-bit, ARMv6+ hard-float):
  static musl, built in Alpine, also tested on the runner's glibc system.
  (The 32-bit builds run natively on the x86_64 and arm64 runners; the
  32-bit ARM one is expected to, not seen yet.)
  A 32-bit userland on a 64-bit kernel takes the 64-bit build: a static
  program needs only the kernel.
- macOS: universal arm64 + x86_64, ad-hoc signed by the linker (checked).
  This is enough: the helper reaches a Mac through ssh, without the
  quarantine attribute Gatekeeper checks, and macOS charges its file access
  to sshd ("Allow full disk access for remote users"), not to its own
  signature.
- Windows x86_64 and aarch64: static MSVC runtime, version resource.

Pushing a tag `remote-helper-v<version>`, matching `project()`'s version in
`CMakeLists.txt`, publishes a GitHub release on that repository (Kate Code
uses github.com/yuyichao/kate-code) with one `.tar.gz` per platform (only
the program inside), `SHA256SUMS` and `manifest.json`:

    {"name": "kate-code-helper", "version": "0.1.0", "protocol": 1,
     "platforms": {"linux-x86_64": {"archive": "kate-code-helper-linux-x86_64.tar.gz",
                                    "sha256": "...", "size": 123, "program": "kate-code-helper"},
                   ...}}

(keys as in the ACP registry: `linux-x86_64`, `linux-x86_64-musl`,
`linux-arm`, `darwin-aarch64`, `windows-x86_64`, ...).

### Pinning a release in Kate Code

Kate Code will not trust the release as it is at download time: it will
install only archives whose SHA-256 is pinned in its own source,
`src/acp/remote-helper-pin.json` (not there yet: no release so far, and
Kate Code does not use the helper yet). After the release workflow has run:

    remote-helper/pin-release.py 0.1.0

downloads the release (`gh release download`), hashes each archive itself,
checks it against `manifest.json` and `SHA256SUMS` and that it holds only
the program, and writes the pin (version, protocol, download URL, per
platform the archive, SHA-256, size and program, and the program's own
SHA-256 and size, to check a copy installed on a host). Review the diff and
commit it. A release asset replaced later no longer matches, so the helper
a Kate Code build installs is exactly the one pinned in its source, and the
helper is only as trusted as that source. The release job runs the same
checks (without writing a pin) before publishing. Turn on GitHub's immutable
releases for the repository too, so assets cannot be replaced at all.

Version numbers: the helper's own (`project()`, any change) and the
protocol's (`ProtocolVersion` in `src/Server.h`, only for incompatible
changes; additions are capabilities). See PROTOCOL.md "Compatibility".

Not done: Authenticode signing of the Windows builds (copied over ssh,
they carry no Mark of the Web, so SmartScreen does not check them; a
signature would mostly help with Defender's heuristics and publisher-based
policies).
