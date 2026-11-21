# Building chiaki-tv from source

Most people do not need this: the [release archive](https://github.com/MuratDugan/chiaki-tv/releases) contains a ready package and an installer. Build from source to change the code, or if your TV refuses the released package's signature.

Requirements:

- A Mac. `build.py` uses the macOS toolchain of the SDK; other hosts are untested.
- A Samsung TV with developer mode enabled and this computer's address set as Host PC IP.
- Samsung's Pepper 47 toolchain (`NaCl_SDK_v47f`) from the Samsung Developers site. Set `NACL_SDK_ROOT` to its `pepper_47` directory, or leave it at `~/Downloads/NaCl_SDK_v47f/pepper_47`.
- Python 3, git and OpenSSL.

Then, from `chiaki-tv`:

```bash
export TV_IP=192.168.1.50      # the TV's address
python3 native/fetch_deps.py   # third-party sources, host tools, signing files
python3 native/build.py        # dependencies and the module
python3 tvbuild.py build       # signed package: build/ChiakiTv.wgt
python3 tvbuild.py install
python3 tvbuild.py launch
```

`python3 tvbuild.py build` signs the package with a certificate it creates on first use under `tv-probe/certs`. Keep that directory private.

A release archive is made with `python3 tvbuild.py release`.

## What fetch_deps.py downloads

Third-party sources are not in this repository. `native/fetch_deps.py` downloads them, about 210 MB on disk, and checks each one: git checkouts against a commit, files against a SHA-256. It installs nothing outside the repository and leaves alone any directory that already exists. `python3 native/fetch_deps.py --check` reports what is present without downloading.

| Goes to | What | Version |
|---|---|---|
| `chiaki-tv/vendor/chiaki-ng` | chiaki-ng | commit `43728af` |
| `chiaki-tv/vendor/nanopb` | nanopb | 0.4.9.1 |
| `chiaki-tv/vendor/jerasure`, `gf-complete` | the forks chiaki-ng uses | the commits chiaki-ng pins |
| `chiaki-tv/vendor/mbedtls` | mbedTLS | 2.28.9 |
| `chiaki-tv/vendor/opus` | Opus | 1.5.2 |
| `chiaki-tv/vendor/json-c` | json-c | 0.17 |
| `chiaki-tv/vendor/curl` | curl | 8.10.1 |
| `chiaki-tv/vendor/cacert.pem` | Mozilla CA bundle from curl.se | 2026-11-05 |
| `chiaki-tv/tools/host/protoc` | protoc, to compile chiaki-ng's protobuf file | 28.3 |
| `chiaki-tv/tools/host/pylib` | Python `protobuf` package, for the nanopb generator | 5.28.3 |
| `tv-probe/vendor/certgen` | certificate files from Tizen Studio's certificate generator | 0.1.4 |

The certificate files come from the Tizen SDK repository at download.tizen.org. They are the developer CA that issues your author certificate and the public distributor certificate, the same ones Tizen Studio signs with.

To move to a newer version of a dependency, change its commit or URL and hash at the top of `fetch_deps.py`, remove its directory, and run the script again.

## Development tools

All in `chiaki-tv/tools`:

- `mockserver.py` serves the three interfaces without a TV.
- `tvdebug.py` runs the app on the TV in debug mode, follows its log over UDP, measures latency and resolves crash addresses. The log, capture and measurement features need a module built with `CHIAKI_TV_DEBUG=1 python3 native/build.py`; the normal build leaves those endpoints out.
- `padtest.py` checks the controller WebSocket on the TV.
- `screenshots.py` regenerates the pictures in `chiaki-tv/docs/screenshots`.
- `sfiscan.py` scans the module for instructions the Native Client validator rejects.

Code comments and most tool output are in Turkish. `install.py` and `fetch_deps.py` print English.
