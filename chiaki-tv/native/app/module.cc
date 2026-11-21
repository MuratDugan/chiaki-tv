// chiaki-tv: Pepper modulu. JS ile mesajlasir, cekirdegi (Core) video/ses/girdi katmanlarina baglar
// ve telefondan kurulum icin HTTP uclarini sunar.
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

#include <nacl/nacl_exception.h>

#include <map>
#include <string>
#include <vector>

#include <chiaki/common.h>
#include <curl/curl.h>

#include "nacl_io/nacl_io.h"
#include "ppapi/cpp/core.h"
#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/module.h"
#include "ppapi/cpp/rect.h"
#include "ppapi/cpp/var.h"
#include "ppapi/cpp/view.h"
#include "ppapi/utility/completion_callback_factory.h"

#include "audio.h"
#include "core.h"
#include "dbgsink.h"
#include "httpd.h"
#include "input.h"
#include "platform_nacl.h"
#include "i18n.h"
#include "util.h"
#include "video.h"

#ifdef PostMessage
#undef PostMessage
#endif

// build.py'nin urettigi gomulu veriler (gen/blobs.c).
extern "C" {
extern const unsigned char g_cacert_pem[];
extern const unsigned int g_cacert_pem_len;
extern const unsigned char g_setup_html[];
extern const unsigned int g_setup_html_len;
extern const unsigned char g_pad_html[];
extern const unsigned int g_pad_html_len;
}

namespace {

const uint16_t kSetupPort = 8730;
const int kTickStreamingMs = 8;  // oyun kolunu ~120 Hz ornekle
const int kTickIdleMs = 200;

std::string JsonError(const std::string& message) {
  json_object* o = json_object_new_object();
  util::JAddBool(o, "ok", false);
  util::JAdd(o, "error", message);
  return util::JDump(o);
}

const char kOk[] = "{\"ok\":true}";

// ------------------------------------------------------------------ cokme tanilamasi
// Donanim hatasinda (gecersiz bellek erisimi, yigin tasmasi, abort) yazmaclari JS'e gonderir;
// adresler derlemedeki chiaki_arm_unstripped.nexe ile arm-nacl-addr2line'a verilir.
pp::Instance* g_instance = NULL;

void CrashHandler(struct NaClExceptionContext* context) {
  char line[640];
  const NaClUserRegisterState& r = context->regs;
  snprintf(line, sizeof(line),
           "{\"ev\":\"crash\",\"pc\":\"%x\",\"lr\":\"%x\",\"sp\":\"%x\",\"r0\":\"%x\",\"r1\":\"%x\","
           "\"r2\":\"%x\",\"r3\":\"%x\",\"r4\":\"%x\",\"r5\":\"%x\",\"r6\":\"%x\",\"r7\":\"%x\",\"thread\":\"%s\",\"main\":%s}",
           static_cast<unsigned>(r.prog_ctr), static_cast<unsigned>(r.lr), static_cast<unsigned>(r.stack_ptr),
           static_cast<unsigned>(r.r0), static_cast<unsigned>(r.r1), static_cast<unsigned>(r.r2),
           static_cast<unsigned>(r.r3), static_cast<unsigned>(r.r4), static_cast<unsigned>(r.r5),
           static_cast<unsigned>(r.r6), static_cast<unsigned>(r.r7), util::ThreadTag(),
           pp::Module::Get()->core()->IsMainThread() ? "true" : "false");
  if (g_instance) g_instance->PostMessage(pp::Var(line));
  // Yigindaki donus adresleri: NaCl ARM'da cagri donusleri 16 baytlik demet basina denk gelir.
  int n = snprintf(line, sizeof(line), "{\"ev\":\"crashstack\",\"frames\":\"");
  const uint32_t* sp = reinterpret_cast<const uint32_t*>(r.stack_ptr & ~3u);
  int found = 0;
  for (int i = 0; i < 400 && found < 28; i++) {
    uint32_t v = sp[i];
    if (v >= 0x20000 && v < 0x01000000 && (v & 0xF) == 0) {
      n += snprintf(line + n, sizeof(line) - n, "%x ", static_cast<unsigned>(v));
      found++;
    }
  }
  snprintf(line + n, sizeof(line) - n, "\"}");
  if (g_instance) g_instance->PostMessage(pp::Var(line));
  // Ham yigin: tarayici tarafindaki (IRT) cagri zinciri icin.
  n = snprintf(line, sizeof(line), "{\"ev\":\"crashraw\",\"words\":\"");
  for (int i = 0; i < 56; i++) n += snprintf(line + n, sizeof(line) - n, "%x ", static_cast<unsigned>(sp[i]));
  snprintf(line + n, sizeof(line) - n, "\"}");
  if (g_instance) g_instance->PostMessage(pp::Var(line));
  usleep(400 * 1000);  // mesajlar tarayiciya ulassin
  _exit(86);
}

}  // namespace

