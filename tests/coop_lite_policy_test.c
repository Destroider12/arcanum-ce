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

static int coop_lite_party_death_tests(void);

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
    if (coop_lite_mp_packet_role(11) != COOP_MP_PKT_BOTH) { /* PT12: client toggles own PC */
        fprintf(stderr, "pkt11 combat mode should be BOTH (client own-PC toggle)\n");
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
    /* PT8 design: clients only talk to merchants; everyone else barks. */
    if (coop_lite_classify_talk(true, false, false, true) != COOP_TALK_BARK) {
        fprintf(stderr, "story NPC should BARK for a client\n");
        return 1;
    }
    if (coop_lite_classify_talk(true, false, false, false) != COOP_TALK_BARK) {
        fprintf(stderr, "ordinary NPC should BARK for a client\n");
        return 1;
    }
    /* Another player's follower (Virgil) barks even if it could trade. */
    if (coop_lite_classify_client_talk(true, true) != COOP_TALK_BARK_FOLLOWER
        || coop_lite_classify_client_talk(false, true) != COOP_TALK_BARK_FOLLOWER
        || coop_lite_classify_client_talk(true, false) != COOP_TALK_ALLOW
        || coop_lite_classify_client_talk(false, false) != COOP_TALK_BARK) {
        fprintf(stderr, "classify_client_talk wrong\n");
        return 1;
    }
    if (coop_lite_bark_text(COOP_TALK_BARK) == NULL
        || coop_lite_bark_text(COOP_TALK_BARK_FOLLOWER) == NULL
        || coop_lite_bark_text(COOP_TALK_ALLOW) != NULL) {
        fprintf(stderr, "bark text wrong\n");
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
    if (coop_lite_can_talk(99, 1) != COOP_TALK_BARK) {
        fprintf(stderr, "client: story NPC should BARK\n");
        return 1;
    }
    if (coop_lite_client_may_open_dialog(99, 1)) {
        fprintf(stderr, "client: non-merchant must not open dialog\n");
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
    if (sizeof(PacketCoopPcPos) != 0x40 || offsetof(PacketCoopPcPos, map) != 0x38
        || sizeof(PacketCoopCharOffer) != 0x78
        || offsetof(PacketCoopCharOffer, token) != 0x68
        || offsetof(PacketCoopSessionSync, flags) != 0xDC) {
        fprintf(stderr, "packet layout changed: pcpos=%zu offer=%zu\n",
            sizeof(PacketCoopPcPos), sizeof(PacketCoopCharOffer));
        return 1;
    }
    printf("pc_pos helpers / rate limit OK\n");
    return 0;
}

static bool bundle_visit(void* ctx, int index, const uint8_t* blob, size_t size)
{
    int* seen = (int*)ctx;

    if (size != (size_t)(index + 1) * 3 || blob[0] != (uint8_t)(0xA0 + index)) {
        return false;
    }
    seen[0]++;
    return true;
}

/* P0-2: stale poses cannot undo a correction. */
static int test_pose_order(void)
{
    CoopPoseOrder host;
    CoopPoseOrder peer;
    uint16_t e;

    memset(&host, 0, sizeof(host));
    memset(&peer, 0, sizeof(peer));
    if (offsetof(PacketCoopPcPos, epoch) != 0x3C || offsetof(PacketCoopPcPos, seq) != 0x3E
        || sizeof(PacketCoopPcPos) != 0x40 || sizeof(PacketCoopFollowHost) != 0x18
        || offsetof(PacketCoopSessionSync, map_gen) != 0xD4
        || offsetof(PacketCoopSessionSync, pose_epoch) != 0xD8) {
        fprintf(stderr, "pose ordering fields misplaced\n");
        return 1;
    }
    /* Host side (owner link): join bumps to epoch 1. */
    e = coop_lite_pose_order_bump(&host);
    if (e != 1 || coop_lite_pose_order_accept(&host, 0, 1, false, true)) {
        fprintf(stderr, "pre-join epoch accepted\n");
        return 1;
    }
    if (!coop_lite_pose_order_accept(&host, 1, 5, false, true)
        || !coop_lite_pose_order_accept(&host, 1, 6, false, true)
        || coop_lite_pose_order_accept(&host, 1, 6, false, true)
        || coop_lite_pose_order_accept(&host, 1, 4, false, true)) {
        fprintf(stderr, "seq ordering wrong\n");
        return 1;
    }
    /* The host corrects: epoch 2. The in-flight pose (epoch 1, seq 7) that
       arrives after the FORCE must not be applied (the PT race). */
    e = coop_lite_pose_order_bump(&host);
    if (e != 2 || coop_lite_pose_order_accept(&host, 1, 7, false, true)) {
        fprintf(stderr, "stale pose undid a correction\n");
        return 1;
    }
    /* A client may not invent a newer epoch. */
    if (coop_lite_pose_order_accept(&host, 3, 8, false, true)) {
        fprintf(stderr, "client-minted epoch accepted\n");
        return 1;
    }
    /* After adopting epoch 2 the owner's poses (any seq) flow again. */
    if (!coop_lite_pose_order_accept(&host, 2, 8, false, true)
        || !coop_lite_pose_order_accept(&host, 2, 9, false, true)) {
        fprintf(stderr, "post-correction pose refused\n");
        return 1;
    }
    /* Wrap-safe seq. */
    if (!coop_lite_pose_order_accept(&host, 2, 0x7000, false, true)
        || !coop_lite_pose_order_accept(&host, 2, 0xE000, false, true)
        || !coop_lite_pose_order_accept(&host, 2, 0x0001, false, true)
        || coop_lite_pose_order_accept(&host, 2, 0xE001, false, true)) {
        fprintf(stderr, "seq wrap refused\n");
        return 1;
    }
    /* Peer side (from host): newer epoch adopted (e.g. relayed pose overtakes
       the reliable FORCE), older dropped, FORCE in same epoch accepted. */
    if (!coop_lite_pose_order_accept(&peer, 4, 10, false, false)
        || !coop_lite_pose_order_accept(&peer, 5, 2, false, false)
        || coop_lite_pose_order_accept(&peer, 4, 11, false, false)
        || !coop_lite_pose_order_accept(&peer, 5, 2, true, false)) {
        fprintf(stderr, "peer epoch handling wrong\n");
        return 1;
    }
    /* Map identity: same index, new load generation = different map. */
    if (coop_lite_pose_map_key(7, 1) == coop_lite_pose_map_key(7, 2)
        || coop_lite_pose_map_key(7, 1) == coop_lite_pose_map_key(8, 1)
        || coop_lite_pose_map_key(7, 3) != coop_lite_pose_map_key(7, 3)) {
        fprintf(stderr, "map key wrong\n");
        return 1;
    }
    printf("pose epoch/seq + map generation OK\n");
    return 0;
}

/* P0-1: the .coopchar character bundle round-trips and rejects damage. */
static int test_char_bundle(void)
{
    CoopCharBundleBuf buf;
    uint8_t blob[16];
    int seen = 0;
    int count = 0;
    const char* rule = NULL;
    const char* name = NULL;
    int i;
    size_t cut;

    if (!coop_char_bundle_begin(&buf, 3)) {
        fprintf(stderr, "bundle begin failed\n");
        return 1;
    }
    for (i = 0; i < 3; i++) {
        memset(blob, 0xA0 + i, sizeof(blob));
        if (!coop_char_bundle_add_blob(&buf, blob, (size_t)(i + 1) * 3)) {
            fprintf(stderr, "bundle add failed\n");
            return 1;
        }
    }
    if (!coop_char_bundle_finish(&buf, "Fighter", "Melee")) {
        fprintf(stderr, "bundle finish failed\n");
        return 1;
    }
    if (!coop_char_bundle_parse(buf.data, buf.size, bundle_visit, &seen, &count, &rule, &name)
        || seen != 3 || count != 3 || strcmp(rule, "Fighter") != 0 || strcmp(name, "Melee") != 0) {
        fprintf(stderr, "bundle round trip failed (seen=%d count=%d)\n", seen, count);
        return 1;
    }
    /* Every truncation must be refused before the visitor runs. */
    for (cut = 0; cut < buf.size; cut++) {
        seen = 0;
        if (coop_char_bundle_parse(buf.data, cut, bundle_visit, &seen, NULL, NULL, NULL) || seen != 0) {
            fprintf(stderr, "truncated bundle (%zu/%zu) accepted\n", cut, buf.size);
            return 1;
        }
    }
    /* Trailing junk, wrong magic, oversized blob length. */
    {
        uint8_t* copy = (uint8_t*)malloc(buf.size + 1);

        memcpy(copy, buf.data, buf.size);
        copy[buf.size] = 0;
        if (coop_char_bundle_parse(copy, buf.size + 1, NULL, NULL, NULL, NULL, NULL)) {
            fprintf(stderr, "trailing junk accepted\n");
            return 1;
        }
        copy[0] ^= 0xFF;
        if (coop_char_bundle_parse(copy, buf.size, NULL, NULL, NULL, NULL, NULL)) {
            fprintf(stderr, "bad magic accepted\n");
            return 1;
        }
        copy[0] ^= 0xFF;
        copy[12] = 0xFF;
        copy[13] = 0xFF;
        copy[14] = 0xFF;
        copy[15] = 0x7F;
        if (coop_char_bundle_parse(copy, buf.size, NULL, NULL, NULL, NULL, NULL)) {
            fprintf(stderr, "huge blob length accepted\n");
            return 1;
        }
        free(copy);
    }
    coop_char_bundle_free(&buf);
    if (coop_char_bundle_begin(&buf, 0) || coop_char_bundle_begin(&buf, COOP_CHAR_BUNDLE_MAX_OBJECTS + 1)) {
        fprintf(stderr, "bundle object count limits wrong\n");
        return 1;
    }
    coop_char_bundle_free(&buf);
    printf("character bundle format OK\n");
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
            4, 8, 9, 11, 26, 27, 28, 29, 57, 81, 98, 100, 125,
            PACKET_COOP_ATTACK_INTENT, PACKET_COOP_CHAR_OFFER, PACKET_COOP_PC_POS,
            PACKET_COOP_BARTER, PACKET_COOP_OBJ_REQ, PACKET_COOP_CHAR_BUNDLE,
            PACKET_COOP_WORLD_REQ,
        };
        static const int host_sends[] = {
            2, 3, 8, 9, 10, 11, 58, 60, 61, 68, 69, 70, 71, 72, 73, 74, 75, 76,
            77, 92, 94, 98, 99, 100, 106, 118, 121,
            PACKET_COOP_FOLLOW_HOST, PACKET_COOP_SESSION_SYNC, PACKET_COOP_PC_POS,
            PACKET_COOP_FOLLOWERS, PACKET_COOP_NPC_POS, PACKET_COOP_PARTY_STATE,
            PACKET_COOP_INVEN_SYNC, PACKET_COOP_ROSTER, PACKET_COOP_OBJ_BLOB, 54,
            PACKET_COOP_WORLD_STATE,
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

    /* PT11 review H2: the host takes only whitelisted types from clients. */
    {
        static const int never_from_client[] = {
            54, 71, 94, 106, 118, 121, 97, 6, 64,
            PACKET_COOP_INVEN_SYNC, PACKET_COOP_ROSTER, PACKET_COOP_SESSION_SYNC,
            PACKET_COOP_OBJ_BLOB,
        };
        size_t i;

        for (i = 0; i < sizeof(never_from_client) / sizeof(never_from_client[0]); i++) {
            if (coop_lite_packet_allowed(never_from_client[i], true, 1)) {
                fprintf(stderr, "host accepts client-sent type %d\n", never_from_client[i]);
                return 1;
            }
        }
        /* The host's own loopback stays trusted. */
        if (!coop_lite_packet_allowed(121, true, 0)) {
            fprintf(stderr, "host loopback 121 dropped\n");
            return 1;
        }
        /* Clients never take barter requests / item transfers. */
        if (coop_lite_packet_allowed(PACKET_COOP_BARTER, false, 0)
            || coop_lite_packet_allowed(81, false, 0)) {
            fprintf(stderr, "client accepts host-resolve barter types\n");
            return 1;
        }
    }
    printf("packet whitelist (H2) OK\n");

    /* PT11 barter: inventory sync wire checks + transfer flags. */
    {
        PacketCoopInvenSync sync;

        memset(&sync, 0, sizeof(sync));
        sync.type = PACKET_COOP_INVEN_SYNC;
        sync.flags = COOP_INVEN_BATCH_FIRST | COOP_INVEN_BATCH_LAST;
        sync.count = 3;
        sync.gold = 400;
        if (coop_lite_inven_sync_size(3) != offsetof(PacketCoopInvenSync, entries) + 3 * sizeof(PacketCoopInvenEntry)
            || coop_lite_inven_sync_size(1000) != sizeof(PacketCoopInvenSync)
            || coop_lite_inven_sync_size(-5) != offsetof(PacketCoopInvenSync, entries)) {
            fprintf(stderr, "inven_sync_size wrong\n");
            return 1;
        }
        if (!coop_lite_inven_sync_valid(&sync, (int)coop_lite_inven_sync_size(3))) {
            fprintf(stderr, "valid inven sync rejected\n");
            return 1;
        }
        if (coop_lite_inven_sync_valid(&sync, (int)coop_lite_inven_sync_size(2))) {
            fprintf(stderr, "short inven sync accepted\n");
            return 1;
        }
        sync.count = COOP_INVEN_CHUNK_ENTRIES + 1;
        if (coop_lite_inven_sync_valid(&sync, (int)sizeof(sync))) {
            fprintf(stderr, "oversized count accepted\n");
            return 1;
        }
        sync.count = 0;
        sync.gold = -1;
        if (coop_lite_inven_sync_valid(&sync, (int)sizeof(sync))) {
            fprintf(stderr, "negative gold accepted\n");
            return 1;
        }
        sync.gold = 0;
        sync.flags = 0x10;
        if (coop_lite_inven_sync_valid(&sync, (int)sizeof(sync))) {
            fprintf(stderr, "unknown batch flag accepted\n");
            return 1;
        }
        if (!coop_lite_client_transfer_flags_allowed(0x01)
            || !coop_lite_client_transfer_flags_allowed(0x21)
            || !coop_lite_client_transfer_flags_allowed(0x02)
            || !coop_lite_client_transfer_flags_allowed(0x22)
            || coop_lite_client_transfer_flags_allowed(0x03)
            || coop_lite_client_transfer_flags_allowed(0x06)
            || coop_lite_client_transfer_flags_allowed(0x0A)
            || coop_lite_client_transfer_flags_allowed(0x04)
            || coop_lite_client_transfer_flags_allowed(0x24)
            || coop_lite_client_transfer_flags_allowed(0x08)
            || coop_lite_client_transfer_flags_allowed(0x11)
            || coop_lite_client_transfer_flags_allowed(0x12)
            || coop_lite_client_transfer_flags_allowed(0)) {
            fprintf(stderr, "transfer flag policy wrong\n");
            return 1;
        }
        /* PT18: pickpocket bits only as 0x41 / 0x81. */
        if (!coop_lite_client_transfer_flags_allowed(0x01 | COOP_XFER_STEAL)
            || !coop_lite_client_transfer_flags_allowed(0x01 | COOP_XFER_PLANT)
            || coop_lite_client_transfer_flags_allowed(COOP_XFER_STEAL)
            || coop_lite_client_transfer_flags_allowed(0x21 | COOP_XFER_STEAL)
            || coop_lite_client_transfer_flags_allowed(0x01 | COOP_XFER_STEAL | COOP_XFER_PLANT)
            || coop_lite_client_transfer_flags_allowed(0x02 | COOP_XFER_STEAL)) {
            fprintf(stderr, "pickpocket flag policy wrong\n");
            return 1;
        }
        /* PT18 target sessions. */
        if (coop_lite_target_kind(true, false, false, false, false, 2) != COOP_TARGET_LOOT
            || coop_lite_target_kind(true, true, false, false, false, 2) != COOP_TARGET_NONE
            || coop_lite_target_kind(false, false, true, true, false, 1) != COOP_TARGET_LOOT
            || coop_lite_target_kind(false, false, true, false, false, 1) != COOP_TARGET_STEAL
            || coop_lite_target_kind(false, false, true, false, true, 0) != COOP_TARGET_NONE
            || coop_lite_target_kind(false, false, true, true, false, COOP_TARGET_REACH + 1) != COOP_TARGET_NONE
            || coop_lite_target_kind(false, false, false, false, false, 1) != COOP_TARGET_NONE) {
            fprintf(stderr, "target kind policy wrong\n");
            return 1;
        }
        if (!coop_lite_target_transfer_ok(COOP_TARGET_LOOT, 0x01, true, true)
            || !coop_lite_target_transfer_ok(COOP_TARGET_LOOT, 0x21, false, false)
            || coop_lite_target_transfer_ok(COOP_TARGET_LOOT, 0x01 | COOP_XFER_STEAL, true, false)
            || coop_lite_target_transfer_ok(COOP_TARGET_LOOT, 0x02, false, false)
            || !coop_lite_target_transfer_ok(COOP_TARGET_STEAL, 0x01 | COOP_XFER_STEAL, true, false)
            || coop_lite_target_transfer_ok(COOP_TARGET_STEAL, 0x01 | COOP_XFER_STEAL, true, true)
            || coop_lite_target_transfer_ok(COOP_TARGET_STEAL, 0x01, true, false)
            || !coop_lite_target_transfer_ok(COOP_TARGET_STEAL, 0x01 | COOP_XFER_PLANT, false, false)
            || coop_lite_target_transfer_ok(COOP_TARGET_STEAL, 0x01 | COOP_XFER_STEAL, false, false)
            || coop_lite_target_transfer_ok(COOP_TARGET_NONE, 0x01, true, false)) {
            fprintf(stderr, "target transfer policy wrong\n");
            return 1;
        }
        if (coop_lite_coop_packet_min_size(PACKET_COOP_BARTER) != sizeof(PacketCoopBarter)
            || coop_lite_coop_packet_min_size(PACKET_COOP_INVEN_SYNC) != offsetof(PacketCoopInvenSync, entries)
            || coop_lite_coop_packet_min_size(PACKET_COOP_ROSTER) != sizeof(PacketCoopRoster)) {
            fprintf(stderr, "barter/roster min sizes wrong\n");
            return 1;
        }
    }
    printf("barter inventory sync rules OK\n");

    /* Review M2/M3: client PC_POS plausibility on the host. */
    if (!coop_lite_pc_pos_plausible(1, 1, 0, 2, 2)
        || !coop_lite_pc_pos_plausible(1, 1, COOP_LITE_PC_POS_MAX_STEP, 2, 2)
        || coop_lite_pc_pos_plausible(2, 1, 0, 2, 2)
        || coop_lite_pc_pos_plausible(1, 1, COOP_LITE_PC_POS_MAX_STEP + 1, 2, 2)
        || coop_lite_pc_pos_plausible(1, 1, -1, 2, 2)
        || coop_lite_pc_pos_plausible(1, 1, 1, 3, 2)) {
        fprintf(stderr, "pc_pos_plausible wrong\n");
        return 1;
    }
    /* Review H3: roster entries only for other clients' slots. */
    if (!coop_lite_roster_slot_ok(2, 1) || coop_lite_roster_slot_ok(1, 1)
        || coop_lite_roster_slot_ok(0, 1) || coop_lite_roster_slot_ok(8, 1)
        || coop_lite_roster_slot_ok(-1, 1)) {
        fprintf(stderr, "roster_slot_ok wrong\n");
        return 1;
    }
    printf("pose plausibility + roster rules OK\n");

    /* PT8 world replication: host-only state, clients may not inject it. */
    if (coop_lite_packet_allowed(PACKET_COOP_FOLLOWERS, true, 1)
        || coop_lite_packet_allowed(PACKET_COOP_NPC_POS, true, 1)
        || coop_lite_packet_allowed(PACKET_COOP_NPC_POS, false, 2)) {
        fprintf(stderr, "FOLLOWERS/NPC_POS direction wrong\n");
        return 1;
    }
    if (coop_lite_coop_packet_min_size(PACKET_COOP_FOLLOWERS) != 8
        || coop_lite_coop_packet_min_size(PACKET_COOP_NPC_POS) != 8
        || coop_lite_batch_size(0, COOP_NPC_POS_MAX, sizeof(PacketCoopNpcPosEntry)) != 8
        || coop_lite_batch_size(3, COOP_NPC_POS_MAX, sizeof(PacketCoopNpcPosEntry)) != 8 + 3 * 0x38
        || coop_lite_batch_size(COOP_NPC_POS_MAX + 1, COOP_NPC_POS_MAX, 0x38) != 0
        || coop_lite_batch_size(-1, COOP_NPC_POS_MAX, 0x38) != 0
        || sizeof(PacketCoopNpcPos) > 1200 /* ENet fragments bigger unreliable datagrams */
        || coop_lite_batch_size(COOP_NPC_POS_MAX, COOP_NPC_POS_MAX, sizeof(PacketCoopNpcPosEntry)) > TIG_NET_MAX_PACKET
        || coop_lite_batch_size(COOP_FOLLOWERS_MAX, COOP_FOLLOWERS_MAX, sizeof(PacketCoopFollowerEntry)) != sizeof(PacketCoopFollowers)) {
        fprintf(stderr, "batch sizes wrong\n");
        return 1;
    }
    /* A short datagram claiming many entries is rejected. */
    if (coop_lite_batch_valid(5, COOP_NPC_POS_MAX, 0x38, 8 + 4 * 0x38)
        || !coop_lite_batch_valid(5, COOP_NPC_POS_MAX, 0x38, 8 + 5 * 0x38)
        || coop_lite_batch_valid(COOP_NPC_POS_MAX + 1, COOP_NPC_POS_MAX, 0x38, 4096)
        || coop_lite_batch_valid(-2, COOP_NPC_POS_MAX, 0x38, 4096)) {
        fprintf(stderr, "batch_valid wrong\n");
        return 1;
    }
    /* PT10: no auto-revive; party death state machine. */
    if (coop_lite_party_death_tests() != 0) {
        return 1;
    }
    /* Camera: recentre only when the PC leaves the visible area. */
    if (coop_lite_follow_needs_jump(0, 0, COOP_LITE_FOLLOW_JUMP_X, COOP_LITE_FOLLOW_JUMP_Y)
        || coop_lite_follow_needs_jump(COOP_LITE_FOLLOW_JUMP_X, -COOP_LITE_FOLLOW_JUMP_Y,
            COOP_LITE_FOLLOW_JUMP_X, COOP_LITE_FOLLOW_JUMP_Y)
        || !coop_lite_follow_needs_jump(COOP_LITE_FOLLOW_JUMP_X + 1, 0,
            COOP_LITE_FOLLOW_JUMP_X, COOP_LITE_FOLLOW_JUMP_Y)
        || !coop_lite_follow_needs_jump(0, -(COOP_LITE_FOLLOW_JUMP_Y + 1),
            COOP_LITE_FOLLOW_JUMP_X, COOP_LITE_FOLLOW_JUMP_Y)
        || COOP_LITE_FOLLOW_JUMP_X <= COOP_LITE_FOLLOW_DEADZONE_X
        || COOP_LITE_FOLLOW_JUMP_Y <= COOP_LITE_FOLLOW_DEADZONE_Y) {
        fprintf(stderr, "follow_needs_jump wrong\n");
        return 1;
    }
    printf("PT8 world replication helpers: OK\n");

    /* PT9: no host world packets outside a synced, loaded world. */
    if (!coop_lite_world_ready_rule(true, false, false, true, false, true)
        || coop_lite_world_ready_rule(false, false, false, true, false, true)
        || coop_lite_world_ready_rule(true, true, false, true, false, true)
        || coop_lite_world_ready_rule(true, false, true, true, false, true)
        || coop_lite_world_ready_rule(true, false, false, false, false, true)
        || coop_lite_world_ready_rule(true, false, false, true, true, true)
        || coop_lite_world_ready_rule(true, false, false, true, false, false)) {
        fprintf(stderr, "world_ready_rule wrong\n");
        return 1;
    }
    {
        int x = 0;
        int y = 0;

        if (!coop_lite_parse_dev_loc("shrouded", &x, &y)
            || x != COOP_LITE_DEV_SHROUDED_X || y != COOP_LITE_DEV_SHROUDED_Y
            || !coop_lite_parse_dev_loc("merchant", &x, &y) || x != COOP_LITE_DEV_SHROUDED_X
            || !coop_lite_parse_dev_loc("1200,3400", &x, &y) || x != 1200 || y != 3400
            || coop_lite_parse_dev_loc("", &x, &y)
            || coop_lite_parse_dev_loc(NULL, &x, &y)
            || coop_lite_parse_dev_loc("12", &x, &y)
            || coop_lite_parse_dev_loc("12,34x", &x, &y)
            || coop_lite_parse_dev_loc("-1,5", &x, &y)) {
            fprintf(stderr, "parse_dev_loc wrong\n");
            return 1;
        }
    }
    printf("PT9 world-ready / dev loc helpers: OK\n");

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

/* ---- PT10 coop death state machine ---- */
static void party_set(bool present[8], bool dead[8], int n_present)
{
    int s;

    for (s = 0; s < 8; s++) {
        present[s] = s < n_present;
        dead[s] = false;
    }
}

#define PARTY_FAIL(msg) do { fprintf(stderr, "party: %s\n", msg); return 1; } while (0)

static int coop_lite_party_death_tests(void)
{
    CoopPartyWatch w;
    bool present[8];
    bool dead[8];
    unsigned int fell;
    unsigned int rose;
    unsigned int acts;
    unsigned int t;

    /* 1. Client dies -> corpse (no revive, no reload) -> carried across a
          host map change -> resurrected. */
    memset(&w, 0, sizeof(w));
    party_set(present, dead, 2);
    if (coop_lite_party_step(&w, present, dead, 1, 1000u, &fell, &rose) != 0) PARTY_FAIL("idle");
    dead[1] = true;
    acts = coop_lite_party_step(&w, present, dead, 1, 2000u, &fell, &rose);
    if (acts != 0 || fell != 0x2u || rose != 0) PARTY_FAIL("client fell");
    for (t = 2000u; t < 600000u; t += 5000u) {
        if (coop_lite_party_step(&w, present, dead, 1, t, &fell, &rose) != 0 || fell != 0) {
            PARTY_FAIL("client corpse must just stay");
        }
    }
    if (!coop_lite_should_carry_slot(1, 0, true) /* host travel carries the corpse */
        || coop_lite_should_carry_slot(0, 0, true) /* not the mover itself */
        || coop_lite_should_carry_slot(2, 0, false) /* nothing bound */
        || !coop_lite_should_carry_slot(0, 1, true) /* client follow carries the host proxy */
        || coop_lite_should_carry_slot(8, 0, true)) {
        PARTY_FAIL("carry rule");
    }
    /* Host (alive) changes map with the corpse: still no action. */
    if (coop_lite_party_step(&w, present, dead, 2, 610000u, &fell, &rose) != 0) PARTY_FAIL("map change, host alive");
    dead[1] = false;
    acts = coop_lite_party_step(&w, present, dead, 2, 611000u, &fell, &rose);
    if (acts != 0 || rose != 0x2u) PARTY_FAIL("client resurrected");

    /* 2. Host dies, client alive -> notice, waits indefinitely, resurrected
          by the client. */
    memset(&w, 0, sizeof(w));
    party_set(present, dead, 2);
    coop_lite_party_step(&w, present, dead, 1, 0u, NULL, NULL);
    dead[0] = true;
    acts = coop_lite_party_step(&w, present, dead, 1, 1000u, &fell, NULL);
    if (acts != COOP_PARTY_ACT_HOST_DOWN || fell != 0x1u) PARTY_FAIL("host down");
    for (t = 1000u; t < 900000u; t += 7000u) {
        if (coop_lite_party_step(&w, present, dead, 1, t, NULL, NULL) != 0) PARTY_FAIL("host down must wait");
    }
    dead[0] = false;
    if (coop_lite_party_step(&w, present, dead, 1, 900000u, NULL, &rose) != COOP_PARTY_ACT_HOST_UP || rose != 0x1u) {
        PARTY_FAIL("host up");
    }

    /* 3. Everyone dead -> countdown -> reload once after the grace. */
    memset(&w, 0, sizeof(w));
    party_set(present, dead, 3);
    coop_lite_party_step(&w, present, dead, 1, 0u, NULL, NULL);
    dead[1] = true;
    coop_lite_party_step(&w, present, dead, 1, 100u, NULL, NULL);
    dead[0] = true;
    acts = coop_lite_party_step(&w, present, dead, 1, 200u, NULL, NULL);
    if (acts != COOP_PARTY_ACT_HOST_DOWN) PARTY_FAIL("host down with client 2 alive");
    dead[2] = true;
    if (coop_lite_party_step(&w, present, dead, 1, 300u, NULL, NULL) != COOP_PARTY_ACT_WIPE_COUNTDOWN) PARTY_FAIL("wipe countdown");
    if (coop_lite_party_step(&w, present, dead, 1, 300u + COOP_LITE_WIPE_GRACE_MS - 1, NULL, NULL) != 0) PARTY_FAIL("grace");
    if (coop_lite_party_step(&w, present, dead, 1, 300u + COOP_LITE_WIPE_GRACE_MS, NULL, NULL) != COOP_PARTY_ACT_RELOAD) PARTY_FAIL("reload");
    if (coop_lite_party_step(&w, present, dead, 1, 400000u, NULL, NULL) != 0) PARTY_FAIL("reload only once");

    /* 3b. Someone gets up during the grace -> cancel. */
    memset(&w, 0, sizeof(w));
    party_set(present, dead, 2);
    dead[0] = dead[1] = true;
    acts = coop_lite_party_step(&w, present, dead, 1, 50u, NULL, NULL);
    if (acts != (COOP_PARTY_ACT_HOST_DOWN | COOP_PARTY_ACT_WIPE_COUNTDOWN)) PARTY_FAIL("wipe at once");
    dead[1] = false;
    if (coop_lite_party_step(&w, present, dead, 1, 5000u, NULL, NULL) != COOP_PARTY_ACT_WIPE_CANCEL) PARTY_FAIL("cancel");
    if (coop_lite_party_step(&w, present, dead, 1, 60000u, NULL, NULL) != 0) PARTY_FAIL("no reload after cancel");

    /* 4. Host down and the location is left -> reload at once. */
    memset(&w, 0, sizeof(w));
    party_set(present, dead, 2);
    dead[0] = true;
    coop_lite_party_step(&w, present, dead, 5, 10u, NULL, NULL);
    if (coop_lite_party_step(&w, present, dead, 6, 20u, NULL, NULL) != COOP_PARTY_ACT_RELOAD) PARTY_FAIL("left location");

    /* 5. Host alone dies -> wipe; host not in the world -> nothing. */
    memset(&w, 0, sizeof(w));
    party_set(present, dead, 1);
    dead[0] = true;
    if (coop_lite_party_step(&w, present, dead, 1, 10u, NULL, NULL) != (COOP_PARTY_ACT_HOST_DOWN | COOP_PARTY_ACT_WIPE_COUNTDOWN)) {
        PARTY_FAIL("solo host wipe");
    }
    memset(&w, 0, sizeof(w));
    party_set(present, dead, 0);
    if (coop_lite_party_step(&w, present, dead, 1, 10u, NULL, NULL) != 0) PARTY_FAIL("no host");

    /* Packet. */
    if (!coop_lite_packet_allowed(PACKET_COOP_PARTY_STATE, false, 0)
        || coop_lite_packet_allowed(PACKET_COOP_PARTY_STATE, true, 1)
        || coop_lite_coop_packet_min_size(PACKET_COOP_PARTY_STATE) != sizeof(PacketCoopPartyState)
        || PACKET_COOP_PARTY_STATE >= PACKET_COOP_TYPE_LIMIT) {
        PARTY_FAIL("PARTY_STATE packet rules");
    }
    printf("PT10 party death state machine: OK\n");
    return 0;
}

/* P1-2: item blob request / reply validation. */
static int test_obj_blob(void)
{
    static PacketCoopObjBlob blob;
    PacketCoopObjReq req;
    const int hdr = (int)offsetof(PacketCoopObjBlob, data);
    int i;

    if (coop_lite_packet_allowed(PACKET_COOP_OBJ_BLOB, true, 1)
        || !coop_lite_packet_allowed(PACKET_COOP_OBJ_REQ, true, 1)
        || coop_lite_packet_allowed(PACKET_COOP_OBJ_REQ, false, 0)
        || !coop_lite_packet_allowed(PACKET_COOP_OBJ_BLOB, false, 0)) {
        fprintf(stderr, "obj blob: packet roles wrong\n");
        return 1;
    }
    if (coop_lite_coop_packet_min_size(PACKET_COOP_OBJ_REQ) != 8
        || coop_lite_coop_packet_min_size(PACKET_COOP_OBJ_BLOB) != 0x40) {
        fprintf(stderr, "obj blob: min sizes wrong\n");
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.type = PACKET_COOP_OBJ_REQ;
    req.count = 2;
    if (!coop_lite_obj_req_valid(&req, 8 + 48)
        || coop_lite_obj_req_valid(&req, 8 + 47)
        || coop_lite_obj_req_valid(&req, 7)) {
        fprintf(stderr, "obj req: size checks wrong\n");
        return 1;
    }
    req.count = 0;
    if (coop_lite_obj_req_valid(&req, (int)sizeof(req))) {
        fprintf(stderr, "obj req: empty accepted\n");
        return 1;
    }
    req.count = COOP_OBJ_REQ_MAX + 1;
    if (coop_lite_obj_req_valid(&req, (int)sizeof(req))) {
        fprintf(stderr, "obj req: count over max accepted\n");
        return 1;
    }

    memset(&blob, 0, sizeof(blob));
    blob.type = PACKET_COOP_OBJ_BLOB;
    for (i = 0; i < 24; i++) {
        blob.oid[i] = (uint8_t)(0x40 + i);
    }
    blob.size = 200;
    memcpy(blob.data + 4 + 24, blob.oid, 24);
    if (!coop_lite_obj_blob_valid(&blob, hdr + 200)) {
        fprintf(stderr, "obj blob: good blob refused\n");
        return 1;
    }
    if (coop_lite_obj_blob_valid(&blob, hdr + 199)) {
        fprintf(stderr, "obj blob: truncated blob accepted\n");
        return 1;
    }
    blob.data[4 + 24] ^= 1;
    if (coop_lite_obj_blob_valid(&blob, hdr + 200)) {
        fprintf(stderr, "obj blob: blob of another object accepted\n");
        return 1;
    }
    blob.data[4 + 24] ^= 1;
    blob.size = COOP_OBJ_BLOB_MAX + 1;
    if (coop_lite_obj_blob_valid(&blob, (int)sizeof(blob))) {
        fprintf(stderr, "obj blob: oversize accepted\n");
        return 1;
    }
    blob.size = 10;
    if (coop_lite_obj_blob_valid(&blob, hdr + 10)) {
        fprintf(stderr, "obj blob: tiny blob accepted\n");
        return 1;
    }
    blob.size = -5;
    if (coop_lite_obj_blob_valid(&blob, hdr)) {
        fprintf(stderr, "obj blob: negative size accepted\n");
        return 1;
    }
    blob.size = 0;
    blob.flags = COOP_OBJ_BLOB_F_GONE;
    if (!coop_lite_obj_blob_valid(&blob, hdr)) {
        fprintf(stderr, "obj blob: gone marker refused\n");
        return 1;
    }
    blob.size = 200;
    if (coop_lite_obj_blob_valid(&blob, hdr + 200)) {
        fprintf(stderr, "obj blob: gone marker with data accepted\n");
        return 1;
    }
    blob.flags = 0;
    blob.type = PACKET_COOP_INVEN_SYNC;
    if (coop_lite_obj_blob_valid(&blob, hdr + 200)) {
        fprintf(stderr, "obj blob: wrong type accepted\n");
        return 1;
    }
    return 0;
}

/* Parked PCs and rejoin tokens. */
static int test_char_rx(void)
{
    static PacketCoopCharChunk c;
    CoopCharRx rx;
    int stats[8] = { 8, 8, 8, 8, 8, 8, 8, 8 };
    int hdr = (int)offsetof(PacketCoopCharChunk, data);
    int i;

    memset(&rx, 0, sizeof(rx));
    memset(&c, 0, sizeof(c));
    c.type = PACKET_COOP_CHAR_BUNDLE;
    c.total = 1500;
    c.offset = 0;
    c.len = COOP_CHAR_CHUNK;
    for (i = 0; i < COOP_CHAR_CHUNK; i++) {
        c.data[i] = (uint8_t)i;
    }
    if (!coop_char_rx_accept(&rx, &c, hdr + c.len) || rx.complete || rx.got != COOP_CHAR_CHUNK) {
        fprintf(stderr, "char rx: first chunk\n");
        return 1;
    }
    /* Out of order / duplicate: refused, transfer dropped. */
    if (coop_char_rx_accept(&rx, &c, hdr + c.len - 0) == false) {
        /* offset 0 restarts: accepted */
        fprintf(stderr, "char rx: restart refused\n");
        return 1;
    }
    c.offset = COOP_CHAR_CHUNK;
    c.len = 1500 - COOP_CHAR_CHUNK;
    if (coop_char_rx_accept(&rx, &c, hdr + c.len - 1)) {
        fprintf(stderr, "char rx: short packet accepted\n");
        return 1;
    }
    if (rx.data != NULL) {
        fprintf(stderr, "char rx: refused chunk kept the transfer\n");
        return 1;
    }
    c.offset = 0;
    c.len = COOP_CHAR_CHUNK;
    coop_char_rx_accept(&rx, &c, hdr + c.len);
    c.offset = COOP_CHAR_CHUNK;
    c.len = 1500 - COOP_CHAR_CHUNK;
    if (!coop_char_rx_accept(&rx, &c, hdr + c.len) || !rx.complete || rx.data[1023] != (uint8_t)1023) {
        fprintf(stderr, "char rx: completion\n");
        return 1;
    }
    /* Past the end, oversize totals, skipped offsets. */
    if (coop_char_rx_accept(&rx, &c, hdr + c.len)) {
        fprintf(stderr, "char rx: chunk after completion accepted\n");
        return 1;
    }
    c.offset = 0;
    c.total = COOP_CHAR_WIRE_MAX + 1;
    c.len = 16;
    if (coop_char_rx_accept(&rx, &c, hdr + 16)) {
        fprintf(stderr, "char rx: oversize total\n");
        return 1;
    }
    c.total = 100;
    c.offset = 90;
    c.len = 16;
    if (coop_char_rx_accept(&rx, &c, hdr + 16)) {
        fprintf(stderr, "char rx: chunk past total\n");
        return 1;
    }
    c.offset = 0;
    c.len = 16;
    coop_char_rx_accept(&rx, &c, hdr + 16);
    c.offset = 32;
    if (coop_char_rx_accept(&rx, &c, hdr + 16)) {
        fprintf(stderr, "char rx: gap accepted\n");
        return 1;
    }
    coop_char_rx_free(&rx);

    if (!coop_lite_char_plausible(1, 0, stats, 8, 400, 3)
        || !coop_lite_char_plausible(50, 5000000, stats, 8, 0, 0)) {
        fprintf(stderr, "char plausible: normal characters refused\n");
        return 1;
    }
    stats[3] = 99;
    if (coop_lite_char_plausible(1, 0, stats, 8, 400, 3)) {
        fprintf(stderr, "char plausible: stat 99\n");
        return 1;
    }
    stats[3] = 8;
    if (coop_lite_char_plausible(0, 0, stats, 8, 0, 0)
        || coop_lite_char_plausible(500, 0, stats, 8, 0, 0)
        || coop_lite_char_plausible(1, -1, stats, 8, 0, 0)
        || coop_lite_char_plausible(1, 0, stats, 8, 2000000, 0)
        || coop_lite_char_plausible(1, 0, stats, 8, 0, COOP_CHAR_MAX_ITEMS + 1)) {
        fprintf(stderr, "char plausible: limits\n");
        return 1;
    }
    return 0;
}

static int test_parked_save(void)
{
    static uint8_t buf[COOP_PARKED_SAVE_MAX];
    unsigned char oid_a[24];
    unsigned char oid_b[24];
    unsigned char extra_oid[1][24];
    unsigned char out[24];
    uint64_t extra_hash[1] = { 0x2222 };
    int size;

    memset(oid_a, 0xA1, sizeof(oid_a));
    memset(oid_b, 0xB2, sizeof(oid_b));
    memcpy(extra_oid[0], oid_b, 24);
    coop_lite_parked_reset();
    coop_lite_parked_put(0x1111, oid_a);
    size = coop_lite_parked_serialize(buf, (int)sizeof(buf), extra_hash,
        (const unsigned char(*)[24])extra_oid, 1);
    if (size != 8 + 2 * COOP_PARKED_SAVE_ENTRY || coop_lite_parked_count() != 1) {
        fprintf(stderr, "parked save: size %d / live table changed\n", size);
        return 1;
    }
    if (coop_lite_parked_serialize(buf, 16, NULL, NULL, 0) != -1) {
        fprintf(stderr, "parked save: small buffer accepted\n");
        return 1;
    }
    coop_lite_parked_reset();
    if (!coop_lite_parked_deserialize(buf, size) || coop_lite_parked_count() != 2
        || !coop_lite_parked_has_oid(oid_b)
        || !coop_lite_parked_take(0x2222, out) || memcmp(out, oid_b, 24) != 0
        || !coop_lite_parked_take(0x1111, out) || memcmp(out, oid_a, 24) != 0) {
        fprintf(stderr, "parked save: round trip\n");
        return 1;
    }
    /* Malformed: wrong length, wrong magic, too many entries. */
    if (coop_lite_parked_deserialize(buf, size - 1) || coop_lite_parked_count() != 0) {
        fprintf(stderr, "parked save: short block accepted\n");
        return 1;
    }
    buf[0] ^= 1;
    if (coop_lite_parked_deserialize(buf, size)) {
        fprintf(stderr, "parked save: bad magic accepted\n");
        return 1;
    }
    buf[0] ^= 1;
    {
        uint32_t big = COOP_PARKED_MAX + 1;

        memcpy(buf + 4, &big, 4);
        if (coop_lite_parked_deserialize(buf, 8 + (int)big * COOP_PARKED_SAVE_ENTRY)) {
            fprintf(stderr, "parked save: oversize count accepted\n");
            return 1;
        }
    }
    coop_lite_parked_reset();
    return 0;
}

static int test_parked(void)
{
    unsigned char zero[16] = { 0 };
    unsigned char a[16];
    unsigned char b[16];
    unsigned char oid1[24];
    unsigned char oid2[24];
    unsigned char out[24];
    uint64_t ha;
    uint64_t hb;
    int i;

    for (i = 0; i < 16; i++) {
        a[i] = (unsigned char)(i + 1);
        b[i] = (unsigned char)(i + 1);
    }
    b[15] ^= 0x80;
    memset(oid1, 0x11, sizeof(oid1));
    memset(oid2, 0x22, sizeof(oid2));
    ha = coop_lite_token_hash(a);
    hb = coop_lite_token_hash(b);
    if (coop_lite_token_hash(zero) != 0 || ha == 0 || hb == 0 || ha == hb
        || ha != coop_lite_token_hash(a)) {
        fprintf(stderr, "parked: token hash wrong\n");
        return 1;
    }
    coop_lite_parked_reset();
    if (coop_lite_parked_put(0, oid1) || coop_lite_parked_take(0, out)) {
        fprintf(stderr, "parked: no-token entry accepted\n");
        return 1;
    }
    if (!coop_lite_parked_put(ha, oid1) || !coop_lite_parked_put(hb, oid2)
        || coop_lite_parked_count() != 2) {
        fprintf(stderr, "parked: put failed\n");
        return 1;
    }
    /* Same token again: replaces, does not duplicate. */
    if (!coop_lite_parked_put(ha, oid2) || coop_lite_parked_count() != 2) {
        fprintf(stderr, "parked: same token duplicated\n");
        return 1;
    }
    if (!coop_lite_parked_take(ha, out) || memcmp(out, oid2, 24) != 0
        || coop_lite_parked_take(ha, out) || coop_lite_parked_count() != 1) {
        fprintf(stderr, "parked: take wrong (must hand out once)\n");
        return 1;
    }
    if (!coop_lite_parked_take(hb, out) || memcmp(out, oid2, 24) != 0) {
        fprintf(stderr, "parked: other token lost\n");
        return 1;
    }
    for (i = 0; i < COOP_PARKED_MAX; i++) {
        a[0] = (unsigned char)(0xA0 + i);
        if (!coop_lite_parked_put(coop_lite_token_hash(a), oid1)) {
            fprintf(stderr, "parked: table smaller than COOP_PARKED_MAX\n");
            return 1;
        }
    }
    a[0] = 0xFF;
    if (coop_lite_parked_put(coop_lite_token_hash(a), oid1)) {
        fprintf(stderr, "parked: overfull table accepted\n");
        return 1;
    }
    coop_lite_parked_reset();
    if (coop_lite_parked_count() != 0) {
        fprintf(stderr, "parked: reset failed\n");
        return 1;
    }
    return 0;
}

/* Remote-PC interpolation helpers. */
static int test_interp(void)
{
    CoopInterpRing ring;
    CoopInterpSample a = { 100, 0, 0, 7, 1000 };
    CoopInterpSample b = { 101, 0, 0, 8, 1050 };
    CoopInterpSample c = { 102, 4, 0, 9, 1100 };
    CoopInterpSample far = { 500, 0, 0, 9, 1150 };
    int ia = -1;
    int ib = -1;
    unsigned int num = 0;
    unsigned int den = 0;

    coop_interp_reset(&ring);
    if (coop_interp_pick(&ring, 5000, &ia, &ib, &num, &den) != 0) {
        fprintf(stderr, "interp: empty ring picked something\n");
        return 1;
    }
    if (coop_interp_push(&ring, &a, -1) || ring.n != 1) {
        fprintf(stderr, "interp: first sample must snap\n");
        return 1;
    }
    if (!coop_interp_push(&ring, &b, 1) || !coop_interp_push(&ring, &c, 1) || ring.n != 3) {
        fprintf(stderr, "interp: adjacent samples refused\n");
        return 1;
    }
    /* Same pose again (keyframe): ignored, time kept. */
    c.t_ms = 2000;
    if (!coop_interp_push(&ring, &c, 0) || ring.n != 3 || ring.s[2].t_ms != 1100) {
        fprintf(stderr, "interp: keyframe repeat changed the ring\n");
        return 1;
    }
    if (coop_interp_pick(&ring, 900, &ia, &ib, &num, &den) != 1 || ia != 0) {
        fprintf(stderr, "interp: before oldest must hold it\n");
        return 1;
    }
    if (coop_interp_pick(&ring, 1025, &ia, &ib, &num, &den) != 2 || ia != 0 || ib != 1
        || num != 25 || den != 50) {
        fprintf(stderr, "interp: mid a-b wrong\n");
        return 1;
    }
    if (coop_interp_pick(&ring, 1075, &ia, &ib, &num, &den) != 2 || ia != 1 || ib != 2) {
        fprintf(stderr, "interp: mid b-c wrong\n");
        return 1;
    }
    if (coop_interp_pick(&ring, 1500, &ia, &ib, &num, &den) != 1 || ia != 2) {
        fprintf(stderr, "interp: past newest must show it\n");
        return 1;
    }
    /* Wrap of the ms clock. */
    {
        CoopInterpRing w;
        CoopInterpSample p = { 1, 0, 0, 1, 0xFFFFFFF0u };
        CoopInterpSample q = { 2, 0, 0, 1, 0x00000010u };

        coop_interp_reset(&w);
        coop_interp_push(&w, &p, -1);
        coop_interp_push(&w, &q, 1);
        if (coop_interp_pick(&w, 0, &ia, &ib, &num, &den) != 2 || num != 16 || den != 32) {
            fprintf(stderr, "interp: clock wrap wrong\n");
            return 1;
        }
    }
    /* Jump: snap and restart. */
    if (coop_interp_push(&ring, &far, 40) || ring.n != 1 || ring.s[0].loc != 500) {
        fprintf(stderr, "interp: jump did not snap\n");
        return 1;
    }
    /* Offsets: tile b is 40 px right of a; halfway = 20 px from a, -20 from b. */
    if (coop_interp_offset(0, 0, 40, 1, 2, false) != 20
        || coop_interp_offset(0, 0, 40, 1, 2, true) != -20
        || coop_interp_offset(0, 0, 40, 0, 4, false) != 0
        || coop_interp_offset(0, 0, 40, 4, 4, true) != 0
        || coop_interp_offset(5, -5, 40, 9, 4, true) != -5 /* clamped to b */
        || coop_interp_offset(3, 3, 0, 1, 0, false) != 3) {
        fprintf(stderr, "interp: offset math wrong\n");
        return 1;
    }
    return 0;
}

static int test_world_state(void)
{
    static PacketCoopWorldState pkt;
    static CoopOidSet set;
    uint8_t oid[24];
    int i;

    if (!coop_lite_packet_allowed(PACKET_COOP_WORLD_REQ, true, 1)
        || coop_lite_packet_allowed(PACKET_COOP_WORLD_REQ, false, 0)
        || !coop_lite_packet_allowed(PACKET_COOP_WORLD_STATE, false, 0)
        || coop_lite_packet_allowed(PACKET_COOP_WORLD_STATE, true, 1)
        || coop_lite_coop_packet_min_size(PACKET_COOP_WORLD_REQ) != 8
        || coop_lite_coop_packet_min_size(PACKET_COOP_WORLD_STATE) != 16) {
        fprintf(stderr, "world: packet role/size\n");
        return 1;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_WORLD_STATE;
    pkt.count = 2;
    pkt.entries[0].kind = COOP_WORLD_PORTAL;
    pkt.entries[1].kind = COOP_WORLD_GROUND_ITEM;
    if (!coop_lite_world_state_valid(&pkt, coop_lite_world_state_size(2))
        || coop_lite_world_state_size(2) != 16 + 128) {
        fprintf(stderr, "world: good chunk refused\n");
        return 1;
    }
    if (coop_lite_world_state_valid(&pkt, coop_lite_world_state_size(2) - 1)
        || coop_lite_world_state_valid(&pkt, coop_lite_world_state_size(3))
        || coop_lite_world_state_valid(&pkt, 8)
        || coop_lite_world_state_valid(NULL, 16)) {
        fprintf(stderr, "world: size mismatch accepted\n");
        return 1;
    }
    pkt.entries[1].kind = 9;
    if (coop_lite_world_state_valid(&pkt, coop_lite_world_state_size(2))) {
        fprintf(stderr, "world: bad kind accepted\n");
        return 1;
    }
    pkt.entries[1].kind = 0;
    if (coop_lite_world_state_valid(&pkt, coop_lite_world_state_size(2))) {
        fprintf(stderr, "world: kind 0 accepted\n");
        return 1;
    }
    pkt.count = COOP_WORLD_CHUNK + 1;
    if (coop_lite_world_state_valid(&pkt, coop_lite_world_state_size(COOP_WORLD_CHUNK + 1))) {
        fprintf(stderr, "world: over-long chunk accepted\n");
        return 1;
    }
    pkt.count = -1;
    if (coop_lite_world_state_valid(&pkt, 16)) {
        fprintf(stderr, "world: negative count accepted\n");
        return 1;
    }
    pkt.count = 0;
    pkt.type = PACKET_COOP_INVEN_SYNC;
    if (coop_lite_world_state_valid(&pkt, 16)) {
        fprintf(stderr, "world: wrong type accepted\n");
        return 1;
    }

    /* Request timing: new map waits 1.5 s settled, same map every 30 s. */
    if (coop_lite_world_req_due(5, 6, 1000, 0, 2499)
        || !coop_lite_world_req_due(5, 6, 1000, 0, 2500)
        || !coop_lite_world_req_due(-1, 6, 0xFFFFFF00u, 0, 1300)
        || coop_lite_world_req_due(6, 6, 0, 1000, 30999)
        || !coop_lite_world_req_due(6, 6, 0, 1000, 31000)) {
        fprintf(stderr, "world: request timing wrong\n");
        return 1;
    }

    /* Seen set: dedupe, membership, overflow disables pruning. */
    coop_oid_set_reset(&set);
    memset(oid, 0, sizeof(oid));
    oid[0] = 7;
    coop_oid_set_add(&set, oid);
    coop_oid_set_add(&set, oid);
    if (set.count != 1 || !coop_oid_set_has(&set, oid)) {
        fprintf(stderr, "world: seen set dedupe\n");
        return 1;
    }
    oid[23] = 1;
    if (coop_oid_set_has(&set, oid)) {
        fprintf(stderr, "world: seen set false hit\n");
        return 1;
    }
    for (i = 0; i < COOP_OID_SET_MAX + 5; i++) {
        memcpy(oid, &i, sizeof(i));
        oid[20] = 0xAA;
        coop_oid_set_add(&set, oid);
    }
    if (!set.overflow || set.count != COOP_OID_SET_MAX) {
        fprintf(stderr, "world: seen set overflow\n");
        return 1;
    }
    coop_oid_set_reset(&set);
    if (set.overflow || set.count != 0) {
        fprintf(stderr, "world: seen set reset\n");
        return 1;
    }
    printf("world state: validator, timing, seen set OK\n");
    return 0;
}

/* Argon review: pickup queue (P0-2), item state (P1-3), blob walk (P1-4). */
static int test_review_fixes(void)
{
    CoopPickupQueue q;
    uint8_t oid[24];
    uint8_t out[COOP_PICKUP_QUEUE_MAX][24];
    unsigned int keys_a[3] = { 11, 22, 33 };
    unsigned int keys_b[3] = { 11, 33, 22 };
    uint8_t buf[256];
    uint8_t kinds[8];
    int elem[8];
    int at;
    int v;
    int i;

    /* P0-2: several pickups queue, dedupe, map-tagged, bounded. */
    coop_pickup_queue_reset(&q);
    for (i = 0; i < COOP_PICKUP_QUEUE_MAX; i++) {
        memset(oid, 0, sizeof(oid));
        oid[0] = (uint8_t)(i + 1);
        if (!coop_pickup_queue_push(&q, oid, i < 5 ? 0x10002 : 0x10003)) {
            fprintf(stderr, "pickup: push %d refused\n", i);
            return 1;
        }
    }
    memset(oid, 0, sizeof(oid));
    oid[0] = 1;
    if (!coop_pickup_queue_push(&q, oid, 0x10002) || q.count != COOP_PICKUP_QUEUE_MAX) {
        fprintf(stderr, "pickup: duplicate not merged\n");
        return 1;
    }
    oid[0] = 99;
    if (coop_pickup_queue_push(&q, oid, 0x10002)) {
        fprintf(stderr, "pickup: overflow accepted\n");
        return 1;
    }
    if (coop_pickup_queue_take(&q, 0x10002, out, COOP_PICKUP_QUEUE_MAX) != 5 || out[4][0] != 5
        || q.count != 0 || coop_pickup_queue_take(&q, 0x10002, out, COOP_PICKUP_QUEUE_MAX) != 0) {
        fprintf(stderr, "pickup: take by map key wrong\n");
        return 1;
    }

    /* P1-3: key ring hash is order sensitive, empty = 0, sizes fit. */
    if (coop_lite_keyring_hash(keys_a, 0) != 0
        || coop_lite_keyring_hash(keys_a, 3) == 0
        || coop_lite_keyring_hash(keys_a, 3) == coop_lite_keyring_hash(keys_b, 3)
        || coop_lite_keyring_hash(keys_a, 3) != coop_lite_keyring_hash(keys_a, 3)
        || coop_lite_keyring_hash(keys_a, 2) == coop_lite_keyring_hash(keys_a, 3)) {
        fprintf(stderr, "item state: keyring hash\n");
        return 1;
    }
    if (sizeof(PacketCoopInvenEntry) != 0x38
        || coop_lite_inven_sync_size(COOP_INVEN_CHUNK_ENTRIES) > 4096) {
        fprintf(stderr, "item state: entry/packet size\n");
        return 1;
    }

    /* P1-4: i32, i64 (present), string "ab", int array of 2 with bitset 0b11. */
    memset(buf, 0, sizeof(buf));
    at = 0;
    v = 7;
    memcpy(buf + at, &v, 4);
    at += 4;
    buf[at++] = 1;
    at += 8;
    buf[at++] = 1;
    v = 2;
    memcpy(buf + at, &v, 4);
    at += 4;
    memcpy(buf + at, "ab", 3);
    at += 3;
    buf[at++] = 1;
    v = 4;
    memcpy(buf + at, &v, 4);
    v = 2;
    memcpy(buf + at + 4, &v, 4);
    at += 12 + 8;
    v = 1;
    memcpy(buf + at, &v, 4);
    v = 3;
    memcpy(buf + at + 4, &v, 4);
    at += 8;
    kinds[0] = OBJ_MEM_K_I32;
    kinds[1] = OBJ_MEM_K_I64;
    kinds[2] = OBJ_MEM_K_STRING;
    kinds[3] = OBJ_MEM_K_ARRAY;
    memset(elem, 0, sizeof(elem));
    elem[3] = 4;
    if (coop_lite_blob_walk(buf, at, kinds, elem, 4) != at) {
        fprintf(stderr, "blob walk: valid blob refused (%d)\n", coop_lite_blob_walk(buf, at, kinds, elem, 4));
        return 1;
    }
    for (i = 0; i < at; i++) {
        if (coop_lite_blob_walk(buf, i, kinds, elem, 4) != -1) {
            fprintf(stderr, "blob walk: truncated at %d accepted\n", i);
            return 1;
        }
    }
    /* Huge string length. */
    v = 0x7FFFFFF0;
    memcpy(buf + 4 + 9 + 1, &v, 4);
    if (coop_lite_blob_walk(buf, at, kinds, elem, 4) != -1) {
        fprintf(stderr, "blob walk: huge string accepted\n");
        return 1;
    }
    v = 2;
    memcpy(buf + 4 + 9 + 1, &v, 4);
    /* Unterminated string. */
    buf[4 + 9 + 1 + 4 + 2] = 'x';
    if (coop_lite_blob_walk(buf, at, kinds, elem, 4) != -1) {
        fprintf(stderr, "blob walk: unterminated string accepted\n");
        return 1;
    }
    buf[4 + 9 + 1 + 4 + 2] = 0;
    /* Wrong element size, then count overflow, then bitset > count. */
    elem[3] = 8;
    if (coop_lite_blob_walk(buf, at, kinds, elem, 4) != -1) {
        fprintf(stderr, "blob walk: element size mismatch accepted\n");
        return 1;
    }
    elem[3] = 4;
    {
        int sa_at = 4 + 9 + 1 + 4 + 3 + 1;

        v = 0x40000000;
        memcpy(buf + sa_at + 4, &v, 4);
        if (coop_lite_blob_walk(buf, at, kinds, elem, 4) != -1) {
            fprintf(stderr, "blob walk: huge array accepted\n");
            return 1;
        }
        v = 2;
        memcpy(buf + sa_at + 4, &v, 4);
        /* 2 elements but the bitset names 3 */
        v = 7;
        memcpy(buf + sa_at + 12 + 8 + 4, &v, 4);
        if (coop_lite_blob_walk(buf, at, kinds, elem, 4) != -1) {
            fprintf(stderr, "blob walk: bitset beyond count accepted\n");
            return 1;
        }
        v = 3;
        memcpy(buf + sa_at + 12 + 8 + 4, &v, 4);
        v = 0x7FFFFFFF;
        memcpy(buf + sa_at + 12 + 8, &v, 4);
        if (coop_lite_blob_walk(buf, at, kinds, elem, 4) != -1) {
            fprintf(stderr, "blob walk: huge bitset accepted\n");
            return 1;
        }
    }
    kinds[0] = 77;
    if (coop_lite_blob_walk(buf, at, kinds, elem, 4) != -1) {
        fprintf(stderr, "blob walk: unknown kind accepted\n");
        return 1;
    }
    printf("review fixes: pickup queue, keyring hash, blob walk OK\n");
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
    if (test_char_bundle() != 0) {
        return 1;
    }
    if (test_pose_order() != 0) {
        return 1;
    }
    if (test_obj_blob() != 0) {
        return 1;
    }
    if (test_review_fixes() != 0) {
        return 1;
    }
    if (test_parked() != 0) {
        return 1;
    }
    if (test_char_rx() != 0) {
        return 1;
    }
    if (test_parked_save() != 0) {
        return 1;
    }
    if (!coop_lite_packet_allowed(PACKET_COOP_CHAR_BUNDLE, true, 1)
        || coop_lite_packet_allowed(PACKET_COOP_CHAR_BUNDLE, false, 0)
        || coop_lite_coop_packet_min_size(PACKET_COOP_CHAR_BUNDLE) != 16) {
        fprintf(stderr, "char bundle packet role/size\n");
        return 1;
    }
    if (test_interp() != 0) {
        return 1;
    }
    if (test_world_state() != 0) {
        return 1;
    }
    if (coop_lite_host_left_menu_due(false, 1, 99999)
        || coop_lite_host_left_menu_due(true, 1000, 4999)
        || !coop_lite_host_left_menu_due(true, 1000, 5000)
        || !coop_lite_host_left_menu_due(true, 0xFFFFF000u, 3000u)) {
        fprintf(stderr, "host-left menu timing wrong\n");
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
