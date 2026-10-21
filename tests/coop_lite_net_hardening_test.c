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

#define PORT_A 17911
#define PORT_B 17912

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
static int g_teleports;
static int g_teleport_map;
static int g_disconnect_slot0;
static int g_disconnect_events;

int64_t player_get_local_pc_obj(void)
{
    return 42;
}

bool teleport_do(TeleportData* teleport_data)
{
    g_teleports++;
    g_teleport_map = teleport_data->map;
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

static void on_message(void* msg)
{
    int type = *(int*)msg;

    g_delivered[type & 0xFF]++;
    if (type == PACKET_COOP_CHAR_OFFER) {
        coop_lite_handle_char_offer((PacketCoopCharOffer*)msg);
    } else if (type == PACKET_COOP_SESSION_SYNC) {
        coop_lite_handle_session_sync((PacketCoopSessionSync*)msg);
    }
}

static bool on_event(int type, int client_id, void* data, int size)
{
    (void)data;
    (void)size;
    if (type == TIG_NET_EVENT_DISCONNECT) {
        g_disconnect_events++;
        if (client_id == 0) {
            g_disconnect_slot0++;
        }
        coop_lite_host_on_client_disconnect(client_id);
    }
    return true;
}

static void raw_send(ENetPeer* peer, int channel, const void* data, size_t size)
{
    ENetPacket* packet = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, (enet_uint8)channel, packet);
}

/* Service both sides for ms milliseconds. */
static void pump(ENetHost* raw, int ms)
{
    ENetEvent ev;
    int i;

    for (i = 0; i < ms; i++) {
        tig_net_pump();
        while (raw != NULL && enet_host_service(raw, &ev, 0) > 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(ev.packet);
            }
        }
        usleep(1000);
    }
}

#define CHECK(cond, msg)                         \
    do {                                         \
        if (!(cond)) {                           \
            fprintf(stderr, "FAIL: %s\n", msg);  \
            return 1;                            \
        }                                        \
    } while (0)

