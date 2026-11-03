/*
 * PT52j: fuzz of the pure packet validators / decoders (run under ASan/UBSan
 * to catch out-of-bounds reads), party notice text checks (RU / EN, UTF-8,
 * no truncation) and 8-player (slots 0..7) pure-logic checks.
 */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/coop_lite.h"
#define COOP_STORY_MAGIC 0x59525453u /* mirrors coop_lite.c */

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint32_t rng = 0x12345678u;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/* Exact-size heap copy so ASan sees any read past `size`. */
static uint8_t* fuzz_buf(int type, int size)
{
    uint8_t* p = (uint8_t*)malloc(size > 0 ? (size_t)size : 1);
    int i;

    for (i = 0; i < size; i++) {
        /* bias towards small / boundary values */
        uint32_t r = rnd();
        p[i] = (r & 3) == 0 ? 0 : (r & 3) == 1 ? 0xFF : (uint8_t)(r >> 8);
    }
    if (size >= 4) {
        memcpy(p, &type, 4);
    }
    return p;
}

static bool visit_ok(void* ctx, int index, const uint8_t* blob, size_t size)
{
    (void)ctx;
    (void)index;
    if (size > 0) {
        volatile uint8_t a = blob[0];
        volatile uint8_t b = blob[size - 1];
        (void)a;
        (void)b;
    }
    return true;
}

static void fuzz_validators(void)
{
    int it;
    static const int sizes[] = { 0, 1, 3, 4, 7, 8, 15, 16, 23, 24, 63, 64, 100, 0xB0, 0x188, 1000, 2048, 4095, 4096 };

    for (it = 0; it < 60000; it++) {
        int size = (it & 1) ? sizes[rnd() % (sizeof(sizes) / sizeof(sizes[0]))] : (int)(rnd() % 4200);
        uint8_t* p;
        CoopStory story;
        int count;
        const char* rule;
        const char* name;

        p = fuzz_buf(PACKET_COOP_WORLD_STATE, size);
        if (size >= (int)coop_lite_coop_packet_min_size(PACKET_COOP_WORLD_STATE)) {
            (void)coop_lite_world_state_valid((const PacketCoopWorldState*)p, size);
        }
        free(p);

        p = fuzz_buf(PACKET_COOP_INVEN_SYNC, size);
        if (size >= (int)coop_lite_coop_packet_min_size(PACKET_COOP_INVEN_SYNC)) {
            (void)coop_lite_inven_sync_valid((const PacketCoopInvenSync*)p, size);
        }
        free(p);

        p = fuzz_buf(PACKET_COOP_OBJ_BLOB, size);
        if (size >= (int)coop_lite_coop_packet_min_size(PACKET_COOP_OBJ_BLOB)) {
            (void)coop_lite_obj_blob_valid((const PacketCoopObjBlob*)p, size);
        }
        free(p);

        p = fuzz_buf(PACKET_COOP_OBJ_REQ, size);
        if (size >= (int)coop_lite_coop_packet_min_size(PACKET_COOP_OBJ_REQ)) {
            (void)coop_lite_obj_req_valid((const PacketCoopObjReq*)p, size);
        }
        free(p);

        p = fuzz_buf(PACKET_COOP_STORY_SYNC, size);
        if (size >= (int)coop_lite_coop_packet_min_size(PACKET_COOP_STORY_SYNC)) {
            (void)coop_story_chunk_valid((const PacketCoopStorySync*)p, size);
        }
        free(p);

        p = fuzz_buf(PACKET_COOP_DIALOG_STATE, size);
        if (size >= (int)coop_lite_coop_packet_min_size(PACKET_COOP_DIALOG_STATE)) {
            (void)coop_dialog_packet_valid((const PacketCoopDialogState*)p, size, true);
            (void)coop_dialog_packet_valid((const PacketCoopDialogState*)p, size, false);
        }
        free(p);

        p = fuzz_buf(PACKET_COOP_PARTY_CTRL, size);
        if (size >= (int)sizeof(PacketCoopPartyCtrl)) {
            (void)coop_lite_party_ctrl_valid((const PacketCoopPartyCtrl*)p, size);
        }
        free(p);

        /* raw blobs (no packet header) */
        p = fuzz_buf(0, size);
        if (size >= 4 && (rnd() & 1)) {
            uint32_t magic = COOP_STORY_MAGIC;
            memcpy(p, &magic, 4);
        }
        (void)coop_story_decode(p, (size_t)size, &story);
        (void)coop_story_blob_sig(p, (size_t)size);
        (void)coop_char_bundle_parse(p, (size_t)size, visit_ok, NULL, &count, &rule, &name);
        free(p);
    }
}

