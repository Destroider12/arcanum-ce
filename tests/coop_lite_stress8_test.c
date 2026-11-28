/*
 * PT55: 8-slot stress / edge cases over real ENet loopback (headless).
 * tig_net host + 7..9 raw-ENet clients in one process; host log captured
 * (stderr redirected) so refusals are checked by their log lines.
 *   S1 7 clients at once + a 9th refused (log, no slot change)
 *   S5 protocol mismatch / old short NAME refused (log, slot freed)
 *   S6 max-size packets both ways, oversize, 31+ char names
 *   S2 mass hard drop while large reliable data is queued, rejoin
 *   S7 stalled client vs host broadcast: queue / RSS bound
 *   S8 soak: many ticks with storms, RSS bound (LSan checks leaks at exit)
 *   S4 pure wrap / race helpers (pose epoch+seq, layout echo, rate, doors)
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

#define PORT_S 17941
#define MAXC 9

typedef struct CtrlName {
    int magic;
    int slot;
    char name[TIG_NET_MAX_NAME];
    int protocol;
} CtrlName;

int64_t player_get_local_pc_obj(void) { return 42; }
bool teleport_do(TeleportData* teleport_data) { (void)teleport_data; return true; }

static int g_rx_app;
static int g_disc;
static char g_logpath[] = "/tmp/coop_stress8_XXXXXX";
static int g_logfd = -1;
static int g_saved_err = -1;

static bool on_validate(void* msg) { (void)msg; return true; }
static void on_message(void* msg) { (void)msg; g_rx_app++; }
static bool on_event(int type, int client_id, void* data, int size)
{
    (void)data; (void)size;
    if (type == TIG_NET_EVENT_DISCONNECT) {
        g_disc++;
        coop_lite_host_on_client_disconnect(client_id);
    }
    return true;
}

static ENetHost* g_raw[MAXC];
static ENetPeer* g_peer[MAXC];
static int g_proto[MAXC];
static int g_hello_size[MAXC];
static const char* g_name[MAXC];
static int g_rx[MAXC];
static bool g_stalled[MAXC];

static void pump(int ms)
{
    ENetEvent ev;
    int i, c;

    for (i = 0; i < ms; i++) {
        tig_net_pump();
        for (c = 0; c < MAXC; c++) {
            if (g_raw[c] == NULL || g_stalled[c]) {
                continue;
            }
            while (enet_host_service(g_raw[c], &ev, 0) > 0) {
                if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                    g_rx[c]++;
                    enet_packet_destroy(ev.packet);
                } else if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                    CtrlName hello;
                    ENetPacket* p;

                    memset(&hello, 0, sizeof(hello));
                    hello.magic = 0x454D414E;
                    if (g_name[c] != NULL) {
                        memcpy(hello.name, g_name[c], strlen(g_name[c]) < sizeof(hello.name) ? strlen(g_name[c]) : sizeof(hello.name));
                    } else {
                        snprintf(hello.name, sizeof(hello.name), "P%d", c + 1);
                    }
                    hello.protocol = g_proto[c] != 0 ? g_proto[c] : TIG_NET_PROTOCOL;
                    p = enet_packet_create(&hello, g_hello_size[c] != 0 ? (size_t)g_hello_size[c] : sizeof(hello), ENET_PACKET_FLAG_RELIABLE);
                    if (enet_peer_send(ev.peer, 0, p) != 0) {
                        enet_packet_destroy(p);
                    }
                } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                    g_peer[c] = NULL;
                }
            }
        }
        usleep(500);
    }
}

static int active_count(void)
{
    int s, n = 0;
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
    addr.port = PORT_S;
    g_peer[c] = enet_host_connect(g_raw[c], &addr, 3, 0);
}

static void drop_client(int c, bool hard)
{
    if (g_peer[c] == NULL) return;
    if (hard) enet_peer_reset(g_peer[c]); else enet_peer_disconnect(g_peer[c], 0);
    g_peer[c] = NULL;
}

static long rss_kb(void)
{
    long pages = 0, res = 0;
    FILE* f = fopen("/proc/self/statm", "r");
    if (f != NULL) {
        if (fscanf(f, "%ld %ld", &pages, &res) != 2) res = 0;
        fclose(f);
    }
    return res * (sysconf(_SC_PAGESIZE) / 1024);
}

static void log_begin(void)
{
    fflush(stderr);
    g_logfd = mkstemp(g_logpath);
    g_saved_err = dup(2);
    dup2(g_logfd, 2);
}

static bool log_has(const char* needle)
{
    char buf[1 << 16];
    ssize_t n;
    fflush(stderr);
    n = pread(g_logfd, buf, sizeof(buf) - 1, 0);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

static void log_reset(void)
{
    fflush(stderr);
    if (ftruncate(g_logfd, 0) != 0) { /* ignore */ }
    lseek(g_logfd, 0, SEEK_SET);
}

