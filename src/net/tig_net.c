#include "net/tig_net.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <enet/enet.h>

#ifdef TIG_NET_TEST_STUB_DEBUG
#include <stdio.h>
#include <stdarg.h>
void tig_debug_printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
#else
#include "tig/debug.h"
#endif

#define TIG_NET_CHANNEL_APP 0
#define TIG_NET_CHANNEL_CTRL 1
/* Review M7: high-rate poses, unreliable+sequenced so a lost one never
   holds back events queued behind it on the reliable app channel. */
#define TIG_NET_CHANNEL_POSE 2
#define TIG_NET_NUM_CHANNELS 3

#define TIG_NET_CTRL_WELCOME 1
#define TIG_NET_CTRL_NAME 2
/* Review PT24 #1: the client's NAME/HELLO travels on the APP channel as its
   very first app packet ("NAME" magic, outside the 0..255 app type range),
   so ENet's per-channel ordering guarantees it precedes CHAR_OFFER etc.
   The host drops app packets from a slot until a valid HELLO. */
#define TIG_NET_APP_HELLO 0x454D414E
#define TIG_NET_HANDSHAKE_TIMEOUT_MS 5000u
#define TIG_NET_MAX_EVENTS_PER_PUMP 512

typedef struct TigNetCtrlWelcome {
    int magic; /* TIG_NET_CTRL_WELCOME */
    int slot;
    int max_players;
    int protocol; /* TIG_NET_PROTOCOL */
} TigNetCtrlWelcome;

typedef struct TigNetCtrlName {
    int magic; /* TIG_NET_CTRL_NAME */
    int slot;
    char name[TIG_NET_MAX_NAME];
    int protocol; /* TIG_NET_PROTOCOL */
} TigNetCtrlName;

_Static_assert(sizeof(TigNetCtrlWelcome) == 16, "WELCOME wire layout");
_Static_assert(sizeof(TigNetCtrlName) == 12 + TIG_NET_MAX_NAME, "NAME/HELLO wire layout");

typedef struct TigNetClient {
    ENetPeer* peer;
    bool active;
    bool waiting;
    bool loading;
    bool refused; /* host: protocol mismatch, disconnect pending */
    bool handshaken; /* host: valid HELLO (protocol) received */
    unsigned int connect_ms; /* host: for the handshake timeout */
    char name[TIG_NET_MAX_NAME];
} TigNetClient;

static bool coop_enabled;
static bool library_ready;
static bool active;
static bool is_host;
static int listen_port = TIG_NET_DEFAULT_PORT;
static char join_host[256];
static int join_port = TIG_NET_DEFAULT_PORT;
static bool want_host;
static bool want_join;

static ENetHost* enet_host;

/* Per-type app packet counters (diagnose spam / lag). Logged every ~10 s. */
static unsigned int tig_net_stat_sent[256];
static unsigned int tig_net_stat_recv[256];
static unsigned int tig_net_stat_window_start;
static bool tig_net_send_pending_flush;

static void tig_net_stat_count(unsigned int* table, const void* data, size_t size)
{
    int type;

    if (data == NULL || size < sizeof(int)) {
        return;
    }
    memcpy(&type, data, sizeof(type));
    table[(unsigned int)type & 0xFFu]++;
}

static bool tig_net_stat_logged;

static void tig_net_stat_maybe_log(void)
{
    unsigned int now;
    unsigned int total_s = 0;
    unsigned int total_r = 0;
    int i;
    char line[512];
    size_t len;

    now = (unsigned int)enet_time_get();
    if (tig_net_stat_window_start == 0) {
        tig_net_stat_window_start = now;
        return;
    }
    if (now - tig_net_stat_window_start < 10000u) {
        return;
    }

    for (i = 0; i < 256; i++) {
        total_s += tig_net_stat_sent[i];
        total_r += tig_net_stat_recv[i];
    }
    if (total_s + total_r > 0) {
        len = (size_t)snprintf(line, sizeof(line), "tig_net: 10s stats sent=%u recv=%u top:",
            total_s, total_r);
        for (i = 0; i < 256 && len < sizeof(line) - 24; i++) {
            if (tig_net_stat_sent[i] + tig_net_stat_recv[i] >= 50) {
                len += (size_t)snprintf(line + len, sizeof(line) - len, " %d:%u/%u",
                    i, tig_net_stat_sent[i], tig_net_stat_recv[i]);
            }
        }
        tig_debug_printf("%s\n", line);
    }
    tig_net_stat_logged = true;
    memset(tig_net_stat_sent, 0, sizeof(tig_net_stat_sent));
    memset(tig_net_stat_recv, 0, sizeof(tig_net_stat_recv));
    tig_net_stat_window_start = now;
}
static TigNetClient clients[TIG_NET_MAX_PLAYERS];
static int local_slot;
static int last_sender_slot;
static int max_players = TIG_NET_MAX_PLAYERS;
static unsigned int server_options;
static char server_name[64] = "Arcanum";
static char server_description[128] = "Description";
static char local_name[TIG_NET_MAX_NAME] = "Player";

