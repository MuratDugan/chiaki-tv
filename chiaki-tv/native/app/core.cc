#include "core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <chiaki/base64.h>
#include <chiaki/common.h>

#include "dbgsink.h"
#include "psn.h"
#include "i18n.h"
#include "util.h"

using util::Lock;

// patch_chiaki.py'nin thread.c'ye ekledigi sayac: kilitli olmayan bir kilidi birakma girisimleri.
extern "C" volatile int chiaki_nacl_bad_unlocks;

namespace {

const size_t kMaxLogLines = 300;
const int kMaxPairFails = 5;
const int kPairLockBaseMs = 30 * 1000;  // ilk kilit 30 sn; her seferinde ikiye katlanir
const int kPairLockMaxLevel = 5;        // en fazla 16 dk
const size_t kMaxTokens = 16;

// Cikis nedenini kullaniciya gosterilecek metne cevirir.
std::string QuitText(ChiakiQuitReason reason, const std::string& detail) {
  std::string base;
  switch (reason) {
    case CHIAKI_QUIT_REASON_SESSION_REQUEST_CONNECTION_REFUSED:
      base = L("Konsol bağlantıyı reddetti. Konsol açık ve Uzaktan Oynama etkin mi?");
      break;
    case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE:
      base = L("Uzaktan Oynama başka bir cihazda kullanımda.");
      break;
    case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_CRASH:
      base = L("Konsoldaki Uzaktan Oynama hizmeti çöktü; konsolu yeniden başlatmak gerekebilir.");
      break;
    case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_VERSION_MISMATCH:
      base = L("Konsol yazılım sürümü bu istemciyle uyuşmuyor.");
      break;
    case CHIAKI_QUIT_REASON_CTRL_CONNECT_FAILED:
    case CHIAKI_QUIT_REASON_CTRL_CONNECTION_REFUSED:
      base = L("Konsolla kontrol bağlantısı kurulamadı.");
      break;
    case CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_DISCONNECTED:
      base = L("Konsol bağlantıyı kesti.");
      break;
    case CHIAKI_QUIT_REASON_PSN_REGIST_FAILED:
      base = L("PSN üzerinden konsol kaydı başarısız. Konsol bu hesaba bağlı ve Uzaktan Oynama açık mı?");
      break;
    default:
      base = std::string(L("Bağlantı sonlandı: ")) + chiaki_quit_reason_string(reason);
      break;
  }
  if (!detail.empty()) base += " (" + detail + ")";
  return base;
}

json_object* ConsolesJson(const std::vector<Console>& consoles) {
  json_object* list = json_object_new_array();
  for (size_t i = 0; i < consoles.size(); i++) {
    json_object* item = json_object_new_object();
    util::JAdd(item, "duid", consoles[i].duid);
    util::JAdd(item, "name", consoles[i].name);
    util::JAddBool(item, "remoteplay", consoles[i].remoteplay);
    json_object_array_add(list, item);
  }
  return list;
}

}  // namespace

Core::Core(Platform* platform)
    : platform_(platform),
      quit_(false),
      booted_(false),
      busy_(false),
      connecting_(false),
      streaming_(false),
      setup_port_(0),
      pair_fails_(0),
      pair_lock_level_(0),
      pair_locked_until_ms_(0),
      expiry_(0),
      resolution_(720),
      fps_(60),
      bitrate_kbps_(0),
      low_latency_(true),
      lang_("en"),
      check_refs_(false),
      ref_skips_(0),
      bytes_window_(0),
      frames_lost_(0),
      rtt_ms_(0),
      net_stats_ms_(0),
      session_active_(false),
      stop_requested_(false),
      pending_hs_(NULL),
      need_pin_(false),
      pin_incorrect_(false),
      last_idr_ms_(0),
      audio_channels_(2),
      stat_fps_(0),
      stat_dropped_(0),
      bad_unlocks_seen_(0) {
  pthread_mutex_init(&mu_, NULL);
  pthread_mutex_init(&session_mu_, NULL);
  pthread_mutex_init(&notify_mu_, NULL);
  pthread_mutex_init(&log_mu_, NULL);
  pthread_mutex_init(&queue_mu_, NULL);
  pthread_cond_init(&queue_cv_, NULL);
  pair_code_ = util::RandomDigits(4);
  // Yalnizca bilgi, uyari ve hata: ayrintili duzeyler belirtec iceren adresleri gunluge yazabilir.
  chiaki_log_init(&log_, CHIAKI_LOG_INFO | CHIAKI_LOG_WARNING | CHIAKI_LOG_ERROR, LogCb, this);
  psn::SetLogger([](const std::string& line, void* user) { static_cast<Core*>(user)->Log(line); }, this);
  pthread_create(&worker_, NULL, WorkerMain, this);
}

Core::~Core() {
  {
    Lock l(&queue_mu_);
    quit_ = true;
    pthread_cond_signal(&queue_cv_);
  }
  pthread_join(worker_, NULL);
  pthread_cond_destroy(&queue_cv_);
  pthread_mutex_destroy(&queue_mu_);
  pthread_mutex_destroy(&log_mu_);
  pthread_mutex_destroy(&notify_mu_);
  pthread_mutex_destroy(&session_mu_);
  pthread_mutex_destroy(&mu_);
}

