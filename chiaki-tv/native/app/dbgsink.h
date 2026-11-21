// chiaki-tv tanilama: gunluk satirlarini UDP ile agdaki bir dinleyiciye yollar ve ana (Pepper)
// is parcaciginin takilip takilmadigini izler. Ana is parcacigi ya da tarayici kilitlense bile
// calisir; boylece "en son hangi adimdaydi" sorusu cevaplanabilir.
#ifndef CHIAKI_TV_DBGSINK_H
#define CHIAKI_TV_DBGSINK_H

#include <string>

namespace dbgsink {

// "ip:port" hedefine gondermeyi baslatir (bos dizge kapatir). Her is parcacigindan cagrilabilir.
void SetTarget(const std::string& ip_port);
// Satiri kuyruga koyar; hedef yoksa hicbir sey yapmaz. Asla beklemez.
void Send(const std::string& line);

// Ana is parcacigi: su an yapilan isin adi (sabit dizge) ve "yasiyorum" isareti.
void MainStep(const char* step);
// Bekciyi baslatir: ana is parcacigi stall_ms boyunca isaret vermezse son adimi bildirir.
void StartWatchdog(void (*report)(const std::string& line, void* user), void* user, int stall_ms);

}  // namespace dbgsink

#endif
