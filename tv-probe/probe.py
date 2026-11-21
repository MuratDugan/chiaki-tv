#!/usr/bin/env python3
"""TV'nin tarayicisinda olcum sayfasini acar ve sonuclari toplar.

Kullanim: python3 probe.py [TV_IP] [etiket]
Sonuclar results/ altina JSON satirlari olarak yazilir.
"""
import http.server
import json
import os
import socket
import sys
import threading
import time

from tvws import TV, TVError

TV_IP = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("TV_IP", "")
LABEL = sys.argv[2] if len(sys.argv) > 2 else "olcum"
PORT = 8765
MAX_WAIT = 300
HERE = os.path.dirname(os.path.abspath(__file__))
WWW = os.path.join(HERE, "www")
OUT_DIR = os.path.join(HERE, "results")
FILES = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/probe.js": ("probe.js", "application/javascript; charset=utf-8"),
    "/worker.js": ("worker.js", "application/javascript; charset=utf-8"),
}

os.makedirs(OUT_DIR, exist_ok=True)
out_path = os.path.join(OUT_DIR, "%s-%s.jsonl" % (time.strftime("%Y%m%d-%H%M%S"), LABEL))
done = threading.Event()
seen = threading.Event()
lock = threading.Lock()


class Handler(http.server.BaseHTTPRequestHandler):
    def _allowed(self):
        # Yalnizca TV'ye (ve yerel teste) hizmet ver.
        return self.client_address[0] in (TV_IP, "127.0.0.1")

    def do_GET(self):
        if not self._allowed():
            self.send_error(403)
            return
        entry = FILES.get(self.path.split("?")[0])
        if not entry:
            self.send_error(404)
            return
        seen.set()
        body = open(os.path.join(WWW, entry[0]), "rb").read()
        self.send_response(200)
        self.send_header("Content-Type", entry[1])
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if not self._allowed():
            self.send_error(403)
            return
        n = int(self.headers.get("Content-Length", 0) or 0)
        body = self.rfile.read(min(n, 1 << 20))
        self.send_response(204)
        self.end_headers()
        try:
            d = json.loads(body.decode("utf-8", "replace"))
        except ValueError:
            return
        with lock:
            with open(out_path, "a") as f:
                f.write(json.dumps(d, ensure_ascii=False) + "\n")
            print(json.dumps(d, ensure_ascii=False), flush=True)
        if d.get("type") == "done":
            done.set()

    def log_message(self, *args):
        pass


def my_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect((TV_IP, 9))
    ip = s.getsockname()[0]
    s.close()
    return ip


def main():
    ip = my_ip()
    srv = http.server.ThreadingHTTPServer((ip, PORT), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    url = "http://%s:%d/?t=%d" % (ip, PORT, int(time.time()))
    print("# sunucu:", url, flush=True)

    tv = TV(TV_IP, HERE)
    try:
        tv.connect()
        ack = tv.open_browser(url)
        print("# tarayici acma cevabi:", json.dumps(ack)[:200] if ack else "yok", flush=True)
    except TVError as e:
        print("# TV HATASI:", e, flush=True)
        return 2
    finally:
        tv.close()

    if not seen.wait(45):
        print("# TV 45 sn icinde sayfayi istemedi (tarayici acilmadi ya da Mac guvenlik duvari engelliyor)", flush=True)
        return 3
    if not done.wait(MAX_WAIT):
        print("# olcum %d sn icinde bitmedi; eldeki sonuclar kaydedildi" % MAX_WAIT, flush=True)
    print("# kayit:", out_path, flush=True)
    srv.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