// ------------------------------------------------------------------ is kuyrugu
void* Core::WorkerMain(void* self_ptr) {
  Core* self = static_cast<Core*>(self_ptr);
  util::CrashStackForThisThread();
  util::SetThreadTag("worker");
  for (;;) {
    Job job;
    {
      Lock l(&self->queue_mu_);
      while (self->queue_.empty() && !self->quit_) pthread_cond_wait(&self->queue_cv_, &self->queue_mu_);
      if (self->quit_) return NULL;
      job = self->queue_.front();
      self->queue_.pop_front();
    }
    job();
  }
}

void Core::Enqueue(const Job& job) {
  Lock l(&queue_mu_);
  queue_.push_back(job);
  pthread_cond_signal(&queue_cv_);
}

// ------------------------------------------------------------------ gunluk
void Core::LogCb(ChiakiLogLevel level, const char* msg, void* user) {
  char prefix[4] = {'[', chiaki_log_level_char(level), ']', 0};
  static_cast<Core*>(user)->Log(std::string(prefix) + " " + (msg ? msg : ""));
}

// log_mu_ baska hicbir kilit tutulurken alinmaz ve altinda baska kilit alinmaz: kutuphane gunluk
// yazarken kendi kilitlerini tutuyor olabilir.
void Core::Log(const std::string& line) {
  static const int64_t start_ms = util::NowMs();
  char stamp[24];
  snprintf(stamp, sizeof(stamp), "%7.2f ", (util::NowMs() - start_ms) / 1000.0);
  std::string cut = stamp + (line.size() > 400 ? line.substr(0, 400) : line);
  {
    Lock l(&log_mu_);
    log_lines_.push_back(cut);
    while (log_lines_.size() > kMaxLogLines) log_lines_.pop_front();
  }
  dbgsink::Send(cut);
  platform_->PostLog(cut);
}

void Core::SetVerbose(bool on) {
  log_.level_mask = on ? CHIAKI_LOG_ALL : (CHIAKI_LOG_INFO | CHIAKI_LOG_WARNING | CHIAKI_LOG_ERROR);
  Log(on ? L("> ayrıntılı günlük açık") : L("> ayrıntılı günlük kapalı"));
}

std::string Core::LogJson() {
  json_object* arr = json_object_new_array();
  {
    Lock l(&log_mu_);
    for (size_t i = 0; i < log_lines_.size(); i++) {
      std::string clean = util::Utf8Clean(log_lines_[i]);
      json_object_array_add(arr, json_object_new_string_len(clean.data(), static_cast<int>(clean.size())));
    }
  }
  json_object* root = json_object_new_object();
  json_object_object_add(root, "lines", arr);
  return util::JDump(root);
}

// ------------------------------------------------------------------ durum
void Core::Init(const std::string& local_ip, int setup_port, const std::string& config_json) {
  json_object* cfg = config_json.empty() ? NULL : json_tokener_parse(config_json.c_str());
  bool need_duid, logged_in;
  {
    Lock l(&mu_);
    local_ip_ = local_ip;
    setup_port_ = setup_port;
    if (cfg && !booted_) {  // yeniden init (sayfa yenilemesi) bellekteki guncel oturumu ezmesin
      access_ = util::JStr(cfg, "access");
      refresh_ = util::JStr(cfg, "refresh");
      expiry_ = util::JInt(cfg, "expiry", 0);
      account_id_b64_ = util::JStr(cfg, "accountId");
      client_duid_ = util::JStr(cfg, "clientDuid");
      last_duid_ = util::JStr(cfg, "lastDuid");
      resolution_ = static_cast<int>(util::JInt(cfg, "resolution", 720));
      fps_ = static_cast<int>(util::JInt(cfg, "fps", 60));
      bitrate_kbps_ = static_cast<int>(util::JInt(cfg, "bitrate", 0));
      low_latency_ = util::JBool(cfg, "lowLatency", true);
      lang_ = util::JStr(cfg, "lang") == "tr" ? "tr" : "en";
      i18n::SetLang(lang_);
      json_object* list = NULL;
      if (json_object_object_get_ex(cfg, "consoles", &list) && json_object_is_type(list, json_type_array)) {
        consoles_.clear();
        for (size_t i = 0; i < json_object_array_length(list); i++) {
          json_object* item = json_object_array_get_idx(list, i);
          Console c;
          c.duid = util::JStr(item, "duid");
          c.name = util::JStr(item, "name");
          c.remoteplay = util::JBool(item, "remoteplay", true);
          if (!c.duid.empty()) consoles_.push_back(c);
        }
      }
    }
    if (resolution_ != 540 && resolution_ != 720 && resolution_ != 1080) resolution_ = 720;
    if (fps_ != 30 && fps_ != 60) fps_ = 60;
    booted_ = true;
    need_duid = client_duid_.empty();
    logged_in = !refresh_.empty();
  }
  if (cfg) json_object_put(cfg);
  if (need_duid) {
    char duid[CHIAKI_DUID_STR_SIZE];
    size_t size = sizeof(duid);
    memset(duid, 0, sizeof(duid));
    if (chiaki_holepunch_generate_client_device_uid(duid, &size) == CHIAKI_ERR_SUCCESS) {
      Lock l(&mu_);
      client_duid_ = duid;
    }
    Persist();
  }
  Notify();
  if (logged_in) RefreshConsoles();  // oturum varsa konsol listesini tazele
}