static void log_end(void)
{
    fflush(stderr);
    dup2(g_saved_err, 2);
    close(g_saved_err);
    close(g_logfd);
    unlink(g_logpath);
}

#define CHECK(cond, msg) do { if (!(cond)) { log_end(); fprintf(stderr, "FAIL: %s\n", msg); return 1; } } while (0)

static int test_pure(void)
{
    CoopPoseOrder st;
    CoopRateBucket b;
    uint8_t set[4][24];
    uint8_t oid[24];
    int cnt = 0;
    unsigned int t;
    int i;

    /* pose seq wrap inside one epoch, duplicates / reordering dropped */
    memset(&st, 0, sizeof(st));
    CHECK(coop_lite_pose_order_accept(&st, 0, 65534, false, false), "pose 65534");
    CHECK(coop_lite_pose_order_accept(&st, 0, 65535, false, false), "pose 65535");
    CHECK(coop_lite_pose_order_accept(&st, 0, 0, false, false), "pose seq wraps to 0");
    CHECK(coop_lite_pose_order_accept(&st, 0, 1, false, false), "pose 1 after wrap");
    CHECK(!coop_lite_pose_order_accept(&st, 0, 1, false, false), "duplicate pose dropped");
    CHECK(!coop_lite_pose_order_accept(&st, 0, 65535, false, false), "pre-wrap pose dropped");
    /* epoch wrap: bump many times, the newest epoch keeps winning */
    for (i = 0; i < 70000; i++) {
        uint16_t e = coop_lite_pose_order_bump(&st);
        CHECK(coop_lite_pose_order_accept(&st, e, 0, true, false), "forced pose in new epoch");
    }
    /* layout echo seq wrap: 65535 -> 1 (0 skipped) */
    CHECK(coop_lite_inven_echo_reached(65535, 1), "echo 1 reaches 65535 across wrap");
    CHECK(!coop_lite_inven_echo_reached(1, 65535), "old echo 65535 does not reach 1");
    CHECK(coop_lite_inven_echo_seq((int)coop_lite_inven_echo_flags(70000u)) == (70000u & 0xFFFFu), "echo flags round trip");
    CHECK(coop_lite_inven_echo_reached(0, 12345), "nothing in flight");
    /* rate buckets across the 32-bit ms wrap */
    memset(&b, 0, sizeof(b));
    t = 0xFFFFFF00u;
    for (i = 0; i < 20; i++) {
        (void)coop_lite_rate_take(&b, t, 1, 20, 10);
    }
    CHECK(!coop_lite_rate_take(&b, t + 10, 1, 20, 10), "bucket empty before wrap");
    CHECK(coop_lite_rate_take(&b, t + 600, 1, 20, 10), "bucket refills across the wrap");
    CHECK(coop_lite_interval_elapsed(0xFFFFFFF0u, 0x10u, 0x20u), "interval across wrap");
    CHECK(!coop_lite_interval_elapsed(0xFFFFFFF0u, 0x0Fu, 0x20u), "interval not yet across wrap");
    /* two clients use the same door in one tick: one pending entry */
    memset(oid, 7, sizeof(oid));
    CHECK(coop_lite_door_pending_add(set, &cnt, 4, oid) && coop_lite_door_pending_add(set, &cnt, 4, oid) && cnt == 1, "same door twice = one entry");
    for (i = 0; i < 3; i++) { oid[0] = (uint8_t)(10 + i); (void)coop_lite_door_pending_add(set, &cnt, 4, oid); }
    oid[0] = 99;
    CHECK(!coop_lite_door_pending_add(set, &cnt, 4, oid) && cnt == 4, "door set full refuses cleanly");
    {
        /* PT63: gone-message resolution by tile when the id does not resolve */
        int types[4] = { 3, 1, 1, 1 };
        bool dead[4] = { false, true, false, false };
        unsigned int aids[4] = { 7, 9, 5, 9 };
        CHECK(coop_lite_world_pick_candidate(types, dead, aids, 4, 1, 9) == 3, "same art, live, right type wins");
        CHECK(coop_lite_world_pick_candidate(types, dead, aids, 4, 1, 42) == 2, "no art match: first live of the type");
        CHECK(coop_lite_world_pick_candidate(types, dead, aids, 4, 2, 9) == -1, "no object of the type: not found");
        CHECK(coop_lite_world_pick_candidate(types, dead, aids, 0, 1, 9) == -1, "empty tile");
    }
    {
        /* PT63: hotbar kept per player token, packet check, save round trip */
        PacketCoopHotbar hb;
        CoopHotbarEntry e[COOP_HOTBAR_SLOTS];
        CoopHotbarEntry got[COOP_HOTBAR_SLOTS];
        static uint8_t buf[8 + COOP_HOTBAR_KEPT_MAX * COOP_HOTBAR_SAVE_ENTRY];
        int len;

        memset(&hb, 0, sizeof(hb));
        hb.type = PACKET_COOP_HOTBAR;
        hb.e[0].type = 3; hb.e[0].data = 42; /* spell */
        hb.e[1].type = 1; hb.e[1].data = -1; hb.e[1].oid[0] = 7; /* item */
        CHECK(coop_lite_hotbar_valid(&hb, (int)sizeof(hb)), "hotbar packet ok");
        CHECK(!coop_lite_hotbar_valid(&hb, (int)sizeof(hb) - 1), "short hotbar refused");
        hb.e[2].type = 9;
        CHECK(!coop_lite_hotbar_valid(&hb, (int)sizeof(hb)), "bad slot type refused");
        hb.e[2].type = 0;
        memcpy(e, hb.e, sizeof(e));
        coop_lite_hotbar_kept_reset();
        CHECK(!coop_lite_hotbar_kept_put(0, e), "no token: not kept");
        CHECK(coop_lite_hotbar_kept_put(0x1234, e) && coop_lite_hotbar_kept_count() == 1, "kept");
        e[0].data = 43;
        CHECK(coop_lite_hotbar_kept_put(0x1234, e) && coop_lite_hotbar_kept_count() == 1, "same player replaces");
        len = coop_lite_hotbar_serialize(buf, (int)sizeof(buf));
        coop_lite_hotbar_kept_reset();
        CHECK(len == 8 + COOP_HOTBAR_SAVE_ENTRY && coop_lite_hotbar_deserialize(buf, len), "save round trip");
        CHECK(coop_lite_hotbar_kept_get(0x1234, got) && got[0].data == 43 && got[1].type == 1 && got[1].oid[0] == 7,
            "item oid and spell back after load");
        CHECK(!coop_lite_hotbar_kept_get(0x9999, got), "other player: nothing");
        CHECK(!coop_lite_hotbar_deserialize(buf, len - 1), "truncated save refused");
        coop_lite_hotbar_kept_reset();
    }
    /* S3: ready check, 7 clients: busy, idle near, idle far, dead, READY far,
       no reply, one disconnecting mid-check */
    {
        CoopTravelSlot ts[8];
        int state[8];
        int ready = 0, far = 0, living;

        memset(ts, 0, sizeof(ts));
        for (i = 1; i < 8; i++) { ts[i].present = true; ts[i].dist = 3; state[i] = COOP_TRAVEL_IDLE; }
        state[0] = -1;
        state[1] = COOP_TRAVEL_BUSY;
        ts[2].dist = 40; /* idle but far */
        ts[3].dead = true; state[3] = COOP_TRAVEL_READY;
        ts[4].dist = 99; state[4] = COOP_TRAVEL_READY;
        state[5] = -1;
        living = coop_lite_travel_tally(ts, state, &ready, &far);
        CHECK(living == 6 && ready == 3 && far == ((1 << 2) | (1 << 4)), "S3: tally mix");
        CHECK(state[3] == -1, "S3: dead slot's READY dropped");
        ts[6].present = false; /* disconnects mid-check */
        living = coop_lite_travel_tally(ts, state, &ready, &far);
        CHECK(living == 5 && ready == 2, "S3: disconnect leaves the check");
        CHECK(coop_lite_travel_untold(0x02, 0xFE) == 0xFC, "S3: untold mask ignores host slot");
        ts[3].dead = false;
        living = coop_lite_travel_tally(ts, state, &ready, &far);
        CHECK(living == 6 && ready == 2, "S3: revived slot has no stale READY");
    }
    return 0;
}