static int last_message_size;
static int local_suspend_depth;
/* Client: host connection dropped during this pump; tear down after it. */
static bool lost_host;
/* Client: the host's protocol doesn't match ours. Sticky until the join
   target is configured again: the automatic rejoin (New Game) sends its
   CHAR_OFFER before WELCOME, so it must not reconnect at all. */
static bool host_refused;

static TigNetMessageFunc message_func;
static TigNetMessageValidationFunc validation_func;
static TigNetNetworkEventFunc network_event_func;

static void tig_net_clear_clients(void)
{
    int i;

    for (i = 0; i < TIG_NET_MAX_PLAYERS; i++) {
        clients[i].peer = NULL;
        clients[i].active = false;
        clients[i].waiting = false;
        clients[i].loading = false;
        clients[i].refused = false;
        clients[i].handshaken = false;
        clients[i].connect_ms = 0;
        clients[i].name[0] = '\0';
    }
}

static int tig_net_alloc_slot(void)
{
    int i;

    /* Slot 0 is always the host. */
    for (i = 1; i < max_players; i++) {
        if (!clients[i].active) {
            return i;
        }
    }
    return -1;
}

/*
 * ENet peer->data holds (slot + 1) so that NULL (a rejected / not yet
 * assigned peer) decodes to -1 rather than to the host slot 0.
 */
static void tig_net_peer_set_slot(ENetPeer* peer, int slot)
{
    peer->data = (void*)(intptr_t)(slot + 1);
}

static int tig_net_peer_get_slot(const ENetPeer* peer)
{
    int slot;

    if (peer == NULL) {
        return -1;
    }
    slot = (int)(intptr_t)peer->data - 1;
    if (slot < 0 || slot >= TIG_NET_MAX_PLAYERS) {
        return -1;
    }
    /* Must be the peer we actually registered for that slot. */
    if (clients[slot].peer != peer) {
        return -1;
    }
    return slot;
}

static void tig_net_fire_event(int type, int client_id, void* data, int size)
{
    if (network_event_func != NULL) {
        network_event_func(type, client_id, data, size);
    }
}

static void tig_net_dispatch_app(const void* data, size_t size, int sender_slot)
{
    /* Review PT24 #10: handlers cast packet structs onto this buffer, so
       give it the strictest scalar alignment. */
    union {
        char bytes[TIG_NET_MAX_PACKET];
        int64_t i64;
        double d;
        void* p;
    } copy_buf;
    char* copy = copy_buf.bytes;

    if (size < sizeof(int) || size > sizeof(copy_buf.bytes)) {
        tig_debug_printf("tig_net: dropping oversized/undersized app packet (%zu)\n", size);
        return;
    }

    memcpy(copy, data, size);
    last_sender_slot = sender_slot;
    last_message_size = (int)size;

    if (validation_func != NULL && !validation_func(copy)) {
        tig_debug_printf("tig_net: validation failed for packet type %d (size %zu, from %d)\n",
            *(int*)copy, size, sender_slot);
    } else if (message_func != NULL) {
        message_func(copy);
    }

    /* Outside dispatch, "sender" is the local peer. */
    last_sender_slot = local_slot;
    last_message_size = 0;
}

static void tig_net_send_ctrl_to_peer(ENetPeer* peer, const void* data, size_t size)
{
    ENetPacket* packet;

    if (peer == NULL || enet_host == NULL) {
        return;
    }

    packet = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
    if (packet == NULL) {
        return;
    }
    if (enet_peer_send(peer, TIG_NET_CHANNEL_CTRL, packet) != 0) {
        enet_packet_destroy(packet); /* not queued: still ours */
    }
}