void Core::SetLocalIp(const std::string& local_ip) {
  {
    Lock l(&mu_);
    if (local_ip_ == local_ip) return;
    local_ip_ = local_ip;
  }
  Notify();
}

std::string Core::local_ip() {
  Lock l(&mu_);
  return local_ip_;
}

std::string Core::StateJson(bool for_tv) {
  json_object* root = json_object_new_object();
  Lock l(&mu_);
  const char* phase = streaming_ ? "streaming" : (connecting_ ? "connecting" : (busy_ ? "busy" : (booted_ ? "idle" : "boot")));
  util::JAdd(root, "phase", phase);
  util::JAdd(root, "message", message_);
  util::JAdd(root, "error", error_);
  util::JAdd(root, "ip", local_ip_);
  util::JAddInt(root, "port", setup_port_);
  if (for_tv) util::JAdd(root, "code", pair_code_);
  util::JAddBool(root, "loggedIn", !refresh_.empty());
  util::JAddBool(root, "needPin", need_pin_);
  util::JAddBool(root, "pinIncorrect", pin_incorrect_);
  util::JAdd(root, "lastDuid", last_duid_);
  util::JAdd(root, "lang", lang_);
  util::JAdd(root, "gamepad", gamepad_);
  json_object_object_add(root, "consoles", ConsolesJson(consoles_));
  json_object* settings = json_object_new_object();
  util::JAddInt(settings, "resolution", resolution_);
  util::JAddInt(settings, "fps", fps_);
  util::JAddInt(settings, "bitrate", bitrate_kbps_);
  util::JAddBool(settings, "lowLatency", low_latency_);
  json_object_object_add(root, "settings", settings);
  json_object* stream = json_object_new_object();
  util::JAddInt(stream, "fps", stat_fps_);
  util::JAddInt(stream, "dropped", stat_dropped_);
  json_object_object_add(root, "stream", stream);
  return util::JDump(root);
}

// Anlik goruntu alma ile gonderme tek kilit altinda: farkli is parcaciklarindan gelen durumlar
// TV arayuzune eski-yeni sirasi bozulmadan ulasir (yoksa bayat bir "baglaniyor" son soz olabilir).
void Core::Notify() {
  Lock n(&notify_mu_);
  platform_->PostState(StateJson(true));
}

void Core::Persist() {
  Lock n(&notify_mu_);
  json_object* root = json_object_new_object();
  {
    Lock l(&mu_);
    util::JAdd(root, "access", access_);
    util::JAdd(root, "refresh", refresh_);
    util::JAddInt(root, "expiry", expiry_);
    util::JAdd(root, "accountId", account_id_b64_);
    util::JAdd(root, "clientDuid", client_duid_);
    util::JAdd(root, "lastDuid", last_duid_);
    util::JAddInt(root, "resolution", resolution_);
    util::JAddInt(root, "fps", fps_);
    util::JAddInt(root, "bitrate", bitrate_kbps_);
    util::JAddBool(root, "lowLatency", low_latency_);
    util::JAdd(root, "lang", lang_);
    json_object_object_add(root, "consoles", ConsolesJson(consoles_));
  }
  platform_->PostPersist(util::JDump(root));
}

void Core::BeginJob(const std::string& message) {
  {
    Lock l(&mu_);
    busy_ = true;
    if (!connecting_ && !streaming_) {  // baglanti akisi kendi mesajini yonetir
      message_ = message;
      error_.clear();
    }
  }
  Log("> " + message);
  Notify();
}

void Core::EndJob(const std::string& error) {
  {
    Lock l(&mu_);
    busy_ = false;
    if (!connecting_ && !streaming_) message_.clear();
    if (!error.empty()) error_ = error;
  }
  if (!error.empty()) Log("! " + error);
  Notify();
}

void Core::ConnectStep(const std::string& message) {
  {
    Lock l(&mu_);
    message_ = message;
  }
  Log("> " + message);
  Notify();
}

void Core::ConnectFailed(const std::string& error) {
  {
    Lock l(&mu_);
    connecting_ = false;
    need_pin_ = false;
    message_.clear();
    error_ = error;
  }
  if (!error.empty()) Log("! " + error);
  Notify();
}

bool Core::streaming() {
  Lock l(&mu_);
  return streaming_;
}

// ------------------------------------------------------------------ eslestirme
Core::PairResult Core::Pair(const std::string& code, std::string* token, int* retry_after_s) {
  PairResult result = kPairWrong;
  bool changed = false;
  {
    Lock l(&mu_);
    int64_t now = util::NowMs();
    if (now < pair_locked_until_ms_) {
      *retry_after_s = static_cast<int>((pair_locked_until_ms_ - now + 999) / 1000);
      return kPairLocked;
    }
    if (!code.empty() && code == pair_code_) {
      *token = util::RandomHex(16);
      if (tokens_.size() >= kMaxTokens) tokens_.clear();  // eski telefonlar yeniden eslesir
      tokens_.insert(*token);
      pair_fails_ = 0;
      pair_lock_level_ = 0;
      result = kPairOk;
    } else if (++pair_fails_ >= kMaxPairFails) {
      // Deneme yanilmaya karsi: kodu degistir ve katlanarak uzayan bir sure boyunca denemeyi kapat.
      pair_code_ = util::RandomDigits(4);
      pair_fails_ = 0;
      pair_locked_until_ms_ = now + (static_cast<int64_t>(kPairLockBaseMs) << pair_lock_level_);
      if (pair_lock_level_ < kPairLockMaxLevel) pair_lock_level_++;
      changed = true;
    }
  }
  if (changed) Notify();
  return result;
}

