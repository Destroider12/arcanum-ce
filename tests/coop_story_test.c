/*
 * PT35 story sync: blob codec, merge, chunking / reassembly, packet policy.
 * No network: only the pure functions of coop_lite.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/coop_lite.h"
#include "net/tig_net.h"

int64_t player_get_local_pc_obj(void)
{
    return 42;
}

bool teleport_do(void* teleport_data)
{
    (void)teleport_data;
    return true;
}

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, msg); \
            return 1; \
        } \
    } while (0)

static CoopStory g_a, g_b, g_c;
static unsigned char g_blob[COOP_STORY_MAX_BLOB + 64];
static unsigned char g_blob2[COOP_STORY_MAX_BLOB + 64];
static CoopStoryRx g_rx;

static void fill_some(CoopStory* s)
{
    coop_story_clear(s);
    s->quest_state[1005] = 2; /* accepted */
    s->quest_ts[1005] = 123456789012ull;
    s->quest_state[1100] = 4; /* completed */
    s->quest_ts[1100] = 5;
    s->quest_state[1200] = 2 | 0x100; /* botched */
    s->quest_ts[1200] = 77;
    s->gquest[5] = 4;
    s->gquest[999] = 6;
    s->rumor_ts[0] = 999;
    s->rumor_ts[1999] = 1;
    s->rumor_quelled[0] = 1;
    s->rumor_quelled[7] = 1; /* quelled for everyone, unknown to the PC */
    s->n_rep = 2;
    s->rep[0] = 3;
    s->rep_ts[0] = 111;
    s->rep[1] = 9;
    s->rep_ts[1] = 222;
}

static void fill_max(CoopStory* s)
{
    int i;

    coop_story_clear(s);
    for (i = 0; i < COOP_STORY_QUESTS; i++) {
        s->quest_state[i] = (unsigned short)(1 + i % 6);
        s->quest_ts[i] = 1000ull + (unsigned)i * 7919ull;
    }
    for (i = 0; i < COOP_STORY_GQUESTS; i++) {
        s->gquest[i] = (unsigned char)(3 + i % 3);
    }
    for (i = 0; i < COOP_STORY_RUMORS; i++) {
        s->rumor_ts[i] = 5000ull + (unsigned)i;
        s->rumor_quelled[i] = (unsigned char)(i & 1);
    }
    s->n_rep = COOP_STORY_REPS;
    for (i = 0; i < COOP_STORY_REPS; i++) {
        s->rep[i] = (unsigned)i + 1;
        s->rep_ts[i] = 9000ull + (unsigned)i;
    }
}

static int test_roundtrip(void)
{
    size_t n;

    fill_some(&g_a);
    n = coop_story_encode(&g_a, g_blob, sizeof(g_blob));
    CHECK(n > 0 && n < 400, "small snapshot is small");
    CHECK(coop_story_decode(g_blob, n, &g_b), "decode");
    CHECK(memcmp(&g_a, &g_b, sizeof(g_a)) == 0, "round trip identical");
    CHECK(coop_story_encode(&g_a, g_blob2, n - 1) == 0, "cap too small -> 0");
    CHECK(coop_story_encode(&g_a, g_blob2, sizeof(g_blob2)) == n
            && memcmp(g_blob, g_blob2, n) == 0,
        "deterministic");
    coop_story_clear(&g_a);
    n = coop_story_encode(&g_a, g_blob, sizeof(g_blob));
    CHECK(n == 4 + 2 * 4, "empty story = header + 4 counts");
    CHECK(coop_story_decode(g_blob, n, &g_b) && memcmp(&g_a, &g_b, sizeof(g_a)) == 0, "empty round trip");
    CHECK(g_b.gquest[0] == COOP_STORY_GQUEST_DEFAULT, "global quests default to accepted");
    return 0;
}

static int test_decode_strict(void)
{
    size_t n;
    unsigned char bad[64];

    fill_some(&g_a);
    n = coop_story_encode(&g_a, g_blob, sizeof(g_blob));
    CHECK(!coop_story_decode(g_blob, n - 1, &g_b), "truncated");
    g_blob[n] = 0;
    CHECK(!coop_story_decode(g_blob, n + 1, &g_b), "trailing byte");
    memcpy(bad, g_blob, 8);
    CHECK(!coop_story_decode(NULL, 0, &g_b), "null");
    CHECK(!coop_story_decode(g_blob, 3, &g_b), "tiny");
    memcpy(g_blob2, g_blob, n);
    g_blob2[0] ^= 0xFF;
    CHECK(!coop_story_decode(g_blob2, n, &g_b), "magic");
    /* quest count above the array */
    memcpy(g_blob2, g_blob, n);
    g_blob2[4] = 0xFF;
    g_blob2[5] = 0xFF;
    CHECK(!coop_story_decode(g_blob2, n, &g_b), "quest count");
    /* quest index out of range: first record idx = 1005 -> 5000 */
    memcpy(g_blob2, g_blob, n);
    g_blob2[6] = 0x88;
    g_blob2[7] = 0x13;
    CHECK(!coop_story_decode(g_blob2, n, &g_b), "quest idx");
    /* quest state 7 */
    memcpy(g_blob2, g_blob, n);
    g_blob2[8] = 7;
    g_blob2[9] = 0;
    CHECK(!coop_story_decode(g_blob2, n, &g_b), "quest state");
    /* empty story, then a bad rep count */
    coop_story_clear(&g_a);
    n = coop_story_encode(&g_a, g_blob, sizeof(g_blob));
    g_blob[n - 2] = 0xFF;
    g_blob[n - 1] = 0x0F;
    CHECK(!coop_story_decode(g_blob, n, &g_b), "rep count");
    return 0;
}