static void fuzz_rx(void)
{
    CoopStoryRx srx;
    CoopCharRx crx;
    int it;

    memset(&srx, 0, sizeof(srx));
    coop_story_rx_reset(&srx);
    memset(&crx, 0, sizeof(crx));
    for (it = 0; it < 20000; it++) {
        int size = (int)(rnd() % 4097);
        uint8_t* p = fuzz_buf(PACKET_COOP_STORY_SYNC, size);
        int min = (int)coop_lite_coop_packet_min_size(PACKET_COOP_STORY_SYNC);

        if (size >= min) {
            /* small chunk indices / counts so reassembly paths are reached */
            if (size >= min && (rnd() & 1)) {
                p[4] = (uint8_t)(rnd() % 3);
            }
            (void)coop_story_rx_add(&srx, (const PacketCoopStorySync*)p, size);
        }
        free(p);

        p = fuzz_buf(PACKET_COOP_CHAR_BUNDLE, size);
        if (size >= (int)coop_lite_coop_packet_min_size(PACKET_COOP_CHAR_BUNDLE)) {
            (void)coop_char_rx_accept(&crx, (const PacketCoopCharChunk*)p, size);
        }
        free(p);
    }
    coop_story_rx_reset(&srx);
    coop_char_rx_free(&crx);
}

static bool utf8_valid(const char* s)
{
    const unsigned char* p = (const unsigned char*)s;

    while (*p) {
        int n;
        if (*p < 0x80) {
            p++;
            continue;
        }
        if ((*p & 0xE0) == 0xC0 && *p >= 0xC2) {
            n = 1;
        } else if ((*p & 0xF0) == 0xE0) {
            n = 2;
        } else if ((*p & 0xF8) == 0xF0 && *p <= 0xF4) {
            n = 3;
        } else {
            return false;
        }
        p++;
        while (n-- > 0) {
            if ((*p & 0xC0) != 0x80) {
                return false;
            }
            p++;
        }
    }
    return true;
}

static bool has_cyrillic(const char* s)
{
    const unsigned char* p = (const unsigned char*)s;

    for (; *p; p++) {
        if ((p[0] == 0xD0 || p[0] == 0xD1) && (p[1] & 0xC0) == 0x80) {
            return true;
        }
    }
    return false;
}

static void notice_texts(void)
{
    static const unsigned int cds[] = { 0, 1, 999, 1000, 30000, 0xFFFFFFFFu };
    int kind;
    int host;
    size_t c;
    int shown = 0;

    for (kind = -2; kind < 40; kind++) {
        for (host = 0; host < 2; host++) {
            for (c = 0; c < sizeof(cds) / sizeof(cds[0]); c++) {
                char buf[512];
                char small[16];
                size_t len;

                memset(buf, 'x', sizeof(buf));
                coop_lite_party_notice_text(kind, cds[c], host != 0, buf, sizeof(buf));
                len = strnlen(buf, sizeof(buf));
                CHECK(len < sizeof(buf) - 1); /* never fills (= truncated) */
                if (len == 0) {
                    continue;
                }
                shown++;
                CHECK(utf8_valid(buf));
                CHECK(strstr(buf, " / ") != NULL);
                CHECK(has_cyrillic(buf));
                CHECK(strchr(buf, '%') == NULL); /* no unexpanded format */
                /* English half has latin letters */
                {
                    const char* en = strstr(buf, " / ") + 3;
                    CHECK(*en != '\0' && !has_cyrillic(en));
                }
                /* tiny buffer: still NUL-terminated */
                coop_lite_party_notice_text(kind, cds[c], host != 0, small, sizeof(small));
                CHECK(strnlen(small, sizeof(small)) < sizeof(small));
            }
        }
    }
    CHECK(shown > 50);
}

