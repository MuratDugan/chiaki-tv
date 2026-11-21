// chiaki-tv: ortak yardimcilar (kilit, zaman, kodlama, JSON).
#ifndef CHIAKI_TV_UTIL_H
#define CHIAKI_TV_UTIL_H

#include <pthread.h>
#include <stdint.h>

#include <string>
#include <vector>

#include <json-c/json.h>

namespace util {

// Kapsam boyunca pthread kilidi tutar.
class Lock {
 public:
  explicit Lock(pthread_mutex_t* m) : m_(m) { pthread_mutex_lock(m_); }
  ~Lock() { pthread_mutex_unlock(m_); }

 private:
  pthread_mutex_t* m_;
  Lock(const Lock&);
  void operator=(const Lock&);
};

int64_t NowSec();  // duvar saati (belirtec son kullanma icin)
int64_t NowMs();   // monoton saat (sure olcumu icin)

std::string Base64Encode(const uint8_t* data, size_t size);
std::string Hex(const uint8_t* data, size_t size);
bool ParseHex(const std::string& hex, std::vector<uint8_t>* out);
std::string UrlEncode(const std::string& s);
std::string Trim(const std::string& s);
std::string Num(int64_t value);
// Gecersiz UTF-8 baytlarini '?' ile degistirir. Gecersiz UTF-8 iceren bir metin pp::Var'a
// verilirse mesajin tamami JS'e null olarak ulasir; JSON'a giren her dizge buradan gecer.
std::string Utf8Clean(const std::string& s);
std::string RandomDigits(int count);
std::string RandomHex(int bytes);

// JSON okuma: anahtar yoksa ya da turu uymuyorsa varsayilan doner.
std::string JStr(json_object* obj, const char* key, const std::string& def = std::string());
int64_t JInt(json_object* obj, const char* key, int64_t def = 0);
bool JBool(json_object* obj, const char* key, bool def = false);
void JAdd(json_object* obj, const char* key, const std::string& value);
void JAddInt(json_object* obj, const char* key, int64_t value);
void JAddBool(json_object* obj, const char* key, bool value);
// Cokme raporunda is parcacigini tanimak icin kisa ad.
void SetThreadTag(const char* tag);
const char* ThreadTag();

// Cagiran is parcacigina ayri bir hata isleyici yigini verir (yigin tasmasi da yakalanabilsin).
void CrashStackForThisThread();

// Nesneyi metne cevirir ve serbest birakir.
std::string JDump(json_object* obj);

}  // namespace util

#endif
