#!/usr/bin/env python3
"""chiaki-tv yerel derleme: bagimliliklar + libchiaki + uygulama -> build/native/chiaki_arm.nexe

  python3 native/build.py deps      # yalnizca dis kutuphaneler
  python3 native/build.py chiaki    # + libchiaki
  python3 native/build.py           # hepsi + baglama
  python3 native/build.py clean

Hedef: ARMv7 NaCl, newlib, statik. Arac zinciri Samsung Pepper 47 (gcc 4.9.2).
Artimli: her nesne dosyasi kaynagina, bayraklarina ve iceri aldigi basliklara gore yeniden derlenir.
"""
import concurrent.futures
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
VENDOR = os.path.join(ROOT, "vendor")
COMPAT = os.path.join(HERE, "compat")
OUT = os.path.join(ROOT, "build", "native")
GEN, INC, OBJ, LIB = (os.path.join(OUT, d) for d in ("gen", "include", "obj", "lib"))
SDK = os.environ.get("NACL_SDK_ROOT", os.path.expanduser("~/Downloads/NaCl_SDK_v47f/pepper_47"))
TC = os.path.join(SDK, "toolchain", "mac_arm_newlib", "bin")
CC, CXX, AR, STRIP = (os.path.join(TC, "arm-nacl-" + t) for t in ("gcc", "g++", "ar", "strip"))
HOST = os.path.join(ROOT, "tools", "host")

V = lambda *p: os.path.join(VENDOR, *p)

# Her derleme biriminde ortak: mbedTLS ayari yapi boyutlarini etkiledigi icin herkes ayni dosyayi gormeli.
BASE = ["-O2", "-fno-strict-aliasing", "-ffunction-sections", "-fdata-sections", "-pthread",
        "-DNDEBUG", "-D_GNU_SOURCE", '-DMBEDTLS_USER_CONFIG_FILE="chiaki_mbedtls_user.h"',
        "-I" + COMPAT, "-I" + os.path.join(SDK, "include", "newlib"), "-I" + os.path.join(SDK, "include")]
CSTD, CXXSTD = ["-std=gnu11"], ["-std=gnu++11"]

# libchiaki'yi derleyen ve kullanan her birim icin (uygulama kodu dahil)
CHIAKI_FLAGS = ["-DCHIAKI_NACL=1", "-DCHIAKI_LIB_ENABLE_MBEDTLS", "-DPB_C99_STATIC_ASSERT", "-DCURL_STATICLIB",
                "-I" + V("chiaki-ng", "lib", "include"), "-I" + V("chiaki-ng", "lib", "src"), "-I" + GEN, "-I" + INC,
                "-I" + V("nanopb"), "-I" + V("jerasure", "include"), "-I" + V("gf-complete", "include"),
                "-I" + V("mbedtls", "include"), "-I" + V("curl", "include")]


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kw)


def deps_fresh(obj, dep):
    """Nesne dosyasi, .d dosyasinda listelenen her kaynaktan yeni mi?"""
    try:
        t = os.path.getmtime(obj)
        text = open(dep).read().replace("\\\n", " ")
        for path in text.split(":", 1)[1].split():
            if os.path.getmtime(path) > t:
                return False
        return True
    except (OSError, IndexError):
        return False


def compile_one(job):
    src, obj, flags = job
    dep, sig = obj[:-2] + ".d", obj[:-2] + ".sig"
    cxx = src.endswith((".cc", ".cpp"))
    cmd = [CXX if cxx else CC] + (CXXSTD if cxx else CSTD) + BASE + flags + ["-MMD", "-MF", dep, "-c", src, "-o", obj]
    want = hashlib.sha1(" ".join(cmd).encode()).hexdigest()
    try:
        if open(sig).read() == want and deps_fresh(obj, dep):
            return None
    except OSError:
        pass
    res = run(cmd)
    if res.returncode != 0:
        return src, res.stdout.decode(errors="replace")
    open(sig, "w").write(want)
    return None


