// chiaki-tv: H.264 akisini TV'nin donanim cozucusune verir ve cikan kareleri ekrana cizer.
// Push() disindaki her sey Pepper ana is parcaciginda calisir.
#ifndef CHIAKI_TV_VIDEO_H
#define CHIAKI_TV_VIDEO_H

#include <pthread.h>
#include <stdint.h>

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "ppapi/c/pp_codecs.h"
#include "ppapi/cpp/graphics_3d.h"
#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/video_decoder.h"
#include "ppapi/utility/completion_callback_factory.h"

#include "sps.h"

class VideoRenderer {
 public:
  explicit VideoRenderer(pp::Instance* instance);
  ~VideoRenderer();

  void SetViewSize(int width, int height);
  // wait_idr: cozucu akis ortasinda yeniden kuruluyorsa ilk anahtar kareye kadar gelenleri at.
  void Start(bool wait_idr = false);
  void Stop();

  // Herhangi bir is parcacigindan: bir H.264 erisim birimi (ya da yalnizca SPS/PPS basligi).
  void Push(const uint8_t* data, size_t size);

  // Saniyede bir cagrilir: son araliktaki kare sayilarini dondurur ve sifirlar.
  void TakeStats(int* shown, int* dropped);
  // Son araligin olcumleri (ana is parcacigi). Sureler ms.
  struct Timing {
    int pictures, skipped, depth;
    int queue_avg, queue_max, decode_avg, decode_max, swap_avg, swap_max, present_avg, present_max;
    int width, height;  // son resmin gorunen boyutu
  };
  Timing TakeTiming();
  // Tanilama/ekran goruntusu: bir sonraki cizilen kareyi RGBA olarak sakla (alt satir once).
  void RequestShot() { shot_requested_ = true; }
  bool TakeShot(std::string* rgba, int* width, int* height);
  // Cozucuye verilmeden once SPS'e uygulanacak dusuk gecikme yamasi (her is parcacigindan).
  void SetSpsPatch(const sps::Patch& patch);
  // Bir sonraki anahtar kareye kadar gelen kareleri at (her is parcacigindan).
  void WaitIdr();
  // Her gercek karenin arkasina bos kare ekle (sps.h'ye bak). Bir sonraki akistan itibaren gecerli.
  void SetSkipFrames(bool on);

  // Cozucu kareleri kaybettiginde yeni bir anahtar kare istemek icin (ana is parcaciginda cagrilir).
  std::function<void()> on_need_idr;
  // Cozucu ya da grafik baglami hic acilamadiysa (ana is parcaciginda cagrilir).
  std::function<void(const std::string&)> on_fatal;
  // Tanilama satirlari (ana is parcaciginda cagrilir).
  std::function<void(const std::string&)> on_log;

 private:
  struct Program {
    unsigned int id;
    int scale_loc;
    Program() : id(0), scale_loc(-1) {}
  };

  bool EnsureContext();
  void InitDecoder(PP_HardwareAcceleration acceleration);
  void OnInit(int32_t result);
  void DrainOnMain(int32_t);
  void Drain();
  void OnDecodeDone(int32_t result);
  void GetNextPicture();
  void OnPicture(int32_t result, PP_VideoPicture picture);
  void Paint(const PP_VideoPicture& picture);
  void OnSwap(int32_t result);
  void Recover(const char* why, int32_t code, bool force = false);
  void DoRecover(int32_t result, int generation);
  void OnReset(int32_t result);
  void Fatal(const std::string& message);
  void ClearScreen();
  void Note(const std::string& line);
  Program* ProgramFor(uint32_t texture_target);

  pp::Instance* instance_;
  pp::CompletionCallbackFactory<VideoRenderer> factory_;       // cozucu geri cagrilari; Stop() iptal eder
  pp::CompletionCallbackFactory<VideoRenderer> swap_factory_;  // SwapBuffers; hic iptal edilmez (ucustaki takas tamamlanmali)
  pp::CompletionCallbackFactory<VideoRenderer, pp::ThreadSafeThreadTraits> ts_factory_;  // Push icin
  pp::Graphics3D context_;
  pp::VideoDecoder decoder_;
  int width_, height_;
  bool gl_ready_;
  unsigned int vbo_;
  Program prog_2d_, prog_rect_, prog_external_;

  enum State { kStopped, kInitializing, kReady, kResetting };
  State state_;
  bool tried_fallback_;
  bool decoding_;
  bool swap_pending_;
  bool clear_after_swap_;  // Stop() sirasinda takas ucustaydi: bitince ekrani temizle
  bool has_current_, has_queued_;
  PP_VideoPicture current_, queued_;
  std::vector<uint8_t> in_flight_;  // Decode tamamlanana kadar gecerli kalmali
  uint32_t next_id_;
  int64_t last_recover_ms_;
  bool recover_scheduled_;
  int errors_;          // art arda cozucu hatasi sayisi
  int ok_since_error_;  // son hatadan beri gelen resim sayisi
  int generation_;
  int decoded_, pictures_, painted_;
  // --- gecikme olcumu ---
  struct Stamp { uint32_t id; int64_t push_ms; int64_t decode_ms; };
  Stamp stamps_[64];           // decode_id -> zamanlar (halka)
  int64_t current_push_ms_;    // ekrandaki/cizilen karenin agdan gelis ani
  int64_t queued_push_ms_;
  struct Acc { int64_t sum; int64_t max; int n; void Add(int64_t v) { sum += v; if (v > max) max = v; n++; } };
  Acc t_queue_, t_decode_, t_present_, t_swap_;
  int64_t swap_call_ms_;
  int pictures_window_, max_queue_;
  int pic_width_, pic_height_;
  bool shot_requested_;
  std::string shot_;
  int shot_width_, shot_height_;  // tanilama sayaclari (bu Start()'tan beri)  // her Start() ile artar: eski bir kurtarma istegi yeni akisi yeniden baslatmasin

  // Push ile ana is parcacigi arasinda paylasilanlar
  pthread_mutex_t mu_;
  std::deque<std::vector<uint8_t> > queue_;
  std::deque<int64_t> queue_ms_;
  sps::Patch sps_patch_;
  bool skip_frames_;        // bos kare ekleme acik
  sps::Info sps_info_;      // gecerliyse bos kare uretilebilir
  int skipped_window_;      // tanilama: son aralikta cozucuden cikan bos kare sayisi (ana is parcacigi)  // queue_ ile ayni sirada: karenin agdan gelis ani
  std::vector<uint8_t> header_;  // son SPS/PPS
  bool need_header_;
  bool wait_idr_;
  bool drain_scheduled_;
  bool accepting_;
  int shown_, dropped_;
};

#endif
