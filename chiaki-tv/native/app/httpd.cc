#include "httpd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ppapi/c/pp_errors.h"
#include "ppapi/cpp/net_address.h"

#include <mbedtls/sha1.h>

#include "util.h"

namespace {

const size_t kMaxRequestBytes = 64 * 1024;
const size_t kMaxClients = 8;
const int64_t kClientTimeoutMs = 6000;
const int64_t kWsIdleMs = 12000;         // WebSocket: bu kadar sure hic cerceve gelmezse kapat
const size_t kMaxWsClients = 3;
const size_t kMaxWsPayload = 4096;
const size_t kMaxWsPending = 64 * 1024;
const char kWsGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const int64_t kRetryMs = 5000;
const int64_t kGraveMs = 2000;

const char* StatusText(int status) {
  switch (status) {
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 503: return "Service Unavailable";
    default: return "Error";
  }
}

std::string Lower(std::string s) {
  for (size_t i = 0; i < s.size(); i++)
    if (s[i] >= 'A' && s[i] <= 'Z') s[i] = static_cast<char>(s[i] - 'A' + 'a');
  return s;
}

}  // namespace

SetupServer::SetupServer(pp::Instance* instance, const Handler& handler)
    : instance_(instance), handler_(handler), factory_(this), port_(0), listening_(false), retry_at_ms_(0), next_id_(1) {}

SetupServer::~SetupServer() {
  factory_.CancelAll();
  for (std::map<int, Client*>::iterator it = clients_.begin(); it != clients_.end(); ++it) delete it->second;
  for (size_t i = 0; i < closed_.size(); i++) delete closed_[i].second;
}

void SetupServer::Start(uint16_t port) {
  port_ = port;
  listening_ = false;
  retry_at_ms_ = 0;
  if (!pp::TCPSocket::IsAvailable()) return;
  listener_ = pp::TCPSocket(instance_);
  if (listener_.is_null()) return;
  PP_NetAddress_IPv4 any;
  memset(&any, 0, sizeof(any));
  any.port = static_cast<uint16_t>(((port & 0xff) << 8) | (port >> 8));  // ag bayt sirasi
  listener_.Bind(pp::NetAddress(instance_, any), factory_.NewCallback(&SetupServer::OnBound));
}

void SetupServer::OnBound(int32_t result) {
  if (result != PP_OK) {
    retry_at_ms_ = util::NowMs() + kRetryMs;
    return;
  }
  listener_.Listen(8, factory_.NewCallback(&SetupServer::OnListening));
}

void SetupServer::OnListening(int32_t result) {
  if (result != PP_OK) {
    retry_at_ms_ = util::NowMs() + kRetryMs;
    return;
  }
  listening_ = true;
  AcceptNext();
}

void SetupServer::AcceptNext() {
  listener_.Accept(factory_.NewCallbackWithOutput(&SetupServer::OnAccepted));
}

void SetupServer::OnAccepted(int32_t result, pp::TCPSocket socket) {
  if (result != PP_OK) {
    if (result == PP_ERROR_ABORTED) return;
    listening_ = false;
    retry_at_ms_ = util::NowMs() + kRetryMs;
    return;
  }
  if (clients_.size() >= kMaxClients) {
    socket.Close();  // dolu: yeni geleni geri cevir
  } else {
    Client* client = new Client();
    client->id = next_id_++;
    client->sock = socket;
    client->sent = 0;
    client->scan = 0;
    client->need = 0;
    client->ws = false;
    client->writing = false;
    client->closing = false;
    client->since_ms = util::NowMs();
    clients_[client->id] = client;
    ReadMore(client);
  }
  AcceptNext();
}

SetupServer::Client* SetupServer::Find(int id) {
  std::map<int, Client*>::iterator it = clients_.find(id);
  return it == clients_.end() ? NULL : it->second;
}

void SetupServer::ReadMore(Client* client) {
  client->sock.Read(client->buf, sizeof(client->buf), factory_.NewCallback(&SetupServer::OnRead, client->id));
}

void SetupServer::OnRead(int32_t result, int id) {
  Client* client = Find(id);
  if (!client) return;
  if (result <= 0) {
    Close(id);
    return;
  }
  client->in.append(client->buf, static_cast<size_t>(result));
  if (client->ws) {
    OnWsData(id);
    return;
  }
  if (client->in.size() > kMaxRequestBytes) {
    HttpResponse too_big;
    too_big.status = 413;
    too_big.body = "{\"ok\":false,\"error\":\"istek cok buyuk\"}";
    Respond(client, too_big);
    return;
  }
  // Bayt bayt damlatilan bir istek her okumada tum tamponu yeniden ayristirtmasin: baslik sonunu
  // yalnizca yeni gelen kisimda ara, govde beklenirken hic ayristirma.
  if (client->need) {
    if (client->in.size() < client->need) {
      ReadMore(client);
      return;
    }
  } else {
    size_t from = client->scan > 3 ? client->scan - 3 : 0;
    if (client->in.find("\r\n\r\n", from) == std::string::npos) {
      client->scan = client->in.size();
      ReadMore(client);
      return;
    }
  }
  HttpRequest request;
  if (!RequestComplete(client->in, &request, &client->need)) {
    ReadMore(client);
    return;
  }
  std::map<std::string, std::string>::const_iterator up = request.headers.find("upgrade");
  if (up != request.headers.end() && Lower(up->second) == "websocket") {
    Upgrade(client, request);
    return;
  }
  HttpResponse response;
  handler_(request, &response);
  Respond(client, response);
}

