// chiaki-tv / asama 1: TV'deki NaCl modulunden ag erisimini sinar.
// JS'ten gelen komutlar (bosluk ayracli metin):
//   info                      -> soket arayuzleri var mi
//   udp <ip> <port> <hex>     -> UDP gonder, gelen ilk cevabi bildir
//   tcp <ip> <port>           -> TCP baglan
//   resolve <alanadi>         -> DNS coz
//   listen <port>             -> TCP dinle; gelen HTTP istegine kisa cevap ver
// Her sonuc PostMessage ile "etiket: mesaj" metni olarak JS'e doner.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sstream>
#include <string>
#include <vector>

#include "ppapi/c/pp_errors.h"
#include "ppapi/c/ppb_graphics_3d.h"
#include "ppapi/cpp/graphics_3d.h"
#include "ppapi/cpp/host_resolver.h"
#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/module.h"
#include "ppapi/cpp/net_address.h"
#include "ppapi/cpp/tcp_socket.h"
#include "ppapi/cpp/udp_socket.h"
#include "ppapi/cpp/var.h"
#include "ppapi/cpp/video_decoder.h"
#include "ppapi/utility/completion_callback_factory.h"

#ifdef PostMessage
#undef PostMessage
#endif

namespace {

std::vector<std::string> Split(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  std::string tok;
  while (in >> tok) out.push_back(tok);
  return out;
}

std::string ToHex(const char* data, int n) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  for (int i = 0; i < n; i++) {
    unsigned char c = static_cast<unsigned char>(data[i]);
    out += kDigits[c >> 4];
    out += kDigits[c & 15];
  }
  return out;
}

std::string FromHex(const std::string& hex) {
  std::string out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2)
    out += static_cast<char>(strtol(hex.substr(i, 2).c_str(), NULL, 16));
  return out;
}

std::string Num(int32_t v) {
  std::ostringstream s;
  s << v;
  return s.str();
}

// Sik gorulen Pepper hata kodlarinin adlari.
std::string Err(int32_t code) {
  switch (code) {
    case PP_OK: return "OK";
    case PP_ERROR_FAILED: return "FAILED(-2)";
    case PP_ERROR_ABORTED: return "ABORTED(-3)";
    case PP_ERROR_BADARGUMENT: return "BADARGUMENT(-4)";
    case PP_ERROR_NOINTERFACE: return "NOINTERFACE(-5)";
    case PP_ERROR_NOACCESS: return "NOACCESS(-7)";
    case PP_ERROR_NOTSUPPORTED: return "NOTSUPPORTED(-12)";
    case PP_ERROR_CONNECTION_REFUSED: return "CONNECTION_REFUSED(-102)";
    case PP_ERROR_CONNECTION_TIMEDOUT: return "CONNECTION_TIMEDOUT(-105)";
    case PP_ERROR_ADDRESS_UNREACHABLE: return "ADDRESS_UNREACHABLE(-107)";
    case PP_ERROR_ADDRESS_IN_USE: return "ADDRESS_IN_USE(-108)";
    case PP_ERROR_NAME_NOT_RESOLVED: return "NAME_NOT_RESOLVED(-110)";
    default: return "hata(" + Num(code) + ")";
  }
}

bool ParseIPv4(const std::string& ip, int port, PP_NetAddress_IPv4* out) {
  unsigned a, b, c, d;
  if (sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
  out->port = static_cast<uint16_t>(((port & 0xff) << 8) | ((port >> 8) & 0xff));  // ag bayt sirasi
  out->addr[0] = a; out->addr[1] = b; out->addr[2] = c; out->addr[3] = d;
  return true;
}

}  // namespace

class ProbeInstance : public pp::Instance {
 public:
  explicit ProbeInstance(PP_Instance instance) : pp::Instance(instance), factory_(this) {}

  virtual void HandleMessage(const pp::Var& var) {
    if (!var.is_string()) return;
    std::vector<std::string> a = Split(var.AsString());
    if (a.empty()) return;
    if (a[0] == "info") {
      Log(std::string("info: udp=") + (pp::UDPSocket::IsAvailable() ? "var" : "yok") +
          " tcp=" + (pp::TCPSocket::IsAvailable() ? "var" : "yok") +
          " dns=" + (pp::HostResolver::IsAvailable() ? "var" : "yok"));
    } else if (a[0] == "udp" && a.size() >= 4) {
      Udp(a[1], atoi(a[2].c_str()), FromHex(a[3]));
    } else if (a[0] == "tcp" && a.size() >= 3) {
      Tcp(a[1], atoi(a[2].c_str()));
    } else if (a[0] == "resolve" && a.size() >= 2) {
      Resolve(a[1]);
    } else if (a[0] == "listen" && a.size() >= 2) {
      Listen(atoi(a[1].c_str()));
    } else if (a[0] == "iface") {
      // Verilen Pepper arayuz adlarindan hangilerini TV sunuyor?
      std::string out = "iface:";
      for (size_t i = 1; i < a.size(); i++)
        out += " " + a[i] + "=" + (pp::Module::Get()->GetBrowserInterface(a[i].c_str()) ? "1" : "0");
      Log(out);
    } else if (a[0] == "video" && a.size() >= 3) {
      Video(atoi(a[1].c_str()), atoi(a[2].c_str()));
    } else {
      Log("hata: bilinmeyen komut " + a[0]);
    }
  }

