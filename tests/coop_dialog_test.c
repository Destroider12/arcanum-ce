/*
 * PT36 dialog viewer + voting: packet codec, strict validation, vote
 * aggregation / rate limit, packet policy. No network, no UI.
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

static PacketCoopDialogState g_pkt;
static CoopDialogView g_view;
static CoopDialogVotes g_votes;

static void fill_open(PacketCoopDialogState* p, unsigned int seq, int n)
{
    const char* opts[5] = { "Hello.", "Who are you?", "Goodbye.", "Buy", "Quest" };
    unsigned char votes[5] = { 1, 0, 2, 0, 3 };

    coop_dialog_fill_state(p, seq, 7, "Virgil", "Welcome, stranger.", n, opts, votes);
}

static int test_codec(void)
{
    char big[3000];
    const char* opts[5];
    int i;

    fill_open(&g_pkt, 1, 3);
    CHECK(coop_dialog_packet_valid(&g_pkt, (int)sizeof(g_pkt), true), "open packet valid for a client");
    CHECK(coop_dialog_wire_size(&g_pkt) == (int)sizeof(g_pkt) && sizeof(g_pkt) <= 4096, "open size <= 4096");
    CHECK(g_pkt.votes[3] == 0 && g_pkt.votes[4] == 0 && g_pkt.votes[2] == 2, "votes beyond num_options dropped");
    coop_dialog_view_from_packet(&g_view, &g_pkt);
    CHECK(g_view.open && g_view.num_options == 3 && g_view.my_vote == -1 && g_view.line == 7, "view fields");
    CHECK(strcmp(g_view.npc_name, "Virgil") == 0 && strcmp(g_view.reply, "Welcome, stranger.") == 0, "view text");
    CHECK(strcmp(g_view.options[1], "Who are you?") == 0 && g_view.votes[0] == 1, "view options / votes");

    /* Over-long text is truncated by the sender, never overflows. */
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    for (i = 0; i < 5; i++) {
        opts[i] = big;
    }
    coop_dialog_fill_state(&g_pkt, 2, 1, big, big, 9, opts, NULL);
    CHECK(g_pkt.num_options == COOP_DIALOG_MAX_OPTIONS, "option count capped");
    CHECK(strlen(g_pkt.npc_name) == COOP_DIALOG_NAME_MAX - 1 && strlen(g_pkt.reply) == COOP_DIALOG_TEXT_MAX - 1
            && strlen(g_pkt.options[4]) == COOP_DIALOG_OPT_MAX - 1,
        "texts truncated to the caps");
    CHECK(coop_dialog_packet_valid(&g_pkt, (int)sizeof(g_pkt), true), "truncated packet valid");

    /* Close: header only. */
    coop_dialog_fill_close(&g_pkt, 3, 7);
    CHECK(coop_dialog_wire_size(&g_pkt) == COOP_DIALOG_HDR_SIZE, "close is header only");
    CHECK(coop_dialog_packet_valid(&g_pkt, COOP_DIALOG_HDR_SIZE, true), "close valid");
    CHECK(!coop_dialog_packet_valid(&g_pkt, COOP_DIALOG_HDR_SIZE + 1, true), "close with trailing bytes");
    coop_dialog_view_from_packet(&g_view, &g_pkt);
    CHECK(!g_view.open, "close view not open");

    /* Control characters are blanked for display. */
    {
        char s[8] = "a\nb\x01\x7f" "c";

        coop_dialog_sanitize(s);
        CHECK(strcmp(s, "a b  c") == 0, "sanitize");
    }
    return 0;
}