class ChiakiInstance : public pp::Instance, public Platform {
 public:
  // capture_mu_ kurucu govdesinde baslatilir.
  explicit ChiakiInstance(PP_Instance instance)
      : pp::Instance(instance),
        factory_(this),
        ts_factory_(this),
        video_(this),
        audio_(this),
        input_(this),
        server_(this, [this](const HttpRequest& req, HttpResponse* res) { HandleHttp(req, res); }),
        core_(NULL),
        last_slow_ms_(0),
        capturing_(false),
        was_streaming_(false),
        controller_dirty_(false),
        pad_owner_(0),
        sps_override_(false),
        skip_override_(false),
        last_tick_ms_(0),
        max_tick_gap_(0),
        pad_frame_ms_(0),
        max_pad_wait_(0),
        pad_updates_(0) {
    pthread_mutex_init(&capture_mu_, NULL);
  }

  virtual ~ChiakiInstance() { delete core_; }

  virtual bool Init(uint32_t, const char*[], const char*[]) {
    // BSD soketleri ve dosya cagrilari nacl_io uzerinden Pepper'a baglanir.
    g_instance = this;
    nacl_exception_set_handler(CrashHandler);
    nacl_io_init_ppapi(pp_instance(), pp::Module::Get()->get_browser_interface());
    umount("/");
    mount("", "/", "memfs", 0, "");
    // curl ve chiaki kok sertifikalari dosyadan okur: gomulu paketi bellek dosya sistemine yaz.
    FILE* f = fopen("/cacert.pem", "wb");
    if (f) {
      fwrite(g_cacert_pem, 1, g_cacert_pem_len, f);
      fclose(f);
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    chiaki_lib_init();

    core_ = new Core(this);
    video_.on_need_idr = [this]() { core_->RequestIdr(); };
    video_.on_fatal = [this](const std::string& message) { core_->ReportStreamError(message); };
    video_.on_log = [this](const std::string& line) { core_->Log(line); };
    dbgsink::StartWatchdog([](const std::string& line, void* user) { static_cast<Core*>(user)->Log(line); }, core_, 2500);
    WsHooks hooks;
    hooks.accept = [this](const HttpRequest& req) { return WsAccept(req); };
    hooks.open = [this](int id) { pads_[id] = PadClient(); };
    hooks.message = [this](int id, bool binary, const std::string& payload) { OnPadMessage(id, binary, payload); };
    hooks.close = [this](int id) { OnPadClose(id); };
    server_.SetWebSocket(hooks);
    server_.Start(kSetupPort);
    ScheduleTick(kTickIdleMs);
    PostMessage(pp::Var("{\"ev\":\"ready\"}"));
    return true;
  }

  virtual void DidChangeView(const pp::View& view) {
    pp::Rect rect = view.GetRect();
    video_.SetViewSize(rect.width(), rect.height());
  }

  // ---------------------------------------------------------------- JS -> yerel
  virtual void HandleMessage(const pp::Var& message) {
    if (!message.is_string() || !core_) return;
    json_object* msg = json_tokener_parse(message.AsString().c_str());
    if (!msg) return;
    std::string cmd = util::JStr(msg, "cmd");
    if (cmd == "init") {
      std::string ip = util::JStr(msg, "ip");
      chiaki_nacl_set_local_ip(ip.c_str());
      core_->Init(ip, kSetupPort, util::JStr(msg, "config"));
    } else if (cmd == "ip") {  // ag uygulama acildiktan sonra geldi ya da adres degisti
      std::string ip = util::JStr(msg, "ip");
      chiaki_nacl_set_local_ip(ip.c_str());
      core_->SetLocalIp(ip);
    } else if (cmd == "connect") {
      core_->Connect(util::JStr(msg, "duid"));
    } else if (cmd == "stop") {
      core_->Stop();
    } else if (cmd == "refresh") {
      core_->RefreshConsoles();
    } else if (cmd == "logout") {
      core_->Logout();
    } else if (cmd == "pin") {
      core_->SetPin(util::JStr(msg, "pin"));
    } else if (cmd == "settings") {
      core_->SetSettings(static_cast<int>(util::JInt(msg, "resolution")), static_cast<int>(util::JInt(msg, "fps")),
                         static_cast<int>(util::JInt(msg, "bitrate")));
      if (json_object_object_get_ex(msg, "lang", NULL)) core_->SetLang(util::JStr(msg, "lang"));
      if (json_object_object_get_ex(msg, "lowLatency", NULL)) core_->SetLowLatency(util::JBool(msg, "lowLatency"));
    } else if (cmd == "menu") {  // arayuz menuyu acti/kapatti: kol girdisini menuye ya da konsola yonlendir
      input_.SetCaptured(util::JBool(msg, "open"));
    } else if (cmd == "sleep") {
      core_->Sleep();
    } else if (cmd == "key") {
      input_.SetRemoteKey(util::JStr(msg, "key"), util::JBool(msg, "down"));
    }
    json_object_put(msg);
  }

  // ---------------------------------------------------------------- Platform (her is parcacigindan)
  virtual void PostState(const std::string& state_json) {
    PostMessage(pp::Var("{\"ev\":\"state\",\"state\":" + state_json + "}"));
  }
  virtual void PostPersist(const std::string& config_json) {
    PostMessage(pp::Var("{\"ev\":\"persist\",\"config\":" + config_json + "}"));
  }
  virtual void PostLog(const std::string& line) {
    json_object* o = json_object_new_object();
    util::JAdd(o, "ev", "log");
    util::JAdd(o, "line", line);
    PostMessage(pp::Var(util::JDump(o)));
  }
  virtual void StreamStart() {
    pp::Module::Get()->core()->CallOnMainThread(0, ts_factory_.NewCallback(&ChiakiInstance::OnStreamStart));
  }
  virtual void StreamStop() {
    pp::Module::Get()->core()->CallOnMainThread(0, ts_factory_.NewCallback(&ChiakiInstance::OnStreamStop));
  }
  virtual void VideoPush(const uint8_t* data, size_t size) {
    if (capturing_) {  // tanilama: ham H.264 birimlerini (4 bayt uzunluk + veri) bellekte biriktir
      util::Lock l(&capture_mu_);
      if (capture_.size() + size + 4 <= kMaxCapture) {
        const uint8_t len[4] = {static_cast<uint8_t>(size >> 24), static_cast<uint8_t>(size >> 16),
                                static_cast<uint8_t>(size >> 8), static_cast<uint8_t>(size)};
        capture_.append(reinterpret_cast<const char*>(len), 4);
        capture_.append(reinterpret_cast<const char*>(data), size);
      }
    }
    video_.Push(data, size);
  }
  virtual void VideoWaitIdr() { video_.WaitIdr(); }
  virtual void AudioPush(const int16_t* pcm, size_t frames, int channels) { audio_.Push(pcm, frames, channels); }

 private:
  void OnStreamStart(int32_t) {
    dbgsink::MainStep("akis:baslat");
    if (!sps_override_) {  // dusuk gecikme: cozucuye tek referans kare bildir
      sps::Patch patch;
      if (core_->low_latency()) patch.max_refs = patch.max_dpb = 1;
      video_.SetSpsPatch(patch);
    }
    input_.Reset();
    video_.Start();
    dbgsink::MainStep("ses:baslat");
    audio_.Start();
    dbgsink::MainStep("akis:basladi");
  }
  void OnStreamStop(int32_t) {
    dbgsink::MainStep("akis:durdur");
    input_.SetCaptured(false);
    video_.Stop();
    dbgsink::MainStep("ses:durdur");
    audio_.Stop();
    input_.Reset();
    dbgsink::MainStep("akis:durdu");
  }

  void ScheduleTick(int delay_ms) {
    pp::Module::Get()->core()->CallOnMainThread(delay_ms, factory_.NewCallback(&ChiakiInstance::Tick));
  }

  void Tick(int32_t) {
    dbgsink::MainStep("tick");
    {
      int64_t tick_ms = util::NowMs();
      if (last_tick_ms_ && tick_ms - last_tick_ms_ > max_tick_gap_) max_tick_gap_ = tick_ms - last_tick_ms_;
      last_tick_ms_ = tick_ms;
    }
    bool streaming = core_->streaming();
    ChiakiControllerState state;
    bool changed = input_.Sample(&state);
    if (streaming) {
      // Yayin menusu: L1+R1+L3+R3 ac/kapat; menu acikken kol tuslari menuyu gezdirir.
      if (input_.TakeMenuToggle()) PostMessage(pp::Var("{\"ev\":\"menu\"}"));
      std::vector<std::string> nav = input_.TakeNav();
      for (size_t i = 0; i < nav.size(); i++) PostMessage(pp::Var("{\"ev\":\"nav\",\"key\":\"" + nav[i] + "\"}"));
    }
    if (!streaming) controller_dirty_ = false;
    else if (changed || !was_streaming_ || controller_dirty_) {
      controller_dirty_ = !core_->SetController(state);
      if (!controller_dirty_ && pad_frame_ms_) {  // ag kolundan gelen son cercevenin cekirdege ulasma suresi
        int64_t wait = util::NowMs() - pad_frame_ms_;
        if (wait > max_pad_wait_) max_pad_wait_ = wait;
        pad_updates_++;
      }
      if (!controller_dirty_) pad_frame_ms_ = 0;
    }
    was_streaming_ = streaming;

    int64_t now = util::NowMs();
    if (now - last_slow_ms_ >= 1000) {
      last_slow_ms_ = now;
      int shown = 0, dropped = 0;
      video_.TakeStats(&shown, &dropped);
      core_->SetStreamStats(shown, dropped, input_.gamepad_name());
      if (streaming) {
        VideoRenderer::Timing tm = video_.TakeTiming();
        double mbps = 0;
        int lost = 0, rtt = 0;
        core_->TakeNetStats(&mbps, &lost, &rtt);
        char line[320];
        snprintf(line, sizeof(line),
                 "{\"ev\":\"stats\",\"fps\":%d,\"dropped\":%d,\"decoded\":%d,\"decodeMs\":%d,\"screenMs\":%d,"
                 "\"mbps\":%.1f,\"lost\":%d,\"rtt\":%d,\"w\":%d,\"h\":%d}",
                 shown, dropped, tm.pictures, tm.decode_avg, tm.present_avg, mbps, lost, rtt, tm.width, tm.height);
        PostMessage(pp::Var(line));
#ifdef CHIAKI_TV_DEBUG  // saniyelik olcum satirlari yalnizca tanilama surumunde gunluge yazilir
        snprintf(line, sizeof(line), "> video: %d kare gosterildi, %d atildi (son 1 sn), %.1f Mbit/sn, kayip %d", shown,
                 dropped, mbps, lost);
        core_->Log(line);
        snprintf(line, sizeof(line),
                 "> zaman: cozucuden %d resim/sn (+%d bos) | kuyruk ort %d en cok %d ms (derinlik %d) | cozucu ort %d en cok "
                 "%d ms | takas ort %d en cok %d ms | agdan ekrana ort %d en cok %d ms",
                 tm.pictures, tm.skipped, tm.queue_avg, tm.queue_max, tm.depth, tm.decode_avg, tm.decode_max, tm.swap_avg,
                 tm.swap_max, tm.present_avg, tm.present_max);
        core_->Log(line);
        char more[160];
        snprintf(more, sizeof(more), "> ana dongu: en uzun tick araligi %d ms | kol: %d guncelleme, cekirdege en gec %d ms",
                 static_cast<int>(max_tick_gap_), pad_updates_, static_cast<int>(max_pad_wait_));
        core_->Log(more);
#endif
        max_tick_gap_ = 0;
        max_pad_wait_ = 0;
        pad_updates_ = 0;
      }
      server_.Tick();
      // Kimligini kanitlamayan kol baglantilari yer tutmasin.
      std::vector<int> drop;
      for (std::map<int, PadClient>::iterator it = pads_.begin(); it != pads_.end(); ++it)
        if (!it->second.authed && now - it->second.opened_ms > 5000) drop.push_back(it->first);
      for (size_t i = 0; i < drop.size(); i++) server_.WsClose(drop[i]);
    }
    ScheduleTick(streaming ? kTickStreamingMs : kTickIdleMs);
  }

  // ---------------------------------------------------------------- telefon kurulum sayfasi
  // Istek gercekten bu televizyona mi yapilmis? Baska bir alan adini bu adrese cozduren bir web
  // sayfasinin (DNS yeniden baglama) tarayici uzerinden API'ye erismesini engeller.
  bool HostAllowed(const HttpRequest& req) {
    std::map<std::string, std::string>::const_iterator it = req.headers.find("host");
    if (it == req.headers.end()) return false;
    const std::string& host = it->second;
    std::string port = ":" + util::Num(kSetupPort);
    std::string ip = core_->local_ip();
    return (!ip.empty() && host == ip + port) || host == "localhost" + port || host == "127.0.0.1" + port;
  }

  // ---------------------------------------------------------------- tarayicidan oyun kolu (WebSocket)
  struct PadClient {
    bool authed;
    int64_t opened_ms;
    PadClient() : authed(false), opened_ms(util::NowMs()) {}
  };

  // Yalnizca bu televizyonun adresinden yuklenen sayfa baglanabilir: Origin, Host ile ayni olmali.
  // (Tarayici disi istemciler Origin gondermez; onlar da ilk mesajda belirtec vermek zorunda.)
  bool WsAccept(const HttpRequest& req) {
    if (req.path != "/ws/pad" || !HostAllowed(req)) return false;
    std::map<std::string, std::string>::const_iterator origin = req.headers.find("origin");
    if (origin != req.headers.end() && origin->second != "http://" + req.headers.find("host")->second) return false;
    // Yuvalar doluysa kimligini kanitlamamis en eski baglantiyi at: agdaki biri bos baglantilarla
    // gercek kolun girmesini engelleyemesin.
    if (pads_.size() >= 3) {
      int oldest = 0;
      int64_t oldest_ms = 0;
      for (std::map<int, PadClient>::iterator it = pads_.begin(); it != pads_.end(); ++it)
        if (!it->second.authed && (!oldest || it->second.opened_ms < oldest_ms)) {
          oldest = it->first;
          oldest_ms = it->second.opened_ms;
        }
      if (oldest) server_.WsClose(oldest);
    }
    return true;
  }

  std::string PadStatus(const char* type) {
    json_object* o = json_object_new_object();
    util::JAdd(o, "t", type);
    util::JAddBool(o, "streaming", core_->streaming());
    return util::JDump(o);
  }

  void OnPadMessage(int id, bool binary, const std::string& payload) {
    std::map<int, PadClient>::iterator it = pads_.find(id);
    if (it == pads_.end()) return;
    if (!it->second.authed) {
      // Ilk mesaj: {"t":"auth","token":"...","name":"..."}
      json_object* msg = binary ? NULL : json_tokener_parse(payload.c_str());
      bool ok = msg && util::JStr(msg, "t") == "auth" && core_->CheckToken(util::JStr(msg, "token"));
      std::string name = msg ? util::JStr(msg, "name") : "";
      if (msg) json_object_put(msg);
      if (!ok) {
        server_.WsSend(id, "{\"t\":\"denied\"}");
        server_.WsCloseAfterSend(id);
        return;
      }
      it->second.authed = true;
      if (pad_owner_ && pad_owner_ != id) {  // son baglanan kol gecerli olur
        // Eski kol artik sahip degil (cerceveleri yok sayilir); "replaced" mesaji gidince kapanir.
        int old = pad_owner_;
        pad_owner_ = 0;
        server_.WsSend(old, "{\"t\":\"replaced\"}");
        server_.WsCloseAfterSend(old);
      }
      pad_owner_ = id;
      input_.ClearNetPad();
      input_.SetNetPadName(name.size() > 48 ? name.substr(0, 48) : name);
      server_.WsSend(id, PadStatus("ok"));
      core_->Log(L("> tarayıcı kolu bağlandı: ") + (name.empty() ? std::string(L("adsız")) : name));
      return;
    }
    if (!binary) {  // {"t":"ping","n":sayi} -> gecikme olcumu + yayin durumu; {"t":"name",...} -> kol adi
      json_object* msg = json_tokener_parse(payload.c_str());
      if (!msg) return;
      std::string type = util::JStr(msg, "t");
      if (type == "ping") {
        json_object* o = json_object_new_object();
        util::JAdd(o, "t", "pong");
        util::JAddInt(o, "n", util::JInt(msg, "n"));
        util::JAddBool(o, "streaming", core_->streaming());
        server_.WsSend(id, util::JDump(o));
      } else if (type == "menu" && id == pad_owner_) {  // kol sayfasindaki "Menu" dugmesi
        if (core_->streaming()) PostMessage(pp::Var("{\"ev\":\"menu\"}"));
      } else if (type == "name" && id == pad_owner_) {
        std::string name = util::JStr(msg, "name");
        input_.SetNetPadName(name.size() > 48 ? name.substr(0, 48) : name);
      }
      json_object_put(msg);
      return;
    }
    // Durum cercevesi: [1][dugmeler u32][l2][r2][lx i16][ly i16][rx i16][ry i16], kucuk uclu.
    if (id != pad_owner_ || payload.size() != 15 || payload[0] != 1) return;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(payload.data());
    uint32_t buttons = p[1] | (p[2] << 8) | (p[3] << 16) | (static_cast<uint32_t>(p[4]) << 24);
    if (!pad_frame_ms_) pad_frame_ms_ = util::NowMs();
    input_.SetNetPad(buttons, p[5], p[6], static_cast<int16_t>(p[7] | (p[8] << 8)), static_cast<int16_t>(p[9] | (p[10] << 8)),
                     static_cast<int16_t>(p[11] | (p[12] << 8)), static_cast<int16_t>(p[13] | (p[14] << 8)));
  }

  void OnPadClose(int id) {
    pads_.erase(id);
    if (id == pad_owner_) {
      pad_owner_ = 0;
      input_.ClearNetPad();
      core_->Log(L("> tarayıcı kolu ayrıldı"));
    }
  }

  void HandleHttp(const HttpRequest& req, HttpResponse* res) {
    if (!HostAllowed(req)) {
      res->status = 403;
      res->body = JsonError(L("Bu adres üzerinden erişime izin yok."));
      return;
    }
    if (req.method == "GET" && (req.path == "/" || req.path == "/index.html")) {
      res->type = "text/html; charset=utf-8";
      res->body.assign(reinterpret_cast<const char*>(g_setup_html), g_setup_html_len);
      return;
    }
    if (req.method == "GET" && req.path == "/pad") {
      res->type = "text/html; charset=utf-8";
      res->body.assign(reinterpret_cast<const char*>(g_pad_html), g_pad_html_len);
      return;
    }
    if (req.path.compare(0, 5, "/api/") != 0) {
      res->status = 404;
      res->body = JsonError(L("Bulunamadı."));
      return;
    }
    json_object* body = req.body.empty() ? NULL : json_tokener_parse(req.body.c_str());
    HandleApi(req, body, res);
    if (body) json_object_put(body);
  }

  void HandleApi(const HttpRequest& req, json_object* body, HttpResponse* res) {
    const std::string& path = req.path;
    bool post = req.method == "POST";
    if (path == "/api/pair" && post) {
      std::string token;
      int retry_after = 0;
      Core::PairResult result = core_->Pair(util::JStr(body, "code"), &token, &retry_after);
      if (result == Core::kPairLocked) {
        res->status = 429;
        res->body = JsonError(L("Çok fazla hatalı deneme. ") + util::Num(retry_after) + L(" saniye sonra yeniden dene."));
        return;
      }
      if (result != Core::kPairOk) {
        res->status = 401;
        res->body = JsonError(L("Kod uyuşmadı. Televizyondaki kodu kontrol et."));
        return;
      }
      json_object* o = json_object_new_object();
      util::JAddBool(o, "ok", true);
      util::JAdd(o, "token", token);
      res->body = util::JDump(o);
      return;
    }
    if (path == "/api/lang" && !post) {  // sayfalar eslesmeden once de dogru dilde acilsin
      json_object* o = json_object_new_object();
      util::JAdd(o, "lang", i18n::Lang());
      res->body = util::JDump(o);
      return;
    }
    // Bundan sonraki her uc eslestirme ister.
    std::map<std::string, std::string>::const_iterator it = req.headers.find("x-token");
    if (it == req.headers.end() || !core_->CheckToken(it->second)) {
      res->status = 401;
      res->body = JsonError(L("Eşleştirme gerekli."));
      return;
    }
    if (path == "/api/state" && !post) {
      res->body = core_->StateJson(false);
    } else if (path == "/api/log" && !post) {
      res->body = core_->LogJson();
    } else if (path == "/api/psn/begin" && post) {
      json_object* o = json_object_new_object();
      util::JAddBool(o, "ok", true);
      util::JAdd(o, "url", core_->LoginUrl());
      res->body = util::JDump(o);
    } else if (path == "/api/psn/finish" && post) {
      std::string redirect = util::JStr(body, "redirect");
      if (redirect.empty()) {
        res->status = 400;
        res->body = JsonError(L("Yönlendirme adresi boş."));
        return;
      }
      core_->PsnFinish(redirect);
      res->body = kOk;
    } else if (path == "/api/consoles/refresh" && post) {
      core_->RefreshConsoles();
      res->body = kOk;
    } else if (path == "/api/connect" && post) {
      core_->Connect(util::JStr(body, "duid"));
      res->body = kOk;
    } else if (path == "/api/stop" && post) {
      core_->Stop();
      res->body = kOk;
    } else if (path == "/api/pin" && post) {
      core_->SetPin(util::JStr(body, "pin"));
      res->body = kOk;
    } else if (path == "/api/settings" && post) {
      core_->SetSettings(static_cast<int>(util::JInt(body, "resolution")), static_cast<int>(util::JInt(body, "fps")),
                         static_cast<int>(util::JInt(body, "bitrate")));
      if (body && json_object_object_get_ex(body, "lang", NULL)) core_->SetLang(util::JStr(body, "lang"));
      if (body && json_object_object_get_ex(body, "lowLatency", NULL)) core_->SetLowLatency(util::JBool(body, "lowLatency"));
      res->body = kOk;
#ifdef CHIAKI_TV_DEBUG
      // Tanilama uclari yalnizca CHIAKI_TV_DEBUG=1 ile derlenen surumde vardir: ayrintili gunluk oturum verisi
      // icerebilir, "sink" gunlugu agdaki baska bir adrese yollar, "capture"/"shot" goruntuyu disari verir.
    } else if (path == "/api/debug" && post) {
      if (body && json_object_object_get_ex(body, "verbose", NULL)) core_->SetVerbose(util::JBool(body, "verbose"));
      if (body && json_object_object_get_ex(body, "sink", NULL)) dbgsink::SetTarget(util::JStr(body, "sink"));
      if (body && json_object_object_get_ex(body, "sps", NULL)) {  // "refs,dpb,level" (0 = dokunma)
        sps::Patch patch;
        sscanf(util::JStr(body, "sps").c_str(), "%d,%d,%d", &patch.max_refs, &patch.max_dpb, &patch.level);
        video_.SetSpsPatch(patch);
        sps_override_ = true;
        core_->Log(L("> SPS yaması: refs=") + util::Num(patch.max_refs) + L(" dpb=") + util::Num(patch.max_dpb) + L(" level=") +
                   util::Num(patch.level));
      }
      if (body && json_object_object_get_ex(body, "skip", NULL)) {  // bos kare ekleme denemesi
        video_.SetSkipFrames(util::JBool(body, "skip"));
        skip_override_ = true;
        core_->Log(std::string("> bos kare ekleme: ") + (util::JBool(body, "skip") ? "acik" : "kapali"));
      }
      if (body && json_object_object_get_ex(body, "shot", NULL)) video_.RequestShot();  // bir sonraki kareyi sakla
      if (body && json_object_object_get_ex(body, "capture", NULL)) {
        util::Lock l(&capture_mu_);
        capture_.clear();
        capturing_ = util::JBool(body, "capture");
      }
      res->body = kOk;
    } else if (path == "/api/dump" && post) {  // yakalanan yayindan bir dilim (ikili)
      size_t offset = static_cast<size_t>(util::JInt(body, "offset", 0));
      size_t length = static_cast<size_t>(util::JInt(body, "length", 256 * 1024));
      if (length > 512 * 1024) length = 512 * 1024;
      if (util::JStr(body, "what") == "shot") {  // saklanan kare: 8 baytlik boyut basligi + RGBA
        if (offset == 0) {
          std::string pixels;
          int w = 0, h = 0;
          if (video_.TakeShot(&pixels, &w, &h)) {
            char head[9];
            snprintf(head, sizeof(head), "%04d%04d", w, h);
            shot_blob_ = std::string(head, 8) + pixels;
          } else {
            shot_blob_.clear();
          }
        }
        res->type = "application/octet-stream";
        if (offset < shot_blob_.size()) res->body = shot_blob_.substr(offset, length);
        return;
      }
      util::Lock l(&capture_mu_);
      res->type = "application/octet-stream";
      if (offset < capture_.size()) res->body = capture_.substr(offset, length);
      return;
#endif  // CHIAKI_TV_DEBUG
    } else if (path == "/api/logout" && post) {
      core_->Logout();
      res->body = kOk;
    } else {
      res->status = 404;
      res->body = JsonError(L("Bulunamadı."));
    }
  }

  pp::CompletionCallbackFactory<ChiakiInstance> factory_;
  pp::CompletionCallbackFactory<ChiakiInstance, pp::ThreadSafeThreadTraits> ts_factory_;
  VideoRenderer video_;
  AudioOut audio_;
  Input input_;
  SetupServer server_;
  Core* core_;
  int64_t last_slow_ms_;
  bool was_streaming_;
  static const size_t kMaxCapture = 12 * 1024 * 1024;
  volatile bool capturing_;
  pthread_mutex_t capture_mu_;
  std::string capture_;
  bool controller_dirty_;  // son durum cekirdege iletilemedi
  std::map<int, PadClient> pads_;  // acik kol baglantilari (ana is parcacigi)
  int pad_owner_;                  // girdisi gecerli sayilan baglanti (0 = yok)
  bool sps_override_;              // tanilama: SPS yamasi /api/debug ile elle verildi
  bool skip_override_;             // tanilama: bos kare ekleme /api/debug ile elle verildi
  std::string shot_blob_;          // tanilama: indirilmekte olan kare
  int64_t last_tick_ms_, max_tick_gap_;  // tanilama: ana dongunun tepki suresi
  int64_t pad_frame_ms_, max_pad_wait_;  // tanilama: ag kolu cercevesi -> cekirdek
  int pad_updates_;
};

class ChiakiModule : public pp::Module {
 public:
  virtual pp::Instance* CreateInstance(PP_Instance instance) { return new ChiakiInstance(instance); }
};

namespace pp {
Module* CreateModule() { return new ChiakiModule(); }
}  // namespace pp
