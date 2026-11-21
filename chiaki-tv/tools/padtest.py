#!/usr/bin/env python3
"""TV'deki modulun kol WebSocket ucunu (/ws/pad) sinar: el sikisma, kimlik dogrulama, durum iletimi.

  python3 tools/padtest.py     # uygulamayi hata ayiklama modunda acar, eslestirir ve sinar
"""
import base64
import json
import os
import socket
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import tvdebug  # noqa: E402

TV, PORT = tvdebug.TV, tvdebug.PORT


class Ws:
    def __init__(self, origin=None, host=None):
        self.sock = socket.create_connection((TV, PORT), timeout=6)
        key = base64.b64encode(os.urandom(16)).decode()
        req = ("GET /ws/pad HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n" % (host or "%s:%d" % (TV, PORT), key))
        if origin:
            req += "Origin: %s\r\n" % origin
        self.sock.sendall((req + "\r\n").encode())
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            self.buf += chunk
        head, _, self.buf = self.buf.partition(b"\r\n\r\n")
        self.status = int(head.split(b" ", 2)[1]) if head else 0

    def send(self, payload, binary=False):
        data = payload if binary else payload.encode()
        mask = os.urandom(4)
        head = bytes([0x82 if binary else 0x81, 0x80 | len(data)]) + mask
        self.sock.sendall(head + bytes(b ^ mask[i & 3] for i, b in enumerate(data)))

    def recv(self, timeout=3):
        """Bir metin cercevesi dondurur; baglanti kapanirsa None."""
        self.sock.settimeout(timeout)
        try:
            while True:
                if len(self.buf) >= 2:
                    n, off = self.buf[1] & 0x7f, 2
                    if n == 126:
                        n, off = struct.unpack(">H", self.buf[2:4])[0], 4
                    if len(self.buf) >= off + n:
                        op, data = self.buf[0] & 0x0f, self.buf[off:off + n]
                        self.buf = self.buf[off + n:]
                        if op == 8:
                            return None
                        return data.decode()
                chunk = self.sock.recv(4096)
                if not chunk:
                    return None
                self.buf += chunk
        except (socket.timeout, ConnectionError):
            return None

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def state_frame(buttons=0, l2=0, r2=0, lx=0, ly=0, rx=0, ry=0):
    return struct.pack("<BIBBhhhh", 1, buttons, l2, r2, lx, ly, rx, ry)


def check(name, ok, detail=""):
    print("%s  %s %s" % ("GECTI " if ok else "KALDI ", name, detail))
    return ok


def main():
    cdp = tvdebug.open_app()
    snap = tvdebug.watch(cdp, 5)
    status, res = tvdebug.api("/api/pair", {"code": snap["code"]})
    if status != 200:
        print("eslestirilemedi:", status, res)
        return 1
    token = res["token"]
    good = True

    w = Ws(origin="http://kotu.example")
    good &= check("yabanci Origin reddedilir", w.status == 403, "(HTTP %d)" % w.status)
    w.close()
    w = Ws(host="kotu.example:8730")
    good &= check("yabanci Host reddedilir", w.status == 403, "(HTTP %d)" % w.status)
    w.close()

    w = Ws(origin="http://%s:%d" % (TV, PORT))
    good &= check("el sikisma", w.status == 101, "(HTTP %d)" % w.status)
    w.send(json.dumps({"t": "auth", "token": "yanlis"}))
    good &= check("yanlis belirtec reddedilir", "denied" in (w.recv() or ""))
    good &= check("reddedilen baglanti kapatilir", w.recv(2) is None)
    w.close()

    w = Ws(origin="http://%s:%d" % (TV, PORT))
    w.send(state_frame(buttons=1), binary=True)  # kimlik dogrulamadan durum
    good &= check("dogrulamasiz durum reddedilir", "denied" in (w.recv() or ""))
    w.close()

    w = Ws(origin="http://%s:%d" % (TV, PORT))
    w.send(json.dumps({"t": "auth", "token": token, "name": "Deneme kolu"}))
    reply = w.recv() or ""
    good &= check("dogru belirtec kabul edilir", '"ok"' in reply, reply)
    t0 = time.time()
    w.send(json.dumps({"t": "ping", "n": 42}))
    reply = w.recv() or ""
    good &= check("ping -> pong", '"pong"' in reply and "42" in reply, "(%.0f ms) %s" % ((time.time() - t0) * 1000, reply))
    for i in range(20):  # 1 sn boyunca durum gonder
        w.send(state_frame(buttons=1 << 5, lx=20000), binary=True)
        time.sleep(0.05)
    status, state = tvdebug.api("/api/state", token=token)
    good &= check("TV kol adini gosteriyor", state.get("gamepad") == "Deneme kolu", repr(state.get("gamepad")))

    w2 = Ws(origin="http://%s:%d" % (TV, PORT))
    w2.send(json.dumps({"t": "auth", "token": token, "name": "Ikinci kol"}))
    w2.recv()
    good &= check("yeni kol eskisinin yerini alir", "replaced" in (w.recv() or ""))
    for i in range(26):  # TV kol adini saniyede bir yeniler
        w2.send(state_frame(), binary=True)
        time.sleep(0.05)
    status, state = tvdebug.api("/api/state", token=token)
    good &= check("TV yeni kolun adini gosteriyor", state.get("gamepad") == "Ikinci kol", repr(state.get("gamepad")))
    w2.close()
    time.sleep(1.5)
    status, state = tvdebug.api("/api/state", token=token)
    good &= check("kol ayrilinca ad silinir", state.get("gamepad") == "", repr(state.get("gamepad")))

    status, _ = tvdebug.api("/api/state", token=token)
    good &= check("HTTP sunucusu hala calisiyor", status == 200)
    import urllib.request
    page = urllib.request.urlopen("http://%s:%d/pad" % (TV, PORT), timeout=8).read()
    good &= check("kol sayfasi sunuluyor", b"chiaki-tv kol" in page, "(%d bayt)" % len(page))
    print("SONUC:", "hepsi gecti" if good else "BASARISIZ")
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
