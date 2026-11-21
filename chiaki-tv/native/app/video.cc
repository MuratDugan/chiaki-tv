#include "video.h"

#include <stdio.h>
#include <string.h>

#include <GLES2/gl2.h>

#include "ppapi/c/pp_errors.h"
#include "ppapi/c/ppb_graphics_3d.h"
#include "ppapi/cpp/core.h"
#include "ppapi/cpp/module.h"
#include "ppapi/lib/gl/gles2/gl2ext_ppapi.h"

#include "dbgsink.h"
#include "i18n.h"
#include "util.h"

#ifndef GL_TEXTURE_RECTANGLE_ARB
#define GL_TEXTURE_RECTANGLE_ARB 0x84F5
#endif
#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

using util::Lock;

namespace {

// Kuyruk bu kadar kareyi gecerse cozucu yetisemiyor demektir: birikeni at, anahtar kare iste.
const size_t kMaxQueuedFrames = 16;

const char kVertexShader[] =
    "varying vec2 v_texCoord;\n"
    "attribute vec4 a_position;\n"
    "attribute vec2 a_texCoord;\n"
    "uniform vec2 v_scale;\n"
    "void main() {\n"
    "  v_texCoord = v_scale * a_texCoord;\n"
    "  gl_Position = a_position;\n"
    "}";
const char kFragment2D[] =
    "precision mediump float;\n"
    "varying vec2 v_texCoord;\n"
    "uniform sampler2D s_texture;\n"
    "void main() { gl_FragColor = texture2D(s_texture, v_texCoord); }";
const char kFragmentRect[] =
    "#extension GL_ARB_texture_rectangle : require\n"
    "precision mediump float;\n"
    "varying vec2 v_texCoord;\n"
    "uniform sampler2DRect s_texture;\n"
    "void main() { gl_FragColor = texture2DRect(s_texture, v_texCoord).rgba; }";
const char kFragmentExternal[] =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "varying vec2 v_texCoord;\n"
    "uniform samplerExternalOES s_texture;\n"
    "void main() { gl_FragColor = texture2D(s_texture, v_texCoord); }";

// Ucgen seridi: once konumlar, sonra doku koordinatlari (dikeyde cevrik; cozucu dokusu ustten baslar).
const float kVertices[] = {
    -1, -1, -1, 1, 1, -1, 1, 1,
    0,  1,  0,  0, 1, 1,  1, 0,
};

// Annex-B akisinda NAL turlerini tarar.
struct NalInfo {
  bool has_sps, has_idr, has_slice;
};

NalInfo ScanNals(const uint8_t* d, size_t n) {
  NalInfo info = {false, false, false};
  for (size_t i = 0; i + 3 < n; i++) {
    if (d[i] != 0 || d[i + 1] != 0) continue;
    size_t hdr;
    if (d[i + 2] == 1) hdr = i + 3;
    else if (d[i + 2] == 0 && i + 4 < n && d[i + 3] == 1) hdr = i + 4;
    else continue;
    if (hdr >= n) break;
    int type = d[hdr] & 0x1f;
    if (type == 7 || type == 8) info.has_sps = true;
    else if (type == 5) info.has_idr = info.has_slice = true;
    else if (type == 1) info.has_slice = true;
    i = hdr;
  }
  return info;
}

unsigned int CompileShader(unsigned int type, const char* source) {
  unsigned int shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  int ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

}  // namespace

VideoRenderer::VideoRenderer(pp::Instance* instance)
    : instance_(instance),
      factory_(this),
      swap_factory_(this),
      ts_factory_(this),
      width_(1920),
      height_(1080),
      gl_ready_(false),
      vbo_(0),
      state_(kStopped),
      tried_fallback_(false),
      decoding_(false),
      swap_pending_(false),
      clear_after_swap_(false),
      has_current_(false),
      has_queued_(false),
      next_id_(1),
      last_recover_ms_(0),
      recover_scheduled_(false),
      errors_(0),
      ok_since_error_(0),
      generation_(0),
      decoded_(0),
      pictures_(0),
      painted_(0),
      current_push_ms_(0),
      queued_push_ms_(0),
      swap_call_ms_(0),
      pictures_window_(0),
      max_queue_(0),
      pic_width_(0),
      pic_height_(0),
      shot_requested_(false),
      shot_width_(0),
      shot_height_(0),
      skip_frames_(false),
      skipped_window_(0),
      need_header_(true),
      wait_idr_(false),
      drain_scheduled_(false),
      accepting_(false),
      shown_(0),
      dropped_(0) {
  pthread_mutex_init(&mu_, NULL);
  memset(stamps_, 0, sizeof(stamps_));
  memset(&t_queue_, 0, sizeof(t_queue_));
  memset(&t_decode_, 0, sizeof(t_decode_));
  memset(&t_present_, 0, sizeof(t_present_));
  memset(&t_swap_, 0, sizeof(t_swap_));
}

VideoRenderer::~VideoRenderer() {
  pthread_mutex_destroy(&mu_);
}

void VideoRenderer::SetViewSize(int width, int height) {
  if (width <= 0 || height <= 0 || (width == width_ && height == height_)) return;
  width_ = width;
  height_ = height;
  if (!context_.is_null()) context_.ResizeBuffers(width_, height_);
}

void VideoRenderer::Note(const std::string& line) {
  if (on_log) on_log("> video: " + line);
}

bool VideoRenderer::EnsureContext() {
  dbgsink::MainStep("video:baglam");
  if (context_.is_null()) {
    static bool gl_initialized = false;
    if (!gl_initialized) {
      if (!glInitializePPAPI(pp::Module::Get()->get_browser_interface())) return false;
      gl_initialized = true;
    }
    const int32_t attribs[] = {PP_GRAPHICS3DATTRIB_ALPHA_SIZE,     0, PP_GRAPHICS3DATTRIB_DEPTH_SIZE, 0,
                               PP_GRAPHICS3DATTRIB_STENCIL_SIZE,   0, PP_GRAPHICS3DATTRIB_WIDTH,      width_,
                               PP_GRAPHICS3DATTRIB_HEIGHT,         height_, PP_GRAPHICS3DATTRIB_NONE};
    context_ = pp::Graphics3D(instance_, attribs);
    if (context_.is_null() || !instance_->BindGraphics(context_)) {
      context_ = pp::Graphics3D();
      return false;
    }
    gl_ready_ = false;
  }
  glSetCurrentContextPPAPI(context_.pp_resource());
  if (!gl_ready_) {
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices, GL_STATIC_DRAW);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0, 0, 0, 1);
    gl_ready_ = true;
  }
  return true;
}

