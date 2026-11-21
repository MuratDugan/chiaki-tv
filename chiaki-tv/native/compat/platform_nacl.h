#ifndef CHIAKI_PLATFORM_NACL_H
#define CHIAKI_PLATFORM_NACL_H
#ifdef __cplusplus
extern "C" {
#endif
/* TV'nin yerel IPv4 adresini (ornegin "192.168.1.50") bildirir; delik acma katmani bunu kullanir. */
void chiaki_nacl_set_local_ip(const char *dotted);
#ifdef __cplusplus
}
#endif
#endif
