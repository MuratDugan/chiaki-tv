"""Samsung Tizen TV kumanda kanali (websocket) icin bagimliliksiz istemci.

TV kendi imzaladigi bir sertifika kullanir; dogrulamayi tamamen kapatmak yerine
ilk baglantida sertifikanin parmak izini kaydedip sonrakilerde karsilastiririz.
"""
import base64
import hashlib
import json
import os
import socket
import ssl
import struct
import time


class TVError(Exception):
    pass


class TV:
    def __init__(self, ip, state_dir, name="tv-probe"):
        self.ip = ip
        self.token_file = os.path.join(state_dir, "tv-token.txt")
        self.pin_file = os.path.join(state_dir, "tv-cert.sha256")
        self.name = base64.b64encode(name.encode()).decode()
        self.sock = None
        self.buf = b""

    # --- baglanti ---------------------------------------------------------
    def connect(self, wait=60):
        token = ""
        if os.path.exists(self.token_file):
            token = open(self.token_file).read().strip()
        path = "/api/v2/channels/samsung.remote.control?name=" + self.name
        if token:
            path += "&token=" + token

        raw = socket.create_connection((self.ip, 8002), timeout=8)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE  # asagida parmak izi ile sabitleniyor
        self.sock = ctx.wrap_socket(raw)
        fp = hashlib.sha256(self.sock.getpeercert(binary_form=True)).hexdigest()
        if os.path.exists(self.pin_file):
            if open(self.pin_file).read().strip() != fp:
                self.sock.close()
                raise TVError("TV sertifikasi degismis; baglanti kesildi")
        else:
            open(self.pin_file, "w").write(fp)

        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((
            "GET %s HTTP/1.1\r\nHost: %s:8002\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n" % (path, self.ip, key)).encode())
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise TVError("el sikisma sirasinda baglanti kapandi")
            self.buf += chunk
        head, _, self.buf = self.buf.partition(b"\r\n\r\n")
        if b" 101 " not in head.split(b"\r\n")[0]:
            raise TVError("websocket reddedildi: %r" % head[:80])

        deadline = time.time() + wait
        while time.time() < deadline:
            msg = self.recv(2)
            if msg is None:
                continue
            ev = msg.get("event")
            if ev == "ms.channel.connect":
                tok = (msg.get("data") or {}).get("token")
                if tok:
                    open(self.token_file, "w").write(str(tok))
                return
            if ev in ("ms.channel.unauthorized", "ms.channel.timeOut"):
                raise TVError("TV izin vermedi: " + ev)
        raise TVError("TV %d sn icinde cevap vermedi (ekranda izin bekliyor olabilir)" % wait)

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass

    # --- cerceveler -------------------------------------------------------
    def send(self, obj):
        payload = json.dumps(obj).encode()
        mask = os.urandom(4)
        n = len(payload)
        if n < 126:
            hdr = bytes([0x81, 0x80 | n])
        elif n < 65536:
            hdr = bytes([0x81, 0x80 | 126]) + struct.pack(">H", n)
        else:
            hdr = bytes([0x81, 0x80 | 127]) + struct.pack(">Q", n)
        self.sock.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def _need(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise TVError("TV baglantiyi kapatti")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self, timeout=5):
        """Bir JSON olayi dondurur; sure dolarsa None."""
        self.sock.settimeout(timeout)
        while True:
            try:
                b0, b1 = self._need(2)
                op, n = b0 & 0x0F, b1 & 0x7F
                if n == 126:
                    n = struct.unpack(">H", self._need(2))[0]
                elif n == 127:
                    n = struct.unpack(">Q", self._need(8))[0]
                payload = self._need(n)
            except (socket.timeout, TimeoutError):
                return None
            if op == 8:
                raise TVError("TV kanali kapatti: " + payload[2:].decode(errors="replace"))
            if op == 9:  # ping -> pong
                self.sock.sendall(bytes([0x8A, 0x80]) + os.urandom(4))
                continue
            if op == 1:
                return json.loads(payload.decode(errors="replace"))

    # --- komutlar ---------------------------------------------------------
    def emit(self, event, data=None):
        params = {"event": event, "to": "host"}
        if data is not None:
            params["data"] = data
        self.send({"method": "ms.channel.emit", "params": params})

    def wait_event(self, event, timeout=15):
        deadline = time.time() + timeout
        while time.time() < deadline:
            msg = self.recv(2)
            if msg and msg.get("event") == event:
                return msg
        return None

    def installed_apps(self):
        self.emit("ed.installedApp.get")
        msg = self.wait_event("ed.installedApp.get")
        return ((msg or {}).get("data") or {}).get("data") or []

    def open_browser(self, url):
        self.emit("ed.apps.launch", {"appId": "org.tizen.browser",
                                     "action_type": "NATIVE_LAUNCH", "metaTag": url})
        return self.wait_event("ed.apps.launch", 20)

    def key(self, code):
        self.send({"method": "ms.remote.control",
                   "params": {"Cmd": "Click", "DataOfCmd": code,
                              "Option": "false", "TypeOfRemote": "SendRemoteKey"}})