VideoRenderer::Program* VideoRenderer::ProgramFor(uint32_t target) {
  Program* p = &prog_2d_;
  const char* fragment = kFragment2D;
  if (target == GL_TEXTURE_RECTANGLE_ARB) {
    p = &prog_rect_;
    fragment = kFragmentRect;
  } else if (target == GL_TEXTURE_EXTERNAL_OES) {
    p = &prog_external_;
    fragment = kFragmentExternal;
  }
  if (p->id) return p;
  unsigned int vs = CompileShader(GL_VERTEX_SHADER, kVertexShader);
  unsigned int fs = CompileShader(GL_FRAGMENT_SHADER, fragment);
  if (!vs || !fs) return NULL;
  unsigned int program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glBindAttribLocation(program, 0, "a_position");
  glBindAttribLocation(program, 1, "a_texCoord");
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  int ok = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    glDeleteProgram(program);
    return NULL;
  }
  glUseProgram(program);
  glUniform1i(glGetUniformLocation(program, "s_texture"), 0);
  p->scale_loc = glGetUniformLocation(program, "v_scale");
  p->id = program;
  return p;
}

// ------------------------------------------------------------------ yasam dongusu
void VideoRenderer::Start(bool wait_idr) {
  dbgsink::MainStep("video:baslat");
  Stop();
  generation_++;
  decoded_ = pictures_ = painted_ = 0;
  if (!wait_idr) errors_ = ok_since_error_ = 0;
  Note(std::string("baslatiliyor") + (wait_idr ? " (kurtarma)" : "") + ", gorunum " + util::Num(width_) + "x" + util::Num(height_));
  if (!EnsureContext()) {
    if (on_fatal) on_fatal(L("Grafik bağlamı oluşturulamadı."));
    return;
  }
  {
    Lock l(&mu_);
    queue_.clear();
    queue_ms_.clear();
    need_header_ = true;
    wait_idr_ = wait_idr;  // accepting_ ile ayni kilit altinda: araya ara kare giremez
    accepting_ = true;
    shown_ = dropped_ = 0;
  }
  clear_after_swap_ = false;  // hemen yeni kareler gelecek: araya siyah kare sokma
  tried_fallback_ = false;
  InitDecoder(PP_HARDWAREACCELERATION_ONLY);
}