static int test_merge(void)
{
    fill_some(&g_a);
    coop_story_clear(&g_b);
    g_b.quest_state[1005] = 4; /* further than accepted */
    g_b.quest_ts[1005] = 50;
    g_b.quest_state[1100] = 2; /* behind completed: ignored */
    g_b.quest_ts[1100] = 1;
    g_b.quest_state[1300] = 3; /* new */
    g_b.quest_ts[1300] = 8;
    g_b.quest_state[1200] = 2; /* same state, dst already botched */
    g_b.rumor_ts[0] = 500; /* earlier than 999 */
    g_b.rumor_ts[3] = 42;
    g_b.n_rep = 2;
    g_b.rep[0] = 9; /* already there */
    g_b.rep_ts[0] = 1;
    g_b.rep[1] = 4;
    g_b.rep_ts[1] = 2;
    g_b.gquest[5] = 2; /* globals are never merged */
    coop_story_merge(&g_a, &g_b);
    CHECK(g_a.quest_state[1005] == 4 && g_a.quest_ts[1005] == 50, "further state wins");
    CHECK(g_a.quest_state[1100] == 4 && g_a.quest_ts[1100] == 5, "behind state ignored");
    CHECK(g_a.quest_state[1300] == 3, "new quest added");
    CHECK(g_a.quest_state[1200] == (2 | 0x100), "botched kept");
    CHECK(g_a.rumor_ts[0] == 500 && g_a.rumor_ts[3] == 42 && g_a.rumor_ts[1999] == 1, "rumors unioned, earliest");
    CHECK(g_a.n_rep == 3 && g_a.rep[2] == 4 && g_a.rep_ts[1] == 222, "reputations unioned");
    CHECK(g_a.gquest[5] == 4 && g_a.rumor_quelled[7] == 1, "globals untouched");
    /* a botched src wins a tie */
    coop_story_clear(&g_b);
    coop_story_clear(&g_c);
    g_b.quest_state[1400] = 2;
    g_c.quest_state[1400] = 2 | 0x100;
    coop_story_merge(&g_b, &g_c);
    CHECK(g_b.quest_state[1400] == (2 | 0x100), "botched wins a tie");
    return 0;
}

