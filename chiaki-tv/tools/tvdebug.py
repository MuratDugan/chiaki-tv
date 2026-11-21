#!/usr/bin/env python3
"""chiaki-tv'yi TV'de hata ayiklama modunda acar ve modulun durumunu izler.

  python3 tools/tvdebug.py watch [saniye]       # ac, gunlugu ve cokme bilgisini izle
  python3 tools/tvdebug.py psntest [saniye]     # ac, eslestir, gecersiz bir PSN koduyla giris dene
                                                # (curl + TLS + PSN yolunu hesap gerektirmeden calistirir)
  python3 tools/tvdebug.py connect [saniye]     # ac, eslestir, kayitli PSN oturumuyla ilk konsola baglan,
                                                # gunlugu izle, sonunda baglantiyi kes
  python3 tools/tvdebug.py symbolize <adres>... # cokme adreslerini islev/satira cevir

Belirtecler (PSN oturumu) hicbir zaman yazdirilmaz; yalnizca var olup olmadiklari bildirilir.
"""
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "..", "tv-probe"))
import tvbuild            # noqa: E402
from cdpclient import Cdp  # noqa: E402

TV = tvbuild.tvapp.TV_IP
PORT = 8730
SDK = tvbuild.SDK
ADDR2LINE = os.path.join(SDK, "toolchain", "mac_arm_newlib", "bin", "arm-nacl-addr2line")
NEXE = os.path.join(ROOT, "build", "native", "chiaki_arm_unstripped.nexe")

SNAPSHOT = """(function () {
  var cfg = {}; try { cfg = JSON.parse(localStorage.getItem('chiakitv.config') || '{}'); } catch (e) {}
  var old = null; try { old = JSON.parse(localStorage.getItem('chiaki-crash') || 'null'); } catch (e) {}
  var d = function (i) { var e = document.getElementById(i); return e ? e.textContent : ''; };
  var f = document.getElementById('fatal');
  return JSON.stringify({
    code: d('d0') + d('d1') + d('d2') + d('d3'),
    fatal: f && f.className.indexOf('hidden') < 0 ? d('fatal-text') : '',
    log: (window.__log || []).slice(%d),
    logLen: (window.__log || []).length,
    crash: window.__crash || null,
    oldCrash: old,
    hasAccess: !!cfg.access, hasRefresh: !!cfg.refresh, hasAccount: !!cfg.accountId,
    consoles: (cfg.consoles || []).length
  });
})()"""


SECRET = re.compile(r"(Bearer\s+\S+|[A-Za-z0-9_\-+/=]{28,}|\d{12,})")


def redact(line):
    """Belirtec, oturum kimligi gibi uzun dizgileri gizler: gunluk ciktisi baska yerlere kopyalanabilir."""
    return SECRET.sub("<gizli>", line)


SINK_PORT = 8799
sink_on = [False]


def save_capture(token):
    """Modulun biriktirdigi ham video birimlerini indirir: build/capture.h264 (Annex-B) + birim boyutlari."""
    blob = b""
    while True:
        req = urllib.request.Request("http://%s:%d/api/dump" % (TV, PORT), method="POST",
                                     data=json.dumps({"offset": len(blob), "length": 512 * 1024}).encode())
        req.add_header("Content-Type", "application/json")
        req.add_header("X-Token", token)
        try:
            chunk = urllib.request.urlopen(req, timeout=15).read()
        except Exception as e:
            print("yakalama indirilemedi:", e)
            break
        if not chunk:
            break
        blob += chunk
    units, pos = [], 0
    while pos + 4 <= len(blob):
        n = int.from_bytes(blob[pos:pos + 4], "big")
        units.append(blob[pos + 4:pos + 4 + n])
        pos += 4 + n
    out = os.path.join(ROOT, "build", "capture.h264")
    open(out, "wb").write(b"".join(units))
    open(os.path.join(ROOT, "build", "capture.bin"), "wb").write(blob)  # uzunluk onekli: kare sinirlari korunur
    print("yakalama: %d birim, %d bayt -> build/capture.h264; ilk boyutlar: %s" % (
        len(units), sum(map(len, units)), [len(u) for u in units[:12]]))


def pad_feeder(token):
    """Yayin baslayinca /ws/pad'e 60 Hz ile cok kucuk sag cubuk sapmalari yollar (oyunlarin olu bolgesinin altinda)."""
    import padtest
    while True:
        status, state = api("/api/state", token=token)
        if state.get("phase") == "streaming":
            break
        time.sleep(1)
    ws = padtest.Ws(origin="http://%s:%d" % (TV, PORT))
    ws.send(json.dumps({"t": "auth", "token": token, "name": "Olcum kolu"}))
    ws.recv()
    i = 0
    try:
        while True:
            ws.send(padtest.state_frame(rx=1500 if i % 2 else -1500), binary=True)
            i += 1
            time.sleep(1 / 60.0)
    except OSError:
        pass