void VideoRenderer::InitDecoder(PP_HardwareAcceleration acceleration) {
  decoder_ = pp::VideoDecoder(instance_);
  if (decoder_.is_null()) {
    state_ = kStopped;
    if (on_fatal) on_fatal(L("Video çözücü oluşturulamadı."));
    return;
  }
  state_ = kInitializing;
  dbgsink::MainStep("video:cozucu-ac");
  Note(acceleration == PP_HARDWAREACCELERATION_ONLY ? "donanim cozucu aciliyor" : "cozucu (yedekli) aciliyor");
  decoder_.Initialize(context_, PP_VIDEOPROFILE_H264HIGH, acceleration, 0, factory_.NewCallback(&VideoRenderer::OnInit));
}

void VideoRenderer::OnInit(int32_t result) {
  dbgsink::MainStep("video:cozucu-acildi");
  Note("cozucu acilis sonucu " + util::Num(result));
  if (result != PP_OK) {
    if (!tried_fallback_) {  // donanim cozucu acilmadiysa tarayicinin sectigine birak
      tried_fallback_ = true;
      InitDecoder(PP_HARDWAREACCELERATION_WITHFALLBACK);
      return;
    }
    state_ = kStopped;
    if (on_fatal) on_fatal(L("Televizyonun H.264 çözücüsü açılamadı (hata ") + util::Num(result) + ").");
    return;
  }
  state_ = kReady;
  GetNextPicture();
  Drain();
}

void VideoRenderer::Stop() {
  dbgsink::MainStep("video:durdur");
  {
    Lock l(&mu_);
    accepting_ = false;
    queue_.clear();
    queue_ms_.clear();
  }
  factory_.CancelAll();  // bekleyen cozucu geri cagrilari eski cozucuye ait
  decoder_ = pp::VideoDecoder();
  state_ = kStopped;
  decoding_ = false;
  has_current_ = has_queued_ = false;  // resimler cozucuyle birlikte gecersiz oldu
  in_flight_.clear();
  // Ucusta bir takas varken yenisi baslatilamaz: temizligi o bitince yap.
  if (swap_pending_) clear_after_swap_ = true;
  else ClearScreen();
}

void VideoRenderer::ClearScreen() {
  if (context_.is_null()) return;
  dbgsink::MainStep("video:temizle");
  glSetCurrentContextPPAPI(context_.pp_resource());
  glViewport(0, 0, width_, height_);
  glClear(GL_COLOR_BUFFER_BIT);
  swap_pending_ = true;
  context_.SwapBuffers(swap_factory_.NewCallback(&VideoRenderer::OnSwap));
}

// Cozucu hata verdiginde: sifirdan kur ve anahtar kare iste. Saniyede birden sik deneme.
void VideoRenderer::Fatal(const std::string& message) {
  {
    Lock l(&mu_);
    accepting_ = false;
    queue_.clear();
    queue_ms_.clear();
  }
  state_ = kStopped;
  Note("vazgecildi: " + message);
  if (on_fatal) on_fatal(message);
}