static void tig_net_handle_ctrl(const void* data, size_t size, int from_slot)
{
    int magic;

    if (size < sizeof(int)) {
        return;
    }

    memcpy(&magic, data, sizeof(magic));
    if (magic == TIG_NET_CTRL_WELCOME) {
        TigNetCtrlWelcome welcome_copy;
        const TigNetCtrlWelcome* welcome = &welcome_copy;

        /* Only a client accepts WELCOME, only from the host, only once,
           and only with a sane client slot (never 0 = host). */
        if (is_host || from_slot != 0) {
            tig_debug_printf("tig_net: ignoring WELCOME (host=%d from=%d size=%zu)\n",
                is_host ? 1 : 0, from_slot, size);
            return;
        }
        memset(&welcome_copy, 0, sizeof(welcome_copy));
        memcpy(&welcome_copy, data, size < sizeof(welcome_copy) ? size : sizeof(welcome_copy));
        if (size < sizeof(TigNetCtrlWelcome) || welcome->protocol != TIG_NET_PROTOCOL) {
            if (size < sizeof(TigNetCtrlWelcome)) {
                /* An older host sends a shorter WELCOME. */
                tig_debug_printf("tig_net: refusing old host (WELCOME %zu bytes, no protocol field; ours is %d) - run the same build on both\n",
                    size, TIG_NET_PROTOCOL);
            } else {
                tig_debug_printf("tig_net: refusing host: protocol %d != ours %d - run the same build on both\n",
                    welcome->protocol, TIG_NET_PROTOCOL);
            }
            if (local_slot == 0) {
                host_refused = true; /* never welcomed: no app traffic either */
                if (clients[0].peer != NULL) {
                    enet_peer_disconnect(clients[0].peer, 0);
                }
            }
            return;
        }
        if (!tig_net_welcome_slot_valid(welcome->slot)) {
            tig_debug_printf("tig_net: ignoring WELCOME with bad slot %d\n", welcome->slot);
            return;
        }
        if (local_slot != 0) {
            tig_debug_printf("tig_net: ignoring duplicate WELCOME\n");
            return;
        }
        local_slot = welcome->slot;
        max_players = welcome->max_players;
        if (max_players < 1 || max_players > TIG_NET_MAX_PLAYERS) {
            max_players = TIG_NET_MAX_PLAYERS;
        }
        clients[local_slot].active = true;
        strncpy(clients[local_slot].name, local_name, TIG_NET_MAX_NAME - 1);
        tig_debug_printf("tig_net: welcomed as slot %d\n", local_slot);
        tig_net_fire_event(TIG_NET_EVENT_CONNECT, local_slot, NULL, 0);

        /* Our HELLO already went out on the app channel at CONNECT. */
        return;
    }

    if (magic == TIG_NET_CTRL_NAME || magic == TIG_NET_APP_HELLO) {
        TigNetCtrlName name_pkt;

        /* Host only; the slot is the authenticated sender, not the payload. */
        if (!is_host || from_slot <= 0 || from_slot >= TIG_NET_MAX_PLAYERS) {
            return;
        }
        memset(&name_pkt, 0, sizeof(name_pkt));
        memcpy(&name_pkt, data, size < sizeof(name_pkt) ? size : sizeof(name_pkt));
        if (size < sizeof(TigNetCtrlName) || name_pkt.protocol != TIG_NET_PROTOCOL) {
            if (size < sizeof(TigNetCtrlName)) {
                /* An older client sends a shorter NAME. */
                tig_debug_printf("tig_net: refusing old client in slot %d (NAME %zu bytes, no protocol field; ours is %d) - run the same build on both\n",
                    from_slot, size, TIG_NET_PROTOCOL);
            } else {
                tig_debug_printf("tig_net: refusing slot %d: protocol %d != ours %d - run the same build on both\n",
                    from_slot, name_pkt.protocol, TIG_NET_PROTOCOL);
            }
            /* Its CHAR_OFFER etc. may already be queued behind the NAME:
               drop everything from it until the disconnect lands. */
            clients[from_slot].refused = true;
            if (clients[from_slot].peer != NULL) {
                enet_peer_disconnect(clients[from_slot].peer, 0);
            }
            return;
        }
        if (clients[from_slot].handshaken) {
            return; /* duplicate */
        }
        name_pkt.name[TIG_NET_MAX_NAME - 1] = '\0';
        strncpy(clients[from_slot].name, name_pkt.name, TIG_NET_MAX_NAME - 1);
        clients[from_slot].name[TIG_NET_MAX_NAME - 1] = '\0';
        clients[from_slot].handshaken = true;
        tig_debug_printf("tig_net: slot %d handshake ok (protocol %d)\n", from_slot, TIG_NET_PROTOCOL);
        /* The game hears about the slot only now (player list, slot init). */
        tig_net_fire_event(TIG_NET_EVENT_CONNECT, from_slot, NULL, 0);
    }
}