int main(void)
{
    int c, i, round;
    long rss0, rss1, rss2;
    static uint8_t big[TIG_NET_MAX_PACKET + 64];

    if (test_pure() != 0) return 1;
    if (enet_initialize() != 0) return 1;
    log_begin();
    coop_lite_session_reset();
    tig_net_set_coop_enabled(true);
    tig_net_configure_host(PORT_S);
    tig_net_local_server_set_max_players(8);
    tig_net_local_client_set_name("Host");
    tig_net_on_message(on_message);
    tig_net_on_message_validation(on_validate);
    tig_net_on_network_event(on_event);
    CHECK(tig_net_start_client() == TIG_OK && tig_net_is_host(), "host start");

    /* PT55 polish: the main menu status line is English by default */
    {
        char line[512];
        const char* q;
        CHECK(tig_net_describe_coop_mode(line, sizeof(line)) && strncmp(line, "Co-op: host, port", 17) == 0, "menu line English");
        for (q = line; *q != '\0'; q++) CHECK((unsigned char)*q < 0x80, "menu line has no Cyrillic by default");
        CHECK(strlen(line) < 64, "menu line short");
    }

    /* S1 */
    g_name[6] = "ThisNameIsWayTooLongForTheThirtyTwoByteField!!";
    for (c = 0; c < 7; c++) connect_client(c);
    for (i = 0; i < 6000 && active_count() < 7; i++) pump(1);
    pump(200);
    CHECK(active_count() == 7, "S1: 7 clients connected");
    {
        int s, named = 0;
        for (s = 1; s < 8; s++) {
            const char* n = tig_net_client_info_get_name(s);
            if (n != NULL && n[0] == 'P') named++;
            if (n != NULL && strncmp(n, "ThisName", 8) == 0) {
                CHECK(strlen(n) == TIG_NET_MAX_NAME - 1, "S6: long name truncated to 31");
                named++;
            }
        }
        CHECK(named == 7, "S1: all 7 handshakes named");
    }
    log_reset();
    connect_client(7);
    pump(400);
    CHECK(active_count() == 7 && sub_529520() == 0, "S1: 9th refused, slots intact");
    CHECK(log_has("no free slots"), "S1: refusal logged");

    /* S5: wrong protocol, then an old short NAME */
    drop_client(0, false);
    drop_client(7, true);
    for (i = 0; i < 6000 && active_count() > 6; i++) pump(1);
    CHECK(active_count() == 6, "S5: slot freed");
    log_reset();
    g_proto[0] = TIG_NET_PROTOCOL - 1;
    connect_client(0);
    for (i = 0; i < 3000 && !log_has("!= ours"); i++) pump(1);
    {
        char want[64];

        snprintf(want, sizeof(want), "protocol %d != ours %d", TIG_NET_PROTOCOL - 1, TIG_NET_PROTOCOL);
        CHECK(log_has(want), "S5: protocol mismatch logged");
    }
    for (i = 0; i < 6000 && active_count() > 6; i++) pump(1);
    CHECK(active_count() == 6, "S5: mismatched client dropped");
    g_proto[0] = 0;
    g_hello_size[0] = (int)offsetof(CtrlName, protocol);
    log_reset();
    drop_client(0, true);
    connect_client(0);
    for (i = 0; i < 3000 && !log_has("refusing old client"); i++) pump(1);
    CHECK(log_has("refusing old client"), "S5: old build refused with a message");
    for (i = 0; i < 6000 && active_count() > 6; i++) pump(1);
    CHECK(active_count() == 6, "S5: old client dropped");
    g_hello_size[0] = 0;
    drop_client(0, true);
    connect_client(0);
    for (i = 0; i < 6000 && active_count() < 7; i++) pump(1);
    CHECK(active_count() == 7, "S5: good client takes the slot after refusals");

    /* S6: max-size packets */
    memset(big, 0, sizeof(big));
    *(int*)big = PACKET_COOP_WORLD_STATE;
    for (c = 0; c < 7; c++) g_rx[c] = 0;
    tig_net_send_app_all(big, TIG_NET_MAX_PACKET);
    tig_net_send_app_all(big, TIG_NET_MAX_PACKET + 1); /* oversize: dropped, no crash */
    pump(300);
    {
        int got = 0;
        for (c = 0; c < 7; c++) got += g_rx[c] > 0;
        CHECK(got == 7, "S6: max-size packet reached all 7");
    }
    {
        int before = g_rx_app;
        ENetPacket* p = enet_packet_create(big, TIG_NET_MAX_PACKET, ENET_PACKET_FLAG_RELIABLE);
        ENetPacket* q = enet_packet_create(big, TIG_NET_MAX_PACKET + 32, ENET_PACKET_FLAG_RELIABLE);
        if (enet_peer_send(g_peer[1], 0, p) != 0) enet_packet_destroy(p);
        if (enet_peer_send(g_peer[1], 0, q) != 0) enet_packet_destroy(q);
        pump(300);
        CHECK(g_rx_app - before >= 1 && g_rx_app - before <= 1, "S6: client max-size accepted, oversize dropped");
    }

    /* S2: queue a lot of reliable data, hard-drop 3 clients mid-stream */
    for (i = 0; i < 64; i++) tig_net_send_app_all(big, TIG_NET_MAX_PACKET);
    pump(2);
    drop_client(1, true);
    drop_client(3, true);
    drop_client(5, true);
    for (i = 0; i < 40000 && active_count() > 4; i++) pump(1);
    CHECK(active_count() == 4, "S2: 3 hard drops detected");
    connect_client(1); connect_client(3); connect_client(5);
    for (i = 0; i < 6000 && active_count() < 7; i++) pump(1);
    CHECK(active_count() == 7 && sub_529520() == 0, "S2: rejoin into freed slots");

    /* S7: one client stops servicing; host keeps broadcasting */
    rss0 = rss_kb();
    g_stalled[2] = true;
    for (round = 0; round < 3000; round++) {
        *(int*)big = PACKET_COOP_PC_POS;
        tig_net_send_pose_all(big, 96);
        if (round % 10 == 0) tig_net_send_app_all(big, 1024);
        pump(1);
    }
    rss1 = rss_kb();
    fprintf(g_saved_err >= 0 ? stdout : stderr, "S7: rss %ld -> %ld kB, active %d\n", rss0, rss1, active_count());
    CHECK(rss1 - rss0 < 64 * 1024, "S7: stalled client does not grow memory unbounded");
    /* ENet's peer timeout (<= 30 s) must reclaim the stalled peer */
    for (round = 0; round < 80000 && active_count() == 7; round++) {
        if (round % 10 == 0) tig_net_send_pose_all(big, 96);
        pump(1);
    }
    CHECK(active_count() == 6, "S7: stalled client timed out and freed its slot");
    fprintf(stdout, "S7: stalled peer dropped after ~%d ticks, rss %ld kB\n", round, rss_kb());
    g_stalled[2] = false;
    if (g_peer[2] != NULL) { enet_peer_reset(g_peer[2]); g_peer[2] = NULL; }
    connect_client(2);
    for (i = 0; i < 6000 && active_count() < 7; i++) pump(1);
    CHECK(active_count() == 7, "S7: stalled client rejoins");

    /* S8: soak, storms + churn */
    for (round = 0; round < 20000; round++) {
        c = round % 7;
        if (g_peer[c] != NULL && g_peer[c]->state == ENET_PEER_STATE_CONNECTED) {
            ENetPacket* p;
            *(int*)big = PACKET_COOP_PC_POS;
            p = enet_packet_create(big, 64, 0);
            if (enet_peer_send(g_peer[c], 2, p) != 0) enet_packet_destroy(p);
        }
        if (round % 97 == 0) tig_net_send_app_all(big, 2048);
        if (round % 2500 == 1) drop_client((round / 2500) % 7, (round & 2) != 0);
        if (round % 2500 == 600) { for (c = 0; c < 7; c++) if (g_peer[c] == NULL) connect_client(c); }
        pump(0 == round % 4 ? 1 : 0);
        tig_net_pump();
    }
    pump(500);
    rss2 = rss_kb();
    fprintf(stdout, "S8: rss %ld -> %ld kB after soak, active %d, disc %d\n", rss1, rss2, active_count(), g_disc);
    CHECK(rss2 - rss1 < 64 * 1024, "S8: soak memory bound");
    CHECK(tig_net_is_host() && sub_529520() == 0 && active_count() <= 7, "S8: host slot intact");

    for (c = 0; c < MAXC; c++) drop_client(c, false);
    pump(200);
    tig_net_reset_connection();
    for (c = 0; c < MAXC; c++) if (g_raw[c] != NULL) enet_host_destroy(g_raw[c]);
    enet_deinitialize();
    log_end();
    printf("coop_lite_stress8: ok (%d disconnects)\n", g_disc);
    return 0;
}