// Cozucu hata verdiginde. Cozucuyu yok edip hemen yenisini kurmak bu televizyonda ana is parcacigini
// kilitliyor (Initialize donmuyor); bu yuzden once Reset() denenir, o da olmazsa yayin duzgunce
// sonlandirilir. Yalnizca grafik baglami kayboldugunda (force) her sey gecikmeli olarak yeniden kurulur.
void VideoRenderer::Recover(const char* why, int32_t code, bool force) {
  if (recover_scheduled_ || state_ == kResetting || state_ == kStopped) return;
  Note(std::string("hata: ") + why + " " + util::Num(code) + " (cozulen " + util::Num(decoded_) + ", resim " +
       util::Num(pictures_) + ")");
  if (force) {
    recover_scheduled_ = true;
    factory_.CancelAll();
    decoder_ = pp::VideoDecoder();
    state_ = kInitializing;  // Drain beklesin
    pp::Module::Get()->core()->CallOnMainThread(1500, swap_factory_.NewCallback(&VideoRenderer::DoRecover, generation_));
    return;
  }
  ok_since_error_ = 0;
  if (++errors_ > 3 || decoder_.is_null()) {
    Fatal(L("Televizyonun video çözücüsü bu yayını çözemedi (hata ") + util::Num(code) + ").");
    return;
  }
  state_ = kResetting;
  dbgsink::MainStep("video:sifirla");
  decoder_.Reset(factory_.NewCallback(&VideoRenderer::OnReset));
}

void VideoRenderer::OnReset(int32_t result) {
  dbgsink::MainStep("video:sifirlandi");
  Note("sifirlama sonucu " + util::Num(result));
  if (result != PP_OK) {
    Fatal(L("Televizyonun video çözücüsü bu yayını çözemedi (sıfırlama hatası ") + util::Num(result) + ").");
    return;
  }
  decoding_ = false;
  {
    // Sifirlanan cozucu ara karelerle devam edemez: anahtar kare gelene kadar gelenleri at.
    Lock l(&mu_);
    queue_.clear();
    queue_ms_.clear();
    wait_idr_ = true;
    need_header_ = true;
  }
  state_ = kReady;
  if (on_need_idr) on_need_idr();
  GetNextPicture();
}

void VideoRenderer::DoRecover(int32_t, int generation) {
  recover_scheduled_ = false;
  if (generation != generation_) return;  // arada akis durdu ya da yeniden basladi
  {
    Lock l(&mu_);
    if (!accepting_) return;
  }
  Start(true);  // yeni cozucu ara karelerle baslayamaz
  if (on_need_idr) on_need_idr();
}

// ------------------------------------------------------------------ giris
void VideoRenderer::SetSkipFrames(bool on) {
  Lock l(&mu_);
  skip_frames_ = on;
}

void VideoRenderer::WaitIdr() {
  Lock l(&mu_);
  wait_idr_ = true;
}

void VideoRenderer::SetSpsPatch(const sps::Patch& patch) {
  Lock l(&mu_);
  sps_patch_ = patch;
}

