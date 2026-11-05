/*
 * PT53d: transport-level fuzz of the Coop Lite receive handlers (stub build).
 * A tig_net host receives random / truncated / oversized app packets of every
 * coop type from a raw-ENet rogue client; the validator mirrors
 * multiplayer_validate_message and every coop handler is dispatched as in
 * multiplayer.c. Then the same handlers run in client role on exact-size heap
 * copies. Run under ASan/UBSan. COOP_RX_FUZZ_SECONDS (default 5) sets the
 * host-phase duration.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

#define PORT_F 17921

/* Must match tig_net.c private ctrl layout (see coop_lite_net_hardening_test). */
typedef struct CtrlName {
    int magic;
    int slot;
    char name[TIG_NET_MAX_NAME];
    int protocol;
} CtrlName;

static long g_delivered;
static uint32_t rng = 0xC0FFEE11u;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

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

    if (type <= 0 || type >= PACKET_COOP_TYPE_LIMIT) {
        return false;
    }
    min_size = coop_lite_coop_packet_min_size(type);
    if (min_size == 0) {
        min_size = sizeof(int);
    }
    if (size < (int)min_size) {
        return false;
    }
    return coop_lite_packet_allowed(type, tig_net_is_host(), sub_52A530());
}

/* Handlers compiled only in the full game (not in the stub build) are
   left out: char_chunk, dialog_state, followers, npc_pos, npc_req,
   obj_blob, obj_req, party_state, story_sync. */
static void dispatch(void* msg)
{
    switch (*(int*)msg) {
    case PACKET_COOP_FOLLOW_HOST: coop_lite_handle_follow_packet(msg); break;
    case PACKET_COOP_ATTACK_INTENT: coop_lite_handle_attack_intent(msg); break;
    case PACKET_COOP_SWING: coop_lite_handle_swing(msg); break;
    case PACKET_COOP_CHAR_OFFER: coop_lite_handle_char_offer(msg); break;
    case PACKET_COOP_SESSION_SYNC: coop_lite_handle_session_sync(msg); break;
    case PACKET_COOP_PC_POS: coop_lite_handle_pc_pos(msg); break;
    case PACKET_COOP_BARTER: coop_lite_handle_barter(msg); break;
    case PACKET_COOP_INVEN_SYNC: coop_lite_handle_inven_sync(msg); break;
    case PACKET_COOP_ROSTER: coop_lite_handle_roster(msg); break;
    case PACKET_COOP_PC_STATS: coop_lite_handle_pc_stats(msg); break;
    case PACKET_COOP_WORLD_REQ: coop_lite_handle_world_req(msg); break;
    case PACKET_COOP_WORLD_STATE: coop_lite_handle_world_state(msg); break;
    case PACKET_COOP_DOOR_USE: coop_lite_handle_door_use(msg); break;
    case PACKET_COOP_SKILL_USE: coop_lite_handle_skill_use(msg); break;
    case PACKET_COOP_CRAFT: coop_lite_handle_craft(msg); break;
    case PACKET_COOP_PARTY_CTRL: coop_lite_handle_party_ctrl(msg); break;
    case PACKET_COOP_INVEN_LAYOUT: coop_lite_host_handle_inven_layout(msg); break;
    default: break;
    }
}

static void on_message(void* msg)
{
    g_delivered++;
    dispatch(msg);
}

static bool on_event(int type, int client_id, void* data, int size)
{
    (void)data;
    (void)size;
    if (type == TIG_NET_EVENT_DISCONNECT) {
        coop_lite_host_on_client_disconnect(client_id);
    }
    return true;
}

static const int k_types[] = {
    PACKET_COOP_FOLLOW_HOST, PACKET_COOP_ATTACK_INTENT, PACKET_COOP_SWING, PACKET_COOP_CHAR_BUNDLE,
    PACKET_COOP_FOLLOWERS, PACKET_COOP_NPC_REQ, PACKET_COOP_NPC_POS, PACKET_COOP_PARTY_STATE,
    PACKET_COOP_CHAR_OFFER, PACKET_COOP_SESSION_SYNC, PACKET_COOP_PC_POS, PACKET_COOP_STORY_SYNC,
    PACKET_COOP_DIALOG_STATE, PACKET_COOP_BARTER, PACKET_COOP_OBJ_REQ, PACKET_COOP_OBJ_BLOB,
    PACKET_COOP_INVEN_SYNC, PACKET_COOP_ROSTER, PACKET_COOP_PC_STATS, PACKET_COOP_WORLD_REQ,
    PACKET_COOP_WORLD_STATE, PACKET_COOP_DOOR_USE, PACKET_COOP_SKILL_USE, PACKET_COOP_CRAFT,
    PACKET_COOP_PARTY_CTRL, PACKET_COOP_INVEN_LAYOUT,
};
#define NTYPES ((int)(sizeof(k_types) / sizeof(k_types[0])))