// ------------------------------------------------------------------ WebSocket (RFC 6455, yalnizca gereken kadari)
void SetupServer::Upgrade(Client* client, const HttpRequest& request) {
  std::map<std::string, std::string>::const_iterator key = request.headers.find("sec-websocket-key");
  HttpResponse refuse;
  refuse.body = "{\"ok\":false}";
  if (request.method != "GET" || key == request.headers.end() || key->second.size() > 64) refuse.status = 400;
  else if (!ws_.accept || !ws_.accept(request)) refuse.status = 403;
  else {
    // Kabul isleyicisi yer acmak icin eski bir baglantiyi kapatmis olabilir: sayimi ondan sonra yap.
    size_t ws_count = 0;
    for (std::map<int, Client*>::iterator it = clients_.begin(); it != clients_.end(); ++it)
      if (it->second->ws) ws_count++;
    if (ws_count >= kMaxWsClients) refuse.status = 503;
  }
  if (refuse.status != 200) {
    Respond(client, refuse);
    return;
  }
  std::string accept_src = key->second + kWsGuid;
  unsigned char digest[20];
  mbedtls_sha1_ret(reinterpret_cast<const unsigned char*>(accept_src.data()), accept_src.size(), digest);
  client->ws = true;
  client->in.erase(0, client->need);  // basliklardan sonra gelmis cerceve baytlari (varsa) kalsin
  client->since_ms = util::NowMs();
  client->out = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                "Sec-WebSocket-Accept: " + util::Base64Encode(digest, sizeof(digest)) + "\r\n\r\n";
  client->sent = 0;
  client->writing = true;
  int id = client->id;
  WriteMore(client);
  if (ws_.open) ws_.open(id);
  if (Find(id)) OnWsData(id);  // ayni anda okumayi da baslatir
}

// Tampondaki tam cerceveleri isler, sonra okumayi surdurur.
void SetupServer::OnWsData(int id) {
  for (;;) {
    Client* client = Find(id);
    if (!client) return;  // bir isleyici baglantiyi kapatti
    const std::string& in = client->in;
    if (in.size() < 2) break;
    uint8_t b0 = static_cast<uint8_t>(in[0]), b1 = static_cast<uint8_t>(in[1]);
    size_t len = b1 & 0x7f, header = 2;
    if (len == 126) {
      if (in.size() < 4) break;
      len = (static_cast<size_t>(static_cast<uint8_t>(in[2])) << 8) | static_cast<uint8_t>(in[3]);
      header = 4;
    }
    // Istemci cerceveleri maskeli olmali; parcali ya da buyuk cerceve bu sunucuda kullanilmiyor.
    if (!(b1 & 0x80) || !(b0 & 0x80) || (b1 & 0x7f) == 127 || len > kMaxWsPayload) {
      Close(id);
      return;
    }
    if (in.size() < header + 4 + len) break;
    std::string payload(in, header + 4, len);
    for (size_t i = 0; i < len; i++) payload[i] = static_cast<char>(payload[i] ^ in[header + (i & 3)]);
    int opcode = b0 & 0x0f;
    client->in.erase(0, header + 4 + len);
    client->since_ms = util::NowMs();
    if (opcode == 1 || opcode == 2) {
      if (ws_.message) ws_.message(id, opcode == 2, payload);
    } else if (opcode == 9) {
      WsFrame(client, 10, payload);  // ping -> pong
    } else if (opcode == 8) {
      Close(id);
      return;
    } else if (opcode != 10) {
      Close(id);
      return;
    }
  }
  Client* client = Find(id);
  if (client) ReadMore(client);
}

void SetupServer::WsFrame(Client* client, int opcode, const std::string& payload) {
  if (client->closing) return;
  std::string frame;
  frame += static_cast<char>(0x80 | opcode);
  if (payload.size() < 126) {
    frame += static_cast<char>(payload.size());
  } else {
    frame += static_cast<char>(126);
    frame += static_cast<char>((payload.size() >> 8) & 0xff);
    frame += static_cast<char>(payload.size() & 0xff);
  }
  frame += payload;
  if (client->writing) {
    if (client->pending.size() + frame.size() > kMaxWsPending) {  // karsi taraf okumuyor
      Close(client->id);
      return;
    }
    client->pending += frame;
    return;
  }
  client->out.swap(frame);
  client->sent = 0;
  client->writing = true;
  WriteMore(client);
}

