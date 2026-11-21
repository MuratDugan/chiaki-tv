#!/usr/bin/env python3
"""chiaki-tv: yerel modulu derler, uygulamayi imzalar ve TV'ye kurar.

  python3 tvbuild.py native     # native/build.py -> build/native/chiaki_arm.nexe
  python3 tvbuild.py build      # imzali build/ChiakiTv.wgt
  python3 tvbuild.py install    # TV'ye gonder ve kur (TV'de gelistirici modu acik olmali)
  python3 tvbuild.py launch     # TV'de ac
  python3 tvbuild.py all        # native + build + install
  python3 tvbuild.py uninstall

Arac zinciri: Samsung Pepper 47 (NACL_SDK_ROOT, varsayilan ~/Downloads/NaCl_SDK_v47f/pepper_47).
Imzalama ve sdb icin ../tv-probe icindeki araclar kullanilir.
"""
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import time
import urllib.request
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tv-probe"))
import tvapp      # noqa: E402
import wgtsign    # noqa: E402
from sdb import Sdb, SdbError, run as sdb_run  # noqa: E402

SDK = os.environ.get("NACL_SDK_ROOT", os.path.expanduser("~/Downloads/NaCl_SDK_v47f/pepper_47"))
PKG = "ChiakiTv01"
APP_ID = PKG + ".ChiakiTv"
# CHIAKI_APP_DIR: paketlenecek arayuz klasoru (varsayilan app/); deneme icin baska bir kopya secilebilir.
SRC = os.environ.get("CHIAKI_APP_DIR") or os.path.join(HERE, "app")
BUILD = os.path.join(HERE, "build")
WGT = os.path.join(BUILD, "ChiakiTv.wgt")
REMOTE = "/home/owner/share/tmp/sdk_tools/tmp/ChiakiTv.wgt"
NEXE = "chiaki_arm.nexe"


def native():
    res = subprocess.run([sys.executable, os.path.join(HERE, "native", "build.py")])
    if res.returncode != 0:
        raise SystemExit("yerel derleme basarisiz")


def icon_png(size=256):
    """Uygulama simgesi: indigo zemin, beyaz oynat ucgeni, yesil nokta (RGBA)."""
    def rounded(x, y, half, r):  # yuvarlak koseli karenin isaretli uzakligi
        dx, dy = abs(x - 128) - (half - r), abs(y - 128) - (half - r)
        return math.hypot(max(dx, 0), max(dy, 0)) + min(max(dx, dy), 0) - r

    def cov(d):
        return max(0.0, min(1.0, 0.5 - d))

    def tri(x, y):  # (96,80) (184,128) (96,176) ucgeninin icinde mi (yumusatilmis)
        d1 = (x - 96)
        d2 = ((184 - x) * 48 - abs(y - 128) * 88) / 100.0
        return cov(-min(d1, d2))

    s = size / 256.0
    rows = []
    for y in range(size):
        row = bytearray([0])
        for x in range(size):
            px, py = x / s, y / s
            alpha = cov(rounded(px, py, 124, 56) * s)
            t = py / 256.0
            col = [0x43 + (0x1E - 0x43) * t, 0x38 + (0x1B - 0x38) * t, 0xCA + (0x4B - 0xCA) * t]
            w = tri(px, py)
            col = [c + (0xF8 - c) * w for c in col]
            dot = cov((math.hypot(px - 188, py - 76) - 15) * s)
            col = [col[0] + (0x22 - col[0]) * dot, col[1] + (0xC5 - col[1]) * dot, col[2] + (0x5E - col[2]) * dot]
            row += bytes([int(col[0]), int(col[1]), int(col[2]), int(alpha * 255)])
        rows.append(bytes(row))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))


def build():
    nexe = os.path.join(BUILD, "native", NEXE)
    if not os.path.exists(nexe):
        raise SystemExit("once 'native' adimini calistir: %s yok" % nexe)
    author_key, author_chain, dist_key, dist_chain = tvapp.ensure_certs("old")
    stage = os.path.join(BUILD, "app")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    for name in ("config.xml", "index.html", "style.css", "app.js"):
        shutil.copy(os.path.join(SRC, name), stage)
    shutil.copy(nexe, stage)
    with open(os.path.join(stage, "chiaki.nmf"), "w", newline="\n") as f:
        json.dump({"program": {"arm": {"url": NEXE}}}, f)
    open(os.path.join(stage, "icon.png"), "wb").write(icon_png())
    files = wgtsign.sign_dir(stage, author_key, author_chain, dist_key, dist_chain)
    if os.path.exists(WGT):
        os.remove(WGT)
    with zipfile.ZipFile(WGT, "w", zipfile.ZIP_DEFLATED) as z:
        for name in files + ["author-signature.xml", "signature1.xml"]:
            z.write(os.path.join(stage, name), name)
    print("paket: build/ChiakiTv.wgt (%d KB)" % (os.path.getsize(WGT) // 1024))


def install():
    dev = Sdb(tvapp.TV_IP)
    dev.connect()
    try:
        print("gonderildi: %d bayt" % dev.push(WGT, REMOTE))
    finally:
        dev.close()
    out = sdb_run("shell:0 vd_appinstall %s %s" % (APP_ID, REMOTE), timeout=180)
    print("\n".join(l for l in out.splitlines() if "installing[" not in l).strip())


def close_app():
    try:
        req = urllib.request.Request("http://%s:8001/api/v2/applications/%s" % (tvapp.TV_IP, APP_ID), method="DELETE")
        urllib.request.urlopen(req, timeout=10).read()
        time.sleep(1.5)
    except Exception:
        pass


def launch(debug=False):
    """Uygulamayi acar; debug=True ise uzaktan denetim portunu dondurur."""
    close_app()
    out = sdb_run("shell:0 %s %s %s" % ("debug" if debug else "execute", APP_ID, "300" if debug else "0"), timeout=25)
    line = (out.strip().splitlines() or [""])[-1]
    print("TV:", line)
    return int(line.rsplit("port:", 1)[1].strip()) if debug and "port:" in line else None


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd not in ("native", "build", "install", "launch", "all", "uninstall"):
        print(__doc__)
        return 1
    try:
        if cmd in ("native", "all"):
            native()
        if cmd in ("build", "all"):
            build()
        if cmd in ("install", "all"):
            install()
        if cmd == "launch":
            launch()
        if cmd == "uninstall":
            print(sdb_run("shell:0 vd_appuninstall " + APP_ID, timeout=60).strip())
    except SdbError as e:
        print("sdb hatasi:", e)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