def start_sink():
    """TV'nin UDP ile yolladigi gunluk satirlarini yazdirir (ana is parcacigi kilitlense de gelir)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", SINK_PORT))

    def loop():
        while True:
            data, _ = sock.recvfrom(4096)
            line = redact(data.decode("utf-8", "replace"))
            if "ws curl" in line or "WS-ENC" in line:
                continue
            print("  ~", line, flush=True)
    threading.Thread(target=loop, daemon=True).start()


def api(path, body=None, token=None):
    req = urllib.request.Request("http://%s:%d%s" % (TV, PORT, path), method="POST" if body is not None else "GET")
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("X-Token", token)
    try:
        with urllib.request.urlopen(req, data=data, timeout=8) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")
    except Exception as e:  # modul coktuyse baglanti reddedilir
        return 0, {"error": str(e)}


def symbolize(addrs):
    if not addrs:
        return
    out = subprocess.run([ADDR2LINE, "-f", "-C", "-i", "-e", NEXE] + addrs, capture_output=True, text=True).stdout
    lines = out.strip().splitlines()
    for i in range(0, len(lines) - 1, 2):
        where = lines[i + 1].split("chiaki-tv/")[-1]
        print("    %-44s %s" % (lines[i][:44], where))


def report_crash(crash):
    c = crash.get("crash") or {}
    print("COKME: pc=%s lr=%s sp=%s r0-r7=%s is_parcacigi=%s ana=%s" % (
        c.get("pc"), c.get("lr"), c.get("sp"), " ".join(str(c.get("r%d" % i)) for i in range(8)),
        c.get("thread"), c.get("main")))
    print("  pc / lr:")
    symbolize(["0x" + c[k] for k in ("pc", "lr") if c.get(k)])
    frames = ((crash.get("crashstack") or {}).get("frames") or "").split()
    if frames:
        print("  yigindaki donus adresleri:")
        symbolize(["0x" + f for f in frames])
    raw = ((crash.get("crashraw") or {}).get("words") or "").split()
    if raw:
        print("  ham yigin:", " ".join(raw))


def open_app():
    port = tvbuild.launch(debug=True)
    if not port:
        raise SystemExit("hata ayiklama portu alinamadi")
    time.sleep(4)
    return Cdp(TV, port)


def watch(cdp, seconds, seen=0, token=None):
    end = time.time() + seconds
    was_busy = False
    # CHIAKI_JS="ad1::js1;;ad2::js2": yayin basladiktan sonra 12 sn arayla TV sayfasinda calistirilacak denemeler
    tweaks = [t.split("::", 1) for t in os.environ.get("CHIAKI_JS", "").split(";;") if "::" in t]
    stream_since = None
    while time.time() < end:
        if tweaks and stream_since and time.time() - stream_since > 12:
            name, js = tweaks.pop(0)
            try:
                print("=== deneme:", name, "->", cdp.js(js), flush=True)
            except Exception as e:
                print("=== deneme basarisiz:", name, e)
            stream_since = time.time()
        if token:  # baglanti denemesi bittiyse (bosta + hata ya da yayin) beklemeyi kes
            status, state = api("/api/state", token=token)
            phase = state.get("phase")
            was_busy = was_busy or phase in ("connecting", "streaming")
            if phase == "streaming" and stream_since is None:
                stream_since = time.time()
            if was_busy and phase == "idle":
                end = min(end, time.time() + 2)
        try:
            snap = json.loads(cdp.js(SNAPSHOT % seen))
        except Exception as e:
            print("denetim baglantisi koptu:", e)
            if sink_on[0]:  # tarayici kilitlendi; UDP kanalindan gelenleri izlemeyi surdur
                time.sleep(max(0, min(25, end - time.time())))
            return None
        for line in snap["log"]:
            if not sink_on[0]:  # UDP kanali aciksa satirlar oradan gelir
                print("  |", redact(line))
        seen = snap["logLen"]
        if snap["crash"]:
            time.sleep(0.8)  # sonraki mesajlar (yigin) da gelsin
            snap = json.loads(cdp.js(SNAPSHOT % seen))
            report_crash(snap["crash"])
            return snap
        if snap["fatal"]:
            print("TV ekrani:", snap["fatal"])
            return snap
        time.sleep(1.0)
    return json.loads(cdp.js(SNAPSHOT % seen))


def summary(snap):
    print("durum: kod=%s  erisim=%s yenileme=%s hesap=%s konsol=%d" % (
        "var" if snap["code"].strip("–- ") else "yok", snap["hasAccess"], snap["hasRefresh"], snap["hasAccount"],
        snap["consoles"]))
    if snap.get("oldCrash"):
        print("onceki cokme kaydi (%s):" % snap["oldCrash"].get("at"))
        report_crash(snap["oldCrash"].get("crash") or {})
        for line in (snap["oldCrash"].get("log") or [])[-12:]:
            print("  |", line)


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "symbolize":
        symbolize(sys.argv[2:])
        return 0
    if cmd not in ("watch", "psntest", "connect"):
        print(__doc__)
        return 2
    seconds = int(sys.argv[2]) if len(sys.argv) > 2 else 45
    cdp = open_app()
    snap = watch(cdp, 6)
    if not snap:
        return 1
    summary(snap)
    if snap["crash"] or snap["fatal"]:
        return 1
    if cmd == "psntest":
        status, res = api("/api/pair", {"code": snap["code"]})
        print("eslestirme: HTTP", status, res.get("error", "tamam"))
        if status != 200:
            return 1
        status, res = api("/api/psn/finish",
                          {"redirect": "https://remoteplay.dl.playstation.net/remoteplay/redirect?code=TEST0000&cid=test"},
                          res["token"])
        print("psn/finish: HTTP", status, res.get("error", "tamam"))
    token = None
    if cmd == "connect":
        status, res = api("/api/pair", {"code": snap["code"]})
        print("eslestirme: HTTP", status, res.get("error", "tamam"))
        if status != 200:
            return 1
        token = res["token"]
        state = {}
        for _ in range(25):  # acilistaki konsol listesi tazelemesi bitsin
            status, state = api("/api/state", token=token)
            if status == 200 and state.get("phase") == "idle":
                break
            time.sleep(1)
        consoles = state.get("consoles") or []
        print("durum: faz=%s oturum=%s konsol=%d hata=%r" % (state.get("phase"), state.get("loggedIn"), len(consoles),
                                                             state.get("error")))
        target = next((c for c in consoles if c.get("duid") == state.get("lastDuid") and c.get("remoteplay")), None) \
            or next((c for c in consoles if c.get("remoteplay")), None)
        if not target:
            print("baglanilacak konsol yok")
            return 1
        if os.environ.get("CHIAKI_VERBOSE"):
            api("/api/debug", {"verbose": True}, token)
        if os.environ.get("CHIAKI_RES"):  # ornek: CHIAKI_RES=720 ya da 720x30
            res, _, fps = os.environ["CHIAKI_RES"].partition("x")
            print("ayar:", api("/api/settings", {"resolution": int(res), "fps": int(fps or 60), "bitrate": 0}, token)[0])
        if os.environ.get("CHIAKI_SKIP"):  # bos kare ekleme denemesi (1 = acik, 0 = kapali)
            api("/api/debug", {"skip": os.environ["CHIAKI_SKIP"] == "1"}, token)
        if os.environ.get("CHIAKI_SPS"):  # "refs,dpb,level": dusuk gecikme icin SPS yamasi denemesi
            api("/api/debug", {"sps": os.environ["CHIAKI_SPS"]}, token)
        if os.environ.get("CHIAKI_CAPTURE"):
            api("/api/debug", {"capture": True}, token)
        start_sink()
        api("/api/debug", {"sink": "%s:%d" % (tvbuild.tvapp.my_ip(), SINK_PORT)}, token)
        sink_on[0] = True
        if os.environ.get("CHIAKI_PAD"):  # yayin boyunca ag kolundan zararsiz girdi gonder (gecikme olcumu icin)
            threading.Thread(target=pad_feeder, args=(token,), daemon=True).start()
        status, res = api("/api/connect", {"duid": target["duid"]}, token)
        print("connect: HTTP", status, res.get("error", "tamam"))
    snap = watch(cdp, seconds, snap["logLen"], token)
    if token and os.environ.get("CHIAKI_CAPTURE"):
        save_capture(token)
    if token:
        status, state = api("/api/state", token=token)
        print("son durum: faz=%s mesaj=%r hata=%r akis=%s" % (state.get("phase"), state.get("message"),
                                                              state.get("error"), state.get("stream")))
        api("/api/stop", {}, token)
        if status != 200:  # modul yanit vermiyor: konsoldaki oturum acik kalmasin diye sureci sonlandir
            try:
                print("surec sonlandiriliyor:", tvbuild.sdb_run("shell:0 kill " + tvbuild.PKG, timeout=20).strip().splitlines()[0])
            except Exception as e:
                print("sonlandirilamadi:", e)
    if snap and not snap["crash"]:
        status, state = api("/api/log")
        print("modul yasiyor:", "evet" if status in (200, 401) else "HAYIR (HTTP %s)" % status)
        summary(snap)
    return 0


if __name__ == "__main__":
    sys.exit(main())
