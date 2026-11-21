#!/usr/bin/env python3
"""TV'ye teshis uygulamasi kurar ve calistirir (gelistirici modu + sdb gerekir).

  python3 tvapp.py build [old|new]   # yazar sertifikasi + imzali .wgt (dagitici sertifika secimi)
  python3 tvapp.py install           # sdb ile yukle ve kur
  python3 tvapp.py run [etiket]      # sunucuyu ac, uygulamayi baslat, olcumleri topla
  python3 tvapp.py watch [etiket]    # YouTube/Netflix/Disney+ sirayla acilirken bellegi izle
  python3 tvapp.py uninstall         # uygulamayi TV'den kaldir
"""
import http.server
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.request
import zipfile
import zlib

import wgtsign
from sdb import Sdb, SdbError, run as sdb_run, tv_ip

TV_IP = tv_ip()  # TV_IP ortam degiskeni ya da tv-probe/tv-ip.txt
PORT = 8765
PKG = "TvTeshis01"
APP_ID = PKG + ".TvTeshis"
REMOTE_WGT = "/home/owner/share/tmp/sdk_tools/tmp/TvTeshis.wgt"
P12_PASS = "tizenpkcs12passfordsigner"  # Tizen SDK'nin herkese acik dagitici parolasi

HERE = os.path.dirname(os.path.abspath(__file__))
VENDOR = os.path.join(HERE, "vendor", "certgen")
CERTS = os.path.join(HERE, "certs")
BUILD = os.path.join(HERE, "build")
SRC = os.path.join(HERE, "app")
WWW = os.path.join(HERE, "www")
OUT_DIR = os.path.join(HERE, "results")
WGT = os.path.join(BUILD, "TvTeshis.wgt")


