#!/usr/bin/env python3
"""Install chiaki-tv on a Samsung TV. Needs only Python 3; no Tizen Studio.

    python3 install.py                 asks for the TV's address
    python3 install.py 192.168.1.50
    python3 install.py 192.168.1.50 --uninstall

The TV must be in developer mode with this computer's address as Host PC IP (see the README).
The package (ChiakiTv.wgt) is taken from next to this script, or from chiaki-tv/build after a build.
"""
import argparse
import json
import os
import socket
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
for extra in (HERE, os.path.join(HERE, "tv-probe")):
    if os.path.exists(os.path.join(extra, "sdb.py")):
        sys.path.insert(0, extra)
        break
try:
    from sdb import Sdb, SdbError
except ImportError:
    sys.exit("sdb.py is missing. Keep install.py next to sdb.py (release archive) or inside the repository.")

APP_ID = "ChiakiTv01.ChiakiTv"
PACKAGE = "ChiakiTv01"
REMOTE = "/home/owner/share/tmp/sdk_tools/tmp/ChiakiTv.wgt"
SDB_PORT = 26101
SETUP_PORT = 8730


def step(n, text):
    print("[%d/4] %s" % (n, text), flush=True)


def fail(text):
    print("\nStopped: " + text)
    sys.exit(1)


def my_address(tv):
    """This computer's address as the TV sees it."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((tv, 9))
        return s.getsockname()[0]
    except OSError:
        return "?"
    finally:
        s.close()


def find_package(explicit):
    choices = [explicit] if explicit else [os.path.join(HERE, "ChiakiTv.wgt"),
                                           os.path.join(HERE, "chiaki-tv", "build", "ChiakiTv.wgt")]
    for path in choices:
        if path and os.path.exists(path):
            return path
    fail("ChiakiTv.wgt not found. Download the release archive from "
         "https://github.com/MuratDugan/chiaki-tv/releases and run install.py from inside it.")


def open_sdb(tv):
    """Connect to the TV's developer service. The first attempt after the TV wakes is sometimes reset."""
    last = None
    for _ in range(3):
        dev = Sdb(tv)
        try:
            dev.connect()
            return dev
        except (OSError, SdbError) as e:
            last = e
            time.sleep(2)
    me = my_address(tv)
    if not isinstance(last, (ConnectionRefusedError, ConnectionResetError, SdbError)):
        fail("Nothing answers at %s (%s).\n"
             "  - Is the TV on, and on the same network as this computer?\n"
             "  - Is the address right? On the TV: Settings > General > Network > Network Status > IP Settings." % (tv, last))
    fail("The TV refused the connection (%s).\n"
         "  - Developer mode must be On: Apps, press 1 2 3 4 5 on the remote.\n"
         "  - Host PC IP in that dialog must be this computer's address: %s\n"
         "  - After changing either, hold the power button until the TV restarts." % (last, me))


def service(tv, command, timeout):
    dev = open_sdb(tv)
    try:
        return dev.open(command, timeout).decode("utf-8", "replace")
    finally:
        dev.close()


def tv_name(tv):
    try:
        info = json.loads(urllib.request.urlopen("http://%s:8001/api/v2/" % tv, timeout=4).read())
        return info.get("device", {}).get("modelName") or info.get("name") or ""
    except Exception:
        return ""


def uninstall(tv):
    print(service(tv, "shell:0 vd_appuninstall " + APP_ID, 60).strip() or "done")


def install(tv, package):
    step(1, "Checking the TV at %s" % tv)
    dev = open_sdb(tv)
    name = tv_name(tv)
    print("      connected%s" % (" to " + name if name else ""))

    step(2, "Sending the package (%.1f MB)" % (os.path.getsize(package) / 1048576.0))
    try:
        dev.push(package, REMOTE)
    except (OSError, SdbError) as e:
        fail("Could not send the package: %s" % e)
    finally:
        dev.close()

    step(3, "Installing")
    out = service(tv, "shell:0 vd_appinstall %s %s" % (APP_ID, REMOTE), 180)
    if "install completed" not in out:
        tail = "\n".join(l for l in out.splitlines() if l.strip() and "installing[" not in l)[-600:]
        fail("The TV did not install the package. Its answer:\n" + tail)

    step(4, "Starting chiaki-tv")
    try:  # close a running copy first so the new version is the one that opens
        req = urllib.request.Request("http://%s:8001/api/v2/applications/%s" % (tv, APP_ID), method="DELETE")
        urllib.request.urlopen(req, timeout=8).read()
        time.sleep(1.5)
    except Exception:
        pass
    out = service(tv, "shell:0 execute %s 0" % APP_ID, 25)
    started = "successfully launched" in out

    print("\nInstalled." if started else "\nInstalled. Open chiaki-tv from the TV's app list.")
    print("Next: on your phone, open  http://%s:%d  and enter the code shown on the TV." % (tv, SETUP_PORT))
    print("The app stays in the TV's app list; you only need this installer again for updates.")


def main():
    parser = argparse.ArgumentParser(description="Install chiaki-tv on a Samsung TV in developer mode.")
    parser.add_argument("tv", nargs="?", help="the TV's address, for example 192.168.1.50")
    parser.add_argument("--wgt", help="path to ChiakiTv.wgt (default: next to this script)")
    parser.add_argument("--uninstall", action="store_true", help="remove chiaki-tv from the TV")
    args = parser.parse_args()
    tv = args.tv or os.environ.get("TV_IP") or input("TV address (Settings > General > Network > Network Status > IP Settings): ").strip()
    if not tv:
        fail("No TV address given.")
    try:
        socket.inet_aton(tv)
    except OSError:
        fail("'%s' is not an IPv4 address." % tv)
    if args.uninstall:
        uninstall(tv)
    else:
        install(tv, find_package(args.wgt))


if __name__ == "__main__":
    main()