static int test_validation(void)
{
    PacketCoopDialogState p;
    int i;

    fill_open(&p, 5, 5);
    CHECK(coop_dialog_packet_valid(&p, (int)sizeof(p), true), "baseline");
    CHECK(!coop_dialog_packet_valid(NULL, (int)sizeof(p), true), "null");
    CHECK(!coop_dialog_packet_valid(&p, (int)sizeof(p) - 1, true), "short open");
    CHECK(!coop_dialog_packet_valid(&p, (int)sizeof(p) + 1, true), "oversize open");
    CHECK(!coop_dialog_packet_valid(&p, COOP_DIALOG_HDR_SIZE - 1, true), "below header");
    CHECK(!coop_dialog_packet_valid(&p, (int)sizeof(p), false), "state kind refused from a client");

    p.type = 1;
    CHECK(!coop_dialog_packet_valid(&p, (int)sizeof(p), true), "wrong type");
    fill_open(&p, 5, 5);
    p.kind = COOP_DIALOG_KIND_VOTE;
    CHECK(!coop_dialog_packet_valid(&p, (int)sizeof(p), true), "vote kind refused from the host");
    fill_open(&p, 5, 5);
    p.kind = 0;
    CHECK(!coop_dialog_packet_valid(&p, (int)sizeof(p), true), "unknown kind");
    fill_open(&p, 5, 5);
    p.action = 2;
    CHECK(!coop_dialog_packet_valid(&p, (int)sizeof(p), true), "unknown action");
    fill_open(&p, 5, 5);
    p.num_options = 6;
    CHECK(!coop_dialog_packet_valid(&p, (int)sizeof(p), true), "option count out of bounds");
    fill_open(&p, 0, 5);
    CHECK(!coop_dialog_packet_valid(&p, (int)sizeof(p), true), "seq 0");
    fill_open(&p, 5, 5);
    p.vote_index = 1;
    CHECK(!coop_dialog_packet_valid(&p, (int)sizeof(p), true), "stray vote index in state");
    fill_open(&p, 5, 2);
    p.votes[4] = 1;
    CHECK(!coop_dialog_packet_valid(&p, (int)sizeof(p), true), "vote beyond option count");
    fill_open(&p, 5, 5);
    p.votes[1] = COOP_DIALOG_MAX_VOTES + 1;
    CHECK(!coop_dialog_packet_valid(&p, (int)sizeof(p), true), "vote count over the player cap");

    /* NUL termination of every string. */
    fill_open(&p, 5, 5);
    memset(p.npc_name, 'a', sizeof(p.npc_name));
    CHECK(!coop_dialog_packet_valid(&p, (int)sizeof(p), true), "name not terminated");
    fill_open(&p, 5, 5);
    memset(p.reply, 'a', sizeof(p.reply));
    CHECK(!coop_dialog_packet_valid(&p, (int)sizeof(p), true), "reply not terminated");
    for (i = 0; i < COOP_DIALOG_MAX_OPTIONS; i++) {
        fill_open(&p, 5, 5);
        memset(p.options[i], 'a', sizeof(p.options[i]));
        CHECK(!coop_dialog_packet_valid(&p, (int)sizeof(p), true), "option not terminated");
    }
    /* A close packet with an open-sized body is not a close. */
    coop_dialog_fill_close(&p, 6, 5);
    CHECK(!coop_dialog_packet_valid(&p, (int)sizeof(p), true), "close with a body");

    /* Votes (client -> host). */
    coop_dialog_fill_vote(&p, 5, 2);
    CHECK(coop_dialog_packet_valid(&p, COOP_DIALOG_HDR_SIZE, false), "vote valid");
    CHECK(!coop_dialog_packet_valid(&p, COOP_DIALOG_HDR_SIZE, true), "vote kind refused as host state");
    CHECK(!coop_dialog_packet_valid(&p, (int)sizeof(p), false), "vote with a body");
    coop_dialog_fill_vote(&p, 5, -1);
    CHECK(coop_dialog_packet_valid(&p, COOP_DIALOG_HDR_SIZE, false), "vote -1 clears");
    coop_dialog_fill_vote(&p, 5, -2);
    CHECK(!coop_dialog_packet_valid(&p, COOP_DIALOG_HDR_SIZE, false), "vote below -1");
    coop_dialog_fill_vote(&p, 5, COOP_DIALOG_MAX_OPTIONS);
    CHECK(!coop_dialog_packet_valid(&p, COOP_DIALOG_HDR_SIZE, false), "vote index out of bounds");
    coop_dialog_fill_vote(&p, 5, 1);
    p.seq = 9;
    CHECK(!coop_dialog_packet_valid(&p, COOP_DIALOG_HDR_SIZE, false), "vote with a seq");
    coop_dialog_fill_vote(&p, 5, 1);
    p.votes[3] = 1;
    CHECK(!coop_dialog_packet_valid(&p, COOP_DIALOG_HDR_SIZE, false), "vote with counts");
    coop_dialog_fill_vote(&p, 5, 1);
    p.num_options = 3;
    CHECK(!coop_dialog_packet_valid(&p, COOP_DIALOG_HDR_SIZE, false), "vote with an option count");
    return 0;
}

