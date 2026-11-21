# Building chiaki-tv from source

Most people do not need this: the [release archive](https://github.com/MuratDugan/chiaki-tv/releases) contains a ready package and an installer. Build from source to change the code, or if your TV refuses the released package's signature.

Requirements:

- A Samsung TV with developer mode enabled and this computer's address set as Host PC IP.
- Samsung's Pepper 47 toolchain (`NaCl_SDK_v47f`) from the Samsung Developers site. Set `NACL_SDK_ROOT` to its `pepper_47` directory, or leave it at `~/Downloads/NaCl_SDK_v47f/pepper_47`.
- Python 3 and OpenSSL.
- Samsung's certificate generator files in `tv-probe/vendor/certgen`, used once to create a signing certificate.

Third-party sources are not in this repository. `chiaki-tv/native/build.py` expects them under `chiaki-tv/vendor`:

| Directory | Version |
|---|---|
| `chiaki-ng` | commit `43728af`, with its `third-party` submodules |
| `nanopb` | 0.4.9.1 |
| `jerasure`, `gf-complete` | the forks chiaki-ng uses |
| `mbedtls` | 2.28.9 |
| `opus` | 1.5.2 |
| `json-c` | 0.17 |
| `curl` | 8.10.1 |
| `cacert.pem` | Mozilla CA bundle from curl.se |

It also expects `protoc` 28.3 in `chiaki-tv/tools/host/protoc/bin` and a Python environment with `protobuf` 5.28.3 in `chiaki-tv/tools/host/venv`. A script that fetches all of this does not exist yet.

Then, from `chiaki-tv`:

```bash
export TV_IP=192.168.1.50      # the TV's address
python3 native/build.py        # dependencies and the module
python3 tvbuild.py build       # signed package: build/ChiakiTv.wgt
python3 tvbuild.py install
python3 tvbuild.py launch
```

`python3 tvbuild.py build` signs the package with a certificate it creates on first use under `tv-probe/certs`. Keep that directory private.

A release archive is made with `python3 tvbuild.py release`.

## Development tools

All in `chiaki-tv/tools`:

- `mockserver.py` serves the three interfaces without a TV.
- `tvdebug.py` runs the app on the TV in debug mode, follows its log over UDP, measures latency and resolves crash addresses. The log, capture and measurement features need a module built with `CHIAKI_TV_DEBUG=1 python3 native/build.py`; the normal build leaves those endpoints out.
- `padtest.py` checks the controller WebSocket on the TV.
- `screenshots.py` regenerates the pictures in `chiaki-tv/docs/screenshots`.
- `sfiscan.py` scans the module for instructions the Native Client validator rejects.

Code comments and tool output are in Turkish.
