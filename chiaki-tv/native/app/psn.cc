#include "psn.h"

#include <stdlib.h>
#include <string.h>

#include <vector>

#include <curl/curl.h>

#include "i18n.h"
#include "util.h"

namespace psn {

namespace {

// PS Remote Play istemci kimligi; chiaki-ng ile ayni (gui/include/psnaccountid.h).
const char kClientId[] = "ba495a24-818c-472b-b12d-ff231c1b5745";
const char kClientSecret[] = "mvaiZkRsAsI1IBkY";
const char kTokenUrl[] = "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token";
const char kRedirectUri[] = "https://remoteplay.dl.playstation.net/remoteplay/redirect";
const char kScope[] =
    "psn:clientapp referenceDataService:countryConfig.read pushNotification:webSocket.desktop.connect "
    "sessionManager:remotePlaySession.system.update";

struct Response {
  long status;
  std::string body;
  std::string error;
  Response() : status(0) {}
};

void (*g_log)(const std::string&, void*) = NULL;
void* g_log_user = NULL;

int DebugCb(CURL*, curl_infotype type, char* data, size_t size, void*) {
  if (type != CURLINFO_TEXT || !g_log) return 0;  // basliklar ve govde belirtec icerir: yazma
  std::string line(data, size);
  while (!line.empty() && (line[line.size() - 1] == '\n' || line[line.size() - 1] == '\r')) line.resize(line.size() - 1);
  g_log("curl: " + line, g_log_user);
  return 0;
}

size_t WriteCb(char* ptr, size_t size, size_t nmemb, void* user) {
  std::string* out = static_cast<std::string*>(user);
  size_t n = size * nmemb;
  if (out->size() + n > (1 << 20)) return 0;  // 1 MB ustu cevap beklenmez
  out->append(ptr, n);
  return n;
}

std::string BasicAuth() {
  std::string pair = std::string(kClientId) + ":" + kClientSecret;
  return "Authorization: Basic " + util::Base64Encode(reinterpret_cast<const uint8_t*>(pair.data()), pair.size());
}

// post_body bos ise GET, degilse form govdeli POST.
Response Request(const std::string& url, const std::string& post_body) {
  Response res;
  CURL* curl = curl_easy_init();
  if (!curl) {
    res.error = L("Ağ katmanı başlatılamadı.");
    return res;
  }
  std::string auth = BasicAuth();
  struct curl_slist* headers = curl_slist_append(NULL, auth.c_str());
  headers = curl_slist_append(headers, post_body.empty() ? "Content-Type: application/json"
                                                         : "Content-Type: application/x-www-form-urlencoded");
  char errbuf[CURL_ERROR_SIZE];
  errbuf[0] = 0;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
  if (g_log) {
    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, DebugCb);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
  }
  if (!post_body.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(post_body.size()));
  }
  CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK) {
    res.error = std::string(L("Ağ hatası: ")) + (errbuf[0] ? errbuf : curl_easy_strerror(rc));
  } else {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.status);
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return res;
}

bool ParseTokens(const Response& res, Tokens* out, std::string* error) {
  if (!res.error.empty()) {
    *error = res.error;
    return false;
  }
  json_object* obj = json_tokener_parse(res.body.c_str());
  if (!obj) {
    *error = L("PSN beklenmeyen bir cevap verdi (HTTP ") + util::Num(res.status) + ").";
    return false;
  }
  out->access = util::JStr(obj, "access_token");
  out->refresh = util::JStr(obj, "refresh_token");
  int64_t expires_in = util::JInt(obj, "expires_in", 0);
  std::string err_desc = util::JStr(obj, "error_description", util::JStr(obj, "error"));
  json_object_put(obj);
  if (res.status < 200 || res.status >= 300 || out->access.empty()) {
    *error = L("PSN oturumu açılamadı (HTTP ") + util::Num(res.status) + ")" +
             (err_desc.empty() ? "" : ": " + err_desc);
    return false;
  }
  out->expiry = util::NowSec() + (expires_in > 0 ? expires_in : 3600);
  return true;
}

std::string TokenBody(const std::string& grant) {
  return grant + "&scope=" + util::UrlEncode(kScope) + "&redirect_uri=" + util::UrlEncode(kRedirectUri) + "&";
}

}  // namespace

void SetLogger(void (*log)(const std::string& line, void* user), void* user) {
  g_log = log;
  g_log_user = user;
}

std::string LoginUrl(const std::string& client_duid) {
  return std::string("https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/authorize"
                     "?service_entity=urn:service-entity:psn&response_type=code&client_id=") +
         kClientId + "&redirect_uri=" + kRedirectUri + "&scope=" + util::UrlEncode(kScope) +
         "&request_locale=en_US&ui=pr&service_logo=ps&layout_type=popup&smcid=remoteplay&prompt=always"
         "&PlatformPrivacyWs1=minimal&duid=" + client_duid + "&";
}

std::string ExtractCode(const std::string& redirect_or_code) {
  std::string s = util::Trim(redirect_or_code);
  // Yalnizca sorgu parametresi olan "code": "error_code=" gibi adlarla eslesmesin.
  size_t pos = s.find("?code=");
  if (pos == std::string::npos) pos = s.find("&code=");
  if (pos == std::string::npos) {
    // Adres degilse kullanici yalnizca kodu yapistirmis olabilir; adres ise kod yok demektir.
    return s.find("://") == std::string::npos && s.find('=') == std::string::npos ? s : std::string();
  }
  pos += 6;
  size_t end = s.find_first_of("&# \r\n", pos);
  return s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

bool ExchangeCode(const std::string& code, Tokens* out, std::string* error) {
  if (code.empty()) {
    *error = L("Yönlendirme adresinde giriş kodu yok. Sony girişinden sonra açılan sayfanın adresini yapıştır.");
    return false;
  }
  return ParseTokens(Request(kTokenUrl, TokenBody("grant_type=authorization_code&code=" + util::UrlEncode(code))),
                     out, error);
}

bool Refresh(const std::string& refresh_token, Tokens* out, std::string* error) {
  if (refresh_token.empty()) {
    *error = L("PSN oturumu açık değil.");
    return false;
  }
  return ParseTokens(
      Request(kTokenUrl, TokenBody("grant_type=refresh_token&refresh_token=" + util::UrlEncode(refresh_token))),
      out, error);
}

bool AccountId(const std::string& access_token, std::string* account_id_b64, std::string* error) {
  Response res = Request(std::string(kTokenUrl) + "/" + access_token, std::string());
  if (!res.error.empty()) {
    *error = res.error;
    return false;
  }
  json_object* obj = json_tokener_parse(res.body.c_str());
  std::string user_id = util::JStr(obj, "user_id");
  if (obj) json_object_put(obj);
  if (res.status < 200 || res.status >= 300 || user_id.empty()) {
    *error = L("PSN hesap kimliği alınamadı (HTTP ") + util::Num(res.status) + ").";
    return false;
  }
  uint64_t id = strtoull(user_id.c_str(), NULL, 10);
  uint8_t bytes[8];
  for (int i = 0; i < 8; i++) bytes[i] = static_cast<uint8_t>((id >> (8 * i)) & 0xff);
  *account_id_b64 = util::Base64Encode(bytes, sizeof(bytes));
  return true;
}

}  // namespace psn