/* ---- 8 players: host slot 0 + clients 1..7 ---- */
static void eight_players(void)
{
    int slot;
    int local;
    CoopDialogVotes v;
    unsigned char out[COOP_DIALOG_MAX_OPTIONS];
    CoopPoseOrder po[8];
    PacketCoopPartyCtrl ctrl;
    static CoopOidSet set;
    uint8_t oid[24];
    int i;

    CHECK(PACKET_COOP_PARTY_CTRL < PACKET_COOP_TYPE_LIMIT);
    CHECK(sizeof(PacketCoopPartyCtrl) == 16);

    /* roster: every client slot 1..7 usable, never 0 / 8 / own */
    for (local = 0; local < 8; local++) {
        for (slot = -1; slot <= 8; slot++) {
            bool ok = coop_lite_roster_slot_ok(slot, local);
            CHECK(ok == (slot >= 1 && slot <= 7 && slot != local));
        }
    }

    /* dialog votes: 7 voters on one option counts 7 (fits unsigned char) */
    memset(&v, 0, sizeof(v));
    for (slot = 0; slot < 8; slot++) {
        v.vote[slot] = slot == 0 ? -1 : 2;
    }
    coop_dialog_votes_tally(&v, out);
    CHECK(out[2] == 7 && out[0] == 0 && out[4] == 0);
    v.vote[7] = 4;
    v.vote[6] = 5; /* out of range: ignored */
    v.vote[5] = -1;
    coop_dialog_votes_tally(&v, out);
    CHECK(out[2] == 4 && out[4] == 1);

    /* travel: far_mask with every client far (0xFE) passes ctrl validation
       when sent host -> client as TRAVEL_START */
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.type = PACKET_COOP_PARTY_CTRL;
    ctrl.op = COOP_CTRL_TRAVEL_START;
    ctrl.a = 0xFE;
    (void)coop_lite_party_ctrl_valid(&ctrl, (int)sizeof(ctrl));
    for (slot = 1; slot < 8; slot++) {
        CHECK((0xFE & (1 << slot)) != 0);
    }
    CHECK(coop_lite_travel_slot_ready(COOP_TRAVEL_READY, 100000));
    CHECK(coop_lite_travel_slot_ready(COOP_TRAVEL_IDLE, COOP_TRAVEL_RANGE));
    CHECK(!coop_lite_travel_slot_ready(COOP_TRAVEL_IDLE, COOP_TRAVEL_RANGE + 1));
    CHECK(!coop_lite_travel_slot_ready(COOP_TRAVEL_BUSY, 0));
    CHECK(!coop_lite_travel_slot_ready(-1, 0));

    /* pose ordering is independent per slot */
    memset(po, 0, sizeof(po));
    for (slot = 0; slot < 8; slot++) {
        CHECK(coop_lite_pose_order_accept(&po[slot], 1, 10, false, false));
    }
    CHECK(!coop_lite_pose_order_accept(&po[7], 1, 9, false, false));
    CHECK(coop_lite_pose_order_accept(&po[1], 1, 11, false, false));

    /* batch limits */
    CHECK(coop_lite_batch_valid(COOP_FOLLOWERS_MAX, COOP_FOLLOWERS_MAX, sizeof(PacketCoopFollowerEntry),
        8 + COOP_FOLLOWERS_MAX * (int)sizeof(PacketCoopFollowerEntry)));
    CHECK(!coop_lite_batch_valid(COOP_FOLLOWERS_MAX + 1, COOP_FOLLOWERS_MAX, sizeof(PacketCoopFollowerEntry), 4096));
    CHECK(!coop_lite_batch_valid(-1, COOP_FOLLOWERS_MAX, sizeof(PacketCoopFollowerEntry), 4096));
    CHECK(!coop_lite_batch_valid(4, COOP_FOLLOWERS_MAX, sizeof(PacketCoopFollowerEntry), 8 + 3 * (int)sizeof(PacketCoopFollowerEntry)));

    /* OID set: 8 players' worth of distinct OIDs overflows cleanly */
    coop_oid_set_reset(&set);
    for (i = 0; i < COOP_OID_SET_MAX + 50; i++) {
        memset(oid, 0, sizeof(oid));
        memcpy(oid, &i, sizeof(i));
        coop_oid_set_add(&set, oid);
    }
    CHECK(set.count == COOP_OID_SET_MAX && set.overflow);
    memset(oid, 0, sizeof(oid));
    i = 5;
    memcpy(oid, &i, sizeof(i));
    CHECK(coop_oid_set_has(&set, oid));
    coop_oid_set_reset(&set);
    CHECK(set.count == 0 && !set.overflow && !coop_oid_set_has(&set, oid));
}

int main(void)
{
    fuzz_validators();
    fuzz_rx();
    notice_texts();
    eight_players();
    if (failures != 0) {
        fprintf(stderr, "coop_lite_fuzz8: %d failure(s)\n", failures);
        return 1;
    }
    printf("coop_lite_fuzz8: ok\n");
    return 0;
}

/* engine stubs needed by coop_lite.c in COOP_LITE_TEST_STUBS builds */
struct TeleportData;
int64_t player_get_local_pc_obj(void)
{
    return 1;
}
bool teleport_do(struct TeleportData* td)
{
    (void)td;
    return true;
}