/* Client: first packet on the APP channel, sent the moment ENet connects. */
static void tig_net_send_hello(ENetPeer* peer)
{
    TigNetCtrlName hello;
    ENetPacket* packet;

    memset(&hello, 0, sizeof(hello));
    hello.magic = TIG_NET_APP_HELLO;
    hello.slot = 0; /* the host uses the authenticated peer, not this */
    strncpy(hello.name, local_name, TIG_NET_MAX_NAME - 1);
    hello.protocol = TIG_NET_PROTOCOL;
    packet = enet_packet_create(&hello, sizeof(hello), ENET_PACKET_FLAG_RELIABLE);
    if (packet == NULL) {
        return;
    }
    if (enet_peer_send(peer, TIG_NET_CHANNEL_APP, packet) != 0) {
        enet_packet_destroy(packet);
        return;
    }
    enet_host_flush(enet_host);
}

bool tig_net_welcome_slot_valid(int slot)
{
    return slot > 0 && slot < TIG_NET_MAX_PLAYERS;
}

void tig_net_set_coop_enabled(bool enabled)
{
    coop_enabled = enabled;
}

bool tig_net_join_configured(void)
{
    return want_join && join_host[0] != '\0';
}

bool tig_net_is_coop_enabled(void)
{
    return coop_enabled;
}

void tig_net_configure_host(int port)
{
    want_host = true;
    want_join = false;
    if (port > 0 && port < 65536) {
        listen_port = port;
    }
}

void tig_net_configure_join(const char* host, int port)
{
    want_join = true;
    want_host = false;
    host_refused = false; /* a new join target gets a fresh check */
    if (host != NULL) {
        strncpy(join_host, host, sizeof(join_host) - 1);
        join_host[sizeof(join_host) - 1] = '\0';
    }
    if (port > 0 && port < 65536) {
        join_port = port;
    }
}

int tig_net_start_client(void)
{
    if (!coop_enabled) {
        return TIG_OK;
    }

    if (!library_ready) {
        if (enet_initialize() != 0) {
            tig_debug_printf("tig_net: enet_initialize failed\n");
            return TIG_ERR_GENERIC;
        }
        library_ready = true;
        atexit(enet_deinitialize);
    }

    tig_net_clear_clients();
    local_slot = 0;
    last_sender_slot = 0;
    active = false;
    is_host = false;

    if (want_host) {
        tig_net_start_server();
        return active ? TIG_OK : TIG_ERR_GENERIC;
    }

    if (want_join) {
        if (host_refused) {
            tig_debug_printf("tig_net: not rejoining %s:%d - its protocol doesn't match ours (%d); restart with the same build on both\n",
                join_host, join_port, TIG_NET_PROTOCOL);
            return TIG_ERR_GENERIC;
        }
        return tig_net_connect_configured();
    }

    /* Coop enabled but no host/join yet — subsystem ready. */
    return TIG_OK;
}

void tig_net_start_server(void)
{
    ENetAddress address;

    if (!coop_enabled) {
        return;
    }

    if (!library_ready && tig_net_start_client() != TIG_OK) {
        return;
    }

    if (enet_host != NULL) {
        return;
    }

    address.host = ENET_HOST_ANY;
    address.port = (enet_uint16)listen_port;

    enet_host = enet_host_create(&address, (size_t)max_players, TIG_NET_NUM_CHANNELS, 0, 0);
    if (enet_host == NULL) {
        tig_debug_printf("tig_net: failed to listen on port %d\n", listen_port);
        return;
    }

    is_host = true;
    active = true;
    local_slot = 0;
    clients[0].active = true;
    strncpy(clients[0].name, local_name, TIG_NET_MAX_NAME - 1);
    tig_debug_printf("tig_net: hosting on port %d (max %d)\n", listen_port, max_players);
    tig_net_fire_event(TIG_NET_EVENT_CONNECT, 0, NULL, 0);
}

