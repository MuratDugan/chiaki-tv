#!/usr/bin/env python3
"""Tizen sdb (Smart Development Bridge) icin bagimliliksiz istemci.

sdb, adb'nin Tizen catalidir: 24 baytlik baslik + veri, CNXN/OPEN/OKAY/WRTE/CLSE.
TV yalnizca gelistirici modunda girilen "Host PC IP" adresinden gelen baglantiyi kabul eder.

Kullanim:
  python3 sdb.py capability
  python3 sdb.py shell "<komut>"          # TV'de yalnizca "0 <komut>" bicimi izinli
  python3 sdb.py raw "<servis>"
  python3 sdb.py push <yerel> <uzak>
"""
import os
import socket
import struct
import sys
import time

A_CNXN = 0x4E584E43
A_OPEN = 0x4E45504F
A_OKAY = 0x59414B4F
A_CLSE = 0x45534C43
A_WRTE = 0x45545257
A_AUTH = 0x48545541
NAMES = {A_CNXN: "CNXN", A_OPEN: "OPEN", A_OKAY: "OKAY", A_CLSE: "CLSE", A_WRTE: "WRTE", A_AUTH: "AUTH"}
TIMEOUTS = (socket.timeout, TimeoutError)


class SdbError(Exception):
    pass


def tv_ip():
    """TV'nin adresi: TV_IP ortam degiskeni, yoksa bu klasordeki tv-ip.txt (depoya girmez). Yoksa bos."""
    ip = os.environ.get("TV_IP", "").strip()
    if not ip:
        try:
            ip = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "tv-ip.txt")).read().strip()
        except OSError:
            ip = ""
    return ip