/* Random packet: valid type, size around the type's minimum (truncated,
   exact, padded, oversized), random body with small counts/indices often. */
static int make_packet(uint8_t* buf, int cap)
{
    int type = k_types[rnd() % NTYPES];
    int min = (int)coop_lite_coop_packet_min_size(type);
    int size;
    int i;

    switch (rnd() % 5) {
    case 0: size = (int)(rnd() % (unsigned)(min + 1)); break;
    case 1: size = min; break;
    case 2: size = min + (int)(rnd() % 64); break;
    case 3: size = (int)(rnd() % (unsigned)cap); break;
    default: size = cap; break;
    }
    if (size < 4) {
        size = 4;
    }
    if (size > cap) {
        size = cap;
    }
    for (i = 0; i < size; i++) {
        uint32_t r = rnd();
        buf[i] = (r & 3) == 0 ? (uint8_t)(r >> 8) : (uint8_t)((r >> 8) % 9); /* small values: counts, slots */
    }
    if (rnd() & 1) {
        for (i = 4; i + 4 <= size && i < 64; i += 4) {
            int v = (int)(rnd() % 5) - 1; /* -1..3 */
            if (rnd() & 1) {
                memcpy(buf + i, &v, 4);
            }
        }
    }
    memcpy(buf, &type, 4);
    return size;
}

