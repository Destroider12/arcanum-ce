/*
 * Policy + follow-packet + dialog/save gates for Coop Lite.
 * Host/client via fork; verifies worldmap/time/save gates, talk classify,
 * per-slot follow offset, and PACKET_COOP_FOLLOW_HOST.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

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

#define TEST_PORT 17888
#define LOCATION_MAKE(x, y) ((int64_t)((x) & 0xFFFFFFFF) | (((int64_t)(y) & 0xFFFFFFFF) << 32))
#define LOCATION_GET_X(l) ((int64_t)((l) & 0xFFFFFFFF))
#define LOCATION_GET_Y(l) ((int64_t)(((l) >> 32) & 0xFFFFFFFF))

static int g_applied_map = -1;
static int64_t g_applied_loc = 0;
static int g_teleport_calls = 0;

extern int g_coop_lite_test_merchant;
extern int g_coop_lite_test_story;

int64_t player_get_local_pc_obj(void)
{
    return 1;
}

bool teleport_do(TeleportData* teleport_data)
{
    g_teleport_calls++;
    g_applied_map = teleport_data->map;
    g_applied_loc = teleport_data->loc;
    return true;
}

static void pump_ms(int ms)
{
    int i;
    for (i = 0; i < ms; i++) {
        tig_net_pump();
        usleep(1000);
    }
}

static void on_message(void* msg)
{
    int type = *(int*)msg;
    if (type == PACKET_COOP_FOLLOW_HOST) {
        coop_lite_handle_follow_packet((PacketCoopFollowHost*)msg);
    }
}

static bool on_validate(void* msg)
{
    int type = *(int*)msg;
    return type > 0 && type < PACKET_COOP_TYPE_LIMIT;
}


static int test_mp_packet_roles(void)
{
    if (coop_lite_mp_packet_role(4) != COOP_MP_PKT_HOST_RESOLVE) {
        fprintf(stderr, "pkt4 should be host-resolve\n");
        return 1;
    }
    if (coop_lite_mp_packet_role(8) != COOP_MP_PKT_BOTH) {
        fprintf(stderr, "pkt8 (anim modify) is sent by host and clients\n");
        return 1;
    }
    if (coop_lite_mp_packet_role(10) != COOP_MP_PKT_CLIENT_APPLY) {
        fprintf(stderr, "pkt10 should be client-apply\n");
        return 1;
    }
    if (coop_lite_mp_packet_role(11) != COOP_MP_PKT_CLIENT_APPLY) {
        fprintf(stderr, "pkt11 combat mode should be client-apply\n");
        return 1;
    }
    if (coop_lite_mp_packet_role(PACKET_COOP_ATTACK_INTENT) != COOP_MP_PKT_HOST_RESOLVE) {
        fprintf(stderr, "attack intent should be HOST_RESOLVE\n");
        return 1;
    }
    if (coop_lite_mp_packet_role(57) != COOP_MP_PKT_HOST_RESOLVE) {
        fprintf(stderr, "pkt57 should be host-resolve\n");
        return 1;
    }
    if (coop_lite_mp_packet_role(58) != COOP_MP_PKT_CLIENT_APPLY) {
        fprintf(stderr, "pkt58 should be client-apply\n");
        return 1;
    }
    if (coop_lite_mp_packet_role(99) != COOP_MP_PKT_CLIENT_APPLY) {
        fprintf(stderr, "pkt99 should be client-apply\n");
        return 1;
    }
    if (coop_lite_mp_packet_role(100) != COOP_MP_PKT_BOTH) {
        fprintf(stderr, "pkt100 should be both\n");
        return 1;
    }
    printf("mp packet roles: OK\n");
    return 0;
}

static int test_classify_and_offsets(void)
{
    int dx, dy;
    int64_t host = LOCATION_MAKE(100, 200);
    int64_t loc1;
    int64_t loc2;

    if (coop_lite_classify_talk(false, false, false, true) != COOP_TALK_ALLOW) {
        fprintf(stderr, "SP classify should ALLOW\n");
        return 1;
    }
    if (coop_lite_classify_talk(true, true, false, true) != COOP_TALK_ALLOW) {
        fprintf(stderr, "host classify should ALLOW\n");
        return 1;
    }
    if (coop_lite_classify_talk(true, false, true, true) != COOP_TALK_ALLOW) {
        fprintf(stderr, "merchant should ALLOW even with story\n");
        return 1;
    }
    if (coop_lite_classify_talk(true, false, false, true) != COOP_TALK_ALLOW) {
        fprintf(stderr, "story NPC should ALLOW (per-node filter) for companion\n");
        return 1;
    }
    if (coop_lite_classify_talk(true, false, false, false) != COOP_TALK_ALLOW) {
        fprintf(stderr, "ordinary NPC should ALLOW\n");
        return 1;
    }
    printf("classify: OK\n");

    coop_lite_slot_preferred_offset(0, &dx, &dy);
    if (dx != 0 || dy != 0) {
        fprintf(stderr, "slot0 offset expected 0,0 got %d,%d\n", dx, dy);
        return 1;
    }
    coop_lite_slot_preferred_offset(1, &dx, &dy);
    if (dx != 1 || dy != 0) {
        fprintf(stderr, "slot1 offset expected 1,0 got %d,%d\n", dx, dy);
        return 1;
    }
    loc1 = coop_lite_follow_loc_for_slot(host, 1);
    loc2 = coop_lite_follow_loc_for_slot(host, 2);
    if (loc1 == host || loc2 == host || loc1 == loc2) {
        fprintf(stderr, "slot offsets should differ from host and each other (%lld %lld host=%lld)\n",
            (long long)loc1, (long long)loc2, (long long)host);
        return 1;
    }
    if (LOCATION_GET_X(loc1) != 101 || LOCATION_GET_Y(loc1) != 200) {
        fprintf(stderr, "slot1 loc wrong\n");
        return 1;
    }
    printf("follow offsets: OK\n");
    return 0;
}

static int run_client(void)
{
    int64_t host_loc = 0x1234;
    int64_t expect;
    int slot;

    tig_net_set_coop_enabled(true);
    tig_net_configure_join("127.0.0.1", TEST_PORT);
    tig_net_on_message(on_message);
    tig_net_on_message_validation(on_validate);
    tig_net_local_client_set_name("Client");

    if (tig_net_start_client() != TIG_OK) {
        fprintf(stderr, "client: start failed\n");
        return 1;
    }

    {
        int waits = 0;
        while (sub_529520() < 1 && waits < 3000) {
            tig_net_pump();
            usleep(1000);
            waits++;
        }
    }
    if (!tig_net_is_active() || tig_net_is_host() || sub_529520() < 1) {
        fprintf(stderr, "client: not a connected client (active=%d host=%d slot=%d)\n",
            tig_net_is_active(), tig_net_is_host(), sub_529520());
        return 1;
    }
    if (coop_lite_can_use_worldmap()) {
        fprintf(stderr, "client: worldmap should be blocked\n");
        return 1;
    }
    if (coop_lite_can_advance_game_time()) {
        fprintf(stderr, "client: time advance should be blocked\n");
        return 1;
    }
    if (coop_lite_can_save_world()) {
        fprintf(stderr, "client: world save should be blocked\n");
        return 1;
    }

    g_coop_lite_test_merchant = 0;
    g_coop_lite_test_story = 1;
    if (coop_lite_can_talk(99, 1) != COOP_TALK_ALLOW) {
        fprintf(stderr, "client: story NPC should ALLOW (filtered)\n");
        return 1;
    }
    g_coop_lite_test_merchant = 1;
    if (coop_lite_can_talk(99, 1) != COOP_TALK_ALLOW) {
        fprintf(stderr, "client: merchant should ALLOW\n");
        return 1;
    }
    g_coop_lite_test_merchant = 0;
    g_coop_lite_test_story = 0;
    printf("client: worldmap/time/save/talk gates OK\n");

    slot = sub_529520();
    expect = coop_lite_follow_loc_for_slot(host_loc, slot);

    g_teleport_calls = 0;
    g_applied_map = -1;
    {
        int waits = 0;
        while (g_teleport_calls == 0 && waits < 5000) {
            tig_net_pump();
            usleep(1000);
            waits++;
        }
    }
    if (g_teleport_calls != 1 || g_applied_map != 42 || g_applied_loc != expect) {
        fprintf(stderr, "client: follow not applied (calls=%d map=%d loc=%lld expect=%lld slot=%d)\n",
            g_teleport_calls, g_applied_map, (long long)g_applied_loc,
            (long long)expect, slot);
        return 1;
    }
    printf("client: follow applied map=%d loc=%lld (offset from host)\n",
        g_applied_map, (long long)g_applied_loc);
    tig_net_reset_connection();
    printf("client: PASS\n");
    return 0;
}

static int run_host(void)
{
    tig_net_set_coop_enabled(true);
    tig_net_configure_host(TEST_PORT);
    tig_net_on_message(on_message);
    tig_net_on_message_validation(on_validate);
    tig_net_local_client_set_name("Host");
    tig_net_local_server_set_max_players(8);

    if (tig_net_start_client() != TIG_OK || !tig_net_is_host()) {
        fprintf(stderr, "host: start failed\n");
        return 1;
    }
    if (!coop_lite_can_use_worldmap() || !coop_lite_can_advance_game_time()
        || !coop_lite_can_save_world()) {
        fprintf(stderr, "host: should allow worldmap/time/save\n");
        return 1;
    }
    if (coop_lite_can_talk(1, 1) != COOP_TALK_ALLOW) {
        fprintf(stderr, "host: talk should ALLOW\n");
        return 1;
    }
    printf("host: worldmap/time/save/talk gates OK\n");

    {
        int waits = 0;
        while (!tig_net_client_is_active(1) && waits < 10000) {
            tig_net_pump();
            usleep(1000);
            waits++;
        }
    }
    if (!tig_net_client_is_active(1)) {
        fprintf(stderr, "host: no client\n");
        return 1;
    }

    /* First notify arms follow (skips New-Game spawn); second broadcasts. */
    coop_lite_session_reset();
    coop_lite_notify_host_teleported(42, 0x1234); /* arm only */
    coop_lite_notify_host_teleported(42, 0x1234); /* real travel broadcast */
    pump_ms(800);

    printf("host: PASS\n");
    return 0;
}