void VideoRenderer::Push(const uint8_t* data, size_t size) {
  if (!data || size < 5) return;
  NalInfo info = ScanNals(data, size);
  bool overflow = false, schedule = false;
  std::vector<uint8_t> patched;
  {
    Lock l(&mu_);
    if (!accepting_) return;
    if (info.has_sps && sps_patch_.active() && sps::PatchStream(data, size, sps_patch_, &patched)) {
      data = patched.data();
      size = patched.size();
    }
    if (info.has_sps && !info.has_slice) {  // yalnizca baslik: sakla, bir sonraki kareye ekle
      header_.assign(data, data + size);
      sps::Info fresh;
      if (sps::ReadInfo(data, size, &fresh)) sps_info_ = fresh;
      if (skip_frames_ && sps_info_.valid) {  // bos karelerin PPS'i her anahtar kareyle birlikte gitsin
        std::vector<uint8_t> pps;
        sps::BuildSkipPps(&pps);
        header_.insert(header_.end(), pps.begin(), pps.end());
      }
      need_header_ = true;
      return;
    }
    if (wait_idr_) {
      if (!info.has_idr) {
        dropped_++;
        return;
      }
      wait_idr_ = false;
      need_header_ = true;
    }
    std::vector<uint8_t> frame;
    bool prepend = (need_header_ || info.has_idr) && !info.has_sps && !header_.empty();
    frame.reserve(size + (prepend ? header_.size() : 0));
    if (prepend) frame.insert(frame.end(), header_.begin(), header_.end());
    frame.insert(frame.end(), data, data + size);
    need_header_ = false;
    queue_.push_back(std::vector<uint8_t>());
    queue_.back().swap(frame);
    queue_ms_.push_back(util::NowMs());
    if (skip_frames_ && sps_info_.valid) {
      // Karenin birebir kopyasi olan, referans olmayan bos kare: cozucunun sirasini bir adim ilerletir.
      bool is_ref = false;
      int frame_num = sps::FrameNum(data, size, sps_info_, &is_ref);
      if (frame_num >= 0 && is_ref) {
        queue_.push_back(std::vector<uint8_t>());
        sps::BuildSkipFrame(sps_info_, frame_num + 1, &queue_.back());
        queue_ms_.push_back(-1);  // -1: bos kare
      }
    }
    if (static_cast<int>(queue_.size()) > max_queue_) max_queue_ = static_cast<int>(queue_.size());
    if (queue_.size() > (skip_frames_ ? 2 * kMaxQueuedFrames : kMaxQueuedFrames)) {
      dropped_ += static_cast<int>(queue_.size());
      queue_.clear();
      queue_ms_.clear();
      wait_idr_ = true;
      overflow = true;
    }
    if (!drain_scheduled_) {
      drain_scheduled_ = true;
      schedule = true;
    }
  }
  if (schedule || overflow)
    pp::Module::Get()->core()->CallOnMainThread(0, ts_factory_.NewCallback(&VideoRenderer::DrainOnMain),
                                                overflow ? 1 : 0);
}

void VideoRenderer::DrainOnMain(int32_t overflow) {
  if (overflow) Note("kuyruk tasti, anahtar kare isteniyor (cozulen " + util::Num(decoded_) + ")");
  {
    Lock l(&mu_);
    drain_scheduled_ = false;
  }
  if (overflow && on_need_idr) on_need_idr();
  Drain();
}

void VideoRenderer::Drain() {
  if (state_ != kReady || decoding_) return;
  int64_t push_ms = 0;
  {
    Lock l(&mu_);
    if (queue_.empty()) return;
    in_flight_.swap(queue_.front());
    queue_.pop_front();
    push_ms = queue_ms_.empty() ? 0 : queue_ms_.front();
    if (!queue_ms_.empty()) queue_ms_.pop_front();
  }
  int64_t now_ms = util::NowMs();
  Stamp& stamp = stamps_[next_id_ & 63];
  stamp.id = next_id_;
  stamp.push_ms = push_ms;
  stamp.decode_ms = now_ms;
  if (push_ms > 0) t_queue_.Add(now_ms - push_ms);
  decoding_ = true;
  dbgsink::MainStep("video:decode");
  if (decoded_ == 0) Note("ilk kare cozucuye veriliyor, " + util::Num(in_flight_.size()) + " bayt");
  int32_t rc = decoder_.Decode(next_id_++, static_cast<uint32_t>(in_flight_.size()), in_flight_.data(),
                               factory_.NewCallback(&VideoRenderer::OnDecodeDone));
  if (rc != PP_OK_COMPLETIONPENDING && rc != PP_OK) {
    decoding_ = false;
    Recover("decode", rc);
  }
}

void VideoRenderer::OnDecodeDone(int32_t result) {
  dbgsink::MainStep("video:decode-bitti");
  decoding_ = false;
  if (result == PP_OK && decoded_++ == 0) Note("ilk kare cozucu tarafindan kabul edildi");
  if (result == PP_ERROR_ABORTED) return;
  if (result != PP_OK) {
    Recover("decode", result);
    return;
  }
  Drain();
}

