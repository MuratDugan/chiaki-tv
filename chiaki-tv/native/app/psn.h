// chiaki-tv: PSN oturumu. chiaki-ng'nin gui/src/psnaccountid.cpp ve psntoken.cpp akisinin karsiligi.
// Sifre hicbir zaman bu koddan gecmez: kullanici Sony'nin kendi sayfasinda giris yapar,
// biz yalnizca yonlendirme adresindeki tek kullanimlik kodu belirtece ceviririz.
#ifndef CHIAKI_TV_PSN_H
#define CHIAKI_TV_PSN_H

#include <stdint.h>

#include <string>

namespace psn {

struct Tokens {
  std::string access;
  std::string refresh;
  int64_t expiry;  // epoch saniye
  Tokens() : expiry(0) {}
};

// Tanilama: curl'un baglanti adimlari (yalnizca bilgi satirlari; baslik ve govde asla) buraya yazilir.
void SetLogger(void (*log)(const std::string& line, void* user), void* user);

// Telefonda acilacak Sony giris adresi. client_duid: chiaki_holepunch_generate_client_device_uid ciktisi.
std::string LoginUrl(const std::string& client_duid);

// Kullanicinin yapistirdigi yonlendirme adresinden (ya da dogrudan koddan) "code" degerini cikarir.
std::string ExtractCode(const std::string& redirect_or_code);

bool ExchangeCode(const std::string& code, Tokens* out, std::string* error);
bool Refresh(const std::string& refresh_token, Tokens* out, std::string* error);

// Hesap kimligi: 8 baytlik kucuk-uclu sayi, base64 (chiaki'nin bekledigi bicim).
bool AccountId(const std::string& access_token, std::string* account_id_b64, std::string* error);

}  // namespace psn

#endif