def static_lib(name, sources, flags):
    """Kaynaklari derleyip build/native/lib/lib<name>.a uretir; yolunu dondurur."""
    odir = os.path.join(OBJ, name)
    os.makedirs(odir, exist_ok=True)
    os.makedirs(LIB, exist_ok=True)
    jobs = []
    for src in sources:
        tag = hashlib.sha1(src.encode()).hexdigest()[:8]
        jobs.append((src, os.path.join(odir, "%s_%s.o" % (os.path.splitext(os.path.basename(src))[0], tag)), flags))
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        failed = [r for r in pool.map(compile_one, jobs) if r]
    if failed:
        print("== %s: %d/%d dosya derlenemedi ==" % (name, len(failed), len(jobs)))
        for src, out in failed[:4]:
            lines = [l for l in out.splitlines() if "error" in l or "Error" in l][:8] or out.splitlines()[:8]
            print("-- " + os.path.relpath(src, ROOT))
            print("\n".join("   " + l[-230:] for l in lines))
        if len(failed) > 4:
            print("-- ve %d dosya daha: %s" % (len(failed) - 4, ", ".join(os.path.basename(s) for s, _ in failed[4:20])))
        raise SystemExit(1)
    out = os.path.join(LIB, "lib%s.a" % name)
    objs = [j[1] for j in jobs]
    if not os.path.exists(out) or any(os.path.getmtime(o) > os.path.getmtime(out) for o in objs):
        if os.path.exists(out):
            os.remove(out)
        res = run([AR, "rcs", out] + objs)
        if res.returncode != 0:
            raise SystemExit(res.stdout.decode(errors="replace"))
    print("%-12s %3d dosya -> lib%s.a (%d KB)" % (name, len(jobs), name, os.path.getsize(out) // 1024))
    return out


def parse_mk(path):
    """opus'un *_sources.mk dosyalarindaki 'AD = a.c \\ b.c' listelerini okur."""
    text = open(path).read().replace("\\\n", " ")
    return {m.group(1): m.group(2).split() for m in re.finditer(r"^(\w+)\s*=\s*(.*)$", text, re.M)}


def copy_headers(dst, patterns):
    os.makedirs(dst, exist_ok=True)
    for pat in patterns:
        for h in glob.glob(pat):
            target = os.path.join(dst, os.path.basename(h))
            if not os.path.exists(target) or os.path.getmtime(h) > os.path.getmtime(target):
                shutil.copy2(h, target)


# --------------------------------------------------------------------------- bagimliliklar
def build_nanopb():
    os.makedirs(GEN, exist_ok=True)
    proto = V("chiaki-ng", "lib", "protobuf", "takion.proto")
    pb, out_c = os.path.join(GEN, "takion.pb"), os.path.join(GEN, "takion.pb.c")
    if not os.path.exists(out_c) or os.path.getmtime(proto) > os.path.getmtime(out_c):
        protoc = os.path.join(HOST, "protoc", "bin", "protoc")
        res = run([protoc, "-o" + pb, proto, "-I" + os.path.dirname(proto)])
        if res.returncode != 0:
            raise SystemExit("protoc: " + res.stdout.decode(errors="replace"))
        env = dict(os.environ, PATH=os.path.dirname(protoc) + ":" + os.environ.get("PATH", ""))
        res = run([os.path.join(HOST, "venv", "bin", "python"), V("nanopb", "generator", "nanopb_generator.py"),
                   "takion.pb"], cwd=GEN, env=env)
        if res.returncode != 0 or not os.path.exists(out_c):
            raise SystemExit("nanopb uretici: " + res.stdout.decode(errors="replace")[-1500:])
    return static_lib("nanopb", [V("nanopb", f) for f in ("pb_common.c", "pb_encode.c", "pb_decode.c")] + [out_c],
                      ["-DPB_C99_STATIC_ASSERT", "-I" + V("nanopb"), "-I" + GEN])


def build_fec():
    gf = [V("gf-complete", "src", f) for f in ("gf.c", "gf_wgen.c", "gf_w4.c", "gf_w8.c", "gf_w16.c", "gf_w32.c",
                                                 "gf_w64.c", "gf_w128.c", "gf_rand.c", "gf_general.c", "gf_cpu.c")]
    je = [V("jerasure", "src", f) for f in ("galois.c", "jerasure.c", "reed_sol.c", "cauchy.c", "liberation.c")]
    inc = ["-I" + V("gf-complete", "include"), "-I" + V("jerasure", "include")]
    return [static_lib("jerasure", je, inc), static_lib("gf_complete", gf, inc)]


def build_opus():
    root = V("opus")
    srcs = []
    for mk, names in (("celt_sources.mk", ["CELT_SOURCES"]), ("silk_sources.mk", ["SILK_SOURCES", "SILK_SOURCES_FLOAT"]),
                      ("opus_sources.mk", ["OPUS_SOURCES", "OPUS_SOURCES_FLOAT"])):
        table = parse_mk(os.path.join(root, mk))
        for n in names:
            srcs += [os.path.join(root, f) for f in table.get(n, [])]
    copy_headers(os.path.join(INC, "opus"), [os.path.join(root, "include", "*.h")])
    flags = ["-DOPUS_BUILD", "-DHAVE_LRINT", "-DHAVE_LRINTF", "-DVAR_ARRAYS", '-DPACKAGE_VERSION="1.5.2"',
             "-I" + os.path.join(root, "include"), "-I" + os.path.join(root, "celt"), "-I" + os.path.join(root, "silk"),
             "-I" + os.path.join(root, "silk", "float"), "-I" + root]
    return static_lib("opus", srcs, flags)


def build_jsonc():
    root = V("json-c")
    dst = os.path.join(INC, "json-c")
    copy_headers(dst, [os.path.join(root, "*.h")])
    json_h = os.path.join(dst, "json.h")
    text = open(os.path.join(root, "json.h.cmakein")).read()
    text = text.replace("@JSON_H_JSON_POINTER@", '#include "json_pointer.h"').replace("@JSON_H_JSON_PATCH@", '#include "json_patch.h"')
    if not os.path.exists(json_h) or open(json_h).read() != text:
        open(json_h, "w").write(text)
    cfg = os.path.join(dst, "json_config.h")
    if not os.path.exists(cfg):
        open(cfg, "w").write("#define JSON_C_HAVE_INTTYPES_H 1\n#define JSON_C_HAVE_STDINT_H 1\n")
    names = ["arraylist", "debug", "json_c_version", "json_object", "json_object_iterator", "json_patch", "json_pointer",
             "json_tokener", "json_util", "json_visit", "linkhash", "printbuf", "random_seed", "strerror_override"]
    return static_lib("json-c", [os.path.join(root, n + ".c") for n in names],
                      ["-DHAVE_CONFIG_H", "-I" + os.path.join(COMPAT, "json-c"), "-I" + dst, "-I" + root])


def build_mbedtls():
    root = V("mbedtls")
    skip = {"net_sockets.c", "timing.c", "havege.c"}
    srcs = [f for f in sorted(glob.glob(os.path.join(root, "library", "*.c"))) if os.path.basename(f) not in skip]
    srcs.append(os.path.join(COMPAT, "entropy_nacl.c"))
    return static_lib("mbedtls", srcs, ["-I" + os.path.join(root, "include"), "-I" + os.path.join(root, "library")])


def build_curl():
    root = V("curl")
    srcs = []
    for sub in ("", "vauth", "vtls", "vquic", "vssh"):
        srcs += sorted(glob.glob(os.path.join(root, "lib", sub, "*.c")))
    flags = ["-DHAVE_CONFIG_H", "-DBUILDING_LIBCURL", "-DCURL_STATICLIB", "-I" + os.path.join(COMPAT, "curl"),
             "-I" + os.path.join(root, "include"), "-I" + os.path.join(root, "lib"), "-I" + V("mbedtls", "include")]
    return static_lib("curl", srcs, flags)


def build_chiaki():
    """libchiaki: Switch yollarini NaCl icin acan yamalarla, mbedTLS ve Opus ile."""
    import patch_chiaki
    root = V("chiaki-ng")
    changed = patch_chiaki.apply(root)
    if changed:
        print("yamalandi: " + ", ".join(changed))
    cfg_dir = os.path.join(GEN, "chiaki")
    os.makedirs(cfg_dir, exist_ok=True)
    cfg = ("#ifndef CHIAKI_CONFIG_H\n#define CHIAKI_CONFIG_H\n#define CHIAKI_LIB_ENABLE_OPUS 1\n"
           "#define CHIAKI_LIB_ENABLE_PI_DECODER 0\n#define CHIAKI_LIB_ENABLE_FFMPEG_DECODER 0\n#endif\n")
    cfg_path = os.path.join(cfg_dir, "config.h")
    if not os.path.exists(cfg_path) or open(cfg_path).read() != cfg:
        open(cfg_path, "w").write(cfg)
    skip = {"aia_openssl.c", "ffmpegdecoder.c", "pidecoder.c", "holepunch-test.c"}
    srcs = [f for f in sorted(glob.glob(os.path.join(root, "lib", "src", "*.c")) +
                              glob.glob(os.path.join(root, "lib", "src", "remote", "*.c")))
            if os.path.basename(f) not in skip]
    srcs += [os.path.join(COMPAT, "platform_nacl.c"), os.path.join(COMPAT, "nacl_sock.c")]
    # nacl_sock.h: SO_RCVTIMEO taklidi ve desteklenmeyen soket seceneklerine hosgoru (yalnizca libchiaki).
    return static_lib("chiaki", srcs, CHIAKI_FLAGS + ["-include", os.path.join(COMPAT, "nacl_sock.h")])


# CHIAKI_SETUP_DIR: gomulecek telefon sayfalarinin klasoru (varsayilan setup/).
SETUP = os.environ.get("CHIAKI_SETUP_DIR") or os.path.join(ROOT, "setup")


def gen_blobs():
    """Kok sertifika paketini ve telefon kurulum sayfasini C dizisi olarak gomer."""
    os.makedirs(GEN, exist_ok=True)
    out = os.path.join(GEN, "blobs.c")
    # Kok paket + sunucularin gondermedigi ara sertifikalar (compat/extra_ca.pem) tek dosya olarak gomulur.
    sources = [("g_cacert_pem", [V("cacert.pem"), os.path.join(HERE, "compat", "extra_ca.pem")]),
               ("g_setup_html", [os.path.join(SETUP, "index.html")]),
               ("g_pad_html", [os.path.join(SETUP, "pad.html")])]
    if os.path.exists(out) and all(os.path.getmtime(p) <= os.path.getmtime(out) for _, ps in sources for p in ps):
        return out
    parts = ["/* build.py tarafindan uretildi; elle duzenleme. */\n"]
    for name, paths in sources:
        data = b"\n".join(open(path, "rb").read() for path in paths)
        rows = [",".join(str(x) for x in data[i:i + 32]) for i in range(0, len(data), 32)]
        parts.append("const unsigned char %s[] = {\n%s\n};\nconst unsigned int %s_len = %d;\n"
                     % (name, ",\n".join(rows), name, len(data)))
    open(out, "w").write("\n".join(parts))
    return out


def build_app():
    srcs = sorted(glob.glob(os.path.join(HERE, "app", "*.cc"))) + [gen_blobs()]
    return static_lib("app", srcs, CHIAKI_FLAGS + ["-Wall", "-I" + os.path.join(HERE, "app")])


def build_nacl_io_fixed():
    """Kitin hazir libnacl_io.a'sindaki derleyici hatasini giderir.

    real_pepper_interface.cc'deki (kaynak, kaynak, PP_CompletionCallback) bicimli sarmalayicilar kuyruk
    cagrisi olarak derlenmis ve gcc 4.9 geri cagri yapisini tasirken user_data'yi flags ile eziyor:
    engellemesiz her TCP connect()/accept() NULL isaretciyle geri cagrilip modulu cokertiyor. O dosya
    kuyruk cagrisi eniyilemesi kapali yeniden derlenir ve arsivin ozel bir kopyasina konur.
    """
    sdk_lib = os.path.join(SDK, "lib", "newlib_arm", "Release", "libnacl_io.a")
    src = os.path.join(SDK, "src", "nacl_io", "real_pepper_interface.cc")
    fixed = os.path.join(OUT, "lib", "libnacl_io_fixed.a")
    if os.path.exists(fixed) and os.path.getmtime(fixed) >= max(os.path.getmtime(sdk_lib), os.path.getmtime(__file__)):
        return fixed
    obj = os.path.join(OUT, "obj", "real_pepper_interface_arm.o")
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    res = run([CXX, "-c", src, "-o", obj, "-O2", "-fno-optimize-sibling-calls", "-DNACL_IO_LOGGING=0", "-DNDEBUG",
               "-I" + os.path.join(SDK, "src"), "-I" + os.path.join(SDK, "include")])
    if res.returncode != 0:
        raise SystemExit(res.stdout.decode(errors="replace")[-3000:])
    shutil.copyfile(sdk_lib, fixed)
    for args in (["d", fixed, "real_pepper_interface_arm.o"], ["rs", fixed, obj]):
        res = run([AR] + args)
        if res.returncode != 0:
            raise SystemExit(res.stdout.decode(errors="replace"))
    print("nacl_io      real_pepper_interface yeniden derlendi -> libnacl_io_fixed.a")
    return fixed


def link(app_lib, libs):
    out = os.path.join(OUT, "chiaki_arm_unstripped.nexe")
    final = os.path.join(OUT, "chiaki_arm.nexe")
    nacl_io = build_nacl_io_fixed()
    inputs = [app_lib, nacl_io] + libs
    if os.path.exists(final) and all(os.path.getmtime(p) <= os.path.getmtime(final) for p in inputs):
        print("modul guncel: %d KB" % (os.path.getsize(final) // 1024))
        return final
    # Uygulama arsivi butun olarak alinir: Pepper giris noktasi (pp::CreateModule) orada.
    cmd = [CXX, "-o", out, "-pthread", "-Wl,--gc-sections", "-Wl,--whole-archive", app_lib, "-Wl,--no-whole-archive",
           "-Wl,--start-group"] + libs + ["-Wl,--end-group",
        "-L" + os.path.join(SDK, "lib", "newlib_arm", "Release"),
        nacl_io, "-lnacl_exception", "-lppapi_gles2", "-lppapi_cpp", "-lppapi", "-lpthread", "-lm"]
    res = run(cmd)
    if res.returncode != 0:
        text = res.stdout.decode(errors="replace")
        undef = sorted(set(re.findall(r"undefined reference to `([^']+)'", text)))
        print("== baglama basarisiz ==")
        print("\n".join("   cozulmeyen: " + u for u in undef[:40]) if undef else text[-2500:])
        raise SystemExit(1)
    res = run([STRIP, "-o", final, out])
    if res.returncode != 0:
        raise SystemExit(res.stdout.decode(errors="replace"))
    print("modul: build/native/chiaki_arm.nexe (%d KB)" % (os.path.getsize(final) // 1024))
    return final


def build_deps(only=None):
    table = [("nanopb", build_nanopb), ("fec", build_fec), ("opus", build_opus), ("json-c", build_jsonc),
             ("mbedtls", build_mbedtls), ("curl", build_curl)]
    libs = []
    for name, fn in table:
        if only and name not in only:
            continue
        r = fn()
        libs += r if isinstance(r, list) else [r]
    return libs


def apply_debug_flag():
    """CHIAKI_TV_DEBUG=1: tanilama uclari (/api/debug, /api/dump) ve ayrintili gunluk derlenir. Varsayilan kapali.

    Bayrak degisince ondan etkilenen nesneler (uygulama ve chiaki) yeniden derlenir.
    """
    debug = os.environ.get("CHIAKI_TV_DEBUG") == "1"
    if debug:
        CHIAKI_FLAGS.append("-DCHIAKI_TV_DEBUG=1")
    stamp = os.path.join(OUT, "debug.stamp")
    old = open(stamp).read() if os.path.exists(stamp) else ""
    if old != str(int(debug)):
        for name in ("app", "chiaki"):
            shutil.rmtree(os.path.join(OUT, "obj", name), ignore_errors=True)
        os.makedirs(OUT, exist_ok=True)
        open(stamp, "w").write(str(int(debug)))
    print("surum: %s" % ("TANILAMA (CHIAKI_TV_DEBUG=1)" if debug else "yayin"))


def main():
    apply_debug_flag()
    args = sys.argv[1:]
    if args[:1] == ["clean"]:
        shutil.rmtree(OUT, ignore_errors=True)
        print("temizlendi")
        return 0
    if args[:1] == ["deps"]:
        build_deps(set(args[1:]) or None)
        return 0
    sys.path.insert(0, HERE)
    libs = build_deps()
    libs.insert(0, build_chiaki())
    if args[:1] == ["chiaki"]:
        return 0
    link(build_app(), libs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
