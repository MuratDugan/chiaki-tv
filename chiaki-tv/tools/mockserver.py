#!/usr/bin/env python3
"""Arayuzleri TV olmadan denemek icin sahte sunucu (yalnizca bu makineden erisilir).

  python3 tools/mockserver.py        # http://localhost:8790/        -> telefon kurulum sayfasi
                                     # http://localhost:8790/tv/#mock -> TV arayuzu (modulsuz)
Eslestirme kodu 1234. PSN girisi, konsol listesi ve baglanti adimlari taklit edilir.
"""
import base64
import hashlib
import http.server
import json
import os
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PORT = 8790
TOKEN = "mock-token"
PAIR = {"fails": 0, "until": 0.0}
LOCK = threading.Lock()
STATE = {
    "phase": "idle", "message": "", "error": "", "ip": "192.168.1.50", "port": 8730, "loggedIn": False,
    "needPin": False, "pinIncorrect": False, "lastDuid": "", "gamepad": "",
    "consoles": [], "settings": {"resolution": 720, "fps": 60, "bitrate": 0, "lowLatency": True}, "stream": {"fps": 0, "dropped": 0},
    "lang": "en",
}
CONSOLES = [{"duid": "ab" * 32, "name": "Salon PS5", "remoteplay": True},
            {"duid": "cd" * 32, "name": "Çocuk odası <b>PS5</b>", "remoteplay": False}]
LOG = ["[I] sahte sunucu basladi"]
PAD = {"frames": 0, "last": None, "name": "", "authed": False}
FILES = {"/": ("setup/index.html", "text/html; charset=utf-8"), "/pad": ("setup/pad.html", "text/html; charset=utf-8"), "/tv/": ("app/index.html", "text/html; charset=utf-8"),
         "/tv/style.css": ("app/style.css", "text/css; charset=utf-8"),
         "/tv/app.js": ("app/app.js", "application/javascript; charset=utf-8")}


def later(delay, fn):
    t = threading.Timer(delay, fn)
    t.daemon = True
    t.start()


def L(en, tr):
    """Sahte sunucunun kendi metinleri: gecerli dile gore Ingilizce ya da Turkce."""
    return en if STATE["lang"] == "en" else tr