 private:
  void Log(const std::string& s) { PostMessage(pp::Var(s)); }

  // ---- UDP ----
  void Udp(const std::string& ip, int port, const std::string& payload) {
    PP_NetAddress_IPv4 remote, any;
    memset(&any, 0, sizeof(any));
    if (!ParseIPv4(ip, port, &remote)) { Log("udp: gecersiz adres"); return; }
    udp_.Close();
    udp_ = pp::UDPSocket(this);
    if (udp_.is_null()) { Log("udp: soket olusturulamadi"); return; }
    udp_remote_ = pp::NetAddress(this, remote);
    udp_payload_ = payload;
    udp_.Bind(pp::NetAddress(this, any), factory_.NewCallback(&ProbeInstance::OnUdpBound));
  }
  void OnUdpBound(int32_t result) {
    if (result != PP_OK) { Log("udp: baglama basarisiz " + Err(result)); return; }
    Log("udp: yerel adres " + udp_.GetBoundAddress().DescribeAsString(true).AsString());
    udp_.SendTo(udp_payload_.data(), udp_payload_.size(), udp_remote_,
                factory_.NewCallback(&ProbeInstance::OnUdpSent));
  }
  void OnUdpSent(int32_t result) {
    if (result < 0) { Log("udp: gonderim basarisiz " + Err(result)); return; }
    Log("udp: gonderildi " + Num(result) + " bayt");
    udp_.RecvFrom(udp_buf_, sizeof(udp_buf_),
                  factory_.NewCallbackWithOutput(&ProbeInstance::OnUdpRecv));
  }
  void OnUdpRecv(int32_t result, pp::NetAddress from) {
    if (result < 0) { Log("udp: alim " + Err(result)); return; }
    Log("udp: alindi " + Num(result) + " bayt, kaynak " + from.DescribeAsString(true).AsString() +
        ", veri " + ToHex(udp_buf_, result));
  }

  // ---- TCP istemci ----
  void Tcp(const std::string& ip, int port) {
    PP_NetAddress_IPv4 remote;
    if (!ParseIPv4(ip, port, &remote)) { Log("tcp: gecersiz adres"); return; }
    tcp_.Close();
    tcp_ = pp::TCPSocket(this);
    if (tcp_.is_null()) { Log("tcp: soket olusturulamadi"); return; }
    tcp_.Connect(pp::NetAddress(this, remote), factory_.NewCallback(&ProbeInstance::OnTcpConnected));
  }
  void OnTcpConnected(int32_t result) {
    if (result != PP_OK) { Log("tcp: baglanti basarisiz " + Err(result)); return; }
    Log("tcp: baglandi, yerel " + tcp_.GetLocalAddress().DescribeAsString(true).AsString());
    static const char kHello[] = "chiaki-tv tcp testi\n";
    tcp_.Write(kHello, sizeof(kHello) - 1, factory_.NewCallback(&ProbeInstance::OnTcpWritten));
  }
  void OnTcpWritten(int32_t result) {
    Log(result < 0 ? "tcp: yazma basarisiz " + Err(result) : "tcp: yazildi " + Num(result) + " bayt");
  }

  // ---- DNS ----
  void Resolve(const std::string& host) {
    resolver_ = pp::HostResolver(this);
    if (resolver_.is_null()) { Log("dns: cozucu olusturulamadi"); return; }
    PP_HostResolver_Hint hint = { PP_NETADDRESS_FAMILY_IPV4, 0 };
    resolver_.Resolve(host.c_str(), 0, hint, factory_.NewCallback(&ProbeInstance::OnResolved));
  }
  void OnResolved(int32_t result) {
    if (result != PP_OK) { Log("dns: basarisiz " + Err(result)); return; }
    Log("dns: " + resolver_.GetNetAddress(0).DescribeAsString(false).AsString());
  }

