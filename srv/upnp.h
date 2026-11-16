/* UPnP port mapping, the client's side: ask the router of the network (an
 * "Internet Gateway Device") to pass ports on to an address inside. Written
 * from the UPnP Device Architecture 1.0 and the WANIPConnection:1 service
 * description. This header is the words (portable C99, no allocation) and,
 * at the end, the conversation itself (sockets; Linux). */
#ifndef V90D_SRV_UPNP_H
#define V90D_SRV_UPNP_H
#include <stddef.h>
#include <stdint.h>

/* the search, sent by UDP to 239.255.255.250 port 1900 */
#define UPNP_GROUP 0xEFFFFFFAu
#define UPNP_PORT  1900
#define UPNP_SEARCH "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n" \
                    "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n"

typedef struct { char host[64]; uint16_t port; char path[160]; } upnp_url;
typedef struct { char path[160]; char type[72]; } upnp_ctl;

/* the LOCATION of an answer to the search: 0, or -1 when there is none or it
 * is not plain http */
int upnp_location(const char *resp, size_t len, upnp_url *u);
/* in a device description: the control address and type of the service
 * that maps ports (WANIPConnection, else WANPPPConnection); 0 or -1 */
int upnp_control(const char *xml, size_t len, upnp_ctl *c);
/* The whole HTTP request that adds (add != 0) or removes the mapping of
 * port `port` of protocol 6 or 17 to the same port of address ip (host
 * order). Returns its length (out is also 0-terminated), or -1 without room. */
int upnp_request(char *out, size_t cap, const upnp_url *dev, const upnp_ctl *c,
                 int add, int proto, unsigned port, uint32_t ip, const char *desc);
/* an HTTP answer that says 200 */
int upnp_ok(const char *resp, size_t len);

/* The conversation: find the router from the interface with address
 * local_ip (2 s for an answer), read its description, and add or remove
 * every port of the n ranges (proto, lo, hi) for address ip. Blocking, for a
 * child process. Returns how many requests the router accepted, -1 when no
 * router answered, -2 when its description names no service for this. */
int upnp_run(uint32_t local_ip, int add, uint32_t ip, const uint8_t *proto, const uint16_t *lo, const uint16_t *hi, int n,
             const char *desc);

#endif