static int test_votes(void)
{
    unsigned char t[COOP_DIALOG_MAX_OPTIONS];

    coop_dialog_votes_reset(&g_votes);
    CHECK(!coop_dialog_vote_apply(&g_votes, 1, 0, 1, 1000), "no dialog open: refused");
    coop_dialog_votes_new_line(&g_votes, 1, 3);
    CHECK(coop_dialog_vote_apply(&g_votes, 1, 2, 1, 1000), "first vote");
    CHECK(!coop_dialog_vote_apply(&g_votes, 1, 1, 1, 1100), "rate limited (100 ms)");
    CHECK(coop_dialog_vote_apply(&g_votes, 1, 1, 1, 1000 + COOP_DIALOG_VOTE_INTERVAL_MS), "after the interval");
    CHECK(!coop_dialog_vote_apply(&g_votes, 1, 1, 1, 5000), "same option again: no change");
    CHECK(coop_dialog_vote_apply(&g_votes, 2, 1, 1, 1100), "other slot has its own clock");
    CHECK(coop_dialog_vote_apply(&g_votes, 3, 0, 1, 1100), "third voter");
    CHECK(!coop_dialog_vote_apply(&g_votes, 4, 3, 1, 2000), "option >= num_options");
    CHECK(!coop_dialog_vote_apply(&g_votes, 4, -2, 1, 2000), "option < -1");
    CHECK(!coop_dialog_vote_apply(&g_votes, 0, 1, 1, 2000), "the host (slot 0) does not vote");
    CHECK(!coop_dialog_vote_apply(&g_votes, 8, 1, 1, 2000), "slot out of range");
    CHECK(!coop_dialog_vote_apply(&g_votes, 4, 1, 2, 2000), "stale line");
    coop_dialog_votes_tally(&g_votes, t);
    CHECK(t[0] == 1 && t[1] == 2 && t[2] == 0, "tally");

    CHECK(coop_dialog_vote_apply(&g_votes, 2, -1, 1, 2000), "clear a vote");
    coop_dialog_votes_tally(&g_votes, t);
    CHECK(t[1] == 1, "tally after clear");
    CHECK(coop_dialog_vote_clear_slot(&g_votes, 1) && !coop_dialog_vote_clear_slot(&g_votes, 1), "leaving slot");
    CHECK(!coop_dialog_vote_clear_slot(&g_votes, 0), "slot 0");

    /* New line: votes gone, the rate-limit clock stays. */
    coop_dialog_votes_new_line(&g_votes, 2, 2);
    coop_dialog_votes_tally(&g_votes, t);
    CHECK(t[0] == 0 && t[1] == 0, "votes cleared on a new line");
    CHECK(!coop_dialog_vote_apply(&g_votes, 2, 1, 2, 2100), "clock survives the new line");
    CHECK(coop_dialog_vote_apply(&g_votes, 2, 1, 2, 2000 + COOP_DIALOG_VOTE_INTERVAL_MS), "vote on the new line");
    CHECK(!coop_dialog_vote_apply(&g_votes, 2, 2, 2, 9000), "option 2 of a 2-option line");
    g_votes.open = false;
    CHECK(!coop_dialog_vote_apply(&g_votes, 2, 0, 2, 9000), "closed dialog: refused");
    return 0;
}

static int test_policy(void)
{
    CHECK(TIG_NET_PROTOCOL >= 27, "protocol bumped");
    CHECK(PACKET_COOP_DIALOG_STATE == 148 && PACKET_COOP_TYPE_LIMIT >= 149, "packet id");
    CHECK(sizeof(PacketCoopDialogState) <= 4096 && COOP_DIALOG_HDR_SIZE == 0x18, "sizes");
    CHECK(coop_lite_mp_packet_role(PACKET_COOP_DIALOG_STATE) == COOP_MP_PKT_BOTH, "role: both ways, kind-checked");
    CHECK(coop_lite_packet_allowed(PACKET_COOP_DIALOG_STATE, false, 0), "client accepts the host's");
    CHECK(!coop_lite_packet_allowed(PACKET_COOP_DIALOG_STATE, false, 2), "client ignores peers");
    CHECK(coop_lite_packet_allowed(PACKET_COOP_DIALOG_STATE, true, 1), "host accepts a client's (vote)");
    CHECK(coop_lite_coop_packet_min_size(PACKET_COOP_DIALOG_STATE) == (size_t)COOP_DIALOG_HDR_SIZE, "min size");
    CHECK(!coop_lite_packet_allowed(PACKET_COOP_TYPE_LIMIT, true, 0), "type limit is exclusive");
    return 0;
}

int main(void)
{
    if (test_codec() || test_validation() || test_votes() || test_policy()) {
        return 1;
    }
    printf("PASS: coop dialog viewer (codec, validation, votes, policy)\n");
    return 0;
}