def ossl(*args, data=None):
    return subprocess.run(["openssl"] + list(args), input=data, check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


def my_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect((TV_IP, 9))
    ip = s.getsockname()[0]
    s.close()
    return ip


def png(w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def ca_password():
    for line in open(os.path.join(VENDOR, "developer", "conf.ini"), errors="replace"):
        if line.startswith("PASSWD_OF_ISSUER_FOR_DEV="):
            return line.split("=", 1)[1].strip()
    raise SystemExit("conf.ini icinde CA parolasi yok")


def ensure_certs(dist):
    """Yazar anahtari/sertifikasi uretir, dagitici anahtarini p12'den cikarir."""
    os.makedirs(CERTS, exist_ok=True)
    os.chmod(CERTS, 0o700)
    dev = os.path.join(VENDOR, "developer")
    key, cer = os.path.join(CERTS, "author.key"), os.path.join(CERTS, "author.cer")
    if not (os.path.exists(cer) and os.path.getsize(cer) > 0):
        ossl("genrsa", "-out", key, "2048")
        os.chmod(key, 0o600)
        csr, ext = os.path.join(CERTS, "author.csr"), os.path.join(CERTS, "author.ext")
        ossl("req", "-new", "-key", key, "-subj", "/CN=TvTeshis", "-out", csr)
        open(ext, "w").write("extendedKeyUsage=codeSigning\n")
        # CA anahtarini once coz: "x509 -req" parolayi -CAkey icin kabul etmiyor.
        ca_key = os.path.join(CERTS, "devca.key")
        ossl("rsa", "-in", os.path.join(dev, "tizen-developer-ca-privatekey.pem"),
             "-passin", "pass:" + ca_password(), "-out", ca_key)
        os.chmod(ca_key, 0o600)
        try:
            ossl("x509", "-req", "-in", csr, "-CA", os.path.join(dev, "tizen-developer-ca.cer"),
                 "-CAkey", ca_key, "-set_serial", str(int(time.time())),
                 "-days", "3650", "-sha256", "-extfile", ext, "-out", cer)
        finally:
            os.remove(ca_key)
    author_chain = [ossl("x509", "-in", cer, "-outform", "der"),
                    ossl("x509", "-in", os.path.join(dev, "tizen-developer-ca.cer"), "-outform", "der")]

    suffix = "-new" if dist == "new" else ""
    pub = os.path.join(VENDOR, "public")
    dkey = os.path.join(CERTS, "distributor%s.key" % suffix)
    if not os.path.exists(dkey):
        pem = ossl("pkcs12", "-legacy", "-in", os.path.join(pub, "tizen-distributor-signer%s.p12" % suffix),
                   "-passin", "pass:" + P12_PASS, "-nodes", "-nocerts")
        open(dkey, "wb").write(pem)
        os.chmod(dkey, 0o600)
    dist_chain = [ossl("x509", "-in", os.path.join(pub, "tizen-distributor-signer%s.cer" % suffix), "-outform", "der"),
                  ossl("x509", "-in", os.path.join(pub, "tizen-distributor-ca%s.cer" % suffix), "-outform", "der")]
    return key, author_chain, dkey, dist_chain


def build(dist="old"):
    author_key, author_chain, dist_key, dist_chain = ensure_certs(dist)
    stage = os.path.join(BUILD, "app")
    shutil.rmtree(BUILD, ignore_errors=True)
    os.makedirs(stage)
    shutil.copy(os.path.join(SRC, "config.xml"), stage)
    html = open(os.path.join(SRC, "index.html"), encoding="utf-8").read()
    with open(os.path.join(stage, "index.html"), "w", encoding="utf-8", newline="\n") as f:
        f.write(html.replace("__SERVER__", "http://%s:%d" % (my_ip(), PORT)))
    open(os.path.join(stage, "icon.png"), "wb").write(png(117, 117, (0x7F, 0xD1, 0xFF)))

    files = wgtsign.sign_dir(stage, author_key, author_chain, dist_key, dist_chain)
    for role, sid in (("author", "AuthorSignature"), ("distributor", "DistributorSignature")):
        ok = wgtsign.prop_digest(sid, role) == wgtsign.KNOWN_PROP[role]
        print("kanoniklestirme kontrolu (%s): %s" % (role, "Tizen Studio ile ayni" if ok else "FARKLI"))
    with zipfile.ZipFile(WGT, "w", zipfile.ZIP_DEFLATED) as z:
        for name in files + ["author-signature.xml", "signature1.xml"]:
            z.write(os.path.join(stage, name), name)
    print("paket: %s (%d bayt, dagitici: %s, sunucu: %s)" % (WGT, os.path.getsize(WGT), dist, my_ip()))


def install():
    dev = Sdb(TV_IP)
    dev.connect()
    try:
        print("gonderildi: %d bayt -> %s" % (dev.push(WGT, REMOTE_WGT), REMOTE_WGT))
    finally:
        dev.close()
    # TV, dosya gonderimiyle ayni baglantidan gelen kurulum komutunu reddediyor: yeni baglanti ac.
    out = sdb_run("shell:0 vd_appinstall %s %s" % (APP_ID, REMOTE_WGT), timeout=120)
    lines = [l for l in out.splitlines() if "installing[" not in l]
    print("\n".join(lines).strip())


def uninstall():
    print(sdb_run("shell:0 vd_appuninstall " + APP_ID, timeout=60).strip())


# --- olcum sunucusu ---------------------------------------------------------
done = threading.Event()
seen = threading.Event()
lock = threading.Lock()
out_path = None
js_prefix = b""          # izleme modunda olcum koduna eklenen ayar
started = time.time()
compact = False


def record(d):
    """Olcumu dosyaya yazar ve ekrana basar; rt = sunucu saatine gore saniye."""
    d["rt"] = round(time.time() - started, 1)
    with lock:
        with open(out_path, "a") as f:
            f.write(json.dumps(d, ensure_ascii=False) + "\n")
        if compact and d.get("type") == "w":
            ids = d.get("running")
            print("rt=%6.1f  bos=%6.1f MB  islemci=%5s%%  durum=%-8s calisan=%s  arka_planda=%s" % (
                d["rt"], d.get("availMemMB") or 0, d.get("cpuLoad"), d.get("mem"),
                len(ids) if isinstance(ids, list) else ids, d.get("hidden")), flush=True)
        else:
            line = json.dumps(d, ensure_ascii=False)
            print(line if len(line) < 1500 else line[:1500] + " ...(kisaltildi)", flush=True)


class Handler(http.server.BaseHTTPRequestHandler):
    def _allowed(self):
        return self.client_address[0] in (TV_IP, "127.0.0.1")

    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.end_headers()

    def do_GET(self):
        if not self._allowed() or self.path.split("?")[0] != "/tizen-probe.js":
            self.send_error(404)
            return
        seen.set()
        body = js_prefix + open(os.path.join(WWW, "tizen-probe.js"), "rb").read()
        self.send_response(200)
        self.send_header("Content-Type", "application/javascript; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self._cors()
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if not self._allowed():
            self.send_error(403)
            return
        n = int(self.headers.get("Content-Length", 0) or 0)
        body = self.rfile.read(min(n, 4 << 20))
        self.send_response(204)
        self._cors()
        self.end_headers()
        try:
            d = json.loads(body.decode("utf-8", "replace"))
        except ValueError:
            return
        record(d)
        if d.get("type") == "done":
            done.set()

    def log_message(self, *args):
        pass


def launch():
    # Acik kalmissa kapat ki sayfa bastan yuklensin; TV bu komutlarda zaman asimi argumani istiyor.
    try:
        sdb_run("shell:0 kill " + APP_ID, timeout=10)
        time.sleep(2)
    except SdbError:
        pass
    for svc in ("shell:0 debug %s 300" % APP_ID, "shell:0 execute %s 0" % APP_ID):
        try:
            out = sdb_run(svc, timeout=20).strip()
            print("# baslatma (%s): %s" % (svc.split()[1], out.splitlines()[-1] if out else ""), flush=True)
            return True
        except SdbError as e:
            print("# %s: %s" % (svc, e), flush=True)
    return False


def run_probe(label):
    global out_path
    os.makedirs(OUT_DIR, exist_ok=True)
    out_path = os.path.join(OUT_DIR, "%s-tizen-%s.jsonl" % (time.strftime("%Y%m%d-%H%M%S"), label))
    srv = http.server.ThreadingHTTPServer((my_ip(), PORT), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    if not launch():
        return 2
    if not seen.wait(40):
        print("# uygulama 40 sn icinde sunucuya baglanmadi", flush=True)
        return 3
    if not done.wait(240):
        print("# olcum bitmedi; eldeki sonuclar kaydedildi", flush=True)
    print("# kayit:", out_path, flush=True)
    srv.shutdown()
    return 0


def rest(method, store_id):
    """Magaza uygulamasini TV'nin REST arayuzuyle acar (POST) ya da kapatir (DELETE)."""
    req = urllib.request.Request("http://%s:8001/api/v2/applications/%s" % (TV_IP, store_id),
                                 method=method, data=b"" if method == "POST" else None)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return "%d %s" % (r.status, r.read()[:120].decode(errors="replace"))
    except Exception as e:  # ag hatasi olcumu durdurmasin
        return "HATA %s" % e


WATCH_APPS = [("YouTube", "111299001912"), ("Netflix", "11101200001"), ("Disney+", "3201901017640")]


def watch(label, baseline=20, per_app=45, cooldown=30):
    """Agir uygulamalar sirayla acilirken bos bellegi ve calisan uygulamalari kaydeder."""
    global out_path, js_prefix, started, compact
    total = baseline + per_app * len(WATCH_APPS) + cooldown + 15
    js_prefix = ("window.__CFG=%s;\n" % json.dumps({"watch": True, "seconds": total, "interval": 2000})).encode()
    compact = True
    os.makedirs(OUT_DIR, exist_ok=True)
    out_path = os.path.join(OUT_DIR, "%s-izleme-%s.jsonl" % (time.strftime("%Y%m%d-%H%M%S"), label))
    srv = http.server.ThreadingHTTPServer((my_ip(), PORT), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    if not launch():
        return 2
    if not seen.wait(40):
        print("# uygulama 40 sn icinde sunucuya baglanmadi", flush=True)
        return 3
    started = time.time()
    record({"type": "event", "what": "baslangic (yalnizca teshis uygulamasi)"})
    time.sleep(baseline)
    for name, sid in WATCH_APPS:
        record({"type": "event", "what": "AC " + name, "result": rest("POST", sid)})
        time.sleep(per_app)
    for name, sid in reversed(WATCH_APPS):
        record({"type": "event", "what": "KAPAT " + name, "result": rest("DELETE", sid)})
        time.sleep(3)
    time.sleep(cooldown)
    record({"type": "event", "what": "bitis"})
    print("# kayit:", out_path, flush=True)
    srv.shutdown()
    return 0


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    arg = sys.argv[2] if len(sys.argv) > 2 else None
    try:
        if cmd == "build":
            build(arg or "old")
        elif cmd == "install":
            install()
        elif cmd == "uninstall":
            uninstall()
        elif cmd == "run":
            return run_probe(arg or "olcum")
        elif cmd == "watch":
            return watch(arg or "izleme")
        else:
            print(__doc__)
            return 1
    except SdbError as e:
        print("# sdb HATASI:", e)
        return 2
    except subprocess.CalledProcessError as e:
        print("# openssl HATASI (%s): %s" % (" ".join(e.cmd[1:3]), e.stderr.decode(errors="replace")[:400]))
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
