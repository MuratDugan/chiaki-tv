"""vendor/chiaki-ng kaynaklarina NaCl yamalari.

chiaki-ng'de Nintendo Switch icin ayri kod yollari var; Switch de newlib tabanli ve ayni
eksiklere sahip (ifaddrs yok, libevent yok, IPv6 soketi yok). Bu betik o yollari CHIAKI_NACL
tanimliyken de etkinlestirir. Her dosyanin ilk hali "<dosya>.orig" olarak saklanir ve yama her
seferinde oradan yeniden uretilir; yani betik tekrar tekrar calistirilabilir.
"""
import os
import re
import shutil

_COND = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif)\b")


def _like_switch(line):
    """Bir onislemci kosulundaki Switch sinamasini 'Switch ya da NaCl' yapar."""
    if "__SWITCH__" not in line or "CHIAKI_NACL" in line or not _COND.match(line):
        return line
    m = re.match(r"^(\s*)#\s*ifdef\s+__SWITCH__(.*)$", line, re.S)
    if m:
        return "%s#if defined(__SWITCH__) || defined(CHIAKI_NACL)%s" % (m.group(1), m.group(2))
    m = re.match(r"^(\s*)#\s*ifndef\s+__SWITCH__(.*)$", line, re.S)
    if m:
        return "%s#if !defined(__SWITCH__) && !defined(CHIAKI_NACL)%s" % (m.group(1), m.group(2))
    line = re.sub(r"!\s*defined\(__SWITCH__\)", "!defined(__SWITCH__) && !defined(CHIAKI_NACL)", line)
    line = re.sub(r"(?<![!\w])defined\(__SWITCH__\)(?! && !defined\(CHIAKI_NACL\))",
                  "(defined(__SWITCH__) || defined(CHIAKI_NACL))", line)
    line = re.sub(r"!__SWITCH__\b", "!__SWITCH__ && !defined(CHIAKI_NACL)", line)
    return line


def _all_lines(text):
    return "".join(_like_switch(l) for l in text.splitlines(True))


def _only(matching):
    def apply(text):
        out, hit = [], 0
        for l in text.splitlines(True):
            if matching in l:
                hit += 1
                l = _like_switch(l)
            out.append(l)
        if not hit:
            raise SystemExit("yama hedefi bulunamadi: %r" % matching)
        return "".join(out)
    return apply


def _replace(old, new, count=1):
    def apply(text):
        if text.count(old) < count:
            raise SystemExit("yama hedefi bulunamadi: %r" % old[:60])
        return text.replace(old, new)
    return apply