  // ---- TCP sunucu (telefondan kurulum sayfasi icin on kosul) ----
  void Listen(int port) {
    PP_NetAddress_IPv4 any;
    if (!ParseIPv4("0.0.0.0", port, &any)) return;
    listener_.Close();
    listener_ = pp::TCPSocket(this);
    if (listener_.is_null()) { Log("dinle: soket olusturulamadi"); return; }
    listener_.Bind(pp::NetAddress(this, any), factory_.NewCallback(&ProbeInstance::OnListenBound));
  }
  void OnListenBound(int32_t result) {
    if (result != PP_OK) { Log("dinle: baglama basarisiz " + Err(result)); return; }
    listener_.Listen(4, factory_.NewCallback(&ProbeInstance::OnListening));
  }
  void OnListening(int32_t result) {
    if (result != PP_OK) { Log("dinle: listen basarisiz " + Err(result)); return; }
    Log("dinle: hazir " + listener_.GetLocalAddress().DescribeAsString(true).AsString());
    AcceptNext();
  }
  void AcceptNext() {
    listener_.Accept(factory_.NewCallbackWithOutput(&ProbeInstance::OnAccepted));
  }
  void OnAccepted(int32_t result, pp::TCPSocket client) {
    if (result != PP_OK) { Log("dinle: accept " + Err(result)); return; }
    client_ = client;
    Log("dinle: baglanti geldi " + client_.GetRemoteAddress().DescribeAsString(true).AsString());
    client_.Read(http_buf_, sizeof(http_buf_) - 1, factory_.NewCallback(&ProbeInstance::OnClientRead));
  }
  void OnClientRead(int32_t result) {
    if (result <= 0) { Log("dinle: okuma " + Err(result)); client_.Close(); AcceptNext(); return; }
    http_buf_[result] = 0;
    std::string first(http_buf_);
    first = first.substr(0, first.find('\r'));
    Log("dinle: istek \"" + first + "\"");
    static const char kBody[] = "chiaki-tv: televizyon HTTP sunucusu calisiyor\n";
    std::ostringstream r;
    r << "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: "
      << (sizeof(kBody) - 1) << "\r\nConnection: close\r\n\r\n" << kBody;
    http_reply_ = r.str();
    client_.Write(http_reply_.data(), http_reply_.size(), factory_.NewCallback(&ProbeInstance::OnClientWritten));
  }
  void OnClientWritten(int32_t result) {
    Log(result < 0 ? "dinle: yazma " + Err(result) : "dinle: cevap gonderildi " + Num(result) + " bayt");
    client_.Close();
    AcceptNext();
  }

  // ---- donanim video cozucusu var mi (H.264) ----
  void Video(int profile, int accel) {
    if (g3d_.is_null()) {
      const int32_t attribs[] = {PP_GRAPHICS3DATTRIB_ALPHA_SIZE, 0, PP_GRAPHICS3DATTRIB_DEPTH_SIZE, 0,
                                 PP_GRAPHICS3DATTRIB_WIDTH, 1280, PP_GRAPHICS3DATTRIB_HEIGHT, 720,
                                 PP_GRAPHICS3DATTRIB_NONE};
      g3d_ = pp::Graphics3D(this, attribs);
      if (g3d_.is_null()) { Log("video: Graphics3D olusturulamadi"); return; }
      Log(std::string("video: Graphics3D var, baglama=") + (BindGraphics(g3d_) ? "tamam" : "olmadi"));
    }
    decoder_ = pp::VideoDecoder(this);
    if (decoder_.is_null()) { Log("video: VideoDecoder olusturulamadi"); return; }
    video_tag_ = "profil=" + Num(profile) + " hizlandirma=" + Num(accel);
    decoder_.Initialize(g3d_, static_cast<PP_VideoProfile>(profile),
                        static_cast<PP_HardwareAcceleration>(accel), 0,
                        factory_.NewCallback(&ProbeInstance::OnVideoInit));
  }
  void OnVideoInit(int32_t result) { Log("video: Initialize " + video_tag_ + " -> " + Err(result)); }

  pp::CompletionCallbackFactory<ProbeInstance> factory_;
  pp::Graphics3D g3d_;
  pp::VideoDecoder decoder_;
  std::string video_tag_;
  pp::UDPSocket udp_;
  pp::NetAddress udp_remote_;
  std::string udp_payload_;
  char udp_buf_[2048];
  pp::TCPSocket tcp_;
  pp::HostResolver resolver_;
  pp::TCPSocket listener_;
  pp::TCPSocket client_;
  char http_buf_[2048];
  std::string http_reply_;
};

class ProbeModule : public pp::Module {
 public:
  virtual pp::Instance* CreateInstance(PP_Instance instance) { return new ProbeInstance(instance); }
};

namespace pp {
Module* CreateModule() { return new ProbeModule(); }
}  // namespace pp
