#!/usr/bin/env python3
"""Fetch the third-party sources and host tools that native/build.py needs.

    python3 native/fetch_deps.py            fetch whatever is missing
    python3 native/fetch_deps.py --check    report what is present, download nothing

Sources go to chiaki-tv/vendor, host tools to chiaki-tv/tools/host, and the Tizen certificate
generator files used for signing to tv-probe/vendor/certgen. Everything is pinned: git checkouts
by commit, downloads by SHA-256. Needs git and Python 3. Nothing is installed outside this
repository, and a directory that already exists is never modified or removed.

Samsung's Pepper 47 toolchain cannot be fetched and is only checked. See docs/BUILDING.md.
"""
import argparse
import hashlib
import os
import platform
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
VENDOR = os.path.join(ROOT, "vendor")
HOST = os.path.join(ROOT, "tools", "host")
SDK = os.environ.get("NACL_SDK_ROOT", os.path.expanduser("~/Downloads/NaCl_SDK_v47f/pepper_47"))
CERTGEN = os.path.join(os.path.dirname(ROOT), "tv-probe", "vendor", "certgen")

# (directory, repository, commit, what it is)
GIT = [
    ("chiaki-ng", "https://github.com/chiaki-ng/chiaki-ng.git", "43728af2e639fecb8a7af1cd68c29820a40de973", "chiaki-ng"),
    ("nanopb", "https://github.com/nanopb/nanopb.git", "cad3c18ef15a663e30e3e43e3a752b66378adec1", "nanopb 0.4.9.1"),
    ("jerasure", "https://github.com/streetpea/jerasure.git", "505ccb4bc69eee8d7ea40a6089a056b99671134f", "jerasure"),
    ("gf-complete", "https://github.com/streetpea/gf-complete.git", "fa54a4670a5705c84abf6c24b92b0cd479625478", "gf-complete"),
    ("mbedtls", "https://github.com/Mbed-TLS/mbedtls.git", "5e146adef63b326b04282252639bebc2730939c6", "mbedTLS 2.28.9"),
    ("opus", "https://github.com/xiph/opus.git", "ddbe48383984d56acd9e1ab6a090c54ca6b735a6", "Opus 1.5.2"),
    ("json-c", "https://github.com/json-c/json-c.git", "b4c371fa0cbc4dcbaccc359ce9e957a22988fb34", "json-c 0.17"),
    ("curl", "https://github.com/curl/curl.git", "7eb8c048470ed2cc14dca75be9c1cdae7ac8498b", "curl 8.10.1"),
]

CACERT = ("https://curl.se/ca/cacert-2026-11-05.pem",
          "427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285")
PROTOC = ("https://github.com/protocolbuffers/protobuf/releases/download/v28.3/protoc-28.3-osx-universal_binary.zip",
          "52df502b263da20f3311b23b5c6553d10cc25c6ebb85df381d80a2806b6a698b")
PROTOBUF = ("https://files.pythonhosted.org/packages/ad/c3/2377c159e28ea89a91cf1ca223f827ae8deccb2c9c401e5ca233cd73002f/"
            "protobuf-5.28.3-py3-none-any.whl",
            "cee1757663fa32a1ee673434fcf3bf24dd54763c79690201208bafec62f19eed")

# Tizen Studio's certificate generator package, from the official Tizen SDK repository. Only its
# certificate files are kept: the developer CA that issues the author certificate, and the public
# distributor certificate. The archive is the macOS package; the files in it are not platform specific.
CERTGEN_ZIP = ("https://download.tizen.org/sdk/tizenstudio/official/binary/certificate-generator_0.1.4_macos-64.zip",
               "ca9e2c04423192e5cf42214c480c439af17999d6ff3331426937c39874e3a357")
CERTGEN_PARTS = {"data/tools/certificate-generator/certificates/developer/": "developer",
                 "data/tools/certificate-generator/certificates/distributor/sdk-public/": "public"}