# dosya -> uygulanacak donusumler (sirayla)
PATCHES = {
    # libevent yerine poll, IPv6 soketi yok, yerel adres gethostid() ile (compat/platform_nacl.c)
    "lib/src/remote/holepunch.c": [
        _all_lines,
        # Teklif olusturulduktan sonra delik acma hic calismadan (konsol kapali, kullanici vazgecti)
        # oturum kapatilirsa UDP soketleri ve teklif bellegi sizar: fini'de birak. Basarili yolda
        # bu alanlar zaten gecersiz/NULL yapilmis olur.
        _replace("    if (session->oauth_header)\n        free(session->oauth_header);",
                 "    if(!CHIAKI_SOCKET_IS_INVALID(session->ipv4_sock))\n"
                 "    {\n        CHIAKI_SOCKET_CLOSE(session->ipv4_sock);\n        session->ipv4_sock = CHIAKI_INVALID_SOCKET;\n    }\n"
                 "    if(!CHIAKI_SOCKET_IS_INVALID(session->ipv6_sock))\n"
                 "    {\n        CHIAKI_SOCKET_CLOSE(session->ipv6_sock);\n        session->ipv6_sock = CHIAKI_INVALID_SOCKET;\n    }\n"
                 "    if(session->our_offer_msg)\n"
                 "    {\n        session_message_free(session->our_offer_msg);\n        session->our_offer_msg = NULL;\n    }\n"
                 "    if(session->local_candidates)\n"
                 "    {\n        free(session->local_candidates);\n        session->local_candidates = NULL;\n    }\n"
                 "    if (session->oauth_header)\n        free(session->oauth_header);"),
        # Ust akim hatasi: chiaki_holepunch_session_start basarili yolda state_mutex'i iki kez birakir
        # (dongu sonundaki ve dongu ciktisindaki unlock). glibc/macOS/Windows bunu hos gorur; NaCl'de
        # kilit durumu -1'e duser ve sonraki ilk lock sonsuza dek bekler. Cikistaki unlock yalnizca
        # dongu "break" ile terk edildiginde (kilit hala tutuluyorken) calissin.
        _replace("            CHIAKI_LOGI(session->log, \"chiaki_holepunch_session_start: canceled\");\n"
                 "            err = CHIAKI_ERR_CANCELED;\n            return err;\n        }\n"
                 "        chiaki_mutex_unlock(&session->stop_mutex);\n        http_check_session(session, false);",
                 "            CHIAKI_LOGI(session->log, \"chiaki_holepunch_session_start: canceled\");\n"
                 "            err = CHIAKI_ERR_CANCELED;\n            break;\n        }\n"
                 "        chiaki_mutex_unlock(&session->stop_mutex);\n        http_check_session(session, false);"),
        _replace("        log_session_state(session);\n        chiaki_mutex_unlock(&session->state_mutex);\n    }\n"
                 "    chiaki_mutex_unlock(&session->state_mutex);\n    return err;\n}",
                 "        log_session_state(session);\n        chiaki_mutex_unlock(&session->state_mutex);\n    }\n"
                 "    if(!finished)\n        chiaki_mutex_unlock(&session->state_mutex);\n    return err;\n}"),
        # Tanilama: bildirim WebSocket'inin baglanti adimlari gunluge (yalnizca curl bilgi satirlari;
        # basliklar ve govde belirtec icerdiginden yazilmaz).
        _replace("static void* websocket_thread_func(void *user) {",
                 "#ifdef CHIAKI_NACL\n"
                 "static int nacl_ws_debug(CURL *handle, curl_infotype type, char *data, size_t size, void *user)\n"
                 "{\n    (void)handle;\n    if(type != CURLINFO_TEXT)\n        return 0;\n"
                 "    /* 5 saniyede bir atilan ping satirlari gunlugu doldurmasin */\n"
                 "    if(size >= 3 && (!strncmp(data, \"WS-\", 3) || !strncmp(data, \"WS:\", 3)))\n        return 0;\n"
                 "    while(size > 0 && (data[size - 1] == '\\n' || data[size - 1] == '\\r'))\n        size--;\n"
                 "    CHIAKI_LOGI((ChiakiLog *)user, \"ws curl: %.*s\", (int)size, data);\n    return 0;\n}\n"
                 "#endif\n\n"
                 "static void* websocket_thread_func(void *user) {"),
        _replace("    curl_easy_setopt(curl, CURLOPT_CERTINFO, 1L);\n    res = curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);",
                 "    curl_easy_setopt(curl, CURLOPT_CERTINFO, 1L);\n"
                 "#ifdef CHIAKI_NACL\n"
                 "    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, nacl_ws_debug);\n"
                 "    curl_easy_setopt(curl, CURLOPT_DEBUGDATA, session->log);\n"
                 "    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);\n"
                 "#endif\n"
                 "    res = curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);"),
    ],
    "lib/src/remote/rudp.c": [_all_lines],
    "lib/src/audio.c": [_all_lines],
    # pthread_timedjoin_np newlib'de yok
    # NaCl'de pthread_cond_timedwait bitis zamanini gercek zaman saatine gore yorumlar; kutuphane ise
    # CLOCK_MONOTONIC (acilistan beri gecen sure) verir. Sonuc: her zamanli bekleme aninda "zaman asimi"
    # doner (30 sn'lik WebSocket beklemesi, UPnP, takion...). Bitis zamanini gercek zaman saatinden hesapla.
    "lib/src/thread.c": [
        _only("!__APPLE__ && !__SWITCH__"),
        _replace("#if !__APPLE__\n\tr = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);",
                 "#if !__APPLE__ && !defined(CHIAKI_NACL)\n\tr = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);"),
        # Kilitli olmayan bir kilidi birakmak NaCl'de kilidi kalici olarak bozar (yukaridaki nota bak).
        # Diger platformlardaki gibi zararsiz kalsin; kac kez oldugu sayilir, uygulama gunluge yazar.
        _replace("CHIAKI_EXPORT ChiakiErrorCode chiaki_mutex_unlock(ChiakiMutex *mutex)\n{\n#if _WIN32\n"
                 "\tLeaveCriticalSection(&mutex->cs);\n#else\n",
                 "#ifdef CHIAKI_NACL\nvolatile int chiaki_nacl_bad_unlocks = 0;\n#endif\n\n"
                 "CHIAKI_EXPORT ChiakiErrorCode chiaki_mutex_unlock(ChiakiMutex *mutex)\n{\n#if _WIN32\n"
                 "\tLeaveCriticalSection(&mutex->cs);\n#else\n"
                 "#ifdef CHIAKI_NACL\n"
                 "\tif(mutex->mutex.mutex_type == PTHREAD_MUTEX_FAST_NP && mutex->mutex.mutex_state == 0)\n"
                 "\t{\n\t\t__sync_fetch_and_add(&chiaki_nacl_bad_unlocks, 1);\n\t\treturn CHIAKI_ERR_SUCCESS;\n\t}\n"
                 "#endif\n"),
        _replace("\tclock_gettime(CLOCK_MONOTONIC, timeout);",
                 "#ifdef CHIAKI_NACL\n\tclock_gettime(CLOCK_REALTIME, timeout);\n#else\n"
                 "\tclock_gettime(CLOCK_MONOTONIC, timeout);\n#endif"),
    ],
    # Ust akima gonderilen yama (fix/session-stop-during-data-holepunch): oturum is parcacigi veri deligini
    # acarken state_mutex'i tutar; durdurma once delik acmayi iptal etmeli.
    "lib/src/session.c": [
        _replace("CHIAKI_EXPORT ChiakiErrorCode chiaki_session_stop(ChiakiSession *session)\n{\n"
                 "\tChiakiErrorCode err = chiaki_mutex_lock(&session->state_mutex);",
                 "CHIAKI_EXPORT ChiakiErrorCode chiaki_session_stop(ChiakiSession *session)\n{\n"
                 "\tif(session->holepunch_session)\n"
                 "\t\tchiaki_holepunch_main_thread_cancel(session->holepunch_session, false);\n"
                 "\tChiakiErrorCode err = chiaki_mutex_lock(&session->state_mutex);"),
    ],
    # netinet/ip.h'siz baslik kumesi
    "lib/src/takion.c": [_replace("#elif defined(__SWITCH__)\n#include <unistd.h>",
                                  "#elif defined(__SWITCH__) || defined(CHIAKI_NACL)\n#include <unistd.h>")],
}


def apply(chiaki_root):
    changed = []
    for rel, steps in PATCHES.items():
        path = os.path.join(chiaki_root, rel)
        orig = path + ".orig"
        if not os.path.exists(orig):
            shutil.copy2(path, orig)
        text = open(orig, encoding="utf-8").read()
        for step in steps:
            text = step(text)
        if open(path, encoding="utf-8").read() != text:
            open(path, "w", encoding="utf-8").write(text)
            changed.append(rel)
    return changed