int tig_net_connect_configured(void)
{
    ENetAddress address;
    ENetPeer* peer;
    ENetEvent event;
    int waits;

    if (!coop_enabled) {
        return TIG_OK;
    }

    if (!library_ready && enet_initialize() != 0) {
        return TIG_ERR_GENERIC;
    }
    library_ready = true;

    if (enet_host != NULL) {
        if (active) {
            return TIG_OK;
        }
        /* Review M1: the host dropped us earlier; that ENet host is dead. */
        tig_debug_printf("tig_net: discarding stale client connection before reconnect\n");
        tig_net_reset_connection();
    }

    if (join_host[0] == '\0') {
        tig_debug_printf("tig_net: join host not configured\n");
        return TIG_ERR_GENERIC;
    }

    enet_host = enet_host_create(NULL, 1, TIG_NET_NUM_CHANNELS, 0, 0);
    if (enet_host == NULL) {
        tig_debug_printf("tig_net: client host create failed\n");
        return TIG_ERR_GENERIC;
    }

    if (enet_address_set_host(&address, join_host) != 0) {
        /* Review PT24 #2: unresolvable name - address.host is garbage. */
        tig_debug_printf("tig_net: cannot resolve join host '%s'\n", join_host);
        enet_host_destroy(enet_host);
        enet_host = NULL;
        return TIG_ERR_GENERIC;
    }
    address.port = (enet_uint16)join_port;

    peer = enet_host_connect(enet_host, &address, TIG_NET_NUM_CHANNELS, 0);
    if (peer == NULL) {
        tig_debug_printf("tig_net: connect to %s:%d failed\n", join_host, join_port);
        enet_host_destroy(enet_host);
        enet_host = NULL;
        return TIG_ERR_GENERIC;
    }

    is_host = false;
    active = false;

    for (waits = 0; waits < 50; waits++) {
        if (enet_host_service(enet_host, &event, 100) <= 0) {
            continue;
        }
        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            /* Not expected before CONNECT; never leak the packet. */
            enet_packet_destroy(event.packet);
            continue;
        }
        if (event.type == ENET_EVENT_TYPE_CONNECT) {
            clients[0].peer = peer;
            clients[0].active = true;
            tig_net_peer_set_slot(peer, 0);
            active = true;
            tig_debug_printf("tig_net: connected to %s:%d\n", join_host, join_port);
            tig_net_send_hello(peer);
            /* Slot assignment arrives via WELCOME ctrl packet. */
            return TIG_OK;
        }
    }

    tig_debug_printf("tig_net: timed out connecting to %s:%d\n", join_host, join_port);
    enet_peer_reset(peer);
    enet_host_destroy(enet_host);
    enet_host = NULL;
    return TIG_ERR_GENERIC;
}

void tig_net_reset_connection(void)
{
    int i;

    if (enet_host != NULL) {
        if (tig_net_send_pending_flush) {
            enet_host_flush(enet_host);
            tig_net_send_pending_flush = false;
        }
        for (i = 0; i < TIG_NET_MAX_PLAYERS; i++) {
            if (clients[i].peer != NULL) {
                enet_peer_disconnect_now(clients[i].peer, 0);
                clients[i].peer = NULL;
            }
        }
        enet_host_destroy(enet_host);
        enet_host = NULL;
    }

    tig_net_clear_clients();
    active = false;
    is_host = false;
    local_slot = 0;
}

int sub_5280F0(void)
{
    bool was_active = active;
    tig_net_reset_connection();
    /* Original returns non-zero if shutdown did not complete cleanly.
       multiplayer_end does `return !v1`. */
    return was_active ? 0 : 1;
}

static void tig_net_peer_diag_log(void)
{
    int i;

    /* PT23: queue depth / RTT per peer — a growing outgoing queue means
       latency that grows over a session ("everything gets laggy"). */
    for (i = 0; i < max_players; i++) {
        ENetPeer* p = clients[i].peer;

        if (p == NULL || (is_host && i == 0) || (!is_host && i != 0)) {
            continue;
        }
        tig_debug_printf("tig_net: peer %d rtt=%u ms lost=%u throttle=%u inflight=%u q_out=%u q_rel=%u q_sent=%u\n",
            i, p->roundTripTime, p->packetsLost, p->packetThrottle, p->reliableDataInTransit,
            (unsigned int)enet_list_size(&p->outgoingCommands),
            (unsigned int)enet_list_size(&p->outgoingSendReliableCommands),
            (unsigned int)enet_list_size(&p->sentReliableCommands));
    }
}

