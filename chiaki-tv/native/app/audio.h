// chiaki-tv: cozulmus sesi (16 bit, 48 kHz) Pepper ses cikisina verir.
#ifndef CHIAKI_TV_AUDIO_H
#define CHIAKI_TV_AUDIO_H

#include <pthread.h>
#include <stdint.h>

#include <vector>

#include "ppapi/cpp/audio.h"
#include "ppapi/cpp/instance.h"

class AudioOut {
 public:
  explicit AudioOut(pp::Instance* instance);
  ~AudioOut();

  void Start();  // ana is parcacigi
  void Stop();   // ana is parcacigi

  // Herhangi bir is parcacigindan. frames: kanal basina ornek sayisi.
  void Push(const int16_t* pcm, size_t frames, int channels);

 private:
  static void Fill(void* samples, uint32_t bytes, PP_TimeDelta latency, void* user);

  pp::Instance* instance_;
  pp::Audio audio_;
  pthread_mutex_t mu_;
  std::vector<int16_t> ring_;  // stereo, araya girmeli
  size_t capacity_;            // kare (stereo cift) cinsinden
  size_t read_, fill_;
  bool primed_;                // ilk tampon dolana kadar sessizlik ver
};

#endif