/* PT55: COOP_RX_FUZZ_CLIENTS (1..7) rogue clients fuzz the host at once. */
static int phase_host(int seconds, int nrogue)
{
    ENetHost* rogue_host[7];
    ENetPeer* rogue[7];
    ENetAddress addr;
    ENetEvent ev;
    static uint8_t buf[TIG_NET_MAX_PACKET + 64];
    time_t end;
    time_t last_trace = 0;
    long sent = 0;
    int i;
    int r;
    int up;

    coop_lite_session_reset();
    tig_net_set_coop_enabled(true);
    tig_net_configure_host(PORT_F);
    tig_net_local_server_set_max_players(8);
    tig_net_local_client_set_name("Host");
    tig_net_on_message(on_message);
    tig_net_on_message_validation(on_validate);
    tig_net_on_network_event(on_event);
    if (tig_net_start_client() != TIG_OK || !tig_net_is_host()) {
        fprintf(stderr, "host start failed\n");
        return 1;
    }
    enet_address_set_host(&addr, "127.0.0.1");
    addr.port = PORT_F;
    for (r = 0; r < nrogue; r++) {
        rogue_host[r] = enet_host_create(NULL, 1, 2, 0, 0);
        rogue[r] = enet_host_connect(rogue_host[r], &addr, 2, 0);
    }
    for (i = 0; i < 200 + 3000; i++) {
        tig_net_pump();
        up = 0;
        for (r = 0; r < nrogue; r++) {
            while (enet_host_service(rogue_host[r], &ev, 0) > 0) {
                if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(ev.packet);
                } else if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                    CtrlName name;

                    memset(&name, 0, sizeof(name));
                    name.magic = 2;
                    name.slot = r + 1;
                    snprintf(name.name, sizeof(name.name), "Rogue%d", r + 1);
                    name.protocol = TIG_NET_PROTOCOL;
                    enet_peer_send(rogue[r], 1, enet_packet_create(&name, sizeof(name), ENET_PACKET_FLAG_RELIABLE));
                }
            }
            up += tig_net_client_is_active(r + 1) ? 1 : 0;
        }
        if (up == nrogue && i > 200) {
            break;
        }
        usleep(1000);
    }
    if (up != nrogue) {
        fprintf(stderr, "only %d of %d rogues connected\n", up, nrogue);
        return 1;
    }
    end = time(NULL) + seconds;
    while (time(NULL) < end) {
        up = 0;
        for (r = 0; r < nrogue; r++) {
            if (rogue[r] == NULL) {
                continue;
            }
            up++;
            /* PT55: a sender-side backlog is harness memory, not the host's:
               keep each rogue's ENet queue short (10-minute runs OOMed). */
            if (enet_list_size(&rogue[r]->outgoingSendReliableCommands) + enet_list_size(&rogue[r]->outgoingCommands) > 512) {
                continue;
            }
            for (i = 0; i < 32; i++) {
                int size = make_packet(buf, (int)sizeof(buf));
                ENetPacket* p = enet_packet_create(buf, (size_t)size, ENET_PACKET_FLAG_RELIABLE);

                if (enet_peer_send(rogue[r], (enet_uint8)(rnd() % 2 == 0 ? 0 : 1), p) < 0) {
                    enet_packet_destroy(p);
                }
                sent++;
            }
            enet_host_flush(rogue_host[r]);
        }
        tig_net_pump();
        for (r = 0; r < nrogue; r++) {
            while (enet_host_service(rogue_host[r], &ev, 0) > 0) {
                if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(ev.packet);
                }
            }
            if (rogue[r] != NULL && !tig_net_client_is_active(r + 1)) {
                fprintf(stderr, "rogue %d dropped by host after %ld packets (ok: rate/abuse guard)\n", r + 1, sent);
                rogue[r] = NULL;
            }
        }
        if (up == 0) {
            break;
        }
        if (getenv("COOP_RX_FUZZ_TRACE") != NULL && time(NULL) % 10 == 0 && time(NULL) != last_trace) {
            size_t q = 0;
            long pages = 0, res = 0;
            FILE* f = fopen("/proc/self/statm", "r");

            last_trace = time(NULL);
            if (f != NULL) { if (fscanf(f, "%ld %ld", &pages, &res) != 2) res = 0; fclose(f); }
            for (r = 0; r < nrogue; r++) {
                if (rogue[r] != NULL) q += enet_list_size(&rogue[r]->outgoingSendReliableCommands) + enet_list_size(&rogue[r]->outgoingCommands);
            }
            printf("trace: rss %ld MB, rogue queued %zu\n", res * 4 / 1024, q);
            fflush(stdout);
        }
    }
    fflush(stderr);
    printf("host phase: %d rogues, %ld sent, %ld delivered to handlers\n", nrogue, sent, g_delivered);
    for (r = 0; r < nrogue; r++) {
        if (rogue[r] != NULL) {
            enet_peer_disconnect_now(rogue[r], 0);
        }
        enet_host_destroy(rogue_host[r]);
    }
    tig_net_reset_connection();
    return 0;
}

/* Client role (tig_net inactive => not host): exact-size heap copies. */
static void phase_client(long iters)
{
    static uint8_t buf[TIG_NET_MAX_PACKET];
    long it;

    coop_lite_session_reset();
    for (it = 0; it < iters; it++) {
        int size = make_packet(buf, (int)sizeof(buf));
        int type;
        uint8_t* p;

        memcpy(&type, buf, 4);
        if (size < (int)coop_lite_coop_packet_min_size(type)) {
            continue; /* the validator drops these */
        }
        p = (uint8_t*)malloc((size_t)size);
        memcpy(p, buf, (size_t)size);
        dispatch(p);
        free(p);
    }
    printf("client phase: %ld iterations\n", iters);
}

int main(void)
{
    const char* env = getenv("COOP_RX_FUZZ_SECONDS");
    int seconds = env != NULL ? atoi(env) : 5;

    if (enet_initialize() != 0) {
        return 1;
    }
    const char* envc = getenv("COOP_RX_FUZZ_CLIENTS");
    int nrogue = envc != NULL ? atoi(envc) : 7;

    if (nrogue < 1 || nrogue > 7) {
        nrogue = 7;
    }
    if (phase_host(seconds > 0 ? seconds : 5, nrogue) != 0) {
        return 1;
    }
    phase_client((long)(seconds > 0 ? seconds : 5) * 20000L);
    printf("coop_lite_rx_fuzz_test OK\n");
    return 0;
}