static int test_pc_pos_helpers(void)
{
    PacketCoopPcPos a;
    PacketCoopPcPos b;

    memset(&a, 0, sizeof(a));
    a.type = PACKET_COOP_PC_POS;
    a.loc = 0x0000001200000034LL;
    a.offset_x = 3;
    a.art_id = 0x1234u;
    b = a;
    if (coop_lite_pc_pos_changed(&a, &b)) {
        fprintf(stderr, "pc_pos: identical samples flagged changed\n");
        return 1;
    }
    b.offset_y = 1;
    if (!coop_lite_pc_pos_changed(&a, &b)) {
        fprintf(stderr, "pc_pos: offset change missed\n");
        return 1;
    }
    b = a;
    b.art_id = 0x1235u;
    if (!coop_lite_pc_pos_changed(&a, &b)) {
        fprintf(stderr, "pc_pos: art change missed\n");
        return 1;
    }
    b = a;
    b.loc += 1;
    if (!coop_lite_pc_pos_changed(&a, &b) || !coop_lite_pc_pos_changed(NULL, &a)) {
        fprintf(stderr, "pc_pos: loc/NULL change missed\n");
        return 1;
    }

    /* Rate limit: 100 ms, wrap-safe. */
    if (coop_lite_interval_elapsed(1000u, 1000u + COOP_LITE_PC_POS_INTERVAL_MS - 1u, COOP_LITE_PC_POS_INTERVAL_MS)) {
        fprintf(stderr, "rate: interval-1 should not elapse\n");
        return 1;
    }
    if (!coop_lite_interval_elapsed(1000u, 1000u + COOP_LITE_PC_POS_INTERVAL_MS, COOP_LITE_PC_POS_INTERVAL_MS)) {
        fprintf(stderr, "rate: interval should elapse\n");
        return 1;
    }
    if (!coop_lite_interval_elapsed(0xFFFFFFF0u, 0x00000060u, COOP_LITE_PC_POS_INTERVAL_MS)) {
        fprintf(stderr, "rate: wraparound failed\n");
        return 1;
    }
    if (coop_lite_interval_elapsed(0xFFFFFFF0u, 0x00000008u, COOP_LITE_PC_POS_INTERVAL_MS)) {
        fprintf(stderr, "rate: wraparound short interval elapsed\n");
        return 1;
    }

    if (coop_lite_mp_packet_role(PACKET_COOP_PC_POS) != COOP_MP_PKT_BOTH
        || coop_lite_mp_packet_role(PACKET_COOP_CHAR_OFFER) != COOP_MP_PKT_HOST_RESOLVE) {
        fprintf(stderr, "pc_pos/char_offer role wrong\n");
        return 1;
    }
    if (PACKET_COOP_PC_POS >= PACKET_COOP_TYPE_LIMIT) {
        fprintf(stderr, "PC_POS not under validate limit\n");
        return 1;
    }
    if (sizeof(PacketCoopPcPos) != 0x38 || sizeof(PacketCoopCharOffer) != 0x68) {
        fprintf(stderr, "packet layout changed: pcpos=%zu offer=%zu\n",
            sizeof(PacketCoopPcPos), sizeof(PacketCoopCharOffer));
        return 1;
    }
    printf("pc_pos helpers / rate limit OK\n");
    return 0;
}