static int test_chunks(void)
{
    size_t n;
    int count;
    int i;
    int order[COOP_STORY_MAX_CHUNKS];
    PacketCoopStorySync pkt;
    int r;

    fill_max(&g_a);
    n = coop_story_encode(&g_a, g_blob, sizeof(g_blob));
    CHECK(n > 0 && n <= COOP_STORY_MAX_BLOB, "worst case fits the blob cap");
    count = coop_story_chunk_count(n);
    CHECK(count > 1 && count <= COOP_STORY_MAX_CHUNKS, "worst case chunk count");
    CHECK(coop_story_chunk_count(0) == 0 && coop_story_chunk_count(COOP_STORY_MAX_BLOB + 1) == 0, "size limits");
    CHECK(coop_story_chunk_count(COOP_STORY_CHUNK) == 1 && coop_story_chunk_count(COOP_STORY_CHUNK + 1) == 2,
        "chunk boundary");
    CHECK(sizeof(PacketCoopStorySync) <= 4096, "packet within 4 KB");
    CHECK(coop_story_decode(g_blob, n, &g_b) && memcmp(&g_a, &g_b, sizeof(g_a)) == 0, "worst case round trip");

    /* reassemble in a scrambled order, with duplicates */
    for (i = 0; i < count; i++) {
        order[i] = (i * 7 + 3) % count;
    }
    {
        /* make it a permutation: 7 and count must be coprime, else fall back */
        int seen[COOP_STORY_MAX_CHUNKS] = { 0 };
        int ok = 1;

        for (i = 0; i < count; i++) {
            if (seen[order[i]]++) {
                ok = 0;
            }
        }
        if (!ok) {
            for (i = 0; i < count; i++) {
                order[i] = count - 1 - i;
            }
        }
    }
    coop_story_rx_reset(&g_rx);
    for (i = 0; i < count; i++) {
        coop_story_chunk_fill(&pkt, 5, g_blob, n, order[i]);
        CHECK(coop_story_chunk_valid(&pkt, (int)(offsetof(PacketCoopStorySync, data) + pkt.len)), "chunk valid");
        CHECK(!coop_story_chunk_valid(&pkt, (int)(offsetof(PacketCoopStorySync, data) + pkt.len) - 1), "short chunk");
        r = coop_story_rx_add(&g_rx, &pkt, (int)(offsetof(PacketCoopStorySync, data) + pkt.len));
        CHECK(r == (i == count - 1 ? 1 : 0), "complete only at the last chunk");
        if (i == 0) {
            r = coop_story_rx_add(&g_rx, &pkt, (int)(offsetof(PacketCoopStorySync, data) + pkt.len));
            CHECK(r == 0, "duplicate chunk is harmless");
        }
    }
    CHECK(g_rx.complete && g_rx.total == n && memcmp(g_rx.blob, g_blob, n) == 0, "reassembled blob");
    coop_story_chunk_fill(&pkt, 5, g_blob, n, 0);
    CHECK(coop_story_rx_add(&g_rx, &pkt, (int)sizeof(pkt)) == 0, "repeat of a complete snapshot ignored");

    /* a new snapshot id restarts */
    fill_some(&g_b);
    {
        size_t m = coop_story_encode(&g_b, g_blob2, sizeof(g_blob2));

        coop_story_chunk_fill(&pkt, 6, g_blob2, m, 0);
        CHECK(coop_story_rx_add(&g_rx, &pkt, (int)(offsetof(PacketCoopStorySync, data) + pkt.len)) == 1, "new snap restarts");
        CHECK(g_rx.total == m && memcmp(g_rx.blob, g_blob2, m) == 0, "new snapshot content");
    }

    /* corrupted payload: signature mismatch */
    coop_story_rx_reset(&g_rx);
    coop_story_chunk_fill(&pkt, 7, g_blob2, 100, 0);
    pkt.data[3] ^= 1;
    CHECK(coop_story_rx_add(&g_rx, &pkt, (int)(offsetof(PacketCoopStorySync, data) + pkt.len)) == -1, "bad signature");
    CHECK(!g_rx.complete, "bad snapshot not complete");

    /* inconsistent headers */
    coop_story_chunk_fill(&pkt, 8, g_blob, n, 1);
    pkt.index = (unsigned short)count;
    CHECK(!coop_story_chunk_valid(&pkt, (int)sizeof(pkt)), "index out of range");
    coop_story_chunk_fill(&pkt, 8, g_blob, n, 1);
    pkt.count++;
    CHECK(!coop_story_chunk_valid(&pkt, (int)sizeof(pkt)), "count mismatch");
    coop_story_chunk_fill(&pkt, 8, g_blob, n, 1);
    pkt.len--;
    CHECK(!coop_story_chunk_valid(&pkt, (int)sizeof(pkt)), "len mismatch");
    coop_story_chunk_fill(&pkt, 8, g_blob, n, 1);
    pkt.total = COOP_STORY_MAX_BLOB + 1;
    CHECK(!coop_story_chunk_valid(&pkt, (int)sizeof(pkt)), "total too big");
    coop_story_chunk_fill(&pkt, 8, g_blob, n, 1);
    pkt.type = 1;
    CHECK(!coop_story_chunk_valid(&pkt, (int)sizeof(pkt)), "wrong type");
    /* a second chunk of a different snapshot shape under the same id */
    coop_story_rx_reset(&g_rx);
    coop_story_chunk_fill(&pkt, 9, g_blob, n, 0);
    CHECK(coop_story_rx_add(&g_rx, &pkt, (int)sizeof(pkt)) == 0, "first chunk");
    coop_story_chunk_fill(&pkt, 9, g_blob, n - COOP_STORY_CHUNK, 1);
    CHECK(coop_story_rx_add(&g_rx, &pkt, (int)sizeof(pkt)) == -1, "same id, other shape");
    return 0;
}

static int test_policy(void)
{
    CHECK(TIG_NET_PROTOCOL == 26, "protocol bumped");
    CHECK(PACKET_COOP_STORY_SYNC == 147 && PACKET_COOP_TYPE_LIMIT == 148, "packet id");
    CHECK(coop_lite_mp_packet_role(PACKET_COOP_STORY_SYNC) == COOP_MP_PKT_CLIENT_APPLY, "role: host -> client");
    CHECK(coop_lite_packet_allowed(PACKET_COOP_STORY_SYNC, false, 0), "client accepts the host's");
    CHECK(!coop_lite_packet_allowed(PACKET_COOP_STORY_SYNC, false, 2), "client ignores peers");
    CHECK(!coop_lite_packet_allowed(PACKET_COOP_STORY_SYNC, true, 1), "host refuses a client's");
    CHECK(coop_lite_coop_packet_min_size(PACKET_COOP_STORY_SYNC) == offsetof(PacketCoopStorySync, data), "min size");
    CHECK(!coop_lite_story_write_blocked(), "offline: story writes are open");
    return 0;
}

int main(void)
{
    if (test_roundtrip() || test_decode_strict() || test_merge() || test_chunks() || test_policy()) {
        return 1;
    }
    printf("PASS: coop story sync (codec, merge, chunks, policy)\n");
    return 0;
}