problems = []


def say(name, state, detail=""):
    print("%-12s %-8s %s" % (name, state, detail), flush=True)


def git(*args, cwd=None):
    return subprocess.run(["git"] + list(args), cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def head_of(path):
    """The commit a directory is checked out at, or None if it is not its own git checkout."""
    if not os.path.exists(os.path.join(path, ".git")):
        return None
    res = git("rev-parse", "HEAD", cwd=path)
    return res.stdout.decode().strip() if res.returncode == 0 else None


def fetch_git(name, url, commit, label, check):
    path = os.path.join(VENDOR, name)
    if os.path.isdir(path):
        head = head_of(path)
        if head == commit:
            say(name, "ok", label)
        elif head:
            say(name, "kept", "is at %s, the build was tested with %s" % (head[:10], commit[:10]))
        else:
            say(name, "kept", "not a git checkout; cannot tell its version")
        return
    if check:
        say(name, "missing", label)
        problems.append(name)
        return
    tmp = path + ".partial"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    steps = [("init", "-q"), ("remote", "add", "origin", url), ("fetch", "-q", "--depth", "1", "origin", commit),
             ("-c", "advice.detachedHead=false", "checkout", "-q", "--detach", "FETCH_HEAD")]
    for step in steps:
        res = git(*step, cwd=tmp)
        if res.returncode != 0:
            shutil.rmtree(tmp, ignore_errors=True)
            say(name, "FAILED", "git %s: %s" % (step[0], res.stdout.decode(errors="replace").strip()[-300:]))
            problems.append(name)
            return
    if head_of(tmp) != commit:
        shutil.rmtree(tmp, ignore_errors=True)
        say(name, "FAILED", "the checkout is not commit %s" % commit[:10])
        problems.append(name)
        return
    os.rename(tmp, path)
    say(name, "fetched", "%s (%s)" % (label, commit[:10]))


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def download(url, want, dest):
    """Download to dest and verify it. Returns an error text, or None on success."""
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    tmp = dest + ".partial"
    last = ""
    for attempt in range(3):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "chiaki-tv-fetch"})
            with urllib.request.urlopen(req, timeout=60) as res, open(tmp, "wb") as out:
                shutil.copyfileobj(res, out)
            got = sha256(tmp)
            if got != want:
                os.remove(tmp)
                return "SHA-256 mismatch: got %s, expected %s" % (got, want)
            os.replace(tmp, dest)
            return None
        except OSError as e:
            last = str(e)
            time.sleep(2 * (attempt + 1))
    if os.path.exists(tmp):
        os.remove(tmp)
    return "download failed: " + last


def fetch_cacert(check):
    path = os.path.join(VENDOR, "cacert.pem")
    if os.path.exists(path):
        same = sha256(path) == CACERT[1]
        say("cacert.pem", "ok" if same else "kept", "Mozilla CA bundle" if same else "differs from the pinned bundle")
        return
    if check:
        say("cacert.pem", "missing", "Mozilla CA bundle")
        problems.append("cacert.pem")
        return
    err = download(CACERT[0], CACERT[1], path)
    say("cacert.pem", "FAILED" if err else "fetched", err or "Mozilla CA bundle, 2026-11-05")
    if err:
        problems.append("cacert.pem")


def unpack(archive, dest):
    """Extract a verified zip into a new directory, replacing nothing that exists."""
    tmp = dest + ".partial"
    shutil.rmtree(tmp, ignore_errors=True)
    with zipfile.ZipFile(archive) as z:
        z.extractall(tmp)
    os.rename(tmp, dest)