void tig_net_pump(void)
{
    ENetEvent event;

    if (!coop_enabled || enet_host == NULL) {
        return;
    }

    /* Review PT24 #4: bounded work per frame; the rest stays queued in
       ENet and is handled next frame. */
    int budget = TIG_NET_MAX_EVENTS_PER_PUMP;

    while (budget-- > 0 && enet_host != NULL && enet_host_service(enet_host, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            if (is_host) {
                int slot = tig_net_alloc_slot();
                TigNetCtrlWelcome welcome;

                if (slot < 0) {
                    tig_debug_printf("tig_net: rejecting client, no free slots\n");
                    event.peer->data = NULL; /* decodes to -1 */
                    enet_peer_disconnect(event.peer, 0);
                    break;
                }

                clients[slot].peer = event.peer;
                tig_net_peer_set_slot(event.peer, slot);
                clients[slot].active = true;
                clients[slot].waiting = false;
                clients[slot].loading = false;
                clients[slot].refused = false;
                clients[slot].handshaken = false;
                clients[slot].connect_ms = (unsigned int)enet_time_get();
                snprintf(clients[slot].name, sizeof(clients[slot].name), "Player%d", slot);

                memset(&welcome, 0, sizeof(welcome));
                welcome.magic = TIG_NET_CTRL_WELCOME;
                welcome.slot = slot;
                welcome.max_players = max_players;
                welcome.protocol = TIG_NET_PROTOCOL;
                tig_net_send_ctrl_to_peer(event.peer, &welcome, sizeof(welcome));
                enet_host_flush(enet_host);

                tig_debug_printf("tig_net: client connected -> slot %d (awaiting handshake)\n", slot);
            } else {
                clients[0].peer = event.peer;
                tig_net_peer_set_slot(event.peer, 0);
                tig_net_send_hello(event.peer);
            }
            break;

        case ENET_EVENT_TYPE_RECEIVE:
            {
                int from = tig_net_peer_get_slot(event.peer);
                if (from < 0) {
                    /* Rejected / unassigned peer: drop everything it sends. */
                } else if (event.channelID == TIG_NET_CHANNEL_CTRL) {
                    tig_net_handle_ctrl(event.packet->data, event.packet->dataLength, from);
                } else if (is_host && from > 0
                    && event.packet->dataLength >= sizeof(int)
                    && *(const int*)event.packet->data == TIG_NET_APP_HELLO) {
                    tig_net_handle_ctrl(event.packet->data, event.packet->dataLength, from);
                } else if (is_host ? clients[from].refused : host_refused) {
                    /* Protocol mismatch: disconnect pending, drop it. */
                } else if (is_host && from > 0 && !clients[from].handshaken) {
                    /* No valid HELLO yet: app traffic is not accepted. */
                    tig_debug_printf("tig_net: dropping app packet from slot %d before handshake\n", from);
                } else if (!is_host && from != 0) {
                    /* Clients only accept app traffic from the host. */
                } else if (is_host && from == 0) {
                    /* Host never receives from itself over the wire. */
                } else {
                    tig_net_stat_count(tig_net_stat_recv, event.packet->data, event.packet->dataLength);
                    tig_net_dispatch_app(event.packet->data, event.packet->dataLength, from);
                }
            }
            enet_packet_destroy(event.packet);
            break;

        case ENET_EVENT_TYPE_DISCONNECT: {
            int slot = tig_net_peer_get_slot(event.peer);
            tig_debug_printf("tig_net: disconnect slot %d\n", slot);
            /* Unassigned/rejected peers (slot -1) must not clear anything. */
            if (slot >= 0 && slot < TIG_NET_MAX_PLAYERS) {
                clients[slot].peer = NULL;
                clients[slot].active = false;
                clients[slot].waiting = false;
                clients[slot].loading = false;
                clients[slot].refused = false;
                clients[slot].handshaken = false;
                clients[slot].name[0] = '\0';
                tig_net_fire_event(TIG_NET_EVENT_DISCONNECT, slot, NULL, 0);
            }
            event.peer->data = NULL;
            if (!is_host) {
                active = false;
                lost_host = true;
            }
            break;
        }

        default:
            break;
        }
    }

    if (is_host && enet_host != NULL) {
        unsigned int now = (unsigned int)enet_time_get();
        int i;

        for (i = 1; i < max_players; i++) {
            if (clients[i].peer != NULL && !clients[i].handshaken && !clients[i].refused
                && now - clients[i].connect_ms > TIG_NET_HANDSHAKE_TIMEOUT_MS) {
                tig_debug_printf("tig_net: slot %d sent no valid handshake in %u ms - disconnecting\n",
                    i, TIG_NET_HANDSHAKE_TIMEOUT_MS);
                clients[i].refused = true;
                enet_peer_disconnect(clients[i].peer, 0);
            }
        }
    }
    if (tig_net_send_pending_flush && enet_host != NULL) {
        enet_host_flush(enet_host);
        tig_net_send_pending_flush = false;
    }
    if (lost_host) {
        /* Review M1: free the dead client host so a later Join works. */
        lost_host = false;
        tig_net_reset_connection();
    }
    tig_net_stat_maybe_log();
    if (tig_net_stat_logged) {
        tig_net_stat_logged = false;
        tig_net_peer_diag_log();
    }
}

