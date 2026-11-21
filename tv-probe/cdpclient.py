"""TV'deki bir web uygulamasinin icinde JS calistirmak icin kucuk Chrome DevTools istemcisi.

Port, sdb'nin "0 debug <appid> 300" komutunun ciktisindan gelir.
"""
import base64
import json
import os
import socket
import struct
import urllib.request


class Cdp:
    def __init__(self, ip, port, target_type=None):
        pages = json.loads(urllib.request.urlopen("http://%s:%d/json" % (ip, port), timeout=10).read())
        # Masaustu Chrome uzanti sayfalarini da listeler; target_type="page" gercek sekmeyi secer.
        chosen = [p for p in pages if p.get("type") == target_type] if target_type else pages
        path = "/" + (chosen or pages)[0]["webSocketDebuggerUrl"].split("/", 3)[3]
        self.sock = socket.create_connection((ip, port), timeout=15)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(("GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (path, ip, port, key)).encode())
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            self.buf += self.sock.recv(4096)
        self.buf = self.buf.split(b"\r\n\r\n", 1)[1]
        self.next = 1

    def _need(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(1 << 16)
            if not chunk:
                raise ConnectionError("TV denetim baglantisini kapatti")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _msg(self):
        data = b""
        while True:
            b0, b1 = self._need(2)
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._need(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._need(8))[0]
            data += self._need(n)
            if b0 & 0x80:
                return json.loads(data.decode("utf-8", "replace")) if data else {}

    def call(self, method, params=None, timeout=20):
        mid, self.next = self.next, self.next + 1
        payload = json.dumps({"id": mid, "method": method, "params": params or {}}).encode()
        mask = os.urandom(4)
        n = len(payload)
        if n < 126:
            hdr = bytes([0x81, 0x80 | n])
        elif n < 65536:
            hdr = bytes([0x81, 0x80 | 126]) + struct.pack(">H", n)
        else:
            hdr = bytes([0x81, 0x80 | 127]) + struct.pack(">Q", n)
        self.sock.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))
        self.sock.settimeout(timeout)
        while True:
            msg = self._msg()
            if msg.get("id") == mid:
                return msg.get("result", msg)

    def js(self, expression):
        """Ifadeyi calistirir, degerini dondurur."""
        res = self.call("Runtime.evaluate", {"expression": expression, "returnByValue": True})
        return res.get("result", {}).get("value")
