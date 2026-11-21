/* miniupnpc yerine bos katman: TV'de UPnP port yonlendirme denenmez.
 * upnpDiscover her zaman "cihaz yok" doner; chiaki bu durumda UPnP'siz devam eder. */
#ifndef CHIAKI_COMPAT_MINIUPNPC_H
#define CHIAKI_COMPAT_MINIUPNPC_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MINIUPNPC_API_VERSION 17
#define UPNPDISCOVER_SUCCESS 0
#define UPNPDISCOVER_UNKNOWN_ERROR (-1)
#define MINIUPNPC_URL_MAXSIZE 128
struct UPNPDev { struct UPNPDev *pNext; char *descURL; char *st; char *usn; unsigned int scope_id; char buffer[3]; };
struct UPNPUrls { char *controlURL; char *ipcondescURL; char *controlURL_CIF; char *controlURL_6FC; char *rootdescURL; };
struct IGDdatas_service { char controlurl[MINIUPNPC_URL_MAXSIZE]; char eventsuburl[MINIUPNPC_URL_MAXSIZE];
	char scpdurl[MINIUPNPC_URL_MAXSIZE]; char servicetype[MINIUPNPC_URL_MAXSIZE]; };
struct IGDdatas { char cureltname[MINIUPNPC_URL_MAXSIZE]; char urlbase[MINIUPNPC_URL_MAXSIZE]; char presentationurl[MINIUPNPC_URL_MAXSIZE];
	int level; struct IGDdatas_service CIF; struct IGDdatas_service first; struct IGDdatas_service second;
	struct IGDdatas_service IPv6FC; struct IGDdatas_service tmp; };
struct UPNPDev *upnpDiscover(int delay, const char *multicastif, const char *minissdpdsock, int localport,
	int ipv6, unsigned char ttl, int *error);
void freeUPNPDevlist(struct UPNPDev *devlist);
int UPNP_GetValidIGD(struct UPNPDev *devlist, struct UPNPUrls *urls, struct IGDdatas *data, char *lanaddr, int lanaddrlen);
void FreeUPNPUrls(struct UPNPUrls *urls);
#ifdef __cplusplus
}
#endif
#endif