bool tig_net_is_active(void)
{
    return coop_enabled && active && local_suspend_depth == 0;
}

bool tig_net_is_host(void)
{
    return coop_enabled && active && is_host && local_suspend_depth == 0;
}

bool tig_net_is_coop_client(void)
{
    return coop_enabled && active && !is_host;
}

void tig_net_local_suspend_begin(void)
{
    local_suspend_depth++;
}

void tig_net_local_suspend_end(void)
{
    if (local_suspend_depth > 0) {
        local_suspend_depth--;
    }
}

int tig_net_local_suspend_pause(void)
{
    int depth = local_suspend_depth;

    local_suspend_depth = 0;
    return depth;
}

void tig_net_local_suspend_resume(int depth)
{
    local_suspend_depth = depth > 0 ? depth : 0;
}

bool tig_net_is_locally_suspended(void)
{
    return local_suspend_depth > 0;
}

int tig_net_last_message_size(void)
{
    return last_message_size;
}

void tig_net_on_message(TigNetMessageFunc func)
{
    message_func = func;
}

void tig_net_on_message_validation(TigNetMessageValidationFunc func)
{
    validation_func = func;
}

void tig_net_on_network_event(TigNetNetworkEventFunc func)
{
    network_event_func = func;
}

static void tig_net_send_raw_ex(void* data, int size, int client_id, bool all, bool except, bool pose)
{
    ENetPacket* packet;
    int i;
    int sent = 0;
    enet_uint8 channel = pose ? TIG_NET_CHANNEL_POSE : TIG_NET_CHANNEL_APP;
    enet_uint32 flags = pose ? 0 : ENET_PACKET_FLAG_RELIABLE;

    if (!tig_net_is_active() || data == NULL || size <= 0 || size > TIG_NET_MAX_PACKET) {
        return;
    }
    if (!is_host && host_refused) {
        return; /* refused host (protocol): nothing of ours reaches it */
    }

    /* Review L3: one packet for every recipient; ENet takes a reference per
       successful send, and a failed send leaves it ours to free. */
    packet = enet_packet_create(data, (size_t)size, flags);
    if (packet == NULL) {
        return;
    }

    if (is_host) {
        for (i = 1; i < max_players; i++) {
            if (!clients[i].active || clients[i].peer == NULL) {
                continue;
            }
            if (all) {
                /* send to everyone */
            } else if (except) {
                if (i == client_id) {
                    continue;
                }
            } else {
                if (i != client_id) {
                    continue;
                }
            }
            if (enet_peer_send(clients[i].peer, channel, packet) == 0) {
                sent++;
            }
        }
    } else if (clients[0].peer != NULL) {
        /* Clients only talk to host. */
        if (enet_peer_send(clients[0].peer, channel, packet) == 0) {
            sent++;
        }
    }

    if (packet->referenceCount == 0) {
        enet_packet_destroy(packet);
    }
    if (sent > 0) {
        /* Batched: flushed once per frame in tig_net_pump (was per packet). */
        tig_net_send_pending_flush = true;
        tig_net_stat_count(tig_net_stat_sent, data, (size_t)size);
    }
}

static void tig_net_send_raw(void* data, int size, int client_id, bool all, bool except)
{
    tig_net_send_raw_ex(data, size, client_id, all, except, false);
}