static int test_packet_hardening(void)
{
    char name[8];

    /* Direction matrix. */
    if (coop_lite_packet_allowed(PACKET_COOP_SESSION_SYNC, true, 1)) {
        fprintf(stderr, "host must drop SESSION_SYNC from a client\n");
        return 1;
    }
    if (coop_lite_packet_allowed(PACKET_COOP_FOLLOW_HOST, true, 2)
        || coop_lite_packet_allowed(2, true, 1) || coop_lite_packet_allowed(70, true, 1)
        || coop_lite_packet_allowed(10, true, 1)) {
        fprintf(stderr, "host must drop host->client state from clients\n");
        return 1;
    }
    if (!coop_lite_packet_allowed(PACKET_COOP_CHAR_OFFER, true, 1)
        || !coop_lite_packet_allowed(4, true, 3)
        || !coop_lite_packet_allowed(PACKET_COOP_PC_POS, true, 1)) {
        fprintf(stderr, "host must accept client intents\n");
        return 1;
    }
    if (coop_lite_packet_allowed(PACKET_COOP_CHAR_OFFER, false, 0)
        || coop_lite_packet_allowed(4, false, 0)
        || coop_lite_packet_allowed(PACKET_COOP_ATTACK_INTENT, false, 0)) {
        fprintf(stderr, "client must drop client->host intents\n");
        return 1;
    }
    if (coop_lite_packet_allowed(PACKET_COOP_SESSION_SYNC, false, 2)) {
        fprintf(stderr, "client must only listen to the host\n");
        return 1;
    }
    if (!coop_lite_packet_allowed(PACKET_COOP_SESSION_SYNC, false, 0)
        || !coop_lite_packet_allowed(PACKET_COOP_PC_POS, false, 0)) {
        fprintf(stderr, "client must accept host state\n");
        return 1;
    }
    if (coop_lite_packet_allowed(0, true, 0) || coop_lite_packet_allowed(-7, false, 0)
        || coop_lite_packet_allowed(PACKET_COOP_TYPE_LIMIT, true, 0)
        || coop_lite_packet_allowed(4, true, -1) || coop_lite_packet_allowed(4, true, 8)) {
        fprintf(stderr, "bad type/slot must be dropped\n");
        return 1;
    }

    /* Size table. */
    if (coop_lite_coop_packet_min_size(PACKET_COOP_CHAR_OFFER) != sizeof(PacketCoopCharOffer)
        || coop_lite_coop_packet_min_size(PACKET_COOP_SESSION_SYNC) != sizeof(PacketCoopSessionSync)
        || coop_lite_coop_packet_min_size(PACKET_COOP_PC_POS) != sizeof(PacketCoopPcPos)
        || coop_lite_coop_packet_min_size(4) != 0) {
        fprintf(stderr, "coop min sizes wrong\n");
        return 1;
    }

    /* Proto sanity (player_obj_create_player exit()s on garbage). */
    if (coop_lite_pc_proto_plausible(0) || coop_lite_pc_proto_plausible(-16066)
        || coop_lite_pc_proto_plausible(COOP_LITE_MAX_PROTO) || !coop_lite_pc_proto_plausible(16066)) {
        fprintf(stderr, "proto plausibility wrong\n");
        return 1;
    }

    memset(name, 'A', sizeof(name));
    coop_lite_terminate_name(name, sizeof(name));
    if (name[sizeof(name) - 1] != '\0' || strlen(name) != sizeof(name) - 1) {
        fprintf(stderr, "terminate_name failed\n");
        return 1;
    }
    /*
     * Audit against real senders (Playtest 6: Packet8 from a client was
     * dropped as "wrong direction"). Every type a client really sends must
     * pass on the host, every type the host sends must pass on a client.
     */
    {
        static const int client_sends[] = {
            4, 8, 9, 29, 57, 98, 100,
            PACKET_COOP_ATTACK_INTENT, PACKET_COOP_CHAR_OFFER, PACKET_COOP_PC_POS,
        };
        static const int host_sends[] = {
            2, 3, 8, 9, 10, 11, 58, 60, 61, 68, 69, 70, 71, 72, 73, 74, 75, 76,
            77, 92, 94, 98, 99, 100, 106, 118, 121,
            PACKET_COOP_FOLLOW_HOST, PACKET_COOP_SESSION_SYNC, PACKET_COOP_PC_POS,
        };
        size_t i;

        for (i = 0; i < sizeof(client_sends) / sizeof(client_sends[0]); i++) {
            if (!coop_lite_packet_allowed(client_sends[i], true, 1)) {
                fprintf(stderr, "host drops client-sent type %d\n", client_sends[i]);
                return 1;
            }
        }
        for (i = 0; i < sizeof(host_sends) / sizeof(host_sends[0]); i++) {
            if (!coop_lite_packet_allowed(host_sends[i], false, 0)) {
                fprintf(stderr, "client drops host-sent type %d\n", host_sends[i]);
                return 1;
            }
        }
    }

    /* Camera dead zone follow. */
    {
        int64_t sx;
        int64_t sy;

        if (coop_lite_follow_delta(100, -50, 200, 120, &sx, &sy) || sx != 0 || sy != 0) {
            fprintf(stderr, "follow: inside dead zone must not scroll\n");
            return 1;
        }
        if (!coop_lite_follow_delta(260, -50, 200, 120, &sx, &sy) || sx != 60 || sy != 0) {
            fprintf(stderr, "follow: right of zone should scroll 60 (got %lld)\n", (long long)sx);
            return 1;
        }
        if (!coop_lite_follow_delta(-230, 150, 200, 120, &sx, &sy) || sx != -30 || sy != 30) {
            fprintf(stderr, "follow: corner case wrong (%lld,%lld)\n", (long long)sx, (long long)sy);
            return 1;
        }
    }

    printf("packet hardening (direction/size/proto/name/sender audit/follow) OK\n");
    return 0;
}