class Sdb:
    def __init__(self, ip=None, port=26101, verbose=False):
        self.ip, self.port, self.verbose = ip or tv_ip(), port, verbose
        self.maxdata = 4096
        self.sock = None
        self.next_id = 1
        self.banner = None

    def _log(self, *a):
        if self.verbose:
            print("#", *a, file=sys.stderr, flush=True)

    def _send(self, cmd, arg0, arg1, data=b""):
        hdr = struct.pack("<6I", cmd, arg0, arg1, len(data), sum(data) & 0xFFFFFFFF, cmd ^ 0xFFFFFFFF)
        self.sock.sendall(hdr + data)
        self._log(">>", NAMES.get(cmd, hex(cmd)), arg0, arg1, data[:60])

    def _read(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise SdbError("TV baglantiyi kapatti")
            buf += chunk
        return buf

    def _recv(self):
        cmd, arg0, arg1, length, _check, _magic = struct.unpack("<6I", self._read(24))
        if length > 1 << 22:
            raise SdbError("gecersiz paket uzunlugu: %d" % length)
        data = self._read(length) if length else b""
        self._log("<<", NAMES.get(cmd, hex(cmd)), arg0, arg1, data[:80])
        return cmd, arg0, arg1, data

    def connect(self, timeout=8):
        if not self.ip:
            raise SdbError("TV address not set: export TV_IP=<address> or write it to tv-probe/tv-ip.txt")
        self.sock = socket.create_connection((self.ip, self.port), timeout=timeout)
        self.sock.settimeout(timeout)
        self._send(A_CNXN, 0x01000000, self.maxdata, b"host::\0")
        while True:
            cmd, arg0, _arg1, data = self._recv()
            if cmd == A_CNXN:
                self.banner = data.rstrip(b"\0").decode(errors="replace")
                return self.banner
            if cmd == A_AUTH:
                raise SdbError("TV kimlik dogrulama istiyor (AUTH tip %d)" % arg0)

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass

    def open(self, service, timeout=15):
        """Servisi acar, kapanana kadar gelen tum veriyi dondurur."""
        return Stream(self, service).read_all(timeout)

    def push(self, local_path, remote_path, mode=0o100644):
        """Dosyayi adb/sdb 'sync:' protokoluyle TV'ye gonderir."""
        data = open(local_path, "rb").read()
        st = Stream(self, "sync:")
        spec = ("%s,%d" % (remote_path, mode)).encode()
        buf = b"SEND" + struct.pack("<I", len(spec)) + spec
        for i in range(0, len(data), 60000):
            chunk = data[i:i + 60000]
            buf += b"DATA" + struct.pack("<I", len(chunk)) + chunk
        buf += b"DONE" + struct.pack("<I", int(time.time()))
        st.write(buf)
        tag, n = struct.unpack("<4sI", st.take(8))
        if tag != b"OKAY":
            msg = st.take(n).decode(errors="replace") if n else ""
            st.close()
            raise SdbError("dosya gonderilemedi: %s %s" % (tag.decode(errors="replace"), msg))
        st.write(b"QUIT" + struct.pack("<I", 0))
        st.close()
        return len(data)


class Stream:
    """Tek bir acik sdb servisi (ayni anda bir tane kullanilir)."""

    def __init__(self, dev, service, timeout=15):
        self.dev = dev
        self.local = dev.next_id
        dev.next_id += 1
        self.remote = None
        self.closed = False
        self.inbuf = b""
        dev._send(A_OPEN, self.local, 0, service.encode() + b"\0")
        end = time.time() + timeout
        while self.remote is None:
            ev = self._step(max(0.1, end - time.time()))
            if ev == "close" or (ev == "timeout" and time.time() >= end):
                raise SdbError("servis reddedildi: " + service)

    def _step(self, timeout):
        self.dev.sock.settimeout(timeout)
        try:
            cmd, arg0, arg1, data = self.dev._recv()
        except TIMEOUTS:
            return "timeout"
        if cmd == A_CLSE:
            if not self.closed:
                self.closed = True
                self.dev._send(A_CLSE, self.local, arg0)
            return "close"
        if arg1 != self.local:
            return "other"
        if cmd == A_OKAY:
            self.remote = arg0
            return "okay"
        if cmd == A_WRTE:
            self.remote = arg0
            self.inbuf += data
            self.dev._send(A_OKAY, self.local, self.remote)
            return "data"
        return "other"

    def write(self, data, timeout=20):
        for i in range(0, len(data), self.dev.maxdata):
            self.dev._send(A_WRTE, self.local, self.remote, data[i:i + self.dev.maxdata])
            end = time.time() + timeout
            while True:
                ev = self._step(max(0.1, end - time.time()))
                if ev == "okay":
                    break
                if ev == "close":
                    raise SdbError("TV akisi yazma sirasinda kapatti")
                if ev == "timeout" and time.time() >= end:
                    raise SdbError("TV yazmayi onaylamadi")

    def take(self, n, timeout=20):
        end = time.time() + timeout
        while len(self.inbuf) < n:
            ev = self._step(max(0.1, end - time.time()))
            if ev == "close" or (ev == "timeout" and time.time() >= end):
                raise SdbError("beklenen veri gelmedi (%d/%d bayt)" % (len(self.inbuf), n))
        out, self.inbuf = self.inbuf[:n], self.inbuf[n:]
        return out

    def read_all(self, timeout=15):
        end = time.time() + timeout
        while not self.closed and time.time() < end:
            self._step(max(0.1, end - time.time()))
        if not self.closed:
            self.close()
        out, self.inbuf = self.inbuf, b""
        return out

    def close(self):
        if not self.closed and self.remote is not None:
            self.closed = True
            self.dev._send(A_CLSE, self.local, self.remote)


def run(service, timeout=15, verbose=False):
    """Tek seferlik: baglan, servisi calistir, metni dondur."""
    s = Sdb(verbose=verbose)
    try:
        s.connect()
        return s.open(service, timeout).decode("utf-8", "replace")
    finally:
        s.close()


def main():
    args = [a for a in sys.argv[1:] if a != "-v"]
    verbose = "-v" in sys.argv
    if not args:
        print(__doc__)
        return 1
    try:
        if args[0] == "push" and len(args) == 3:
            s = Sdb(verbose=verbose)
            s.connect()
            print("gonderildi: %d bayt" % s.push(args[1], args[2]))
            s.close()
            return 0
        service = {"capability": "capability:", "shell": "shell:" + " ".join(args[1:]),
                   "raw": " ".join(args[1:])}.get(args[0])
        if service is None:
            print(__doc__)
            return 1
        out = run(service, verbose=verbose)
        sys.stdout.write(out if out.endswith("\n") or not out else out + "\n")
    except (SdbError, OSError) as e:
        print("# HATA:", e, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
