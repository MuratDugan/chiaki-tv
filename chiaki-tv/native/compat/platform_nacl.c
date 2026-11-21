/* NaCl (newlib + nacl_io) icin eksik platform parcalari. */
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#include "platform_nacl.h"

/* ---- yerel IPv4 adresi --------------------------------------------------
 * NaCl'de arayuz listesi alinamaz. Adresi JS tarafi (tizen.systeminfo) okuyup
 * modul acilirken bildirir; chiaki'nin Switch yolu bunu gethostid() ile ister. */
static in_addr_t g_local_ip; /* ag bayt sirasi */

void chiaki_nacl_set_local_ip(const char *dotted)
{
	struct in_addr a;
	g_local_ip = (dotted && inet_pton(AF_INET, dotted, &a) == 1) ? a.s_addr : 0;
}

long gethostid(void)
{
	return (long)g_local_ip;
}

/* ---- C11 aligned_alloc: bu newlib surumunde yok ---------------------------- */
void *aligned_alloc(size_t alignment, size_t size)
{
	return memalign(alignment, size);
}

/* ---- UPnP bos katmani --------------------------------------------------- */
struct UPNPDev *upnpDiscover(int delay, const char *multicastif, const char *minissdpdsock, int localport,
	int ipv6, unsigned char ttl, int *error)
{
	(void)delay; (void)multicastif; (void)minissdpdsock; (void)localport; (void)ipv6; (void)ttl;
	if(error)
		*error = UPNPDISCOVER_UNKNOWN_ERROR;
	return NULL;
}
void freeUPNPDevlist(struct UPNPDev *devlist) { (void)devlist; }
int UPNP_GetValidIGD(struct UPNPDev *devlist, struct UPNPUrls *urls, struct IGDdatas *data, char *lanaddr, int lanaddrlen)
{
	(void)devlist; (void)urls; (void)data; (void)lanaddr; (void)lanaddrlen;
	return 0; /* gecerli ag gecidi bulunamadi */
}
void FreeUPNPUrls(struct UPNPUrls *urls) { (void)urls; }
int UPNP_GetExternalIPAddress(const char *c, const char *s, char *out) { (void)c; (void)s; (void)out; return UPNPCOMMAND_UNKNOWN_ERROR; }
int UPNP_AddPortMapping(const char *c, const char *s, const char *e, const char *i, const char *cl, const char *d,
	const char *p, const char *r, const char *l)
{
	(void)c; (void)s; (void)e; (void)i; (void)cl; (void)d; (void)p; (void)r; (void)l;
	return UPNPCOMMAND_UNKNOWN_ERROR;
}
int UPNP_DeletePortMapping(const char *c, const char *s, const char *e, const char *p, const char *r)
{
	(void)c; (void)s; (void)e; (void)p; (void)r;
	return UPNPCOMMAND_UNKNOWN_ERROR;
}
const char *strupnperror(int err) { (void)err; return "UPnP bu derlemede kapali"; }
