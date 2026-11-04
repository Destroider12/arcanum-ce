#ifndef ARCANUM_NET_TIG_NET_TRANSPORT_H_
#define ARCANUM_NET_TIG_NET_TRANSPORT_H_

/*
 * PT58: transport abstraction under tig_net. tig_net keeps all co-op logic
 * (slots, handshake, protocol check, dispatch); a backend only moves
 * datagrams between peers on numbered channels, reliable or not, and
 * reports connect / receive / disconnect events - the subset of ENet that
 * tig_net used. Backends: ENet (default, unchanged behaviour), Steam
 * Networking Messages (steam_api64.dll loaded at runtime), and an
 * in-process loopback fake for tests.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TigNetTPeer TigNetTPeer; /* opaque, backend-owned */

typedef enum TigNetTEventType {
    TIG_NET_TEV_NONE = 0,
    TIG_NET_TEV_CONNECT = 1,
    TIG_NET_TEV_RECEIVE = 2,
    TIG_NET_TEV_DISCONNECT = 3,
} TigNetTEventType;

typedef struct TigNetTEvent {
    int type;
    TigNetTPeer* peer;
    int channel;
    const void* data; /* RECEIVE: valid until event_done */
    size_t size;
    void* internal; /* backend bookkeeping */
} TigNetTEvent;

typedef struct TigNetTransport {
    const char* name;
    /* Library setup, once; false (with a log line) if unavailable. */
    bool (*init)(void);
    /* Host: accept up to max_peers. port is ignored by non-UDP backends. */
    bool (*listen)(int port, int max_peers, int channels);
    /* Client: start connecting; a CONNECT event follows on success. */
    TigNetTPeer* (*connect)(const char* target, int port, int channels);
    /* 1 = *ev filled, 0 = nothing within timeout_ms. */
    int (*service)(TigNetTEvent* ev, unsigned int timeout_ms);
    /* Release a RECEIVE event's data (call once per serviced event). */
    void (*event_done)(TigNetTEvent* ev);
    /* Send to each peer; returns how many accepted it. */
    int (*send)(TigNetTPeer* const* peers, int count, int channel, const void* data, size_t size, bool reliable);
    void (*flush)(void);
    /* Graceful (DISCONNECT event later) or immediate (no event). */
    void (*disconnect)(TigNetTPeer* peer, bool now);
    /* Drop a half-open connect attempt without an event. */
    void (*reset_peer)(TigNetTPeer* peer);
    void (*set_timeout)(TigNetTPeer* peer, unsigned int min_ms, unsigned int max_ms);
    /* Close the local endpoint (all peers gone, no events). */
    void (*close)(void);
    bool (*is_open)(void);
    /* One user pointer per peer (tig_net keeps slot + 1 there). */
    void** (*peer_data)(TigNetTPeer* peer);
    unsigned int (*now_ms)(void);
    void (*describe_peer)(TigNetTPeer* peer, char* buf, size_t size);
    /* What a client types to reach us (Steam: our SteamID64). */
    bool (*local_id)(char* buf, size_t size);
} TigNetTransport;

const TigNetTransport* tig_net_transport_enet(void);
const TigNetTransport* tig_net_transport_steam(void);
/* ARCANUM_COOP_TRANSPORT: unset / "enet" / "direct" -> ENet, "steam" -> Steam. */
const TigNetTransport* tig_net_transport_pick(const char* env_value);

/* tig_net.c: select before tig_net_start_client (NULL = ENet). */
void tig_net_set_transport(const TigNetTransport* transport);
const TigNetTransport* tig_net_get_transport(void);
/* Host: "what to give your friends" (Steam ID) - false for ENet. */
bool tig_net_local_join_id(char* buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif
