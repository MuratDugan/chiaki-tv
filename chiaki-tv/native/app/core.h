// chiaki-tv: uygulama cekirdegi. Durumu tutar, PSN ve oturum islerini arka planda yurutur.
// Pepper'a (video, ses, mesajlasma) dogrudan dokunmaz; onlari Platform arayuzu uzerinden ister.
#ifndef CHIAKI_TV_CORE_H
#define CHIAKI_TV_CORE_H

#include <pthread.h>
#include <stdint.h>

#include <deque>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <chiaki/bitstream.h>
#include <chiaki/controller.h>
#include <chiaki/log.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/remote/holepunch.h>
#include <chiaki/session.h>

// Cekirdegin ana (Pepper) taraftan istedikleri. Tum yontemler herhangi bir is parcacigindan cagrilabilir.
class Platform {
 public:
  virtual ~Platform() {}
  virtual void PostState(const std::string& state_json) = 0;     // TV arayuzune durum
  virtual void PostPersist(const std::string& config_json) = 0;  // kalici ayarlari JS'e yazdir
  virtual void PostLog(const std::string& line) = 0;             // gunluk satiri (modul cokerse JS'te kalir)
  virtual void StreamStart() = 0;                                // video + ses cikisini hazirla
  virtual void StreamStop() = 0;
  virtual void VideoPush(const uint8_t* data, size_t size) = 0;
  virtual void VideoWaitIdr() = 0;  // bir sonraki anahtar kareye kadar gelen kareleri at
  virtual void AudioPush(const int16_t* pcm, size_t frames, int channels) = 0;
};

struct Console {
  std::string duid;  // 32 bayt, onaltilik
  std::string name;
  bool remoteplay;
  Console() : remoteplay(false) {}
};

class Core {
 public:
  enum PairResult { kPairOk, kPairWrong, kPairLocked };

  explicit Core(Platform* platform);
  ~Core();

  // JS acilista yerel IP'yi ve kayitli ayarlari bildirir.
  void Init(const std::string& local_ip, int setup_port, const std::string& config_json);
  void SetLocalIp(const std::string& local_ip);
  std::string local_ip();

  // for_tv: TV arayuzu icin (eslestirme kodu dahil). Telefon sayfasina kod verilmez.
  std::string StateJson(bool for_tv);
  std::string LogJson();

  // --- telefon kurulum sayfasi yetkisi ---
  PairResult Pair(const std::string& code, std::string* token, int* retry_after_s);
  bool CheckToken(const std::string& token);

  // --- komutlar (hemen doner, is arka planda yurur) ---
  std::string LoginUrl();
  void PsnFinish(const std::string& redirect_or_code);
  void RefreshConsoles();
  void Connect(const std::string& duid);
  void Stop();
  void SetPin(const std::string& pin);
  void SetSettings(int resolution, int fps, int bitrate_kbps);
  void SetLang(const std::string& lang);  // "en" | "tr"
  // Dusuk gecikme: cozucuye tek referans kare bildirilir (TV kareleri ~130 ms daha erken verir); eski bir
  // kareye basvuran toparlanma kareleri atlanip anahtar kare istenir (paket kaybinda kisa donma).
  void SetLowLatency(bool on);
  bool low_latency();
  void Logout();

  // --- akis sirasinda ana is parcacigindan ---
  // false: oturum kilidi mesguldu, durum iletilemedi; cagiran bir sonraki turda yeniden denemeli.
  bool SetController(const ChiakiControllerState& state);
  void RequestIdr();
  void SetStreamStats(int fps, int dropped, const std::string& gamepad);
  // Akis surerken giderilemeyen bir hata (ornegin video cozucu acilamadi): hatayi goster, baglantiyi kes.
  void ReportStreamError(const std::string& message);
  // Son araliktaki video bit hizi (Mbit/sn), oturum boyunca kaybolan kare sayisi ve baglanti kurulurken
  // olculen gidis-donus suresi (ms). Saniyede bir, ana is parcacigindan.
  void TakeNetStats(double* mbps, int* frames_lost, int* rtt_ms);
  // Konsolu dinlenme moduna alir ve baglantiyi keser.
  void Sleep();
  bool streaming();

  void Log(const std::string& line);
  // Tanilama: kutuphanenin ayrintili gunlugunu acar/kapatir (ayrintili satirlar oturum verisi icerebilir).
  void SetVerbose(bool on);

