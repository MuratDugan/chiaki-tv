#!/usr/bin/env python3
"""Asama 1 testi: TV'deki NaCl modulu UDP/TCP/DNS kullanabiliyor mu, TCP dinleyebiliyor mu.

Mac'te UDP yanki (9998) ve TCP dinleyici (9997) acar, uygulamayi TV'de hata ayiklama modunda
baslatir, modulun yuklenmesini bekler ve komutlari sirayla gonderir.
"""
import os
import socket
import struct
import sys
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import tvbuild  # noqa: E402
from cdpclient import Cdp  # noqa: E402  (tvbuild, tv-probe'u yola ekler)

TV = tvbuild.tvapp.TV_IP
seen = {"udp": [], "tcp": []}


def udp_echo():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", 9998))
    while True:
        data, addr = s.recvfrom(4096)
        seen["udp"].append((addr, data))
        s.sendto(b"yanki:" + data, addr)


def tcp_sink():
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 9997))
    s.listen(4)
    while True:
        c, addr = s.accept()
        c.settimeout(3)
        try:
            seen["tcp"].append((addr, c.recv(200)))
        except OSError:
            seen["tcp"].append((addr, b""))
        c.close()


def main():
    me = tvbuild.tvapp.my_ip()
    for fn in (udp_echo, tcp_sink):
        threading.Thread(target=fn, daemon=True).start()

    port = tvbuild.launch(debug=True)
    if not port:
        raise SystemExit("hata ayiklama portu alinamadi")
    time.sleep(3)
    tv = Cdp(TV, port)

    def log():
        return tv.js("window.__log.join('\\n')") or ""

    def wait(text, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if text in log():
                return True
            time.sleep(0.4)
        return False

    print("== modul yukleme ==")
    wait("modul: yuklendi", 25) or wait("hata", 1) or wait("coktu", 1)
    print(log())
    if "modul: yuklendi" not in log():
        print("embed durumu:", tv.js("JSON.stringify({ready: nacl.readyState, err: String(nacl.lastError), exit: String(nacl.exitStatus)})"))
        return 1

    def step(title, cmd, expect, seconds=8):
        before = len(log().splitlines())
        tv.js("window.send(%r)" % cmd)
        ok = wait(expect, seconds)
        print("== %s ==  %s" % (title, "TAMAM" if ok else "BEKLENEN GELMEDI"))
        print("\n".join("   " + l for l in log().splitlines()[before:]))
        return ok

    step("arayuzler", "info", "info:")
    step("UDP -> Mac ve geri", "udp %s 9998 %s" % (me, b"merhaba".hex()), "udp: alindi")
    print("   Mac'in gordugu UDP:", seen["udp"][-1] if seen["udp"] else "hicbir sey")
    step("TCP -> Mac", "tcp %s 9997" % me, "tcp: yazildi")
    time.sleep(0.5)
    print("   Mac'in gordugu TCP:", seen["tcp"][-1] if seen["tcp"] else "hicbir sey")

    if step("DNS", "resolve stun.l.google.com", "dns:"):
        ip = [l for l in log().splitlines() if l.startswith("dns: ")][-1].split()[1]
        if ip[0].isdigit():
            # STUN baglama istegi: internete UDP cikiyor mu, disaridan hangi adres gorunuyor?
            req = struct.pack(">HHI", 0x0001, 0, 0x2112A442) + os.urandom(12)
            if step("UDP -> internet (STUN)", "udp %s 19302 %s" % (ip, req.hex()), "udp: alindi", 10):
                hexdata = [l for l in log().splitlines() if "udp: alindi" in l][-1].rsplit("veri ", 1)[1]
                raw, i = bytes.fromhex(hexdata), 20
                while i + 4 <= len(raw):
                    kind, ln = struct.unpack(">HH", raw[i:i + 4])
                    if kind == 0x0020:  # XOR-MAPPED-ADDRESS
                        xport = struct.unpack(">H", raw[i + 6:i + 8])[0] ^ 0x2112
                        xip = bytes(a ^ b for a, b in zip(raw[i + 8:i + 12], struct.pack(">I", 0x2112A442)))
                        print("   disaridan gorunen adres: %s.%s.x.x:%d (son iki hane gizlendi)" % (xip[0], xip[1], xport))
                    i += 4 + ln + (-ln % 4)

    if step("TCP dinleme (TV sunucu olarak)", "listen 8765", "dinle: hazir"):
        try:
            body = urllib.request.urlopen("http://%s:8765/kurulum" % TV, timeout=6).read().decode()
            print("   Mac'ten TV'ye HTTP istegi cevabi:", body.strip())
        except Exception as e:
            print("   Mac'ten TV'ye HTTP istegi basarisiz:", e)
        time.sleep(0.5)
        print("\n".join("   " + l for l in log().splitlines()[-4:]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
