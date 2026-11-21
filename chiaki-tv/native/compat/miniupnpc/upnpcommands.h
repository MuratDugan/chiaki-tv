#ifndef CHIAKI_COMPAT_UPNPCOMMANDS_H
#define CHIAKI_COMPAT_UPNPCOMMANDS_H
#ifdef __cplusplus
extern "C" {
#endif
#define UPNPCOMMAND_SUCCESS 0
#define UPNPCOMMAND_UNKNOWN_ERROR (-1)
int UPNP_GetExternalIPAddress(const char *controlURL, const char *servicetype, char *extIpAdd);
int UPNP_AddPortMapping(const char *controlURL, const char *servicetype, const char *extPort, const char *inPort,
	const char *inClient, const char *desc, const char *proto, const char *remoteHost, const char *leaseDuration);
int UPNP_DeletePortMapping(const char *controlURL, const char *servicetype, const char *extPort, const char *proto,
	const char *remoteHost);
#ifdef __cplusplus
}
#endif
#endif