static int phase_host(void)
{
    ENetHost* rogue_host;
    ENetHost* rogue2_host;
    ENetPeer* rogue;
    ENetPeer* rogue2;
    ENetAddress addr;
    CtrlWelcome welcome;
    CtrlName name;
    PacketCoopSessionSync sync;
    PacketCoopCharOffer offer;
    char big[TIG_NET_MAX_PACKET + 16];
    int i;

    memset(g_delivered, 0, sizeof(g_delivered));
    coop_lite_session_reset();
    tig_net_set_coop_enabled(true);
    tig_net_configure_host(PORT_A);
    tig_net_local_server_set_max_players(2); /* host + one client */
    tig_net_local_client_set_name("Host");
    tig_net_on_message(on_message);
    tig_net_on_message_validation(on_validate);
    tig_net_on_network_event(on_event);
    CHECK(tig_net_start_client() == TIG_OK && tig_net_is_host(), "host start");

    rogue_host = enet_host_create(NULL, 1, 2, 0, 0);
    enet_address_set_host(&addr, "127.0.0.1");
    addr.port = PORT_A;
    rogue = enet_host_connect(rogue_host, &addr, 2, 0);
    for (i = 0; i < 3000 && !tig_net_client_is_active(1); i++) {
        pump(rogue_host, 1);
    }
    CHECK(tig_net_client_is_active(1), "rogue connected as slot 1");

    /* (3) WELCOME sent *to the host* must be ignored (slot 0 and bad slot). */
    welcome.magic = 1;
    welcome.slot = 0;
    welcome.max_players = 8;
    raw_send(rogue, 1, &welcome, sizeof(welcome));
    welcome.slot = 99;
    raw_send(rogue, 1, &welcome, sizeof(welcome));
    pump(rogue_host, 100);
    CHECK(tig_net_is_host() && sub_529520() == 0, "host ignored WELCOME");

    /* (11) NAME claims slot 0; must land on the sender's slot. */
    memset(&name, 0, sizeof(name));
    name.magic = 2;
    name.slot = 0;
    memset(name.name, 'E', sizeof(name.name)); /* unterminated */
    name.protocol = TIG_NET_PROTOCOL;
    raw_send(rogue, 1, &name, sizeof(name));
    pump(rogue_host, 100);
    CHECK(strcmp(tig_net_client_info_get_name(0), "Host") == 0, "host name not spoofed");
    CHECK(strlen(tig_net_client_info_get_name(1)) == TIG_NET_MAX_NAME - 1, "sender name terminated");

    /* (7) direction: client may not send host→client SESSION_SYNC. */
    memset(&sync, 0, sizeof(sync));
    sync.type = PACKET_COOP_SESSION_SYNC;
    sync.map = 3;
    sync.client_slot = 1;
    raw_send(rogue, 0, &sync, sizeof(sync));
    /* (7) undersized CHAR_OFFER. */
    offer.type = PACKET_COOP_CHAR_OFFER;
    raw_send(rogue, 0, &offer, 8);
    /* oversized app packet. */
    memset(big, 0, sizeof(big));
    *(int*)big = PACKET_COOP_CHAR_OFFER;
    raw_send(rogue, 0, big, sizeof(big));
    /* (1) bad proto. */
    memset(&offer, 0, sizeof(offer));
    offer.type = PACKET_COOP_CHAR_OFFER;
    offer.slot = 1;
    offer.basic_prototype = -5;
    raw_send(rogue, 0, &offer, sizeof(offer));
    pump(rogue_host, 200);
    CHECK(g_delivered[PACKET_COOP_SESSION_SYNC] == 0, "SESSION_SYNC from client dropped");
    CHECK(g_delivered[PACKET_COOP_CHAR_OFFER] == 1, "only the full-size bad-proto offer delivered");
    CHECK(!coop_lite_test_pending_offer(1), "bad proto offer rejected");

    /* (5) spoofed payload slot: stored under the authenticated sender. */
    offer.slot = 5;
    offer.basic_prototype = 16067;
    memset(offer.name, 'N', sizeof(offer.name)); /* (9) unterminated name */
    raw_send(rogue, 0, &offer, sizeof(offer));
    pump(rogue_host, 200);
    CHECK(coop_lite_test_pending_offer(1), "offer stored for sender slot 1");
    CHECK(!coop_lite_test_pending_offer(5), "payload slot ignored");

    /* (6) a rejected peer (no free slot) must not clear slot 0 / reset. */
    rogue2_host = enet_host_create(NULL, 1, 2, 0, 0);
    rogue2 = enet_host_connect(rogue2_host, &addr, 2, 0);
    (void)rogue2;
    for (i = 0; i < 600; i++) {
        pump(rogue_host, 0);
        tig_net_pump();
        {
            ENetEvent ev;
            while (enet_host_service(rogue2_host, &ev, 0) > 0) {
                if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(ev.packet);
                }
            }
        }
        usleep(1000);
    }
    CHECK(g_disconnect_slot0 == 0, "rejected peer did not disconnect slot 0");
    CHECK(tig_net_client_is_active(0) && tig_net_client_is_active(1), "slots intact after reject");

    /* (12) PT22: an old client (NAME without the protocol field), then
       a CHAR_OFFER right behind it: the host refuses the peer and drops
       the offer instead of spawning it. */
    {
        int before = g_delivered[PACKET_COOP_CHAR_OFFER];

        memset(&name, 0, sizeof(name));
        name.magic = 2;
        snprintf(name.name, sizeof(name.name), "Old");
        raw_send(rogue, 1, &name, sizeof(name) - sizeof(int));
        offer.slot = 1;
        offer.basic_prototype = 16067;
        raw_send(rogue, 0, &offer, sizeof(offer));
        for (i = 0; i < 2000 && tig_net_client_is_active(1); i++) {
            pump(rogue_host, 1);
        }
        CHECK(g_delivered[PACKET_COOP_CHAR_OFFER] == before, "offer after an old NAME dropped");
        CHECK(!tig_net_client_is_active(1), "old-protocol client disconnected");
    }

    /* (4) disconnect clears the pending offer. */
    enet_peer_disconnect_now(rogue, 0);
    enet_host_flush(rogue_host);
    for (i = 0; i < 2000 && tig_net_client_is_active(1); i++) {
        pump(rogue_host, 1);
    }
    CHECK(!tig_net_client_is_active(1), "slot 1 disconnected");
    CHECK(!coop_lite_test_pending_offer(1), "offer cleared on disconnect");

    enet_host_destroy(rogue2_host);
    enet_host_destroy(rogue_host);
    tig_net_reset_connection();
    printf("phase A (host vs rogue client) OK\n");
    return 0;
}