def set_state(**kw):
    with LOCK:
        STATE.update(kw)
    LOG.append("> " + ", ".join("%s=%s" % (k, str(v)[:40]) for k, v in kw.items()))


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, body, ctype="application/json; charset=utf-8"):
        raw = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(raw)

    def authed(self):
        return self.headers.get("X-Token") == TOKEN

    def ws_send(self, text):
        raw = text.encode()
        self.wfile.write(bytes([0x81, len(raw)]) + raw)  # sahte sunucu yalnizca kisa metin yollar
        self.wfile.flush()

    def ws_pad(self):
        """Kol sayfasinin WebSocket ucu: gercek modulle ayni mesajlar (auth, ping, 15 baytlik durum)."""
        key = self.headers.get("Sec-WebSocket-Key", "")
        accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())
        self.wfile.flush()
        authed = False
        while True:
            head = self.rfile.read(2)
            if len(head) < 2:
                break
            op, n = head[0] & 0x0f, head[1] & 0x7f
            if n == 126:
                n = int.from_bytes(self.rfile.read(2), "big")
            mask = self.rfile.read(4)
            data = bytes(b ^ mask[i & 3] for i, b in enumerate(self.rfile.read(n)))
            if op == 8:
                break
            if op == 1:
                msg = json.loads(data.decode() or "{}")
                if msg.get("t") == "auth":
                    if msg.get("token") != TOKEN:
                        self.ws_send(json.dumps({"t": "denied"}))
                        break
                    authed = True
                    PAD.update(authed=True, name=msg.get("name", ""))
                    self.ws_send(json.dumps({"t": "ok", "streaming": STATE["phase"] == "streaming"}))
                elif msg.get("t") == "ping":
                    self.ws_send(json.dumps({"t": "pong", "n": msg.get("n"), "streaming": STATE["phase"] == "streaming"}))
                elif msg.get("t") == "menu":
                    PAD["menu"] = PAD.get("menu", 0) + 1
                elif msg.get("t") == "name":
                    PAD["name"] = msg.get("name", "")
            elif op == 2 and authed and len(data) == 15 and data[0] == 1:
                PAD["frames"] += 1
                PAD["last"] = {"buttons": int.from_bytes(data[1:5], "little"), "l2": data[5], "r2": data[6],
                               "lx": int.from_bytes(data[7:9], "little", signed=True),
                               "ly": int.from_bytes(data[9:11], "little", signed=True),
                               "rx": int.from_bytes(data[11:13], "little", signed=True),
                               "ry": int.from_bytes(data[13:15], "little", signed=True)}
        self.close_connection = True

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/ws/pad" and self.headers.get("Upgrade", "").lower() == "websocket":
            return self.ws_pad()
        if path == "/shot/frame.png":  # ekran goruntusu: TV'den alinmis canli kare (tvdebug ile yakalanir)
            frame = os.path.join(ROOT, "build", "shots", "stream-frame.png")
            if os.path.exists(frame):
                return self.send(200, open(frame, "rb").read(), "image/png")
            return self.send(404, {"ok": False})
        if path == "/shot/tv":  # ekran goruntusu: TV arayuzunu ornek durumla ciz (?phase=idle|connecting)
            phase = "connecting" if "phase=connecting" in self.path else "idle"
            demo = {"phase": phase, "message": "Reaching the console" if phase == "connecting" else "", "error": "",
                    "ip": "192.168.1.50", "port": 8730, "code": "4821", "loggedIn": True, "needPin": False,
                    "pinIncorrect": False, "lastDuid": "ab" * 32, "gamepad": "Xbox controller", "lang": "en",
                    "consoles": [{"duid": "ab" * 32, "name": "Living room PS5", "remoteplay": True}],
                    "settings": {"resolution": 720, "fps": 60, "bitrate": 0, "lowLatency": True},
                    "stream": {"fps": 0, "dropped": 0}}
            page = ("<!doctype html><meta charset=utf-8><style>html,body{margin:0;background:#000;overflow:hidden}"
                    "iframe{border:0;width:1920px;height:1080px;display:block}</style>"
                    "<iframe id=f src='/tv/#mock'></iframe><script>var f=document.getElementById('f');"
                    "f.onload=function(){setTimeout(function(){f.contentWindow.__render(%s);},300);};</script>" % json.dumps(demo))
            return self.send(200, page.encode(), "text/html; charset=utf-8")
        if path == "/shot/go":  # ekran goruntusu: eslesmis gibi ac (?to=/ ya da /pad)
            target = "/pad?touch=1" if "to=padtouch" in self.path else ("/pad" if "to=pad" in self.path else "/")
            set_state(message="", error="", loggedIn=True, lastDuid="ab" * 32,
                      consoles=[{"duid": "ab" * 32, "name": "Living room PS5", "remoteplay": True}])
            page = ("<!doctype html><script>localStorage.setItem('chiakitv.token','%s');"
                    "location.replace('%s');</script>" % (TOKEN, target))
            return self.send(200, page.encode(), "text/html; charset=utf-8")
        if path == "/mock/pad":  # deneme icin: sayfanin en son gonderdigi kol durumu
            return self.send(200, dict(PAD))
        if path == "/mock/stream":  # deneme icin: yayin durumunu ac/kapat
            set_state(phase="idle" if STATE["phase"] == "streaming" else "streaming")
            return self.send(200, {"phase": STATE["phase"]})
        if path in FILES:
            rel, ctype = FILES[path]
            return self.send(200, open(os.path.join(ROOT, rel), "rb").read(), ctype)
        if not path.startswith("/api/"):
            return self.send(404, {"ok": False, "error": L("not found", "bulunamadi")})
        if path == "/api/lang":  # belirtec istemez: eslestirme oncesi sayfa ve kol sayfasi dili buradan ogrenir
            return self.send(200, {"lang": STATE["lang"]})
        if not self.authed():
            return self.send(401, {"ok": False, "error": L("Pairing required.", "Eşleştirme gerekli.")})
        if path == "/api/state":
            with LOCK:
                return self.send(200, dict(STATE))
        if path == "/api/log":
            return self.send(200, {"lines": LOG[-300:]})
        self.send(404, {"ok": False, "error": L("not found", "bulunamadi")})

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        try:
            body = json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            body = {}
        path = self.path
        if path == "/api/pair":
            now = time.time()
            if now < PAIR["until"]:
                left = int(PAIR["until"] - now) + 1
                return self.send(429, {"ok": False, "error": L("Too many wrong attempts. Try again in %d seconds.",
                                                                "Çok fazla hatalı deneme. %d saniye sonra yeniden dene.") % left})
            if body.get("code") == "1234":
                PAIR["fails"] = 0
                return self.send(200, {"ok": True, "token": TOKEN})
            PAIR["fails"] += 1
            if PAIR["fails"] >= 5:
                PAIR["fails"] = 0
                PAIR["until"] = now + 30
            return self.send(401, {"ok": False, "error": L("The code did not match. Check the code on the TV.",
                                                            "Kod uyuşmadı. Televizyondaki kodu kontrol et.")})
        if not self.authed():
            return self.send(401, {"ok": False, "error": L("pairing required", "eslestirme gerekli")})
        if path == "/api/psn/begin":
            return self.send(200, {"ok": True, "url": "https://example.invalid/psn-giris"})
        if path == "/api/psn/finish":
            set_state(phase="busy", message=L("Signing in to PSN", "PSN oturumu aciliyor"), error="")
            if "code=" not in body.get("redirect", ""):
                later(1.5, lambda: set_state(phase="idle", message="", error=L(
                    "Could not get the PSN token (HTTP 400): invalid_grant", "PSN belirteci alinamadi (HTTP 400): invalid_grant")))
            else:
                later(1.5, lambda: set_state(message=L("Looking for consoles", "Konsollar araniyor"), loggedIn=True))
                later(3.0, lambda: set_state(phase="idle", message="", consoles=CONSOLES))
        elif path == "/api/consoles/refresh":
            set_state(phase="busy", message=L("Looking for consoles", "Konsollar araniyor"), error="")
            later(1.5, lambda: set_state(phase="idle", message="", consoles=CONSOLES))
        elif path == "/api/connect":
            set_state(phase="connecting", message=L("Verifying the PSN session", "PSN oturumu dogrulaniyor"), error="")
            later(1.5, lambda: set_state(message=L("Setting up the control connection", "Kontrol baglantisi kuruluyor")))
            later(3.0, lambda: set_state(needPin=True))
        elif path == "/api/pin":
            set_state(needPin=False, message=L("Connecting to the console", "Konsola baglaniliyor"))
            later(1.5, lambda: set_state(phase="streaming", message="", stream={"fps": 58, "dropped": 0}))
        elif path == "/api/stop":
            set_state(phase="idle", message="", needPin=False)
        elif path == "/api/settings":
            with LOCK:
                low = STATE["settings"].get("lowLatency", True)
                STATE["settings"] = {k: int(body.get(k, 0)) for k in ("resolution", "fps", "bitrate")}
                STATE["settings"]["lowLatency"] = bool(body.get("lowLatency", low))
                if body.get("lang") in ("en", "tr"):  # istege bagli; bilinmeyen deger yok sayilir
                    STATE["lang"] = body["lang"]
        elif path == "/api/logout":
            set_state(loggedIn=False, consoles=[], phase="idle", message="")
        else:
            return self.send(404, {"ok": False, "error": L("not found", "bulunamadi")})
        self.send(200, {"ok": True})


if __name__ == "__main__":
    print("sahte sunucu: http://localhost:%d/  (kod 1234)" % PORT, flush=True)
    http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
