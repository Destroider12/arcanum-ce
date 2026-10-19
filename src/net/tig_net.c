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

#define TIG_NET_CTRL_WELCOME 1
#define TIG_NET_CTRL_NAME 2

typedef struct TigNetCtrlWelcome {
    int magic; /* TIG_NET_CTRL_WELCOME */
    int slot;
    int max_players;
} TigNetCtrlWelcome;

typedef struct TigNetCtrlName {
    int magic; /* TIG_NET_CTRL_NAME */
    int slot;
    char name[TIG_NET_MAX_NAME];
} TigNetCtrlName;

typedef struct TigNetClient {
    ENetPeer* peer;
    bool active;
    bool waiting;
    bool loading;
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
    char copy[TIG_NET_MAX_PACKET];

    if (size < sizeof(int) || size > sizeof(copy)) {
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
    enet_peer_send(peer, TIG_NET_CHANNEL_CTRL, packet);
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
        if (is_host || from_slot != 0 || size < sizeof(TigNetCtrlWelcome)) {
            tig_debug_printf("tig_net: ignoring WELCOME (host=%d from=%d size=%zu)\n",
                is_host ? 1 : 0, from_slot, size);
            return;
        }
        memcpy(&welcome_copy, data, sizeof(welcome_copy));
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

        /* Tell host our name. */
        if (!is_host && clients[0].peer != NULL) {
            TigNetCtrlName name_pkt;
            memset(&name_pkt, 0, sizeof(name_pkt));
            name_pkt.magic = TIG_NET_CTRL_NAME;
            name_pkt.slot = local_slot;
            strncpy(name_pkt.name, local_name, TIG_NET_MAX_NAME - 1);
            tig_net_send_ctrl_to_peer(clients[0].peer, &name_pkt, sizeof(name_pkt));
        }
        return;
    }

    if (magic == TIG_NET_CTRL_NAME && size >= sizeof(TigNetCtrlName)) {
        TigNetCtrlName name_pkt;

        /* Host only; the slot is the authenticated sender, not the payload. */
        if (!is_host || from_slot <= 0 || from_slot >= TIG_NET_MAX_PLAYERS) {
            return;
        }
        memcpy(&name_pkt, data, sizeof(name_pkt));
        name_pkt.name[TIG_NET_MAX_NAME - 1] = '\0';
        strncpy(clients[from_slot].name, name_pkt.name, TIG_NET_MAX_NAME - 1);
        clients[from_slot].name[TIG_NET_MAX_NAME - 1] = '\0';
    }
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

    enet_host = enet_host_create(&address, (size_t)max_players, 2, 0, 0);
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
        return TIG_OK;
    }

    if (join_host[0] == '\0') {
        tig_debug_printf("tig_net: join host not configured\n");
        return TIG_ERR_GENERIC;
    }

    enet_host = enet_host_create(NULL, 1, 2, 0, 0);
    if (enet_host == NULL) {
        tig_debug_printf("tig_net: client host create failed\n");
        return TIG_ERR_GENERIC;
    }

    enet_address_set_host(&address, join_host);
    address.port = (enet_uint16)join_port;

    peer = enet_host_connect(enet_host, &address, 2, 0);
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

void tig_net_pump(void)
{
    ENetEvent event;

    if (!coop_enabled || enet_host == NULL) {
        return;
    }

    while (enet_host_service(enet_host, &event, 0) > 0) {
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
                snprintf(clients[slot].name, sizeof(clients[slot].name), "Player%d", slot);

                memset(&welcome, 0, sizeof(welcome));
                welcome.magic = TIG_NET_CTRL_WELCOME;
                welcome.slot = slot;
                welcome.max_players = max_players;
                tig_net_send_ctrl_to_peer(event.peer, &welcome, sizeof(welcome));
                enet_host_flush(enet_host);

                tig_debug_printf("tig_net: client connected -> slot %d\n", slot);
                tig_net_fire_event(TIG_NET_EVENT_CONNECT, slot, NULL, 0);
            } else {
                clients[0].peer = event.peer;
                tig_net_peer_set_slot(event.peer, 0);
            }
            break;

        case ENET_EVENT_TYPE_RECEIVE:
            {
                int from = tig_net_peer_get_slot(event.peer);
                if (from < 0) {
                    /* Rejected / unassigned peer: drop everything it sends. */
                } else if (event.channelID == TIG_NET_CHANNEL_CTRL) {
                    tig_net_handle_ctrl(event.packet->data, event.packet->dataLength, from);
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
                clients[slot].name[0] = '\0';
                tig_net_fire_event(TIG_NET_EVENT_DISCONNECT, slot, NULL, 0);
            }
            event.peer->data = NULL;
            if (!is_host) {
                active = false;
            }
            break;
        }

        default:
            break;
        }
    }

    if (tig_net_send_pending_flush && enet_host != NULL) {
        enet_host_flush(enet_host);
        tig_net_send_pending_flush = false;
    }
    tig_net_stat_maybe_log();
}

bool tig_net_is_active(void)
{
    return coop_enabled && active && local_suspend_depth == 0;
}

bool tig_net_is_host(void)
{
    return coop_enabled && active && is_host && local_suspend_depth == 0;
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

static void tig_net_send_raw(void* data, int size, int client_id, bool all, bool except)
{
    ENetPacket* packet;
    int i;

    if (!tig_net_is_active() || data == NULL || size <= 0 || size > TIG_NET_MAX_PACKET) {
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

            packet = enet_packet_create(data, (size_t)size, ENET_PACKET_FLAG_RELIABLE);
            if (packet != NULL) {
                enet_peer_send(clients[i].peer, TIG_NET_CHANNEL_APP, packet);
            }
        }
        /* Batched: flushed once per frame in tig_net_pump (was per packet). */
        tig_net_send_pending_flush = true;
        tig_net_stat_count(tig_net_stat_sent, data, (size_t)size);
    } else {
        /* Clients only talk to host. */
        if (clients[0].peer == NULL) {
            return;
        }
        packet = enet_packet_create(data, (size_t)size, ENET_PACKET_FLAG_RELIABLE);
        if (packet != NULL) {
            enet_peer_send(clients[0].peer, TIG_NET_CHANNEL_APP, packet);
            tig_net_send_pending_flush = true;
            tig_net_stat_count(tig_net_stat_sent, data, (size_t)size);
        }
    }
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
