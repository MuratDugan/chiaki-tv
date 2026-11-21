/* libchiaki'nin soket cagrilarini NaCl'e (nacl_io) uydurur. Derleyiciye -include ile verilir;
 * yalnizca libchiaki kaynaklarina uygulanir.
 *
 * nacl_io su ikisini desteklemez, chiaki ise ikisine de guvenir:
 *  - SO_RCVTIMEO: STUN cevabi beklenirken (remote/stun.h) alim zaman asimi. Burada soket basina
 *    saklanir ve recv/recvfrom oncesinde poll() ile uygulanir.
 *  - IP_TTL, IP_MTU_DISCOVER gibi secenekler: chiaki bunlarin basarisizligini olumcul sayar.
 *    Platformun hic tanimadigi secenekler (ENOPROTOOPT/EOPNOTSUPP) burada basari sayilir.
 */
#ifndef CHIAKI_NACL_SOCK_H
#define CHIAKI_NACL_SOCK_H

#include <sys/types.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif
int chiaki_nacl_socket(int domain, int type, int protocol);
int chiaki_nacl_setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen);
ssize_t chiaki_nacl_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *addr, socklen_t *addrlen);
ssize_t chiaki_nacl_recv(int fd, void *buf, size_t len, int flags);
#ifdef __cplusplus
}
#endif

/* Islev benzeri makrolar: yalnizca cagri bicimindeki kullanimlari degistirir (uye adlarina dokunmaz). */
#define socket(domain, type, protocol) chiaki_nacl_socket(domain, type, protocol)
#define setsockopt(fd, level, name, val, len) chiaki_nacl_setsockopt(fd, level, name, val, len)
#define recvfrom(fd, buf, len, flags, addr, alen) chiaki_nacl_recvfrom(fd, buf, len, flags, addr, alen)
#define recv(fd, buf, len, flags) chiaki_nacl_recv(fd, buf, len, flags)

#endif
