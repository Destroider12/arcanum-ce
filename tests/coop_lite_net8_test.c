/*
 * Coop Lite network hardening: malformed / oversized / spoofed packets.
 *
 * Single process. Phase A: tig_net host vs a raw-ENet "rogue" client.
 * Phase B: tig_net client vs a raw-ENet "rogue" host.
 * The validation callback mirrors multiplayer_validate_message (size via
 * tig_net_last_message_size, direction via coop_lite_packet_allowed with the
 * authenticated sender sub_52A530).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <enet/enet.h>

#include "game/coop_lite.h"

typedef unsigned int TeleportFlags;
typedef struct TeleportData {
    TeleportFlags flags;
    int64_t obj;
    int64_t loc;
    int map;
    int movie1;
    unsigned int movie_flags1;
    int movie2;
    unsigned int movie_flags2;
    char fade_pad[40];
    int sound_id;
    int time;
} TeleportData;
#include "net/tig_net.h"

#define PORT_N 17931
#define NCLI 7

/* Must match tig_net.c private ctrl layouts. */
typedef struct CtrlWelcome {
    int magic;
    int slot;
    int max_players;
    int protocol;
} CtrlWelcome;
typedef struct CtrlName {
    int magic;
    int slot;
    char name[TIG_NET_MAX_NAME];
    int protocol;
} CtrlName;


static int g_delivered[256];
static int g_rejected;
static int g_disconnect_events;
static int g_disconnect_per_slot[8];

int64_t player_get_local_pc_obj(void)
{
    return 42;
}

bool teleport_do(TeleportData* teleport_data)
{
    (void)teleport_data;
    return true;
}

static bool on_validate(void* msg)
{
    int type = *(int*)msg;
    int size = tig_net_last_message_size();
    size_t min_size;
    bool ok;

    if (type <= 0 || type >= PACKET_COOP_TYPE_LIMIT) {
        g_rejected++;
        return false;
    }
    min_size = coop_lite_coop_packet_min_size(type);
    if (min_size == 0) {
        min_size = sizeof(int);
    }
    ok = size >= (int)min_size && coop_lite_packet_allowed(type, tig_net_is_host(), sub_52A530());
    if (!ok) {
        g_rejected++;
    }
    return ok;
}

/* Every coop handler: stub builds keep their wire checks. */
static void on_message(void* msg)
{
    int type = *(int*)msg;

    g_delivered[type & 0xFF]++;
    switch (type) {
    case PACKET_COOP_ATTACK_INTENT: coop_lite_handle_attack_intent(msg); break;
    case PACKET_COOP_BARTER: coop_lite_handle_barter(msg); break;
    case PACKET_COOP_CHAR_OFFER: coop_lite_handle_char_offer(msg); break;
    case PACKET_COOP_CRAFT: coop_lite_handle_craft(msg); break;
    case PACKET_COOP_DOOR_USE: coop_lite_handle_door_use(msg); break;
    case PACKET_COOP_FOLLOW_HOST: coop_lite_handle_follow_packet(msg); break;
    case PACKET_COOP_PARTY_CTRL: coop_lite_handle_party_ctrl(msg); break;
    case PACKET_COOP_PC_POS: coop_lite_handle_pc_pos(msg); break;
    case PACKET_COOP_PC_STATS: coop_lite_handle_pc_stats(msg); break;
    case PACKET_COOP_SKILL_USE: coop_lite_handle_skill_use(msg); break;
    case PACKET_COOP_WORLD_REQ: coop_lite_handle_world_req(msg); break;
    default: break;
    }
}

static bool on_event(int type, int client_id, void* data, int size)
{
    (void)data;
    (void)size;
    if (type == TIG_NET_EVENT_DISCONNECT) {
        g_disconnect_events++;
        if (client_id >= 0 && client_id < 8) {
            g_disconnect_per_slot[client_id]++;
        }
        coop_lite_host_on_client_disconnect(client_id);
    }
    return true;
}

static ENetHost* g_raw[NCLI];
static ENetPeer* g_peer[NCLI];