bool Core::CheckToken(const std::string& token) {
  Lock l(&mu_);
  return !token.empty() && tokens_.count(token) > 0;
}

// ------------------------------------------------------------------ PSN
std::string Core::LoginUrl() {
  Lock l(&mu_);
  return psn::LoginUrl(client_duid_);
}

void Core::PsnFinish(const std::string& redirect_or_code) {
  {
    // Tek is kuyrugu var: yayin sirasinda PSN'e giden yavas bir is anahtar kare isteklerini bekletir.
    Lock l(&mu_);
    if (connecting_ || streaming_) return;
  }
  std::string input = redirect_or_code;
  Enqueue([this, input]() { DoPsnFinish(input); });
}

void Core::DoPsnFinish(std::string input) {
  BeginJob(L("PSN oturumu açılıyor"));
  psn::Tokens tokens;
  std::string error, account;
  if (!psn::ExchangeCode(psn::ExtractCode(input), &tokens, &error) ||
      !psn::AccountId(tokens.access, &account, &error)) {
    EndJob(error);
    return;
  }
  {
    Lock l(&mu_);
    access_ = tokens.access;
    if (!tokens.refresh.empty()) refresh_ = tokens.refresh;
    expiry_ = tokens.expiry;
    account_id_b64_ = account;
  }
  Persist();
  ListConsoles(&error);
  EndJob(error);
}

// Erisim belirtecinin suresi dolmak uzereyse yeniler.
bool Core::EnsureToken(std::string* error) {
  std::string refresh;
  {
    Lock l(&mu_);
    if (refresh_.empty()) {
      *error = L("PSN oturumu açık değil. Telefondan kurulum sayfasını kullan.");
      return false;
    }
    if (!access_.empty() && util::NowSec() < expiry_ - 120) return true;
    refresh = refresh_;
  }
  psn::Tokens tokens;
  if (!psn::Refresh(refresh, &tokens, error)) return false;
  {
    Lock l(&mu_);
    access_ = tokens.access;
    if (!tokens.refresh.empty()) refresh_ = tokens.refresh;
    expiry_ = tokens.expiry;
  }
  Persist();
  return true;
}

void Core::RefreshConsoles() {
  {
    Lock l(&mu_);
    if (connecting_ || streaming_) return;  // PsnFinish ile ayni gerekce
  }
  Enqueue([this]() { DoRefreshConsoles(); });
}

void Core::DoRefreshConsoles() {
  BeginJob(L("Konsollar aranıyor"));
  std::string error;
  ListConsoles(&error);
  EndJob(error);
}

void Core::ListConsoles(std::string* error) {
  std::string access;
  if (!EnsureToken(error)) return;
  {
    Lock l(&mu_);
    access = access_;
  }
  ChiakiHolepunchDeviceInfo* devices = NULL;
  size_t count = 0;
  ChiakiErrorCode err = CHIAKI_ERR_UNKNOWN;
  for (int attempt = 0; attempt < 3 && err != CHIAKI_ERR_SUCCESS; attempt++) {
    devices = NULL;
    count = 0;
    err = chiaki_holepunch_list_devices(access.c_str(), CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS5, &devices, &count, &log_);
  }
  if (err != CHIAKI_ERR_SUCCESS) {
    *error = std::string(L("Konsol listesi alınamadı: ")) + chiaki_error_string(err);
    return;
  }
  std::vector<Console> found;
  for (size_t i = 0; devices && i < count; i++) {
    Console c;
    c.duid = util::Hex(devices[i].device_uid, sizeof(devices[i].device_uid));
    c.name = std::string(devices[i].device_name, strnlen(devices[i].device_name, sizeof(devices[i].device_name)));
    c.remoteplay = devices[i].remoteplay_enabled;
    found.push_back(c);
  }
  if (devices) chiaki_holepunch_free_device_list(&devices);
  {
    Lock l(&mu_);
    consoles_ = found;
  }
  Persist();
}

void Core::Logout() {
  Stop();  // suren baglanti ya da yayin varsa once onu kapat
  Enqueue([this]() {
    {
      Lock l(&mu_);
      access_.clear();
      refresh_.clear();
      account_id_b64_.clear();
      expiry_ = 0;
      consoles_.clear();
      last_duid_.clear();
      error_.clear();
    }
    Persist();
    Notify();
  });
}

void Core::SetSettings(int resolution, int fps, int bitrate_kbps) {
  {
    Lock l(&mu_);
    if (resolution == 540 || resolution == 720 || resolution == 1080) resolution_ = resolution;
    if (fps == 30 || fps == 60) fps_ = fps;
    if (bitrate_kbps == 0 || (bitrate_kbps >= 2000 && bitrate_kbps <= 30000)) bitrate_kbps_ = bitrate_kbps;
  }
  Persist();
  Notify();
}