// ------------------------------------------------------------------ cikis
void VideoRenderer::GetNextPicture() {
  dbgsink::MainStep("video:resim-iste");
  decoder_.GetPicture(factory_.NewCallbackWithOutput(&VideoRenderer::OnPicture));
}

void VideoRenderer::OnPicture(int32_t result, PP_VideoPicture picture) {
  if (result == PP_ERROR_ABORTED) return;
  if (result != PP_OK) {
    Recover("picture", result);
    return;
  }
  dbgsink::MainStep("video:resim-geldi");
  if (++ok_since_error_ > 300) errors_ = 0;  // 5 sn sorunsuz: onceki hatalari unut
  if (pictures_++ == 0)
    Note("ilk resim: doku hedefi 0x" + util::Hex(reinterpret_cast<const uint8_t*>(&picture.texture_target), 4) + " (ters bayt), doku " +
         util::Num(picture.texture_size.width) + "x" + util::Num(picture.texture_size.height) + ", gorunen " +
         util::Num(picture.visible_rect.size.width) + "x" + util::Num(picture.visible_rect.size.height));
  int64_t pic_push_ms = 0;
  {
    const Stamp& stamp = stamps_[picture.decode_id & 63];
    if (stamp.id == picture.decode_id && stamp.push_ms < 0) {  // bos kare: gosterilmez
      decoder_.RecyclePicture(picture);
      skipped_window_++;
      GetNextPicture();
      return;
    }
    if (stamp.id == picture.decode_id && stamp.push_ms) {
      pic_push_ms = stamp.push_ms;
      t_decode_.Add(util::NowMs() - stamp.decode_ms);
    }
    pictures_window_++;
  }
  if (swap_pending_) {
    // Ekran onceki kareyi hala gosteriyor: bekleyen eski kareyi birak, en yenisini tut.
    if (has_queued_) {
      decoder_.RecyclePicture(queued_);
      Lock l(&mu_);
      dropped_++;
    }
    queued_ = picture;
    queued_push_ms_ = pic_push_ms;
    has_queued_ = true;
  } else {
    current_push_ms_ = pic_push_ms;
    Paint(picture);
  }
  GetNextPicture();
}

void VideoRenderer::Paint(const PP_VideoPicture& picture) {
  if (context_.is_null()) {  // grafik baglami kayboldu, kurtarma yolda: kareyi geri ver
    if (!decoder_.is_null()) decoder_.RecyclePicture(picture);
    return;
  }
  current_ = picture;
  has_current_ = true;
  dbgsink::MainStep("video:ciz");
  glSetCurrentContextPPAPI(context_.pp_resource());
  Program* p = ProgramFor(picture.texture_target);
  if (p) {
    float sx = 1.0f, sy = 1.0f;
    int vw = picture.visible_rect.size.width, vh = picture.visible_rect.size.height;
    int tw = picture.texture_size.width, th = picture.texture_size.height;
    if (picture.texture_target == GL_TEXTURE_RECTANGLE_ARB) {  // piksel koordinatlari
      sx = static_cast<float>(vw > 0 ? vw : tw);
      sy = static_cast<float>(vh > 0 ? vh : th);
    } else if (vw > 0 && vh > 0 && tw > 0 && th > 0) {  // cozucunun dolgu satirlarini kirp
      sx = static_cast<float>(vw) / tw;
      sy = static_cast<float>(vh) / th;
    }
    glViewport(0, 0, width_, height_);
    glUseProgram(p->id);
    glUniform2f(p->scale_loc, sx, sy);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(picture.texture_target, picture.texture_id);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, static_cast<const void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, reinterpret_cast<const void*>(8 * sizeof(float)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glUseProgram(0);
    pic_width_ = vw > 0 ? vw : tw;
    pic_height_ = vh > 0 ? vh : th;
    if (shot_requested_) {  // cizilen kareyi geri oku (takastan once arka tampondadir)
      shot_requested_ = false;
      shot_.assign(static_cast<size_t>(width_) * height_ * 4, '\0');
      glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, &shot_[0]);
      shot_width_ = width_;
      shot_height_ = height_;
    }
  }
  swap_pending_ = true;
  dbgsink::MainStep("video:takas");
  swap_call_ms_ = util::NowMs();
  if (painted_++ == 0) Note(std::string("ilk kare ciziliyor, golgelendirici ") + (p ? "hazir" : "DERLENEMEDI"));
  context_.SwapBuffers(swap_factory_.NewCallback(&VideoRenderer::OnSwap));
  dbgsink::MainStep("video:takas-istendi");
  Lock l(&mu_);
  shown_++;
}