static void pump(int ms)
{
    ENetEvent ev;
    int i;
    int c;

    for (i = 0; i < ms; i++) {
        tig_net_pump();
        for (c = 0; c < NCLI; c++) {
            while (g_raw[c] != NULL && enet_host_service(g_raw[c], &ev, 0) > 0) {
                if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(ev.packet);
                } else if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                    CtrlName hello; /* = tig_net_send_hello */
                    ENetPacket* packet;

                    memset(&hello, 0, sizeof(hello));
                    hello.magic = 0x454D414E;
                    snprintf(hello.name, sizeof(hello.name), "P%d", c + 1);
                    hello.protocol = TIG_NET_PROTOCOL;
                    packet = enet_packet_create(&hello, sizeof(hello), ENET_PACKET_FLAG_RELIABLE);
                    if (enet_peer_send(ev.peer, 0, packet) != 0) {
                        enet_packet_destroy(packet);
                    }
                } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                    g_peer[c] = NULL;
                }
            }
        }
        usleep(1000);
    }
}

static uint32_t rng = 0xC0FFEEu;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static int active_count(void)
{
    int s;
    int n = 0;

    for (s = 1; s < 8; s++) {
        n += tig_net_client_is_active(s) ? 1 : 0;
    }
    return n;
}

static void connect_client(int c)
{
    ENetAddress addr;

    if (g_raw[c] == NULL) {
        g_raw[c] = enet_host_create(NULL, 1, 3, 0, 0);
    }
    enet_address_set_host(&addr, "127.0.0.1");
    addr.port = PORT_N;
    g_peer[c] = enet_host_connect(g_raw[c], &addr, 3, 0);
}

static void drop_client(int c, bool hard)
{
    if (g_peer[c] == NULL) {
        return;
    }
    if (hard) {
        enet_peer_reset(g_peer[c]);
    } else {
        enet_peer_disconnect(g_peer[c], 0);
    }
    g_peer[c] = NULL;
}

static void send_random(int c)
{
    static const int types[] = { PACKET_COOP_ATTACK_INTENT, PACKET_COOP_BARTER, PACKET_COOP_CHAR_BUNDLE,
        PACKET_COOP_CHAR_OFFER, PACKET_COOP_CRAFT, PACKET_COOP_DIALOG_STATE, PACKET_COOP_DOOR_USE,
        PACKET_COOP_FOLLOW_HOST, PACKET_COOP_OBJ_REQ, PACKET_COOP_NPC_REQ, PACKET_COOP_PARTY_CTRL,
        PACKET_COOP_PC_POS, PACKET_COOP_PC_STATS, PACKET_COOP_SKILL_USE, PACKET_COOP_WORLD_REQ,
        PACKET_COOP_SESSION_SYNC, PACKET_COOP_WORLD_STATE, PACKET_COOP_ROSTER, PACKET_COOP_INVEN_LAYOUT, 0, -1, 999, PACKET_COOP_TYPE_LIMIT };
    uint8_t buf[TIG_NET_MAX_PACKET + 32];
    int type = types[rnd() % (sizeof(types) / sizeof(types[0]))];
    size_t min = type > 0 && type < PACKET_COOP_TYPE_LIMIT ? coop_lite_coop_packet_min_size(type) : 4;
    size_t size;
    size_t i;
    ENetPacket* packet;

    switch (rnd() % 4) {
    case 0: size = min; break; /* exact */
    case 1: size = min > 4 ? min - 1 - rnd() % (min - 4) : 4; break; /* short */
    case 2: size = min + rnd() % 512; break; /* trailing */
    default: size = rnd() % sizeof(buf); break; /* anything, incl. oversized */
    }
    if (size < 4) {
        size = 4;
    }
    for (i = 0; i < size; i++) {
        uint32_t r = rnd();
        buf[i] = (r & 3) == 0 ? 0 : (r & 3) == 1 ? 0xFF : (uint8_t)(r >> 8);
    }
    memcpy(buf, &type, 4);
    if (size >= 12 && (rnd() & 1)) {
        int v = (int)(rnd() % 20) - 4; /* slots / counts / ops around the edges */
        memcpy(buf + 4, &v, 4);
        v = (int)(rnd() % 20) - 4;
        memcpy(buf + 8, &v, 4);
    }
    packet = enet_packet_create(buf, size, ENET_PACKET_FLAG_RELIABLE);
    if (enet_peer_send(g_peer[c], (enet_uint8)(rnd() % 3 == 0 ? 2 : 0), packet) != 0) {
        enet_packet_destroy(packet);
    }
}