void SetupServer::WsSend(int id, const std::string& text) {
  Client* client = Find(id);
  if (client && client->ws && text.size() < 65536) WsFrame(client, 1, text);
}

void SetupServer::WsCloseAfterSend(int id) {
  Client* client = Find(id);
  if (!client || !client->ws) return;
  if (client->writing) client->closing = true;  // WriteMore kuyruk bosalinca kapatir
  else Close(id);
}

void SetupServer::WsClose(int id) {
  Client* client = Find(id);
  if (client && client->ws) Close(id);
}

// Basliklar ve (varsa) govde tamam mi? Tamamsa istegi ayristirir.
bool SetupServer::RequestComplete(const std::string& in, HttpRequest* request, size_t* need) {
  size_t header_end = in.find("\r\n\r\n");
  if (header_end == std::string::npos) return false;
  size_t line_end = in.find("\r\n");
  std::string first = in.substr(0, line_end);
  size_t sp1 = first.find(' '), sp2 = first.rfind(' ');
  if (sp1 == std::string::npos || sp2 == sp1) {
    request->method = "?";  // bozuk istek satiri: isleyici 404 doner
    return true;
  }
  request->method = first.substr(0, sp1);
  std::string target = first.substr(sp1 + 1, sp2 - sp1 - 1);
  request->path = target.substr(0, target.find('?'));
  size_t pos = line_end + 2;
  while (pos < header_end) {
    size_t eol = in.find("\r\n", pos);
    if (eol == std::string::npos || eol > header_end) eol = header_end;
    std::string line = in.substr(pos, eol - pos);
    size_t colon = line.find(':');
    if (colon != std::string::npos)
      request->headers[Lower(line.substr(0, colon))] = util::Trim(line.substr(colon + 1));
    pos = eol + 2;
  }
  size_t content_length = 0;
  std::map<std::string, std::string>::const_iterator it = request->headers.find("content-length");
  if (it != request->headers.end()) content_length = static_cast<size_t>(strtoul(it->second.c_str(), NULL, 10));
  if (content_length > kMaxRequestBytes) content_length = kMaxRequestBytes;
  size_t body_start = header_end + 4;
  *need = body_start + content_length;
  if (in.size() < *need) return false;
  request->body = in.substr(body_start, content_length);
  return true;
}

void SetupServer::Respond(Client* client, const HttpResponse& response) {
  char head[320];
  // Icerik turu yalnizca sunucunun kendi sabitlerinden gelir (istemci girdisi basliga yazilmaz).
  snprintf(head, sizeof(head),
           "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\n"
           "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nConnection: close\r\n\r\n",
           response.status, StatusText(response.status), response.type.c_str(),
           static_cast<unsigned>(response.body.size()));
  client->out = std::string(head) + response.body;
  client->sent = 0;
  WriteMore(client);
}

void SetupServer::WriteMore(Client* client) {
  if (client->sent >= client->out.size()) {
    if (!client->ws) {
      Close(client->id);
      return;
    }
    // WebSocket: baglanti acik kalir; birikmis cerceve varsa onu gonder.
    client->out.clear();
    client->sent = 0;
    if (client->pending.empty()) {
      client->writing = false;
      if (client->closing) Close(client->id);
      return;
    }
    client->out.swap(client->pending);
  }
  size_t chunk = client->out.size() - client->sent;
  if (chunk > 32 * 1024) chunk = 32 * 1024;
  client->sock.Write(client->out.data() + client->sent, static_cast<int32_t>(chunk),
                     factory_.NewCallback(&SetupServer::OnWritten, client->id));
}

void SetupServer::OnWritten(int32_t result, int id) {
  Client* client = Find(id);
  if (!client) return;
  if (result <= 0) {
    Close(id);
    return;
  }
  client->sent += static_cast<size_t>(result);
  WriteMore(client);
}

void SetupServer::Close(int id) {
  std::map<int, Client*>::iterator it = clients_.find(id);
  if (it == clients_.end()) return;
  Client* client = it->second;
  clients_.erase(it);
  client->sock.Close();  // bekleyen okuma/yazma iptal edilir; geri cagrisi Find() ile bos doner
  closed_.push_back(std::make_pair(util::NowMs(), client));
  if (client->ws && ws_.close) ws_.close(id);
}

void SetupServer::Tick() {
  int64_t now = util::NowMs();
  std::vector<int> stale;
  for (std::map<int, Client*>::iterator it = clients_.begin(); it != clients_.end(); ++it)
    if (now - it->second->since_ms > (it->second->ws ? kWsIdleMs : kClientTimeoutMs)) stale.push_back(it->first);
  for (size_t i = 0; i < stale.size(); i++) Close(stale[i]);
  size_t keep = 0;
  for (size_t i = 0; i < closed_.size(); i++) {
    if (now - closed_[i].first > kGraveMs) delete closed_[i].second;
    else closed_[keep++] = closed_[i];
  }
  closed_.resize(keep);
  if (!listening_ && retry_at_ms_ && now >= retry_at_ms_ && port_) Start(port_);
}
