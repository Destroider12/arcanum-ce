#ifndef ARCANUM_NET_COOP_UPNP_H_
#define ARCANUM_NET_COOP_UPNP_H_

#include <stdbool.h>
#include <stddef.h>

/* PT51: optional UPnP IGD port mapping for the co-op host (miniupnpc).
   Built in when ARCANUM_UPNP is defined; otherwise every call is a no-op and
   the status reads "UPnP: off". Never blocks: discovery + mapping run on a
   worker thread with short timeouts. Kill switch: ARCANUM_COOP_NO_UPNP=1. */

typedef enum CoopUpnpState {
    COOP_UPNP_OFF = 0, /* not built / disabled / not hosting */
    COOP_UPNP_BUSY, /* discovery running */
    COOP_UPNP_MAPPED, /* port mapped */
    COOP_UPNP_FAILED, /* no IGD / mapping refused */
} CoopUpnpState;

/* Host start: map UDP `port` (async). */
void coop_upnp_start(int port);
/* Host stop / exit: remove the mapping (short, bounded). Idempotent. */
void coop_upnp_stop(void);
CoopUpnpState coop_upnp_state(void);
/* Status for the menu line / log, e.g. "UPnP: UDP 7777 open, external 1.2.3.4". */
void coop_upnp_describe(char* buf, size_t size);
/* Pure formatter (tests). */
void coop_upnp_format(CoopUpnpState state, int port, const char* ext_ip, const char* err, char* buf, size_t size);

#endif /* ARCANUM_NET_COOP_UPNP_H_ */