void Core::SetLang(const std::string& lang) {
  std::string value = lang == "tr" ? "tr" : "en";
  {
    Lock l(&mu_);
    if (lang_ == value) return;
    lang_ = value;
    i18n::SetLang(value);
    message_.clear();  // eski dilde kalmis adim/hata metni gorunmesin
    error_.clear();
  }
  Persist();
  Notify();
}

void Core::SetLowLatency(bool on) {
  {
    Lock l(&mu_);
    if (low_latency_ == on) return;
    low_latency_ = on;
  }
  Persist();
  Notify();
}

bool Core::low_latency() {
  Lock l(&mu_);
  return low_latency_;
}

// ------------------------------------------------------------------ oturum
void Core::Connect(const std::string& duid) {
  {
    Lock l(&mu_);
    if (!booted_ || connecting_ || streaming_ || session_active_) return;
    connecting_ = true;
    stop_requested_ = false;
    stream_error_.clear();
    message_ = L("Bağlantı hazırlanıyor");
    error_.clear();
  }
  Notify();
  std::string id = duid;
  Enqueue([this, id]() { DoConnect(id); });
}

void Core::DoConnect(std::string duid) {
  std::string error, access, account_b64, name;
  int resolution, fps, bitrate;
  std::vector<uint8_t> duid_bytes;
  {
    Lock l(&mu_);
    if (session_active_) {  // olmamali: Connect() bunu engeller
      connecting_ = false;
      return;
    }
  }
  if (!util::ParseHex(duid, &duid_bytes) || duid_bytes.size() != 32) {
    ConnectFailed(L("Geçersiz konsol kimliği."));
    return;
  }
  ConnectStep(L("PSN oturumu doğrulanıyor"));
  if (!EnsureToken(&error)) {
    ConnectFailed(error);
    return;
  }
  {
    Lock l(&mu_);
    access = access_;
    account_b64 = account_id_b64_;
    resolution = resolution_;
    // Bu televizyonun donanim cozucusu 1080p'de saniyede ~37 kare cozebiliyor: 60 kare/sn akis kuyrugu
    // tasirir. 1080p secildiyse akis 30 kare/sn istenir.
    fps = resolution_ == 1080 ? 30 : fps_;
    bitrate = bitrate_kbps_;
    for (size_t i = 0; i < consoles_.size(); i++)
      if (consoles_[i].duid == duid) name = consoles_[i].name;
    last_duid_ = duid;
  }
  uint8_t account[CHIAKI_PSN_ACCOUNT_ID_SIZE];
  size_t account_size = sizeof(account);
  if (chiaki_base64_decode(account_b64.c_str(), account_b64.size(), account, &account_size) != CHIAKI_ERR_SUCCESS ||
      account_size != CHIAKI_PSN_ACCOUNT_ID_SIZE) {
    ConnectFailed(L("PSN hesap kimliği eksik. Telefondan yeniden giriş yap."));
    return;
  }

  // 1) PSN uzerinden kontrol baglantisi icin delik ac (chiaki-ng: StreamSession::ConnectPsnConnection).
  ChiakiHolepunchSession hs = chiaki_holepunch_session_init(access.c_str(), &log_);
  if (!hs) {
    ConnectFailed(L("Bağlantı oturumu oluşturulamadı."));
    return;
  }
  {
    Lock s(&session_mu_);
    Lock l(&mu_);
    pending_hs_ = hs;
  }
  struct Step {
    const char* text;
    std::function<ChiakiErrorCode()> run;
  };
  const uint8_t* uid = duid_bytes.data();
  Step steps[] = {
      {L("Ağ hazırlanıyor"), [hs]() { return chiaki_holepunch_upnp_discover(hs); }},
      {L("PSN oturumu oluşturuluyor"), [hs]() { return chiaki_holepunch_session_create(hs); }},
      {L("Bağlantı teklifi hazırlanıyor"), [hs]() { return holepunch_session_create_offer(hs); }},
      {L("Konsola ulaşılıyor"),
       [hs, uid]() { return chiaki_holepunch_session_start(hs, uid, CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS5); }},
      {L("Kontrol bağlantısı kuruluyor"),
       [hs]() { return chiaki_holepunch_session_punch_hole(hs, CHIAKI_HOLEPUNCH_PORT_TYPE_CTRL); }},
  };
  ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;
  const char* failed_at = "";
  for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
    bool stop;
    {
      Lock l(&mu_);
      stop = stop_requested_;
    }
    if (stop) {
      err = CHIAKI_ERR_CANCELED;
      break;
    }
    ConnectStep(steps[i].text);
    err = steps[i].run();
    if (err != CHIAKI_ERR_SUCCESS) {
      failed_at = steps[i].text;
      break;
    }
  }
  bool stop;
  {
    // session_mu_: Stop() tam bu sirada iptal cagiriyorsa bitmesini bekle, sonra isaretciyi birak.
    Lock s(&session_mu_);
    {
      Lock l(&mu_);
      pending_hs_ = NULL;
      stop = stop_requested_;
    }
    if (err != CHIAKI_ERR_SUCCESS) chiaki_holepunch_session_fini(hs);
  }
  if (err != CHIAKI_ERR_SUCCESS) {
    ConnectFailed(stop ? "" : std::string(failed_at) + L(" başarısız: ") + chiaki_error_string(err));
    return;
  }

  // 2) Oturumu baslat. Delik acma oturumunun sahipligi buradan sonra kutuphanededir.
  ChiakiConnectInfo info;
  memset(&info, 0, sizeof(info));
  info.ps5 = true;
  info.host = name.c_str();  // PSN baglantisinda kullanilmaz; yine de gecerli bir dizge olsun
  chiaki_connect_video_profile_preset(
      &info.video_profile,
      resolution == 1080 ? CHIAKI_VIDEO_RESOLUTION_PRESET_1080p
                         : (resolution == 540 ? CHIAKI_VIDEO_RESOLUTION_PRESET_540p : CHIAKI_VIDEO_RESOLUTION_PRESET_720p),
      fps == 30 ? CHIAKI_VIDEO_FPS_PRESET_30 : CHIAKI_VIDEO_FPS_PRESET_60);
  info.video_profile.codec = CHIAKI_CODEC_H264;  // TV'nin Pepper cozucusu yalnizca H.264 sunuyor
  if (bitrate > 0) info.video_profile.bitrate = static_cast<unsigned int>(bitrate);
  info.video_profile_auto_downgrade = true;
  info.enable_keyboard = false;
  info.enable_dualsense = false;
  info.auto_regist = false;
  info.holepunch_session = hs;
  memcpy(info.psn_account_id, account, sizeof(account));
  info.packet_loss_max = 0.05;
  info.enable_idr_on_fec_failure = true;

  err = chiaki_session_init(&session_, &info, &log_);
  if (err != CHIAKI_ERR_SUCCESS) {
    // chiaki_session_init hata yolunda delik acma oturumunu kendisi serbest birakir.
    ConnectFailed(std::string(L("Oturum başlatılamadı: ")) + chiaki_error_string(err));
    return;
  }
  chiaki_bitstream_init(&bitstream_, &log_, CHIAKI_CODEC_H264);
  ref_skips_ = 0;
  {
    Lock l(&mu_);
    check_refs_ = low_latency_;
  }
  chiaki_opus_decoder_init(&opus_, &log_);
  chiaki_opus_decoder_set_cb(&opus_, AudioSettingsCb, AudioFrameCb, this);
  ChiakiAudioSink sink;
  chiaki_opus_decoder_get_sink(&opus_, &sink);
  chiaki_session_set_audio_sink(&session_, &sink);
  chiaki_session_set_video_sample_cb(&session_, VideoCb, this);
  chiaki_session_set_event_cb(&session_, EventCb, this);

  {
    // StreamStart'tan once: video cozucu hemen hata bildirirse ReportStreamError oturumu gorebilsin.
    Lock l(&mu_);
    session_active_ = true;  // bundan sonra temizligi DoCleanupSession yapar
    need_pin_ = pin_incorrect_ = false;
  }
  platform_->StreamStart();
  ConnectStep(L("Konsola bağlanılıyor"));
  err = chiaki_session_start(&session_);
  if (err != CHIAKI_ERR_SUCCESS) {
    // Oturum is parcacigi hic olusmadi: join yok, QUIT olayi gelmeyecek; burada kapat.
    {
      Lock s(&session_mu_);
      {
        Lock l(&mu_);
        session_active_ = false;
      }
      chiaki_session_fini(&session_);
    }
    chiaki_opus_decoder_fini(&opus_);
    platform_->StreamStop();
    ConnectFailed(std::string(L("Oturum başlatılamadı: ")) + chiaki_error_string(err));
    return;
  }
  Persist();
  {
    Lock l(&mu_);
    stop = stop_requested_;
  }
  if (stop) DoStop();  // hazirlik sirasinda vazgecildi
}

