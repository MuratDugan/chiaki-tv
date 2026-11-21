#!/usr/bin/env python3
"""docs/screenshots altindaki arayuz goruntulerini yeniden uretir (TV gerekmez).

Sahte sunucuyu ve bassiz Chrome'u baslatir, her sayfayi ornek verilerle acar ve DevTools
protokoluyle goruntusunu alir.

  python3 tools/screenshots.py
"""
import base64
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "..", "tv-probe"))
from cdpclient import Cdp  # noqa: E402

CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
BASE = "http://localhost:8790"
OUT = os.path.join(ROOT, "docs", "screenshots")
DEBUG_PORT = 9377

DEMO = {
    "phase": "idle", "message": "", "error": "", "ip": "192.168.1.50", "port": 8730, "code": "4821",
    "loggedIn": True, "needPin": False, "pinIncorrect": False, "lastDuid": "ab" * 32, "gamepad": "Xbox controller",
    "lang": "en", "consoles": [{"duid": "ab" * 32, "name": "Living room PS5", "remoteplay": True}],
    "settings": {"resolution": 720, "fps": 60, "bitrate": 0, "lowLatency": True}, "stream": {"fps": 0, "dropped": 0},
}


def get(path):
    return urllib.request.urlopen(BASE + path, timeout=5).read()


def shot(cdp, name, url, width, height, scale=1, mobile=False, js=None, settle=1.5, after=0.6, jpeg=False):
    cdp.call("Emulation.setDeviceMetricsOverride",
             {"width": width, "height": height, "deviceScaleFactor": scale, "mobile": mobile})
    cdp.call("Page.navigate", {"url": url})
    time.sleep(settle)
    if js:
        cdp.js(js)
        time.sleep(after)
    params = {"format": "jpeg", "quality": 88} if jpeg else {"format": "png"}
    data = cdp.call("Page.captureScreenshot", params, timeout=30)["data"]
    path = os.path.join(OUT, name)
    open(path, "wb").write(base64.b64decode(data))
    print("%-20s %dx%d @%dx  %d KB" % (name, width, height, scale, os.path.getsize(path) // 1024))


def main():
    os.makedirs(OUT, exist_ok=True)
    mock = subprocess.Popen([sys.executable, os.path.join(HERE, "mockserver.py")], stdout=subprocess.DEVNULL)
    profile = tempfile.mkdtemp(prefix="chiaki-shots-")
    chrome = subprocess.Popen([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--no-first-run",
                               "--remote-debugging-port=%d" % DEBUG_PORT, "--user-data-dir=" + profile, "about:blank"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(40):  # ikisi de hazir olana kadar bekle
            try:
                get("/api/lang")
                urllib.request.urlopen("http://127.0.0.1:%d/json" % DEBUG_PORT, timeout=2).read()
                break
            except Exception:
                time.sleep(0.25)
        cdp = Cdp("127.0.0.1", DEBUG_PORT, target_type="page")
        cdp.call("Page.enable")

        render = "window.__render(%s); 'ok'"
        shot(cdp, "tv-home.png", BASE + "/tv/#mock", 1920, 1080, js=render % json.dumps(DEMO))
        connecting = dict(DEMO, phase="connecting", message="Reaching the console")
        shot(cdp, "tv-connecting.png", BASE + "/tv/#mock", 1920, 1080, js=render % json.dumps(connecting))

        # Yayin goruntuleri: arka plan TV'den alinmis gercek kare (build/shots/stream-frame.png), ustundeki
        # arayuz ayni sayfanin tarayicida cizilmis hali, sayilar TV'de olculen degerler. Kare yoksa atlanir.
        if os.path.exists(os.path.join(ROOT, "build", "shots", "stream-frame.png")):
            streaming = dict(DEMO, phase="streaming")
            base = ("window.__render(%s);"
                    "document.getElementById('nacl-host').style.background='url(/shot/frame.png) 0 0 / 1920px 1080px no-repeat';"
                    % json.dumps(streaming))
            stats = {"ev": "stats", "fps": 36, "dropped": 24, "decoded": 60, "decodeMs": 104, "screenMs": 128,
                     "mbps": 0.2, "lost": 0, "rtt": 45, "w": 1280, "h": 720}
            shot(cdp, "tv-stream-hint.jpg", BASE + "/tv/#mock", 1920, 1080, js=base + "'ok'", after=1.2, jpeg=True)
            shot(cdp, "tv-stream-menu.jpg", BASE + "/tv/#mock", 1920, 1080,
                 js=base + "window.__ev(%s); window.__ev({ev:'menu'}); 'ok'" % json.dumps(stats), after=1.2, jpeg=True)

        # Telefon sayfalari: /shot/go belirteci yerlestirip hedefe yonlendirir, ornek konsolu da ekler.
        shot(cdp, "setup-phone.png", BASE + "/shot/go?to=/", 390, 1330, scale=2, mobile=True, settle=4)
        get("/mock/stream")  # yayin acik: kol sayfasi "yayina bagli" gostersin
        shot(cdp, "pad-touch.png", BASE + "/shot/go?to=padtouch", 844, 390, scale=2, mobile=True, settle=6.5)
        shot(cdp, "pad-desktop.png", BASE + "/shot/go?to=pad", 1280, 1180, settle=6.5)
    finally:
        chrome.terminate()
        mock.terminate()
    return 0


if __name__ == "__main__":
    sys.exit(main())