def fetch_protoc(check):
    dest = os.path.join(HOST, "protoc")
    binary = os.path.join(dest, "bin", "protoc")
    if os.path.exists(binary):
        say("protoc", "ok", "host tool")
        return
    if sys.platform != "darwin":
        say("protoc", "missing", "only the macOS build is fetched; put protoc 28.3 at " + binary)
        problems.append("protoc")
        return
    if check or os.path.exists(dest):
        say("protoc", "missing", "host tool, expected at " + binary)
        problems.append("protoc")
        return
    archive = os.path.join(HOST, "protoc.zip")
    err = download(PROTOC[0], PROTOC[1], archive)
    if not err:
        unpack(archive, dest)
        os.remove(archive)
        os.chmod(binary, 0o755)
    say("protoc", "FAILED" if err else "fetched", err or "protoc 28.3")
    if err:
        problems.append("protoc")


def fetch_protobuf(check):
    dest = os.path.join(HOST, "pylib")
    if os.path.exists(os.path.join(dest, "google", "protobuf", "__init__.py")):
        say("protobuf", "ok", "Python package for the nanopb generator")
        return
    if check or os.path.exists(dest):
        say("protobuf", "missing", "Python package, expected in " + dest)
        problems.append("protobuf")
        return
    archive = os.path.join(HOST, "protobuf.whl")
    err = download(PROTOBUF[0], PROTOBUF[1], archive)
    if not err:
        unpack(archive, dest)
        os.remove(archive)
    say("protobuf", "FAILED" if err else "fetched", err or "protobuf 5.28.3 (pure Python)")
    if err:
        problems.append("protobuf")


def fetch_certgen(check):
    if os.path.exists(os.path.join(CERTGEN, "developer", "conf.ini")) and os.path.isdir(os.path.join(CERTGEN, "public")):
        say("certgen", "ok", "signing files")
        return
    if check or os.path.exists(CERTGEN):
        say("certgen", "missing", "signing files, expected in " + CERTGEN)
        problems.append("certgen")
        return
    archive = CERTGEN + ".zip"
    err = download(CERTGEN_ZIP[0], CERTGEN_ZIP[1], archive)
    if not err:
        tmp = CERTGEN + ".partial"
        shutil.rmtree(tmp, ignore_errors=True)
        with zipfile.ZipFile(archive) as z:
            for member in z.namelist():
                for prefix, sub in CERTGEN_PARTS.items():
                    name = member[len(prefix):]
                    if member.startswith(prefix) and name and "/" not in name:
                        os.makedirs(os.path.join(tmp, sub), exist_ok=True)
                        with z.open(member) as src, open(os.path.join(tmp, sub, name), "wb") as out:
                            shutil.copyfileobj(src, out)
        os.rename(tmp, CERTGEN)
        os.remove(archive)
    say("certgen", "FAILED" if err else "fetched", err or "Tizen certificate generator 0.1.4, certificate files only")
    if err:
        problems.append("certgen")


def check_toolchain():
    """The one thing this script cannot fetch. Reported, never counted as a failure of the fetch."""
    tool = os.path.join(SDK, "toolchain", "mac_arm_newlib", "bin", "arm-nacl-gcc")
    if os.path.exists(tool):
        say("toolchain", "ok", SDK)
    else:
        say("toolchain", "MANUAL", "Samsung Pepper 47 not found at %s; install it or set NACL_SDK_ROOT" % SDK)


def main():
    parser = argparse.ArgumentParser(description="Fetch the pinned sources and host tools for native/build.py.")
    parser.add_argument("--check", action="store_true", help="report what is present, download nothing")
    args = parser.parse_args()
    if not args.check and not shutil.which("git"):
        sys.exit("git is not installed.")
    os.makedirs(VENDOR, exist_ok=True)
    for dep in GIT:
        fetch_git(*dep, check=args.check)
    fetch_cacert(args.check)
    fetch_protoc(args.check)
    fetch_protobuf(args.check)
    fetch_certgen(args.check)
    check_toolchain()
    if problems:
        print("\nNot ready: " + ", ".join(problems))
        return 1
    print("\nEverything fetched is in place. Next: python3 native/build.py")
    if platform.system() != "Darwin":
        print("Note: build.py uses the macOS toolchain of the SDK; other hosts are untested.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