static int phase_client(void)
{
    ENetHost* rogue_host;
    ENetAddress addr;
    ENetEvent ev;
    ENetPeer* peer = NULL;
    CtrlWelcome welcome;
    PacketCoopSessionSync sync;
    PacketCoopCharOffer offer;
    int i;

    memset(g_delivered, 0, sizeof(g_delivered));
    coop_lite_session_reset();
    g_teleports = 0;

    addr.host = ENET_HOST_ANY;
    addr.port = PORT_B;
    rogue_host = enet_host_create(&addr, 2, 2, 0, 0);
    CHECK(rogue_host != NULL, "rogue host listen");

    tig_net_set_coop_enabled(true);
    tig_net_configure_join("127.0.0.1", PORT_B);
    tig_net_on_message(on_message);
    tig_net_on_message_validation(on_validate);
    tig_net_on_network_event(on_event);

    /* tig_net_start_client blocks in its connect loop; the rogue host must
       accept concurrently, so service it from a child-free loop: ENet
       completes the handshake on the client side as long as the server
       answers within the timeout — run the server in a forked helper. */
    {
        pid_t pid = fork();
        if (pid == 0) {
            /* Helper: accept, then idle; real assertions stay in parent. */
            int got = 0;
            for (i = 0; i < 4000; i++) {
                while (enet_host_service(rogue_host, &ev, 1) > 0) {
                    if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                        peer = ev.peer;
                        got = 1;
                    } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                        enet_packet_destroy(ev.packet);
                    }
                }
                if (got && i > 50) {
                    /* (3) WELCOME slot 0 → ignored; then valid slot 1. */
                    welcome.magic = 1;
                    welcome.slot = 0;
                    welcome.max_players = 8;
                    welcome.protocol = TIG_NET_PROTOCOL;
                    raw_send(peer, 1, &welcome, sizeof(welcome));
                    welcome.slot = 1;
                    raw_send(peer, 1, &welcome, sizeof(welcome));
                    welcome.slot = 3; /* duplicate → ignored */
                    raw_send(peer, 1, &welcome, sizeof(welcome));
                    /* host→client CHAR_OFFER is a client→host type: drop. */
                    memset(&offer, 0, sizeof(offer));
                    offer.type = PACKET_COOP_CHAR_OFFER;
                    offer.basic_prototype = 16067;
                    raw_send(peer, 0, &offer, sizeof(offer));
                    /* (2) SESSION_SYNC for another slot: drop. */
                    memset(&sync, 0, sizeof(sync));
                    sync.type = PACKET_COOP_SESSION_SYNC;
                    sync.map = 9;
                    sync.client_slot = 2;
                    raw_send(peer, 0, &sync, sizeof(sync));
                    /* Ours, with unterminated names (9). */
                    sync.map = 7;
                    sync.client_slot = 1;
                    memset(sync.client_name, 'C', sizeof(sync.client_name));
                    memset(sync.host_name, 'H', sizeof(sync.host_name));
                    raw_send(peer, 0, &sync, sizeof(sync));
                    enet_host_flush(rogue_host);
                    got = 2;
                }
                if (got == 2) {
                    for (i = 0; i < 1500; i++) {
                        enet_host_service(rogue_host, &ev, 1);
                        if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                            enet_packet_destroy(ev.packet);
                        }
                    }
                    _exit(0);
                }
            }
            _exit(1);
        }
        enet_host_destroy(rogue_host); /* parent does not serve */
        CHECK(tig_net_start_client() == TIG_OK && tig_net_is_active() && !tig_net_is_host(),
            "client connected");
        for (i = 0; i < 1500; i++) {
            tig_net_pump();
            usleep(1000);
        }
    }

    CHECK(sub_529520() == 1, "client took slot 1 (slot 0 / duplicate WELCOME ignored)");
    CHECK(g_delivered[PACKET_COOP_CHAR_OFFER] == 0, "client dropped client->host type");
    CHECK(g_teleports == 1 && g_teleport_map == 7, "only our SESSION_SYNC applied");

    tig_net_reset_connection();
    printf("phase B (client vs rogue host) OK\n");
    return 0;
}

int main(void)
{
    if (enet_initialize() != 0) {
        return 1;
    }
    if (phase_host() != 0) {
        return 1;
    }
    if (phase_client() != 0) {
        return 1;
    }
    printf("coop_lite_net_hardening_test OK\n");
    return 0;
}