void VideoRenderer::OnSwap(int32_t result) {
  dbgsink::MainStep("video:takas-bitti");
  if (result != PP_OK) Note("takas sonucu " + util::Num(result));
  {
    int64_t now_ms = util::NowMs();
    if (swap_call_ms_) t_swap_.Add(now_ms - swap_call_ms_);
    if (current_push_ms_ && has_current_) t_present_.Add(now_ms - current_push_ms_);
  }
  swap_pending_ = false;
  if (has_current_) {
    if (!decoder_.is_null()) decoder_.RecyclePicture(current_);
    has_current_ = false;
  }
  if (result == PP_ERROR_CONTEXT_LOST) {
    // Baglam ve ona bagli her sey (golgelendiriciler, tampon, cozucu dokulari) gecersiz: bir
    // sonraki Start() sifirdan kursun.
    context_ = pp::Graphics3D();
    gl_ready_ = false;
    prog_2d_ = prog_rect_ = prog_external_ = Program();
    clear_after_swap_ = false;
    if (has_queued_) {
      if (!decoder_.is_null()) decoder_.RecyclePicture(queued_);
      has_queued_ = false;
    }
    Recover("context", result, true);
    return;
  }
  if (clear_after_swap_) {
    clear_after_swap_ = false;
    ClearScreen();
    return;
  }
  if (has_queued_ && state_ == kReady) {
    has_queued_ = false;
    current_push_ms_ = queued_push_ms_;
    Paint(queued_);
  }
}

VideoRenderer::Timing VideoRenderer::TakeTiming() {
  Timing t;
  {
    Lock l(&mu_);
    t.depth = max_queue_;
    max_queue_ = 0;
  }
#define AVG(a) static_cast<int>((a).n ? (a).sum / (a).n : 0)
  t.pictures = pictures_window_;
  t.skipped = skipped_window_;
  t.queue_avg = AVG(t_queue_);
  t.queue_max = static_cast<int>(t_queue_.max);
  t.decode_avg = AVG(t_decode_);
  t.decode_max = static_cast<int>(t_decode_.max);
  t.swap_avg = AVG(t_swap_);
  t.swap_max = static_cast<int>(t_swap_.max);
  t.present_avg = AVG(t_present_);
  t.present_max = static_cast<int>(t_present_.max);
#undef AVG
  t.width = pic_width_;
  t.height = pic_height_;
  pictures_window_ = 0;
  skipped_window_ = 0;
  memset(&t_queue_, 0, sizeof(t_queue_));
  memset(&t_decode_, 0, sizeof(t_decode_));
  memset(&t_present_, 0, sizeof(t_present_));
  memset(&t_swap_, 0, sizeof(t_swap_));
  return t;
}

bool VideoRenderer::TakeShot(std::string* rgba, int* width, int* height) {
  if (shot_.empty()) return false;
  rgba->swap(shot_);
  shot_.clear();
  *width = shot_width_;
  *height = shot_height_;
  return true;
}

void VideoRenderer::TakeStats(int* shown, int* dropped) {
  Lock l(&mu_);
  *shown = shown_;
  *dropped = dropped_;
  shown_ = dropped_ = 0;
}