int main(int argc, char** argv)
{
    tig_net_set_coop_enabled(false);
    if (!coop_lite_can_use_worldmap() || !coop_lite_can_advance_game_time()
        || !coop_lite_can_save_world()) {
        fprintf(stderr, "SP: gates should allow\n");
        return 1;
    }
    printf("SP: worldmap/time/save allowed OK\n");

    if (test_classify_and_offsets() != 0) {
        return 1;
    }
    if (test_mp_packet_roles() != 0) {
        return 1;
    }
    if (test_pc_pos_helpers() != 0) {
        return 1;
    }
    if (test_packet_hardening() != 0) {
        return 1;
    }

    if (argc >= 2 && strcmp(argv[1], "--client") == 0) {
        return run_client();
    }
    if (argc >= 2 && strcmp(argv[1], "--host") == 0) {
        return run_host();
    }

    {
        char self[512];
        pid_t child;
        int status;
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n < 0) {
            return 1;
        }
        self[n] = '\0';
        child = fork();
        if (child == 0) {
            usleep(400000);
            execl(self, self, "--client", (char*)NULL);
            _exit(1);
        }
        if (run_host() != 0) {
            return 1;
        }
        waitpid(child, &status, 0);
        tig_net_reset_connection();
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "client failed\n");
            return 1;
        }
        printf("PASS: coop_lite dialog/save/follow-offset policy\n");
        return 0;
    }
}
