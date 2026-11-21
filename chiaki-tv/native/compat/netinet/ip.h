/* newlib/NaCl'de netinet/ip.h yok. chiaki yalnizca hizmet turu sabitlerini bekliyor. */
#ifndef CHIAKI_COMPAT_NETINET_IP_H
#define CHIAKI_COMPAT_NETINET_IP_H
#include <netinet/in.h>
#define IPTOS_LOWDELAY 0x10
#define IPTOS_THROUGHPUT 0x08
#define IPTOS_RELIABILITY 0x04
#endif