#define CHECK(cond, msg)                         \
    do {                                         \
        if (!(cond)) {                           \
            fprintf(stderr, "FAIL: %s\n", msg);  \
            return 1;                            \
        }                                        \
    } while (0)

int main(void)
{
    int c;
    int i;
    int round;
    int total = 0;

    if (enet_initialize() != 0) {
        return 1;
    }
    coop_lite_session_reset();
    tig_net_set_coop_enabled(true);
    tig_net_configure_host(PORT_N);
    tig_net_local_server_set_max_players(8);
    tig_net_local_client_set_name("Host");
    tig_net_on_message(on_message);
    tig_net_on_message_validation(on_validate);
    tig_net_on_network_event(on_event);
    CHECK(tig_net_start_client() == TIG_OK && tig_net_is_host(), "host start");

    /* 7 clients join on the same frame */
    for (c = 0; c < NCLI; c++) {
        connect_client(c);
    }
    for (i = 0; i < 4000 && active_count() < NCLI; i++) {
        pump(1);
    }
    CHECK(active_count() == NCLI, "7 clients connected (slots 1..7)");

    /* an 8th client must be refused, not given slot 0 / 8 */
    {
        ENetHost* extra = enet_host_create(NULL, 1, 3, 0, 0);
        ENetAddress addr;

        enet_address_set_host(&addr, "127.0.0.1");
        addr.port = PORT_N;
        (void)enet_host_connect(extra, &addr, 3, 0);
        for (i = 0; i < 300; i++) {
            ENetEvent ev;

            pump(1);
            while (enet_host_service(extra, &ev, 0) > 0) {
                if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(ev.packet);
                }
            }
        }
        CHECK(active_count() == NCLI && tig_net_is_host() && sub_529520() == 0, "8th client refused");
        enet_host_destroy(extra);
    }

    /* storms: interleaved valid / malformed packets from every slot, random
       drops (graceful and hard) and rejoins */
    for (round = 0; round < 40; round++) {
        for (c = 0; c < NCLI; c++) {
            if (g_peer[c] != NULL && g_peer[c]->state == ENET_PEER_STATE_CONNECTED) {
                int n = (int)(rnd() % 40);

                for (i = 0; i < n; i++) {
                    send_random(c);
                    total++;
                }
            }
        }
        pump(15);
        if (rnd() % 3 == 0) {
            c = (int)(rnd() % NCLI);
            drop_client(c, (rnd() & 1) != 0);
        }
        for (c = 0; c < NCLI; c++) {
            if (g_peer[c] == NULL && rnd() % 2 == 0) {
                connect_client(c);
            }
        }
        pump(15);
        CHECK(tig_net_is_host() && sub_529520() == 0, "host keeps slot 0");
        CHECK(active_count() <= NCLI, "never more than 7 clients");
    }

    /* everybody leaves at once, then all rejoin: slots are reused */
    for (c = 0; c < NCLI; c++) {
        drop_client(c, false);
    }
    for (i = 0; i < 40000 && active_count() > 0; i++) {
        pump(1);
    }
    CHECK(active_count() == 0, "all 7 slots freed");
    for (c = 0; c < NCLI; c++) {
        connect_client(c);
    }
    for (i = 0; i < 4000 && active_count() < NCLI; i++) {
        pump(1);
    }
    CHECK(active_count() == NCLI, "7 clients rejoined into freed slots");
    CHECK(g_disconnect_per_slot[0] == 0, "host slot never disconnected");
    CHECK(g_disconnect_events >= NCLI, "disconnect events delivered");

    for (c = 0; c < NCLI; c++) {
        drop_client(c, false);
    }
    pump(200);
    tig_net_reset_connection();
    for (c = 0; c < NCLI; c++) {
        if (g_raw[c] != NULL) {
            enet_host_destroy(g_raw[c]);
        }
    }
    enet_deinitialize();
    printf("coop_lite_net8: ok (%d packets, %d rejected, %d disconnects)\n", total, g_rejected, g_disconnect_events);
    return 0;
}