void Core::Stop() {
  {
    Lock l(&mu_);
    stop_requested_ = true;
  }
  // Durdurma cagrilari kisa sureli kilit ve boru yazimi icerir; ana is parcacigini hic bekletmemek
  // icin ayri, kendini birakan bir is parcaciginda calistir.
  pthread_t thread;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  int rc = pthread_create(&thread, &attr, StopMain, this);
  pthread_attr_destroy(&attr);
  if (rc != 0) Enqueue([this]() { DoStop(); });  // ana is parcaciginda asla calistirma
}

void* Core::StopMain(void* self) {
  static_cast<Core*>(self)->DoStop();
  return NULL;
}

void Core::DoStop() {
  Lock s(&session_mu_);
  ChiakiHolepunchSession hs;
  bool active;
  {
    Lock l(&mu_);
    hs = pending_hs_;
    active = session_active_;
  }
  if (hs) chiaki_holepunch_main_thread_cancel(hs, true);
  if (active) {
    // Oturum is parcacigi veri baglantisi icin delik acarken durum kilidini 30-90 sn tutar ve
    // chiaki_session_stop o kilidi bekler. Once delik acmayi iptal et ki hemen cikabilsin.
    // (Delik acma oturumu chiaki_session_fini'ye kadar yasar; o da session_mu_ altinda calisir.)
    if (session_.holepunch_session) chiaki_holepunch_main_thread_cancel(session_.holepunch_session, false);
    chiaki_session_stop(&session_);
  }
}