 private:
  typedef std::function<void()> Job;
  static void* WorkerMain(void* self);
  static void* StopMain(void* self);
  void Enqueue(const Job& job);

  void Notify();   // durumu TV'ye gonder
  void Persist();  // kalici ayarlari JS'e gonder
  // Arka plan isi (PSN, konsol listesi): yalnizca "mesgul" bayragini ve mesaji yonetir.
  void BeginJob(const std::string& message);
  void EndJob(const std::string& error);
  // Baglanti akisi: adim mesaji ve basarisizlik.
  void ConnectStep(const std::string& message);
  void ConnectFailed(const std::string& error);

  bool EnsureToken(std::string* error);
  void DoPsnFinish(std::string input);
  void DoRefreshConsoles();
  void ListConsoles(std::string* error);
  void DoConnect(std::string duid);
  void DoStop();
  void DoRequestIdr();
  void DoCleanupSession(ChiakiQuitReason reason, std::string reason_str);

  static void EventCb(ChiakiEvent* event, void* user);
  static bool VideoCb(uint8_t* buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void* user);
  static void AudioSettingsCb(uint32_t channels, uint32_t rate, void* user);
  static void AudioFrameCb(int16_t* buf, size_t samples_count, void* user);
  static void LogCb(ChiakiLogLevel level, const char* msg, void* user);

  Platform* platform_;
  pthread_mutex_t mu_;  // asagidaki tum durumu korur
  // session_ ve pending_hs_ uzerindeki kisa cagrilari (durdur, pin, kumanda, IDR, iptal) bunlarin
  // serbest birakilmasiyla yaristirmaz. Kilit sirasi: once session_mu_, sonra mu_.
  pthread_mutex_t session_mu_;
  pthread_mutex_t notify_mu_;  // durum ve kalici ayar gonderimlerini siralar; mu_'dan once alinir
  pthread_mutex_t log_mu_;  // yalnizca log_lines_; kutuphane kendi kilitlerini tutarken gunluk yazar

  // is kuyrugu
  pthread_t worker_;
  pthread_mutex_t queue_mu_;
  pthread_cond_t queue_cv_;
  std::deque<Job> queue_;
  bool quit_;

  // durum: faz bu bayraklardan turetilir (StateJson). Boylece bir arka plan isinin bitmesi,
  // surmekte olan baglantinin durumunu ezemez.
  bool booted_;      // JS init gonderdi
  bool busy_;        // bir arka plan isi suruyor
  bool connecting_;  // Connect() cagrildi, akis henuz baslamadi ya da basarisiz olmadi
  bool streaming_;   // konsol baglandi
  std::string message_;  // surmekte olan isin aciklamasi
  std::string error_;    // son hata (bos = yok)
  std::string local_ip_;
  int setup_port_;

  // eslestirme
  std::string pair_code_;
  int pair_fails_;
  int pair_lock_level_;         // her kilitlemede sure katlanir
  int64_t pair_locked_until_ms_;
  std::set<std::string> tokens_;

  // PSN
  std::string access_, refresh_, account_id_b64_, client_duid_;
  int64_t expiry_;
  std::vector<Console> consoles_;
  std::string last_duid_;

  // ayarlar
  int resolution_, fps_, bitrate_kbps_;
  bool low_latency_;
  std::string lang_;

  // oturum
  ChiakiLog log_;
  ChiakiSession session_;
  ChiakiOpusDecoder opus_;
  ChiakiBitstream bitstream_;     // dilim basliklarini okumak icin (yalnizca video is parcacigi)
  volatile bool check_refs_;      // bu oturumda dusuk gecikme etkin
  int ref_skips_;
  volatile int bytes_window_;    // son araliktaki video baytlari (video is parcacigi yazar)
  volatile int frames_lost_;
  int rtt_ms_;
  int64_t net_stats_ms_;
  bool session_active_;  // session_ baslatildi, henuz temizlenmedi
  bool stop_requested_;
  ChiakiHolepunchSession pending_hs_;  // oturuma devredilmeden onceki delik acma oturumu
  bool need_pin_, pin_incorrect_;
  int64_t last_idr_ms_;
  std::string stream_error_;  // ReportStreamError ile gelen; temizlikte hata olarak gosterilir
  uint32_t audio_channels_;
  int stat_fps_, stat_dropped_;
  int bad_unlocks_seen_;  // kutuphanedeki hatali kilit birakma sayaci (tanilama)
  std::string gamepad_;

  std::deque<std::string> log_lines_;
};

#endif
