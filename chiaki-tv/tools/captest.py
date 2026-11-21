#!/usr/bin/env python3
"""TV'nin sundugu Pepper arayuzlerini ve H.264 donanim cozucusunu yoklar."""
import glob
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import tvbuild  # noqa: E402
from cdpclient import Cdp  # noqa: E402


def interface_names():
    names = set()
    inc = os.path.join(tvbuild.SDK, "include")
    for path in glob.glob(os.path.join(inc, "**", "*.h"), recursive=True):
        for m in re.finditer(r'"(PP[BP]_[A-Za-z0-9_()]+;[0-9.]+)"', open(path, errors="replace").read()):
            names.add(m.group(1))
    return sorted(n for n in names if n.startswith("PPB_"))


def main():
    port = tvbuild.launch(debug=True)
    time.sleep(3)
    tv = Cdp(tvbuild.tvapp.TV_IP, port)
    log = lambda: tv.js("window.__log.join('\\n')") or ""
    end = time.time() + 25
    while "modul: yuklendi" not in log() and time.time() < end:
        time.sleep(0.4)
    if "modul: yuklendi" not in log():
        print(log()); return 1

    names, have = interface_names(), {}
    for i in range(0, len(names), 10):
        batch = names[i:i + 10]
        n0 = log().count("iface:")
        tv.js("window.send(%r)" % ("iface " + " ".join(batch)))
        end = time.time() + 6
        while log().count("iface:") == n0 and time.time() < end:
            time.sleep(0.2)
        last = [l for l in log().splitlines() if l.startswith("iface:")][-1]
        for tok in last.split()[1:]:
            k, v = tok.rsplit("=", 1)
            have[k] = v == "1"
    yes = sorted(k for k, v in have.items() if v)
    no = sorted(k for k, v in have.items() if not v)
    print("sorulan %d arayuz: %d var, %d yok" % (len(have), len(yes), len(no)))
    key = ("UDPSocket", "TCPSocket", "HostResolver", "VideoDecoder", "Graphics3D", "Audio;", "AudioConfig;", "Gamepad",
           "MediaPlayer_Samsung", "ElementaryStream", "ESDataSource", "MessageLoop", "FileIO", "WebSocket", "URLLoader",
           "RemoteController", "UDPSocketExtension", "NetworkMonitor", "InputEvent;", "Compositor;")
    print("--- oneme gore ---")
    for k in key:
        hit = [n for n in have if k in n]
        print("  %-22s %s" % (k.rstrip(";"), ", ".join("%s%s" % (n.split(";")[1], "" if have[n] else "(yok)") for n in sorted(hit)) or "kitte tanimli degil"))
    print("--- olmayanlar ---")
    print("  " + ", ".join(no))

    print("--- H.264 donanim cozucusu ---")
    for profile, pname in ((0, "baseline"), (1, "main"), (3, "high")):
        for accel, aname in ((0, "yalniz donanim"), (1, "donanim ya da yazilim")):
            n0 = log().count("video: Initialize")
            tv.js("window.send('video %d %d')" % (profile, accel))
            end = time.time() + 10
            while log().count("video: Initialize") == n0 and time.time() < end:
                time.sleep(0.2)
            lines = [l for l in log().splitlines() if l.startswith("video:")]
            print("  H.264 %-8s %-22s -> %s" % (pname, aname, lines[-1].split("->")[-1].strip() if "->" in lines[-1] else lines[-1]))
    print("  " + " | ".join(l for l in log().splitlines() if "Graphics3D" in l))
    return 0


if __name__ == "__main__":
    sys.exit(main())
