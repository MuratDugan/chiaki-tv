#include "util.h"

#include <nacl/nacl_exception.h>

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <nacl/nacl_random.h>

namespace util {

int64_t NowSec() {
  return static_cast<int64_t>(time(NULL));
}

int64_t NowMs() {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
  }
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string Base64Encode(const uint8_t* data, size_t size) {
  static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  for (size_t i = 0; i < size; i += 3) {
    uint32_t v = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < size) v |= static_cast<uint32_t>(data[i + 1]) << 8;
    if (i + 2 < size) v |= data[i + 2];
    out += kTable[(v >> 18) & 63];
    out += kTable[(v >> 12) & 63];
    out += (i + 1 < size) ? kTable[(v >> 6) & 63] : '=';
    out += (i + 2 < size) ? kTable[v & 63] : '=';
  }
  return out;
}

std::string Hex(const uint8_t* data, size_t size) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (size_t i = 0; i < size; i++) {
    out += kDigits[data[i] >> 4];
    out += kDigits[data[i] & 15];
  }
  return out;
}

static int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool ParseHex(const std::string& hex, std::vector<uint8_t>* out) {
  out->clear();
  if (hex.size() % 2 != 0) return false;
  for (size_t i = 0; i < hex.size(); i += 2) {
    int hi = HexValue(hex[i]), lo = HexValue(hex[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out->push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return true;
}

std::string UrlEncode(const std::string& s) {
  static const char kDigits[] = "0123456789ABCDEF";
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '-' || c == '_' || c == '.' || c == '~';
    if (plain) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kDigits[c >> 4];
      out += kDigits[c & 15];
    }
  }
  return out;
}

std::string Num(int64_t value) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
  return buf;
}

std::string Utf8Clean(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  size_t i = 0, n = s.size();
  while (i < n) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = 0;
    uint32_t cp = 0, min = 0;
    if (c < 0x80) { len = 1; cp = c; }
    else if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; min = 0x10000; }
    bool ok = len > 0 && i + len <= n;
    for (size_t k = 1; ok && k < len; k++) {
      unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) ok = false;
      else cp = (cp << 6) | (cc & 0x3F);
    }
    // fazla uzun kodlama, vekil cift ve aralik disi kod noktalarini da reddet; denetim karakterlerini at
    if (ok && (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) ok = false;
    if (ok && len == 1 && c < 0x20 && c != '\n' && c != '\t') ok = false;
    if (ok) { out.append(s, i, len); i += len; }
    else { out += '?'; i++; }
  }
  return out;
}

std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
  return s.substr(a, b - a);
}

static void SecureRandom(uint8_t* buf, size_t size) {
  size_t got = 0;
  while (got < size) {
    size_t n = 0;
    if (nacl_secure_random(buf + got, size - got, &n) != 0 || n == 0) {
      // Rastgele kaynak calismazsa (olmamasi gerekir) zamandan turet; guvenlik icin yeterli degil ama cokmeyiz.
      for (; got < size; got++) buf[got] = static_cast<uint8_t>((NowMs() >> (got % 8)) ^ (got * 131));
      return;
    }
    got += n;
  }
}

std::string RandomDigits(int count) {
  std::string out;
  while (static_cast<int>(out.size()) < count) {
    uint8_t b;
    SecureRandom(&b, 1);
    if (b < 250) out += static_cast<char>('0' + b % 10);  // yanliligi onlemek icin 250 ustunu at
  }
  return out;
}

std::string RandomHex(int bytes) {
  std::vector<uint8_t> buf(bytes);
  SecureRandom(buf.data(), buf.size());
  return Hex(buf.data(), buf.size());
}

std::string JStr(json_object* obj, const char* key, const std::string& def) {
  json_object* v = NULL;
  if (!obj || !json_object_object_get_ex(obj, key, &v) || !v) return def;
  if (!json_object_is_type(v, json_type_string)) return def;
  const char* s = json_object_get_string(v);
  return s ? std::string(s, json_object_get_string_len(v)) : def;
}

int64_t JInt(json_object* obj, const char* key, int64_t def) {
  json_object* v = NULL;
  if (!obj || !json_object_object_get_ex(obj, key, &v) || !v) return def;
  if (!json_object_is_type(v, json_type_int) && !json_object_is_type(v, json_type_double)) return def;
  return json_object_get_int64(v);
}

bool JBool(json_object* obj, const char* key, bool def) {
  json_object* v = NULL;
  if (!obj || !json_object_object_get_ex(obj, key, &v) || !v) return def;
  if (!json_object_is_type(v, json_type_boolean)) return def;
  return json_object_get_boolean(v) != 0;
}

void JAdd(json_object* obj, const char* key, const std::string& value) {
  std::string clean = Utf8Clean(value);
  json_object_object_add(obj, key, json_object_new_string_len(clean.data(), static_cast<int>(clean.size())));
}

void JAddInt(json_object* obj, const char* key, int64_t value) {
  json_object_object_add(obj, key, json_object_new_int64(value));
}

void JAddBool(json_object* obj, const char* key, bool value) {
  json_object_object_add(obj, key, json_object_new_boolean(value ? 1 : 0));
}

std::string JDump(json_object* obj) {
  const char* s = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
  std::string out(s ? s : "{}");
  json_object_put(obj);
  return out;
}

static __thread const char* t_tag = NULL;

void SetThreadTag(const char* tag) { t_tag = tag; }
const char* ThreadTag() { return t_tag ? t_tag : "diger"; }

void CrashStackForThisThread() {
  const size_t kSize = 64 * 1024;
  void* stack = malloc(kSize);
  if (stack) nacl_exception_set_stack(stack, kSize);
}

}  // namespace util