void Core::SetPin(const std::string& pin) {
  if (pin.empty()) return;
  std::string value = pin;
  // Kutuphane cagrisi oturum kilidini bekleyebilir: ana is parcaciginda degil, is kuyrugunda yap.
  Enqueue([this, value]() {
    {
      Lock s(&session_mu_);
      {
        Lock l(&mu_);
        if (!session_active_ || !need_pin_) return;
        need_pin_ = false;
      }
      chiaki_session_set_login_pin(&session_, reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
    Notify();
  });
}

// Ana is parcacigindan cagrilir: session_mu_ mesgulse (durdurma ya da kapatma suruyor) beklemez, vazgecer.
bool Core::SetController(const ChiakiControllerState& state) {
  if (pthread_mutex_trylock(&session_mu_) != 0) return false;
  bool ok;
  {
    Lock l(&mu_);
    ok = session_active_ && streaming_;
  }
  if (ok) {
    ChiakiControllerState copy = state;
    chiaki_session_set_controller_state(&session_, &copy);
  }
  pthread_mutex_unlock(&session_mu_);
  return true;
}

// Anahtar kare istegi agdan paket gonderir: ana is parcaciginda degil, is kuyrugunda yapilir.
void Core::RequestIdr() {
  {
    Lock l(&mu_);
    int64_t now = util::NowMs();
    if (!session_active_ || !streaming_ || now - last_idr_ms_ < 500) return;
    last_idr_ms_ = now;
  }
  Enqueue([this]() { DoRequestIdr(); });
}

void Core::DoRequestIdr() {
  Lock s(&session_mu_);
  {
    Lock l(&mu_);
    if (!session_active_ || !streaming_) return;
  }
  chiaki_session_request_idr(&session_);
}

void Core::ReportStreamError(const std::string& message) {
  bool downgraded = false;
  {
    Lock l(&mu_);
    if (!session_active_) return;
    std::string text = message;
    if (resolution_ == 1080) {
      // PS5 1080p'de H.264 Level 5.0 / 9 referans kare bildirir; bu TV'nin donanim cozucusu bunu
      // reddediyor. 720p (Level 4.0) calisiyor: ayari dusur ki yeniden baglanmak yetsin.
      resolution_ = 720;
      downgraded = true;
      text = L("Bu televizyon 1080p yayını çözemiyor. Çözünürlük 720p'ye alındı; yeniden bağlan.");
    }
    if (stream_error_.empty()) stream_error_ = text;
  }
  Log("! " + message);
  if (downgraded) Persist();
  Stop();
}

void Core::TakeNetStats(double* mbps, int* frames_lost, int* rtt_ms) {
  int bytes = __sync_fetch_and_and(&bytes_window_, 0);
  int64_t now = util::NowMs();
  int64_t span = now - net_stats_ms_;
  net_stats_ms_ = now;
  *mbps = span > 0 ? bytes * 8.0 / 1000.0 / static_cast<double>(span) : 0.0;
  *frames_lost = frames_lost_;
  Lock l(&mu_);
  *rtt_ms = rtt_ms_;
}

void Core::Sleep() {
  Enqueue([this]() {
    {
      Lock s(&session_mu_);
      bool ok;
      {
        Lock l(&mu_);
        ok = session_active_ && streaming_;
      }
      if (ok) chiaki_session_goto_bed(&session_);
    }
    usleep(400 * 1000);  // komut konsola ulassin
    Stop();
  });
}

void Core::SetStreamStats(int fps, int dropped, const std::string& gamepad) {
  int bad = chiaki_nacl_bad_unlocks;
  if (bad != bad_unlocks_seen_) {  // saniyede bir cagrilir, yalnizca ana is parcacigindan
    bad_unlocks_seen_ = bad;
    Log(L("! kütüphane kilitli olmayan bir kilidi bıraktı (toplam ") + util::Num(bad) + ")");
  }
  bool changed;
  {
    Lock l(&mu_);
    stat_fps_ = fps;
    stat_dropped_ = dropped;
    changed = gamepad_ != gamepad;
    gamepad_ = gamepad;
  }
  if (changed) Notify();  // kol takildi ya da cikarildi: TV arayuzu hemen ogrensin
}

// Kutuphanenin is parcaciginda calisir: burada beklemek ya da oturumu kapatmak yasak.
void Core::EventCb(ChiakiEvent* event, void* user) {
  Core* self = static_cast<Core*>(user);
  switch (event->type) {
    case CHIAKI_EVENT_CONNECTED: {
      {
        Lock l(&self->mu_);
        self->connecting_ = false;
        self->streaming_ = true;
        self->rtt_ms_ = static_cast<int>(self->session_.rtt_us / 1000);
        self->frames_lost_ = 0;
        self->bytes_window_ = 0;
        self->net_stats_ms_ = util::NowMs();
        self->message_.clear();
        self->error_.clear();
      }
      self->Log(L("> akış başladı"));
      self->Notify();
      break;
    }
    case CHIAKI_EVENT_LOGIN_PIN_REQUEST: {
      {
        Lock l(&self->mu_);
        self->need_pin_ = true;
        self->pin_incorrect_ = event->login_pin_request.pin_incorrect;
      }
      self->Notify();
      break;
    }
    case CHIAKI_EVENT_HOLEPUNCH: {
      {
        Lock l(&self->mu_);
        if (!self->streaming_)
          self->message_ = event->data_holepunch.finished ? L("Konsola bağlanılıyor") : L("Veri bağlantısı kuruluyor");
      }
      self->Notify();
      break;
    }
    case CHIAKI_EVENT_QUIT: {
      ChiakiQuitReason reason = event->quit.reason;
      std::string detail = event->quit.reason_str ? event->quit.reason_str : "";
      {
        // Akis bitti: kumanda ve IDR cagrilari hemen dursun; temizlik bitene kadar "kapatiliyor" goster.
        Lock l(&self->mu_);
        self->streaming_ = false;
        self->connecting_ = true;
        self->need_pin_ = false;
        self->message_ = L("Bağlantı kapatılıyor");
      }
      self->Notify();
      self->Enqueue([self, reason, detail]() { self->DoCleanupSession(reason, detail); });
      break;
    }
    default:
      break;
  }
}

void Core::DoCleanupSession(ChiakiQuitReason reason, std::string reason_str) {
  {
    Lock l(&mu_);
    if (!session_active_) return;
  }
  chiaki_session_join(&session_);
  {
    // session_mu_ tutulurken bayragi indir ve oturumu kapat: kisa cagrilar ya bundan once biter
    // ya da bayragi inmis gorup vazgecer.
    Lock s(&session_mu_);
    {
      Lock l(&mu_);
      session_active_ = false;
    }
    chiaki_session_fini(&session_);
  }
  chiaki_opus_decoder_fini(&opus_);
  platform_->StreamStop();
  std::string error;
  {
    Lock l(&mu_);
    if (!stream_error_.empty()) {
      error = stream_error_;  // bizim bildirdigimiz hata (ornegin video cozucu) kullanici durdurmasindan onceliklidir
    } else if (stop_requested_ || reason == CHIAKI_QUIT_REASON_STOPPED) {
      // kullanici vazgecti ya da kesti: hata degil
    } else if (reason == CHIAKI_QUIT_REASON_NONE) {
      // Kutuphane veri baglantisi ya da kontrol kanali kurulamayinca neden belirtmeden cikar.
      error = L("Konsola bağlanılamadı. Konsolun açık ya da dinlenme modunda olduğundan emin olup yeniden dene; ayrıntı kurulum sayfasındaki günlükte.");
      if (!reason_str.empty()) error += " (" + reason_str + ")";
    } else if (reason == CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_SHUTDOWN) {
      error = L("Konsol kapandı ya da dinlenme moduna geçti.");
    } else {
      error = QuitText(reason, reason_str);
    }
    stream_error_.clear();
    streaming_ = false;
    connecting_ = false;
    need_pin_ = false;
    message_.clear();
    error_ = error;
  }
  Log(error.empty() ? L("> bağlantı kapandı") : "! " + error);
  Notify();
}

bool Core::VideoCb(uint8_t* buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void* user) {
  (void)frame_recovered;
  Core* self = static_cast<Core*>(user);
  __sync_fetch_and_add(&self->bytes_window_, static_cast<int>(buf_size));
  if (frames_lost > 0) __sync_fetch_and_add(&self->frames_lost_, frames_lost);
  if (self->check_refs_ && buf_size > 5) {
    // Ilk NAL biriminin turu: 7 (SPS) ise baslik, degilse bir kare.
    size_t i = 0;
    while (i + 3 < buf_size && !(buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)) i++;
    int type = i + 3 < buf_size ? (buf[i + 3] & 0x1f) : 0;
    ChiakiBitstreamSlice slice;
    if (type == 7) {
      chiaki_bitstream_header(&self->bitstream_, buf, static_cast<unsigned>(buf_size));
    } else if (chiaki_bitstream_slice(&self->bitstream_, buf, static_cast<unsigned>(buf_size), &slice) &&
               slice.slice_type == CHIAKI_BITSTREAM_SLICE_P && slice.reference_frame != 0xff &&
               slice.reference_frame >= 1) {
      // Kare bir oncekine degil daha eski bir kareye basvuruyor (PS5'in kayip sonrasi toparlanma karesi).
      // Cozucuye tek referans bildirildigi icin o kare artik yok: bozuk goruntu yerine anahtar kare iste.
      self->platform_->VideoWaitIdr();
      self->RequestIdr();
      if (self->ref_skips_++ < 20 || self->ref_skips_ % 50 == 0)
        self->Log(std::string("> ") + L("Düşük gecikme modu: çözücüde olmayan eski bir kareye başvuran kare atlandı, anahtar kare istendi") +
                  " (" + util::Num(self->ref_skips_) + ")");
      return true;
    }
  }
  self->platform_->VideoPush(buf, buf_size);
  return true;
}

void Core::AudioSettingsCb(uint32_t channels, uint32_t rate, void* user) {
  Core* self = static_cast<Core*>(user);
  {
    Lock l(&self->mu_);
    self->audio_channels_ = channels;
  }
  char line[96];
  snprintf(line, sizeof(line), "> ses: %u kanal, %u Hz", static_cast<unsigned>(channels), static_cast<unsigned>(rate));
  self->Log(line);
}

void Core::AudioFrameCb(int16_t* buf, size_t samples_count, void* user) {
  Core* self = static_cast<Core*>(user);
  uint32_t channels;
  {
    Lock l(&self->mu_);
    channels = self->audio_channels_;
  }
  self->platform_->AudioPush(buf, samples_count, static_cast<int>(channels));
}
