// chiaki-tv: TV'nin kendi sundugu kucuk HTTP sunucusu (telefondan kurulum sayfasi icin).
// Pepper TCP soketi uzerinde, ana is parcaciginda, geri cagrilarla calisir; ayni anda birkac
// istemciyi isler (tarayicilar bosta bekleyen ek baglantilar acar; biri digerlerini kilitlememeli).
#ifndef CHIAKI_TV_HTTPD_H
#define CHIAKI_TV_HTTPD_H

#include <stdint.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/tcp_socket.h"
#include "ppapi/utility/completion_callback_factory.h"

struct HttpRequest {
  std::string method;
  std::string path;  // sorgu dizgesi haric
  std::string body;
  std::map<std::string, std::string> headers;  // adlar kucuk harf
};

struct HttpResponse {
  int status;
  std::string type;
  std::string body;
  HttpResponse() : status(200), type("application/json; charset=utf-8") {}
};

// WebSocket baglantilari icin geri cagrilar (hepsi ana is parcaciginda, beklememeli).
struct WsHooks {
  std::function<bool(const HttpRequest&)> accept;  // yukseltme istegi kabul edilsin mi?
  std::function<void(int id)> open;
  std::function<void(int id, bool binary, const std::string& payload)> message;
  std::function<void(int id)> close;
};

class SetupServer {
 public:
  // Isleyici ana is parcaciginda calisir ve beklememelidir.
  typedef std::function<void(const HttpRequest&, HttpResponse*)> Handler;

  // WebSocket destegi (oyun kolu sayfasi). Start()'tan once cagrilir.
  void SetWebSocket(const WsHooks& hooks) { ws_ = hooks; }
  void WsSend(int id, const std::string& text);  // metin cercevesi
  void WsClose(int id);
  // Kuyruktaki cerceveler (ornegin ret nedeni) gonderildikten sonra kapatir.
  void WsCloseAfterSend(int id);

  SetupServer(pp::Instance* instance, const Handler& handler);
  ~SetupServer();

  void Start(uint16_t port);
  bool listening() const { return listening_; }

  // Duzenli araliklarla cagrilir: takilan baglantilari kapatir, dinleme dustuyse yeniden kurar.
  void Tick();

 private:
  struct Client {
    int id;
    pp::TCPSocket sock;
    std::string in;
    std::string out;
    size_t sent;
    size_t scan;  // baslik sonu bu konuma kadar arandi
    size_t need;  // 0: basliklar henuz tamam degil; degilse istegin toplam bayt sayisi
    int64_t since_ms;
    bool ws;              // WebSocket'e yukseltildi: baglanti acik kalir
    bool writing;         // (ws) bir yazma suruyor
    std::string pending;  // (ws) suren yazmadan sonra gidecek cerceveler
    bool closing;         // (ws) yazma bitince kapat
    char buf[4096];
  };

  void OnBound(int32_t result);
  void OnListening(int32_t result);
  void AcceptNext();
  void OnAccepted(int32_t result, pp::TCPSocket socket);
  Client* Find(int id);
  void ReadMore(Client* client);
  void OnRead(int32_t result, int id);
  bool RequestComplete(const std::string& in, HttpRequest* request, size_t* need);
  void Respond(Client* client, const HttpResponse& response);
  void Upgrade(Client* client, const HttpRequest& request);
  void OnWsData(int id);
  void WsFrame(Client* client, int opcode, const std::string& payload);
  void WriteMore(Client* client);
  void OnWritten(int32_t result, int id);
  void Close(int id);

  pp::Instance* instance_;
  Handler handler_;
  WsHooks ws_;
  pp::CompletionCallbackFactory<SetupServer> factory_;
  pp::TCPSocket listener_;
  uint16_t port_;
  bool listening_;
  int64_t retry_at_ms_;
  int next_id_;
  // Geri cagrilar istemciyi isaretciyle degil kimlikle bulur: kapanmis istemciye gelen gec
  // (iptal edilmis) geri cagri bos doner.
  std::map<int, Client*> clients_;
  // Kapatilan istemciler hemen silinmez; bekleyen okuma tamponlari gecerli kalsin diye Tick'te silinir.
  std::vector<std::pair<int64_t, Client*> > closed_;
};

#endif
