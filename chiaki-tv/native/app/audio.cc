#include "audio.h"

#include <string.h>

#include "ppapi/cpp/audio_config.h"

#include "util.h"

using util::Lock;

namespace {
const size_t kRate = 48000;
const size_t kCapacityFrames = kRate / 2;      // yarim saniye
const size_t kPrimeFrames = kRate * 60 / 1000;  // calmaya baslamadan once 60 ms biriktir
const size_t kMaxLagFrames = kRate * 200 / 1000;  // birikim 200 ms'yi gecerse eskiyi at (dudak uyumu)
}  // namespace

AudioOut::AudioOut(pp::Instance* instance)
    : instance_(instance), ring_(kCapacityFrames * 2), capacity_(kCapacityFrames), read_(0), fill_(0), primed_(false) {
  pthread_mutex_init(&mu_, NULL);
}

AudioOut::~AudioOut() {
  pthread_mutex_destroy(&mu_);
}

void AudioOut::Start() {
  Stop();
  {
    Lock l(&mu_);
    read_ = fill_ = 0;
    primed_ = false;
  }
  uint32_t frames = pp::AudioConfig::RecommendSampleFrameCount(instance_, PP_AUDIOSAMPLERATE_48000, 960);
  pp::AudioConfig config(instance_, PP_AUDIOSAMPLERATE_48000, frames);
  if (config.is_null()) return;
  audio_ = pp::Audio(instance_, config, Fill, this);
  if (!audio_.is_null()) audio_.StartPlayback();
}

void AudioOut::Stop() {
  if (!audio_.is_null()) {
    audio_.StopPlayback();
    audio_ = pp::Audio();
  }
}

void AudioOut::Push(const int16_t* pcm, size_t frames, int channels) {
  if (!pcm || frames == 0 || channels < 1) return;
  Lock l(&mu_);
  if (fill_ + frames > kMaxLagFrames) {
    // Fazla birikti: en eski ornekleri atarak gecikmeyi sinirla.
    size_t drop = fill_ + frames - kMaxLagFrames;
    if (drop > fill_) drop = fill_;
    read_ = (read_ + drop) % capacity_;
    fill_ -= drop;
  }
  if (frames > capacity_ - fill_) frames = capacity_ - fill_;
  size_t write = (read_ + fill_) % capacity_;
  for (size_t i = 0; i < frames; i++) {
    int16_t left = pcm[i * channels];
    int16_t right = channels > 1 ? pcm[i * channels + 1] : left;
    ring_[write * 2] = left;
    ring_[write * 2 + 1] = right;
    write = (write + 1) % capacity_;
  }
  fill_ += frames;
  if (fill_ >= kPrimeFrames) primed_ = true;
}

void AudioOut::Fill(void* samples, uint32_t bytes, PP_TimeDelta latency, void* user) {
  (void)latency;
  AudioOut* self = static_cast<AudioOut*>(user);
  int16_t* out = static_cast<int16_t*>(samples);
  size_t want = bytes / (2 * sizeof(int16_t));
  size_t got = 0;
  {
    Lock l(&self->mu_);
    if (self->primed_) {
      got = want < self->fill_ ? want : self->fill_;
      for (size_t i = 0; i < got; i++) {
        out[i * 2] = self->ring_[self->read_ * 2];
        out[i * 2 + 1] = self->ring_[self->read_ * 2 + 1];
        self->read_ = (self->read_ + 1) % self->capacity_;
      }
      self->fill_ -= got;
      if (self->fill_ == 0) self->primed_ = false;  // bosaldi: yeniden biriktir
    }
  }
  if (got < want) memset(out + got * 2, 0, (want - got) * 2 * sizeof(int16_t));
}
