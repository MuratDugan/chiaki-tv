#include "i18n.h"

#include <string.h>

namespace i18n {

namespace {

volatile int g_english = 1;

struct Entry {
  const char* tr;
  const char* en;
};

const Entry kTable[] = {
    {"Konsol bağlantıyı reddetti. Konsol açık ve Uzaktan Oynama etkin mi?",
     "The console refused the connection. Is it on, with Remote Play enabled?"},
    {"Uzaktan Oynama başka bir cihazda kullanımda.",
     "Remote Play is in use on another device."},
    {"Konsoldaki Uzaktan Oynama hizmeti çöktü; konsolu yeniden başlatmak gerekebilir.",
     "The Remote Play service on the console crashed; the console may need a restart."},
    {"Konsol yazılım sürümü bu istemciyle uyuşmuyor.",
     "The console's software version does not match this client."},
    {"Konsolla kontrol bağlantısı kurulamadı.",
     "Could not open the control connection to the console."},
    {"Konsol bağlantıyı kesti.",
     "The console closed the connection."},
    {"PSN üzerinden konsol kaydı başarısız. Konsol bu hesaba bağlı ve Uzaktan Oynama açık mı?",
     "Registering with the console over PSN failed. Is the console linked to this account, with Remote Play enabled?"},
    {"Bağlantı sonlandı: ",
     "Connection ended: "},
    {"> ayrıntılı günlük açık",
     "> verbose log on"},
    {"> ayrıntılı günlük kapalı",
     "> verbose log off"},
    {"PSN oturumu açılıyor",
     "Signing in to PSN"},
    {"PSN oturumu açık değil. Telefondan kurulum sayfasını kullan.",
     "Not signed in to PSN. Use the setup page on your phone."},
    {"Konsollar aranıyor",
     "Looking for consoles"},
    {"Konsol listesi alınamadı: ",
     "Could not get the console list: "},
    {"Bağlantı hazırlanıyor",
     "Preparing connection"},
    {"Geçersiz konsol kimliği.",
     "Invalid console ID."},
    {"PSN oturumu doğrulanıyor",
     "Checking PSN session"},
    {"PSN hesap kimliği eksik. Telefondan yeniden giriş yap.",
     "PSN account ID is missing. Sign in again from your phone."},
    {"Bağlantı oturumu oluşturulamadı.",
     "Could not create the connection session."},
    {"Ağ hazırlanıyor",
     "Preparing network"},
    {"PSN oturumu oluşturuluyor",
     "Creating PSN session"},
    {"Bağlantı teklifi hazırlanıyor",
     "Preparing connection offer"},
    {"Konsola ulaşılıyor",
     "Reaching the console"},
    {"Kontrol bağlantısı kuruluyor",
     "Opening control connection"},
    {" başarısız: ",
     " failed: "},
    {"Oturum başlatılamadı: ",
     "Could not start the session: "},
    {"Konsola bağlanılıyor",
     "Connecting to the console"},
    {"Bu televizyon 1080p yayını çözemiyor. Çözünürlük 720p'ye alındı; yeniden bağlan.",
     "This TV cannot decode the 1080p stream. Resolution was set to 720p; connect again."},
    {"! kütüphane kilitli olmayan bir kilidi bıraktı (toplam ",
     "! library released a mutex that was not locked (total "},
    {"> akış başladı",
     "> stream started"},
    {"Veri bağlantısı kuruluyor",
     "Opening data connection"},
    {"Bağlantı kapatılıyor",
     "Closing connection"},
    {"Konsola bağlanılamadı. Konsolun açık ya da dinlenme modunda olduğundan emin olup yeniden dene; ayrıntı kurulum sayfasındaki günlükte.",
     "Could not connect to the console. Make sure it is on or in rest mode and try again; details are in the log on the setup page."},
    {"Konsol kapandı ya da dinlenme moduna geçti.",
     "The console turned off or entered rest mode."},
    {"> bağlantı kapandı",
     "> connection closed"},
    {"Ağ katmanı başlatılamadı.",
     "Could not start the network layer."},
    {"Ağ hatası: ",
     "Network error: "},
    {"PSN oturumu açılamadı (HTTP ",
     "PSN sign-in failed (HTTP "},
    {"Yönlendirme adresinde giriş kodu yok. Sony girişinden sonra açılan sayfanın adresini yapıştır.",
     "The redirect address has no sign-in code. Paste the address of the page that opens after signing in with Sony."},
    {"PSN oturumu açık değil.",
     "Not signed in to PSN."},
    {"PSN hesap kimliği alınamadı (HTTP ",
     "Could not get the PSN account ID (HTTP "},
    {"Grafik bağlamı oluşturulamadı.",
     "Could not create the graphics context."},
    {"Video çözücü oluşturulamadı.",
     "Could not create the video decoder."},
    {"Televizyonun H.264 çözücüsü açılamadı (hata ",
     "Could not open the TV's H.264 decoder (error "},
    {"Televizyonun video çözücüsü bu yayını çözemedi (hata ",
     "The TV's video decoder could not decode this stream (error "},
    {"Televizyonun video çözücüsü bu yayını çözemedi (sıfırlama hatası ",
     "The TV's video decoder could not decode this stream (reset error "},
    {"> tarayıcı kolu bağlandı: ",
     "> browser controller connected: "},
    {"adsız",
     "unnamed"},
    {"> tarayıcı kolu ayrıldı",
     "> browser controller disconnected"},
    {"Bu adres üzerinden erişime izin yok.",
     "Access through this address is not allowed."},
    {"Bulunamadı.",
     "Not found."},
    {"Çok fazla hatalı deneme. ",
     "Too many wrong attempts. Try again in "},
    {" saniye sonra yeniden dene.",
     " seconds."},
    {"Kod uyuşmadı. Televizyondaki kodu kontrol et.",
     "The code did not match. Check the code on the TV."},
    {"Eşleştirme gerekli.",
     "Pairing required."},
    {"Yönlendirme adresi boş.",
     "The redirect address is empty."},
    {"Tarayıcı kolu",
     "Browser controller"},
    {"PSN beklenmeyen bir cevap verdi (HTTP ",
     "PSN returned an unexpected response (HTTP "},
    {"oyun kolu",
     "controller"},
    {"Düşük gecikme modu: çözücüde olmayan eski bir kareye başvuran kare atlandı, anahtar kare istendi",
     "Low-latency mode: skipped a frame that references an older frame the decoder no longer has; requested a key frame"},
};

}  // namespace

void SetLang(const std::string& lang) { g_english = lang == "tr" ? 0 : 1; }

const char* Lang() { return g_english ? "en" : "tr"; }

const char* Translate(const char* tr) {
  if (!g_english) return tr;
  for (size_t i = 0; i < sizeof(kTable) / sizeof(kTable[0]); i++)
    if (strcmp(kTable[i].tr, tr) == 0) return kTable[i].en;
  return tr;
}

}  // namespace i18n