void tig_net_send_pose_all(void* data, int size)
{
    tig_net_send_raw_ex(data, size, -1, true, false, true);
}

void tig_net_send_pose_except(void* data, int size, int client_id)
{
    tig_net_send_raw_ex(data, size, client_id, false, true, true);
}

void tig_net_set_remote_slot_present(int slot, bool present)
{
    /* Client bookkeeping only (review H3): other clients' slots exist so
       their PCs resolve; clients never send to them (only to slot 0). */
    if (is_host || slot <= 0 || slot >= TIG_NET_MAX_PLAYERS || slot == local_slot) {
        return;
    }
    clients[slot].active = present;
    clients[slot].peer = NULL;
}

void tig_net_send_app_all(void* data, int size)
{
    tig_net_send_raw(data, size, -1, true, false);
}

void tig_net_send_app(void* data, int size, int client_id)
{
    tig_net_send_raw(data, size, client_id, false, false);
}

void tig_net_send_app_except(void* data, int size, int client_id)
{
    tig_net_send_raw(data, size, client_id, false, true);
}

int tig_net_local_client_set_name(const char* name)
{
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    strncpy(local_name, name, TIG_NET_MAX_NAME - 1);
    local_name[TIG_NET_MAX_NAME - 1] = '\0';
    if (local_slot >= 0 && local_slot < TIG_NET_MAX_PLAYERS) {
        strncpy(clients[local_slot].name, local_name, TIG_NET_MAX_NAME - 1);
    }
    return 1;
}

int tig_net_local_server_set_max_players(int value)
{
    if (value < 1) {
        value = 1;
    }
    if (value > TIG_NET_MAX_PLAYERS) {
        value = TIG_NET_MAX_PLAYERS;
    }
    max_players = value;
    return 1;
}

int tig_net_local_server_set_description(const char* description)
{
    if (description == NULL) {
        return 0;
    }
    strncpy(server_description, description, sizeof(server_description) - 1);
    return 1;
}

void tig_net_local_server_set_name(const char* name)
{
    if (name == NULL) {
        return;
    }
    strncpy(server_name, name, sizeof(server_name) - 1);
}

int tig_net_local_server_get_max_players(void)
{
    return max_players;
}

unsigned int tig_net_local_server_get_options(void)
{
    return server_options;
}

void tig_net_local_server_set_options(unsigned int options)
{
    server_options = options;
}

bool tig_net_client_is_active(int client_id)
{
    if (client_id < 0 || client_id >= TIG_NET_MAX_PLAYERS) {
        return false;
    }
    return clients[client_id].active;
}

bool tig_net_client_is_waiting(int client_id)
{
    if (client_id < 0 || client_id >= TIG_NET_MAX_PLAYERS) {
        return false;
    }
    return clients[client_id].waiting;
}

bool tig_net_client_is_loading(int client_id)
{
    if (client_id < 0 || client_id >= TIG_NET_MAX_PLAYERS) {
        return false;
    }
    return clients[client_id].loading;
}

const char* tig_net_client_info_get_name(int client_id)
{
    if (client_id < 0 || client_id >= TIG_NET_MAX_PLAYERS) {
        return "";
    }
    if (clients[client_id].name[0] == '\0') {
        return local_name;
    }
    return clients[client_id].name;
}

int tig_net_xfer_count(int client_id)
{
    (void)client_id;
    return 0;
}

void tig_net_xfer_send_as(const char* src, const char* dst, int client_id, void* unused)
{
    (void)src;
    (void)dst;
    (void)client_id;
    (void)unused;
}

void tig_net_xfer_send(const char* path, int client_id, void* unused)
{
    (void)path;
    (void)client_id;
    (void)unused;
}

int sub_529520(void)
{
    return local_slot;
}

int sub_52A530(void)
{
    return last_sender_slot;
}

int sub_52A900(void)
{
    /* Non-zero pauses game/anim time for clients waiting on sync. */
    return 0;
}

void sub_52A940(void)
{
}

void sub_52A950(void)
{
}

void sub_52B210(void)
{
}

void sub_5286E0(void)
{
}

void sub_52A9E0(int client_id)
{
    if (client_id >= 0 && client_id < TIG_NET_MAX_PLAYERS) {
        clients[client_id].loading = false;
        clients[client_id].waiting = false;
    }
}

bool tig_net_host_refused(void)
{
    return host_refused;
}
