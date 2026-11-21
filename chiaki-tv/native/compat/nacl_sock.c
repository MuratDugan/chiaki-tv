#include "nacl_sock.h"

/* Bu dosyada gercek islevler cagrilir. */
#undef socket
#undef setsockopt
#undef recvfrom
#undef recv

#include <errno.h>
#include <poll.h>
#include <sys/time.h>

#define NACL_SOCK_MAX_FD 1024

/* Soket basina alim zaman asimi (ms); 0 = suresiz. Tek tek int yazimlari yeterince atomiktir. */
static int g_rcv_timeout_ms[NACL_SOCK_MAX_FD];

int chiaki_nacl_socket(int domain, int type, int protocol)
{
	int fd = socket(domain, type, protocol);
	if(fd >= 0 && fd < NACL_SOCK_MAX_FD)
		g_rcv_timeout_ms[fd] = 0; /* yeniden kullanilan tanimlayici eski ayari tasimasin */
	return fd;
}

int chiaki_nacl_setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen)
{
	if(level == SOL_SOCKET && optname == SO_RCVTIMEO)
	{
		const struct timeval *tv = (const struct timeval *)optval;
		long long ms;
		if(!optval || optlen < (socklen_t)sizeof(struct timeval) || fd < 0 || fd >= NACL_SOCK_MAX_FD)
		{
			errno = EINVAL;
			return -1;
		}
		ms = (long long)tv->tv_sec * 1000 + tv->tv_usec / 1000;
		if(ms == 0 && tv->tv_usec > 0)
			ms = 1;
		if(ms > 0x7fffffff)
			ms = 0x7fffffff;
		g_rcv_timeout_ms[fd] = (int)ms;
		return 0;
	}
	int r = setsockopt(fd, level, optname, optval, optlen);
	if(r < 0 && (errno == ENOPROTOOPT || errno == EOPNOTSUPP))
		return 0; /* platform bu secenegi hic tanimiyor: onsuz devam et */
	return r;
}

/* Zaman asimi varsa veri gelene kadar en fazla o kadar bekler. 0 donerse errno=EAGAIN. */
static int wait_readable(int fd)
{
	int timeout = (fd >= 0 && fd < NACL_SOCK_MAX_FD) ? g_rcv_timeout_ms[fd] : 0;
	struct pollfd p;
	if(timeout <= 0)
		return 1;
	p.fd = fd;
	p.events = POLLIN;
	p.revents = 0;
	if(poll(&p, 1, timeout) == 0)
	{
		errno = EAGAIN;
		return 0;
	}
	return 1; /* hazir ya da hata: asil cagri bildirsin */
}

ssize_t chiaki_nacl_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *addr, socklen_t *addrlen)
{
	if(!wait_readable(fd))
		return -1;
	return recvfrom(fd, buf, len, flags, addr, addrlen);
}

ssize_t chiaki_nacl_recv(int fd, void *buf, size_t len, int flags)
{
	if(!wait_readable(fd))
		return -1;
	return recv(fd, buf, len, flags);
}
