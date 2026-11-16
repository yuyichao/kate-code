#!/usr/bin/python3 -I
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Kate Code contributors

"""Pin a kate-code-helper release in Kate Code's source.

    remote-helper/pin-release.py <version> [--repo OWNER/NAME] [--output FILE]
    remote-helper/pin-release.py <version> --from-dir DIR   (files already here)

Downloads the release remote-helper-v<version> (gh release download), hashes
every archive itself, checks it against manifest.json and SHA256SUMS and
checks that each archive holds exactly the program, then writes the pin:
the version, protocol, download URL and each platform's archive with its
SHA-256 and size. Kate Code installs only archives matching its pin, so a
release asset replaced later is refused; a new helper release takes a new
pin (a reviewed commit).
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tarfile
import tempfile

NAME = "kate-code-helper"
DEFAULT_REPO = "yuyichao/kate-code"
SOURCE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUTPUT = os.path.join(SOURCE_ROOT, "src", "acp", "remote-helper-pin.json")

# Every release has these (the workflow's builds); "-musl" aliases of the
# Linux keys point at the same archives.
REQUIRED_PLATFORMS = [
    "linux-x86_64", "linux-aarch64", "linux-x86", "linux-arm",
    "darwin-aarch64", "darwin-x86_64",
    "windows-x86_64", "windows-aarch64",
]
ARCHIVE_RE = re.compile(r"kate-code-helper-[a-z0-9_]+-[a-z0-9_]+\.tar\.gz")


def fail(message):
    sys.exit("pin-release: " + message)


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def check_archive(path, program):
    """The archive holds exactly one regular file, the program."""
    try:
        with tarfile.open(path, "r:gz") as tar:
            members = tar.getmembers()
    except (tarfile.TarError, OSError, EOFError) as e:
        fail("%s: not a readable .tar.gz: %s" % (os.path.basename(path), e))
    names = [m.name for m in members]
    if len(members) != 1 or members[0].name != program or not members[0].isfile():
        fail("%s: expected only the file %s, found %s" % (os.path.basename(path), program, names))


def download(repo, tag, directory):
    command = ["gh", "release", "download", tag, "--repo", repo, "--dir", directory]
    print("+ " + " ".join(command), file=sys.stderr)
    try:
        subprocess.run(command, check=True)
    except FileNotFoundError:
        fail("gh (GitHub CLI) is not installed; or download the release and use --from-dir")
    except subprocess.CalledProcessError as e:
        fail("gh release download failed (exit code %d)" % e.returncode)


def build_pin(version, repo, tag, directory):
    try:
        with open(os.path.join(directory, "manifest.json"), "rb") as f:
            manifest = json.load(f)
    except (OSError, ValueError) as e:
        fail("manifest.json: %s" % e)
    if not isinstance(manifest, dict) or manifest.get("name") != NAME:
        fail("manifest.json is not a %s manifest" % NAME)
    if manifest.get("version") != version:
        fail("manifest.json is for version %r, not %s" % (manifest.get("version"), version))
    protocol = manifest.get("protocol")
    if not isinstance(protocol, int) or isinstance(protocol, bool) or protocol < 1:
        fail("manifest.json: bad protocol %r" % (protocol,))
    platforms = manifest.get("platforms")
    if not isinstance(platforms, dict):
        fail("manifest.json has no platforms")
    missing = [p for p in REQUIRED_PLATFORMS if p not in platforms]
    if missing:
        fail("manifest.json lacks platforms: " + ", ".join(missing))

    sums = {}
    sums_path = os.path.join(directory, "SHA256SUMS")
    if os.path.exists(sums_path):
        with open(sums_path) as f:
            for line in f:
                digest, _, archive = line.rstrip("\n").partition("  ")
                sums[archive] = digest

    pinned = {}
    checked = {}
    for platform in sorted(platforms):
        entry = platforms[platform]
        if not re.fullmatch(r"[a-z0-9_]+-[a-z0-9_]+(-musl)?", platform) or not isinstance(entry, dict):
            fail("manifest.json: bad platform entry %r" % platform)
        archive = entry.get("archive")
        program = entry.get("program")
        if not isinstance(archive, str) or not ARCHIVE_RE.fullmatch(archive):
            fail("%s: bad archive name %r" % (platform, archive))
        expected_program = NAME + ".exe" if platform.startswith("windows-") else NAME
        if program != expected_program:
            fail("%s: program %r, expected %r" % (platform, program, expected_program))
        if archive not in checked:
            path = os.path.join(directory, archive)
            if not os.path.isfile(path):
                fail("%s: %s was not downloaded" % (platform, archive))
            digest = sha256_file(path)
            size = os.path.getsize(path)
            check_archive(path, program)
            if sums and sums.get(archive) != digest:
                fail("%s: SHA-256 differs from SHA256SUMS" % archive)
            checked[archive] = (digest, size)
        digest, size = checked[archive]
        if entry.get("sha256") != digest or entry.get("size") != size:
            fail("%s: %s does not match manifest.json (sha256 %s, size %d)" % (platform, archive, digest, size))
        pinned[platform] = {"archive": archive, "sha256": digest, "size": size, "program": program}

    return {
        "name": NAME,
        "version": version,
        "protocol": protocol,
        "repository": repo,
        "tag": tag,
        "baseUrl": "https://github.com/%s/releases/download/%s/" % (repo, tag),
        "platforms": pinned,
    }


def main():
    parser = argparse.ArgumentParser(description="Pin a kate-code-helper release in Kate Code's source.")
    parser.add_argument("version", help="the helper version, e.g. 0.1.0 (tag remote-helper-v<version>)")
    parser.add_argument("--repo", default=DEFAULT_REPO, help="GitHub repository (default %(default)s)")
    parser.add_argument("--from-dir", help="use the release's files from this directory instead of downloading")
    parser.add_argument("--output", default=DEFAULT_OUTPUT, help="pin file to write (default: %(default)s)")
    args = parser.parse_args()

    if not re.fullmatch(r"[0-9]+(\.[0-9]+){1,3}", args.version):
        fail("bad version %r" % args.version)
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repo):
        fail("bad repository %r" % args.repo)
    tag = "remote-helper-v" + args.version

    if args.from_dir:
        pin = build_pin(args.version, args.repo, tag, args.from_dir)
    else:
        # A new, empty directory for the downloads; only the release's
        # files are removed from it afterwards.
        directory = tempfile.mkdtemp(prefix="kate-code-helper-release-")
        try:
            download(args.repo, tag, directory)
            pin = build_pin(args.version, args.repo, tag, directory)
        finally:
            for name in os.listdir(directory):
                path = os.path.join(directory, name)
                if os.path.isfile(path) and not os.path.islink(path):
                    os.remove(path)
            try:
                os.rmdir(directory)
            except OSError:
                print("pin-release: left %s behind" % directory, file=sys.stderr)

    old = None
    try:
        with open(args.output) as f:
            old = json.load(f)
    except (OSError, ValueError):
        pass
    with open(args.output, "w") as f:
        json.dump(pin, f, indent=2, sort_keys=True)
        f.write("\n")

    archives = sorted({p["archive"] for p in pin["platforms"].values()})
    previous = " (was %s)" % old.get("version") if isinstance(old, dict) and old.get("version") != pin["version"] else ""
    print("Pinned %s %s%s, protocol %d, %d platforms, %d archives:" % (
        NAME, pin["version"], previous, pin["protocol"], len(pin["platforms"]), len(archives)))
    for archive in archives:
        entry = next(p for p in pin["platforms"].values() if p["archive"] == archive)
        print("  %s  %s" % (entry["sha256"], archive))
    print("Wrote %s; review the diff and commit it." % os.path.relpath(args.output))


if __name__ == "__main__":
    main()
