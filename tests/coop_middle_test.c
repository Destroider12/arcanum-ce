/*
 * Coop Middle M1 (protocol 53): session rules contract, PACKET_COOP_RULES,
 * gold split, loot feed filter / limiter, REST_ASK / REST_REFUSE ops.
 * Coop Middle M2 (protocol 54): trade partner / transfer rules and lines,
 * SLOT_STATUS pack / get / badge priority, Alt label deoverlap.
 * Coop Middle M3 (protocol 55): pause / ff / hp / xp rules, damage and XP
 * scaling, /tp /stuck /gather parse + lines, PACKET_COOP_PING, PAUSE op.
 * Headless (no engine); also run under ASan/UBSan (nbuild-asan).
 */
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/coop_lite.h"
#include "game/coop_middle.h"
#include "game/coop_rules.h"
#include "game/coop_subinst.h"

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

int64_t player_get_local_pc_obj(void)
{
    return 42;
}

bool teleport_do(TeleportData* teleport_data)
{
    (void)teleport_data;
    return true;
}

static int g_fail;

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            g_fail++;                                                    \
        }                                                                \
    } while (0)

/* 1. defaults + sanitize */
static void test_rules_default_sanitize(void)
{
    CoopRules r;
    CoopRules d;

    coop_rules_default(&r);
    CHECK(r.version == 1u);
    CHECK(r.flags == (COOP_RULE_LOOT_FEED | COOP_RULE_REST_VOTE | COOP_RULE_DIRECT_TRADE | COOP_RULE_NO_FRIENDLY_FIRE_AOE
        | COOP_RULE_SHARED_QUEST_ITEMS | COOP_RULE_SHARED_FOLLOWERS | COOP_RULE_FREE_OVERWORLD | COOP_RULE_UNIFIED_SLEEP
        | COOP_RULE_SUBINSTANCES | COOP_RULE_FOLLOWER_LEVEL | COOP_RULE_PARTY_LEVEL));
    CHECK((r.flags & COOP_RULE_GOLD_SPLIT) == 0);
    CHECK(r.enemy_hp_pct == 100 && r.xp_mult_pct == 100);
    CHECK(r.reserved[0] == 0 && r.reserved[1] == 0 && r.reserved[2] == 0 && r.reserved16 == 0);
    CHECK(r.autosave_min == COOP_RULES_ASAVE_DEFAULT);

    d = r;
    coop_rules_sanitize(&d);
    CHECK(coop_rules_equal(&r, &d)); /* defaults are already sane */

    /* bounds */
    r.enemy_hp_pct = 0;
    r.xp_mult_pct = 0;
    coop_rules_sanitize(&r);
    CHECK(r.enemy_hp_pct == 100 && r.xp_mult_pct == 100);
    r.enemy_hp_pct = 65535;
    r.xp_mult_pct = 999;
    coop_rules_sanitize(&r);
    CHECK(r.enemy_hp_pct == 300 && r.xp_mult_pct == 250);
    r.enemy_hp_pct = 150;
    r.xp_mult_pct = 175;
    coop_rules_sanitize(&r);
    CHECK(r.enemy_hp_pct == 150 && r.xp_mult_pct == 175);

    /* unknown bits dropped, reserved M2-M3 bits kept, reserved[] zeroed */
    r.flags = 0xFFFFFFFFu;
    r.reserved[0] = 7;
    r.reserved[2] = 0xDEADBEEFu;
    r.reserved16 = 0x55;
    r.version = 99;
    coop_rules_sanitize(&r);
    CHECK(r.flags == COOP_RULE_KNOWN_MASK);
    CHECK((r.flags & COOP_RULE_DIRECT_TRADE) && (r.flags & COOP_RULE_CLIENT_PAUSE)
        && (r.flags & COOP_RULE_NO_FRIENDLY_FIRE_AOE));
    CHECK(r.reserved[0] == 0 && r.reserved[2] == 0 && r.reserved16 == 0);
    CHECK(r.version == COOP_RULES_VERSION);

    coop_rules_default(NULL); /* no crash */
    coop_rules_sanitize(NULL);
    CHECK(sizeof(CoopRules) == 28);
}

/* 2. format <-> parse round trip, garbage, partial strings */
static void test_rules_env(void)
{
    CoopRules r;
    CoopRules back;
    char text[128];
    char small[8];
    char desc[1024];
    unsigned int mask;
    int n;

    coop_rules_default(&r);
    coop_rules_format_env(&r, text, sizeof(text));
    CHECK(strcmp(text, "gold=0,loot=1,rest=1,trade=1,pause=0,ff=1,qitems=1,sfol=1,ow=1,usleep=1,subinst=1,flvl=1,plvl=1,hp=100,xp=100,asave=10") == 0);

    /* every flag combination + odd percentages round-trip */
    for (mask = 0; mask <= COOP_RULE_KNOWN_MASK; mask++) {
        coop_rules_default(&r);
        r.flags = mask;
        r.enemy_hp_pct = (uint16_t)(100 + (mask * 7) % 201);
        r.xp_mult_pct = (uint16_t)(100 + (mask * 13) % 151);
        coop_rules_format_env(&r, text, sizeof(text));
        n = coop_rules_parse_env(&back, text);
        CHECK(n >= 6);
        CHECK(coop_rules_equal(&r, &back));
    }

    /* the launcher's own form, with ini aliases and spaces */
    n = coop_rules_parse_env(&r, " gold_split = 1 ; loot_feed=0, rest_vote=on ");
    CHECK(n == 3);
    CHECK(r.flags == (COOP_RULE_GOLD_SPLIT | COOP_RULE_REST_VOTE | COOP_RULE_DIRECT_TRADE | COOP_RULE_NO_FRIENDLY_FIRE_AOE
        | COOP_RULE_SHARED_QUEST_ITEMS | COOP_RULE_SHARED_FOLLOWERS | COOP_RULE_FREE_OVERWORLD | COOP_RULE_UNIFIED_SLEEP
        | COOP_RULE_SUBINSTANCES | COOP_RULE_FOLLOWER_LEVEL | COOP_RULE_PARTY_LEVEL));

    /* partial: missing keys keep defaults */
    n = coop_rules_parse_env(&r, "gold=1");
    CHECK(n == 1 && r.flags == (COOP_RULE_GOLD_SPLIT | COOP_RULE_LOOT_FEED | COOP_RULE_REST_VOTE | COOP_RULE_DIRECT_TRADE
        | COOP_RULE_NO_FRIENDLY_FIRE_AOE | COOP_RULE_SHARED_QUEST_ITEMS | COOP_RULE_SHARED_FOLLOWERS
        | COOP_RULE_FREE_OVERWORLD | COOP_RULE_UNIFIED_SLEEP
        | COOP_RULE_SUBINSTANCES | COOP_RULE_FOLLOWER_LEVEL | COOP_RULE_PARTY_LEVEL));
    CHECK(r.enemy_hp_pct == 100 && r.xp_mult_pct == 100);

    /* garbage: ignored, result is the defaults */
    coop_rules_default(&back);
    n = coop_rules_parse_env(&r, "%%%,=,gold=,=1,hp=abc,xp=12x,loot,,;;rest=maybe,unknown=1");
    CHECK(n == 0); /* unknown keys are not counted */
    n = coop_rules_parse_env(&r, "%%%,=,gold=,=1,hp=abc,xp=12x,loot,,;;rest=maybe");
    CHECK(n == 0);
    CHECK(coop_rules_equal(&r, &back));
    n = coop_rules_parse_env(&r, NULL);
    CHECK(n == 0 && coop_rules_equal(&r, &back));
    n = coop_rules_parse_env(&r, "");
    CHECK(n == 0 && coop_rules_equal(&r, &back));

    /* out of range values are clamped */
    coop_rules_parse_env(&r, "hp=5000,xp=-3");
    CHECK(r.enemy_hp_pct == 300 && r.xp_mult_pct == 100);
    coop_rules_parse_env(&r, "hp=99999999999999999999,xp=250");
    CHECK(r.enemy_hp_pct == 300 && r.xp_mult_pct == 250);

    /* very long key / value does not overflow */
    {
        char longs[600];

        memset(longs, 'a', sizeof(longs) - 1);
        longs[sizeof(longs) - 1] = '\0';
        longs[300] = '=';
        coop_rules_parse_env(&r, longs);
        CHECK(coop_rules_equal(&r, &back));
    }

    /* truncated output stays terminated */
    coop_rules_format_env(&back, small, sizeof(small));
    CHECK(strlen(small) < sizeof(small));
    CHECK(coop_rules_format_env(&back, small, 0) == 0);

    coop_rules_default(&r);
    r.flags |= COOP_RULE_GOLD_SPLIT | COOP_RULE_DIRECT_TRADE;
    coop_rules_describe(&r, false, desc, sizeof(desc));
    CHECK(strstr(desc, "gold split on") != NULL && strstr(desc, "trade") != NULL);
    coop_rules_describe(&r, true, desc, sizeof(desc));
    CHECK(strstr(desc, "Правила") != NULL);
    coop_rules_describe(&r, false, small, sizeof(small));
    CHECK(strlen(small) < sizeof(small));
}

/* 3. PACKET_COOP_RULES: size, direction, protocol */
static void test_rules_packet(void)
{
    PacketCoopRules pkt;
    CoopRules r;

    CHECK(TIG_NET_PROTOCOL >= 59);
    CHECK(PACKET_COOP_RULES == 167 && PACKET_COOP_RULES < PACKET_COOP_TYPE_LIMIT);
    CHECK(sizeof(PacketCoopRules) == 32);
    CHECK(coop_lite_coop_packet_min_size(PACKET_COOP_RULES) == sizeof(PacketCoopRules));
    CHECK(coop_lite_mp_packet_role(PACKET_COOP_RULES) == COOP_MP_PKT_CLIENT_APPLY);
    /* host -> client only */
    CHECK(coop_lite_packet_allowed(PACKET_COOP_RULES, false, 0));
    CHECK(!coop_lite_packet_allowed(PACKET_COOP_RULES, true, 1));
    CHECK(!coop_lite_packet_allowed(PACKET_COOP_RULES, true, 7));
    CHECK(!coop_lite_packet_allowed(PACKET_COOP_RULES, false, 3)); /* not from another client */

    coop_rules_default(&r);
    r.flags = 0xFFFFFFFFu;
    r.enemy_hp_pct = 1;
    coop_rules_packet_fill(&pkt, &r);
    CHECK(pkt.type == PACKET_COOP_RULES);
    CHECK(pkt.rules.flags == COOP_RULE_KNOWN_MASK && pkt.rules.enemy_hp_pct == 100); /* sanitized on send */
    CHECK(coop_rules_packet_valid(&pkt, (int)sizeof(pkt)));
    CHECK(!coop_rules_packet_valid(&pkt, (int)sizeof(pkt) - 1));
    CHECK(!coop_rules_packet_valid(&pkt, (int)sizeof(pkt) + 4));
    CHECK(!coop_rules_packet_valid(NULL, (int)sizeof(pkt)));
    pkt.rules.version = 0;
    CHECK(!coop_rules_packet_valid(&pkt, (int)sizeof(pkt)));
    pkt.rules.version = 2; /* a newer Middle: accepted, the receiver sanitizes */
    CHECK(coop_rules_packet_valid(&pkt, (int)sizeof(pkt)));
    pkt.type = PACKET_COOP_CHAT;
    CHECK(!coop_rules_packet_valid(&pkt, (int)sizeof(pkt)));
    coop_rules_packet_fill(&pkt, NULL);
    CHECK(pkt.rules.flags == COOP_RULE_DEFAULT_FLAGS);
}

/* 4. gold split */
static void test_split_gold(void)
{
    int n;
    int amount;
    int carry;
    int each;
    int rem;

    for (n = 1; n <= 8; n++) {
        for (amount = 0; amount <= 1000; amount += 7) {
            carry = 0;
            rem = coop_middle_split_gold(amount, n, &carry, &each);
            CHECK(each * n + rem == amount); /* nothing lost */
            CHECK(rem >= 0 && rem < n);
            CHECK(carry == 0);
        }
    }
    rem = coop_middle_split_gold(100, 3, NULL, &each);
    CHECK(each == 33 && rem == 1);
    rem = coop_middle_split_gold(5, 8, NULL, &each);
    CHECK(each == 0 && rem == 5); /* too little: the picker keeps it */
    rem = coop_middle_split_gold(10, 0, NULL, &each);
    CHECK(each == 10 && rem == 0); /* <1 player counts as 1 */
    rem = coop_middle_split_gold(-50, 4, NULL, &each);
    CHECK(each == 0 && rem == 0);
    carry = 3;
    rem = coop_middle_split_gold(10, 4, &carry, &each);
    CHECK(each == 3 && rem == 1 && carry == 0);
    /* overflow saturates */
    carry = INT_MAX;
    rem = coop_middle_split_gold(INT_MAX, 2, &carry, &each);
    CHECK((long long)each * 2 + rem == INT_MAX && carry == 0);
    rem = coop_middle_split_gold(INT_MAX, 8, NULL, &each);
    CHECK((long long)each * 8 + rem == INT_MAX);
    rem = coop_middle_split_gold(100, 99, NULL, &each);
    CHECK(each == 12 && rem == 4); /* clamped to 8 players */
}

/* 5. loot feed filter + limiter + text */
static void test_loot_feed(void)
{
    CoopFeedLimiter l;
    char line[256];
    int i;
    int admitted;

    CHECK(coop_middle_loot_feed_worthy(COOP_MID_T_KEY, 0, 1, false));
    CHECK(coop_middle_loot_feed_worthy(COOP_MID_T_KEY_RING, 0, 0, false));
    CHECK(coop_middle_loot_feed_worthy(COOP_MID_T_SCROLL, 0, 5, false));
    CHECK(coop_middle_loot_feed_worthy(COOP_MID_T_WRITTEN, 0, 0, false));
    CHECK(coop_middle_loot_feed_worthy(COOP_MID_T_GENERIC, 0, 3, true)); /* quest */
    CHECK(coop_middle_loot_feed_worthy(COOP_MID_T_GENERIC, COOP_MID_OIF_UBER, 3, false)); /* unique */
    CHECK(coop_middle_loot_feed_worthy(COOP_MID_T_WEAPON, 0, 250, false));
    CHECK(coop_middle_loot_feed_worthy(COOP_MID_T_ARMOR, 0, 4000, false));
    CHECK(!coop_middle_loot_feed_worthy(COOP_MID_T_WEAPON, 0, 249, false));
    CHECK(!coop_middle_loot_feed_worthy(COOP_MID_T_ARMOR, 1, 10, false));
    CHECK(!coop_middle_loot_feed_worthy(COOP_MID_T_FOOD, 0, 30, false)); /* cheap potion */
    CHECK(!coop_middle_loot_feed_worthy(COOP_MID_T_GENERIC, 0, 1, false)); /* junk */
    CHECK(!coop_middle_loot_feed_worthy(COOP_MID_T_AMMO, 0, 100000, true)); /* ammo never */
    CHECK(!coop_middle_loot_feed_worthy(COOP_MID_T_GOLD, 0, 100000, false)); /* gold: own line */
    CHECK(!coop_middle_loot_feed_worthy(COOP_MID_T_NPC, 0, 100000, true)); /* not an item */
    CHECK(!coop_middle_loot_feed_worthy(-1, 0, 100000, true));

    /* <= 5 lines per second, the rest collapses into one summary */
    coop_middle_feed_reset(&l);
    admitted = 0;
    for (i = 0; i < 40; i++) {
        if (coop_middle_feed_admit(&l, 1000u + (unsigned int)i)) {
            admitted++;
        }
    }
    CHECK(admitted == COOP_LOOT_FEED_PER_SEC);
    CHECK(coop_middle_feed_take_suppressed(&l, 1500u) == 0); /* window still open */
    CHECK(coop_middle_feed_take_suppressed(&l, 2001u) == 35);
    CHECK(coop_middle_feed_take_suppressed(&l, 2002u) == 0);
    admitted = 0;
    for (i = 0; i < 10; i++) {
        if (coop_middle_feed_admit(&l, 3100u + (unsigned int)i)) {
            admitted++;
        }
    }
    CHECK(admitted == COOP_LOOT_FEED_PER_SEC);
    /* wrap-around of the ms clock */
    coop_middle_feed_reset(&l);
    CHECK(coop_middle_feed_admit(&l, 0xFFFFFF00u));
    CHECK(coop_middle_feed_admit(&l, 0x00000400u)); /* > 1 s later across the wrap */

    coop_middle_feed_format(COOP_MID_CHAT_LOOT, "Virgil", "Rusty Key", false, line, sizeof(line));
    CHECK(strcmp(line, "[Loot] Virgil picked up: Rusty Key") == 0);
    coop_middle_feed_format(COOP_MID_CHAT_LOOT, "Вергилий", "Ключ", true, line, sizeof(line));
    CHECK(strcmp(line, "[Лут] Вергилий подобрал: Ключ") == 0);
    coop_middle_feed_format(COOP_MID_CHAT_GOLD, "Ann", "100 33", false, line, sizeof(line));
    CHECK(strcmp(line, "[Loot] Ann found 100 gold: split, 33 each") == 0);
    coop_middle_feed_format(COOP_MID_CHAT_MORE, "", "35", false, line, sizeof(line));
    CHECK(strcmp(line, "[Loot] ... and 35 more") == 0);
    coop_middle_feed_format(99, "x", "y", false, line, sizeof(line));
    CHECK(line[0] == '\0');
    coop_middle_feed_format(COOP_MID_CHAT_LOOT, NULL, NULL, false, line, 8);
    CHECK(strlen(line) < 8);
}

/* 6. REST_ASK / REST_REFUSE in coop_lite_party_ctrl_valid */
static void test_rest_ops(void)
{
    PacketCoopPartyCtrl c;
    int r;

    CHECK(COOP_CTRL_REST_ASK == 5 && COOP_CTRL_REST_REFUSE == 6);
    memset(&c, 0, sizeof(c));
    c.type = PACKET_COOP_PARTY_CTRL;
    c.op = COOP_CTRL_REST_ASK;
    c.a = COOP_REST_MODE_NIGHT;
    CHECK(coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
    c.a = COOP_REST_MODE_HEALED;
    CHECK(coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
    CHECK(!coop_lite_party_ctrl_valid(&c, (int)sizeof(c) - 4));
    c.a = 0;
    CHECK(!coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
    c.a = 3;
    CHECK(!coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
    c.a = COOP_REST_MODE_HEALED;
    c.b = 1;
    CHECK(!coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
    c.b = 0;

    c.op = COOP_CTRL_REST_REFUSE;
    for (r = 0; r <= COOP_REST_REFUSE_MAX; r++) {
        c.a = r;
        CHECK(coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
        CHECK(coop_middle_rest_refuse_text(r, false)[0] != '\0');
        CHECK(coop_middle_rest_refuse_text(r, true)[0] != '\0');
    }
    c.a = -1;
    CHECK(!coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
    c.a = COOP_REST_REFUSE_MAX + 1;
    CHECK(!coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));

    /* Lite ops unchanged, unknown ops still refused */
    c.op = COOP_CTRL_TRAVEL_STATE;
    c.a = COOP_TRAVEL_READY;
    CHECK(coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
    c.op = 12; /* 9 / 10 are the M5 sleep ops, 11 the M7 SLEEP_APPLY */
    c.a = 1;
    CHECK(!coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
    c.op = 0;
    CHECK(!coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
    c.type = PACKET_COOP_CHAT;
    c.op = COOP_CTRL_REST_ASK;
    CHECK(!coop_middle_party_ctrl_valid(&c));
    CHECK(!coop_middle_party_ctrl_valid(NULL));

    /* client ask rate limit: 1 per 5 s */
    CHECK(coop_middle_rest_ask_allowed(false, 0, 0));
    CHECK(!coop_middle_rest_ask_allowed(true, 1000, 5999));
    CHECK(coop_middle_rest_ask_allowed(true, 1000, 6000));
    CHECK(coop_middle_rest_ask_allowed(true, 0xFFFFF000u, 0x00001000u)); /* wrap: 8 s */

    /* stubbed game hooks do nothing (test build) */
    CHECK(!coop_middle_handle_party_ctrl(&c));
    CHECK(coop_middle_rules()->flags == COOP_RULE_DEFAULT_FLAGS);
}

/* 7. M2 direct trade: partner rule, transfer rule, trade lines */
static void test_trade(void)
{
    char line[256];
    char tiny[12];

    CHECK(COOP_BARTER_OPEN_TRADE > COOP_BARTER_OPEN_STEAL);
    CHECK(COOP_TARGET_TRADE != COOP_TARGET_NONE && COOP_TARGET_TRADE != COOP_TARGET_LOOT
        && COOP_TARGET_TRADE != COOP_TARGET_STEAL);

    /* partner: rule, PC of another active slot, both alive, <= 6 tiles */
    CHECK(coop_middle_trade_partner_ok(true, 0, 1, true, true, false, false, 0));
    CHECK(coop_middle_trade_partner_ok(true, 2, 5, true, true, false, false, COOP_TRADE_RANGE));
    CHECK(coop_middle_trade_partner_ok(true, 3, 0, true, true, false, false, 3)); /* client -> host */
    CHECK(!coop_middle_trade_partner_ok(true, 2, 5, true, true, false, false, COOP_TRADE_RANGE + 1));
    CHECK(!coop_middle_trade_partner_ok(true, 2, 5, true, true, false, false, -1)); /* other map */
    CHECK(!coop_middle_trade_partner_ok(false, 0, 1, true, true, false, false, 1)); /* rule off */
    CHECK(!coop_middle_trade_partner_ok(true, 1, 1, true, true, false, false, 1)); /* self */
    CHECK(!coop_middle_trade_partner_ok(true, 0, 1, false, true, false, false, 1)); /* NPC */
    CHECK(!coop_middle_trade_partner_ok(true, 0, 1, true, false, false, false, 1)); /* disconnected */
    CHECK(!coop_middle_trade_partner_ok(true, 0, 1, true, true, true, false, 1)); /* self dead */
    CHECK(!coop_middle_trade_partner_ok(true, 0, 1, true, true, false, true, 1)); /* partner dead */
    CHECK(!coop_middle_trade_partner_ok(true, -1, 1, true, true, false, false, 1));
    CHECK(!coop_middle_trade_partner_ok(true, 0, 8, true, true, false, false, 1));

    /* transfer: give always, take never */
    CHECK(coop_middle_trade_transfer_ok(COOP_TARGET_TRADE, 0x01u | COOP_XFER_PLANT, false));
    CHECK(coop_middle_trade_transfer_ok(COOP_TARGET_TRADE, 0x01u, false));
    CHECK(!coop_middle_trade_transfer_ok(COOP_TARGET_TRADE, 0x01u | COOP_XFER_STEAL, true));
    CHECK(!coop_middle_trade_transfer_ok(COOP_TARGET_TRADE, 0x01u, true));
    CHECK(!coop_middle_trade_transfer_ok(COOP_TARGET_TRADE, 0x01u | COOP_XFER_STEAL, false));
    CHECK(!coop_middle_trade_transfer_ok(COOP_TARGET_TRADE, 0x03u, false));
    CHECK(!coop_middle_trade_transfer_ok(COOP_TARGET_STEAL, 0x01u | COOP_XFER_PLANT, false));
    /* Lite's rule routes the TRADE kind to Middle, the others are unchanged */
    CHECK(coop_lite_target_transfer_ok(COOP_TARGET_TRADE, 0x01u | COOP_XFER_PLANT, false, false));
    CHECK(!coop_lite_target_transfer_ok(COOP_TARGET_TRADE, 0x01u | COOP_XFER_STEAL, true, false));
    CHECK(coop_lite_target_transfer_ok(COOP_TARGET_STEAL, 0x01u | COOP_XFER_STEAL, true, false));
    CHECK(!coop_lite_target_transfer_ok(COOP_TARGET_NONE, 0x01u, false, false));

    /* lines */
    coop_middle_feed_format(COOP_MID_CHAT_TRADE, "Ann", "Bob|Sword", false, line, sizeof(line));
    CHECK(strcmp(line, "[Trade] Ann gave Sword to Bob") == 0);
    coop_middle_feed_format(COOP_MID_CHAT_TRADE, "Ann", "Bob|Sword", true, line, sizeof(line));
    CHECK(strcmp(line, "[Обмен] Ann передал Sword игроку Bob") == 0);
    coop_middle_feed_format(COOP_MID_CHAT_TRADE_GOLD, "Ann", "Bob|150", false, line, sizeof(line));
    CHECK(strcmp(line, "[Trade] Ann gave 150 gold to Bob") == 0);
    coop_middle_feed_format(COOP_MID_CHAT_TRADE_GOLD, "Ann", "Bob|150", true, line, sizeof(line));
    CHECK(strcmp(line, "[Обмен] Ann передал 150 золота игроку Bob") == 0);
    coop_middle_feed_format(COOP_MID_CHAT_TRADE, "", "Sword", false, line, sizeof(line)); /* no separator */
    CHECK(strcmp(line, "[Trade] Player gave Sword to player") == 0);
    coop_middle_feed_format(COOP_MID_CHAT_TRADE, "Ann", "|", false, line, sizeof(line));
    CHECK(strcmp(line, "[Trade] Ann gave ? to player") == 0);
    {
        char longs[300];

        memset(longs, 'x', sizeof(longs) - 1);
        longs[sizeof(longs) - 1] = '\0';
        longs[200] = '|';
        coop_middle_feed_format(COOP_MID_CHAT_TRADE, "Ann", longs, false, line, sizeof(line));
        CHECK(strlen(line) < sizeof(line));
    }
    coop_middle_feed_format(COOP_MID_CHAT_TRADE, "Ann", "Bob|Sword", false, tiny, sizeof(tiny));
    CHECK(strlen(tiny) < sizeof(tiny));
    CHECK(COOP_MID_CHAT_KIND_MAX >= COOP_MID_CHAT_TRADE_GOLD);

    /* stubbed game side */
    CHECK(!coop_middle_trade_local_is(42));
    CHECK(coop_middle_trade_barter_kind(42, COOP_BARTER_OPEN_STEAL) == COOP_BARTER_OPEN_STEAL);
    CHECK(!coop_middle_trade_refuse_take(42));
}

/* 8. M2 status badges: pack / get, badge priority, SLOT_STATUS op */
static void test_status(void)
{
    uint8_t bits[8] = { 0x1, 0x2, 0x4, 0x8, 0xF, 0x0, 0x3, 0xC };
    uint8_t over[8] = { 0xFF, 0x10, 0, 0, 0, 0, 0, 0x1F };
    PacketCoopPartyCtrl c;
    uint32_t p;
    char tag[4];
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    int slot;

    p = coop_middle_status_pack(bits);
    CHECK(p == 0xC30F8421u);
    for (slot = 0; slot < 8; slot++) {
        CHECK(coop_middle_status_get(p, slot) == bits[slot]);
    }
    CHECK(coop_middle_status_get(p, -1) == 0 && coop_middle_status_get(p, 8) == 0);
    CHECK(coop_middle_status_pack(NULL) == 0);
    p = coop_middle_status_pack(over); /* only the low nibble of each slot */
    CHECK(coop_middle_status_get(p, 0) == 0xF && coop_middle_status_get(p, 1) == 0
        && coop_middle_status_get(p, 7) == 0xF);
    CHECK(coop_middle_status_get(0xFFFFFFFFu, 7) == 0xF);

    /* priority: X dead > Z unconscious > P poison > D dialog > I inventory > C combat */
    CHECK(coop_middle_status_badge(true, true, 0xF, tag, &r, &g, &b) && strcmp(tag, "X") == 0
        && r == 220 && g == 60 && b == 60);
    CHECK(coop_middle_status_badge(false, true, 0xF, tag, &r, &g, &b) && strcmp(tag, "Z") == 0
        && r == 180 && g == 130 && b == 230);
    CHECK(coop_middle_status_badge(false, false, 0xF, tag, &r, &g, &b) && strcmp(tag, "P") == 0
        && r == 90 && g == 220 && b == 90);
    CHECK(coop_middle_status_badge(false, false, COOP_STATUS_DIALOG | COOP_STATUS_INVEN | COOP_STATUS_COMBAT, tag,
              &r, &g, &b)
        && strcmp(tag, "D") == 0 && r == 240 && g == 200 && b == 80);
    CHECK(coop_middle_status_badge(false, false, COOP_STATUS_INVEN | COOP_STATUS_COMBAT, tag, &r, &g, &b)
        && strcmp(tag, "I") == 0 && r == 100 && g == 210 && b == 240);
    CHECK(coop_middle_status_badge(false, false, COOP_STATUS_COMBAT, tag, &r, &g, &b) && strcmp(tag, "C") == 0
        && r == 240 && g == 140 && b == 60);
    CHECK(!coop_middle_status_badge(false, false, 0, tag, &r, &g, &b) && tag[0] == '\0');
    CHECK(coop_middle_status_badge(true, false, 0, NULL, NULL, NULL, NULL)); /* NULL outputs ok */

    /* SLOT_STATUS: valid with b == 0 and any packing */
    memset(&c, 0, sizeof(c));
    c.type = PACKET_COOP_PARTY_CTRL;
    c.op = COOP_CTRL_SLOT_STATUS;
    c.a = (int)0xC30F8421u;
    CHECK(coop_middle_party_ctrl_valid(&c));
    CHECK(coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
    c.a = COOP_STATUS_INVEN | COOP_STATUS_DIALOG;
    CHECK(coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
    c.b = 1;
    CHECK(!coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
    c.b = 0;
    CHECK(!coop_lite_party_ctrl_valid(&c, (int)sizeof(c) - 1)); /* short */
    CHECK(COOP_CTRL_SLOT_STATUS > COOP_CTRL_REST_REFUSE);
    CHECK(COOP_STATUS_CLIENT_MASK == 0x3);
    /* stubbed game side */
    CHECK(coop_middle_slot_status(0) == 0 && coop_middle_status_packed() == 0);
}

static bool label_overlap(const CoopLabelRect* a, const CoopLabelRect* b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

static void set_label(CoopLabelRect* r, int x, int y, int w, int h)
{
    r->x = x;
    r->y = y;
    r->w = w;
    r->h = h;
    r->hidden = true; /* the function decides */
}

/* 9. M2 Alt labels: deoverlap */
static void test_labels(void)
{
    CoopLabelRect r[COOP_LABELS_MAX];
    int i;
    int j;
    int n;

    /* two on the same spot: the second goes right above the first */
    set_label(&r[0], 100, 200, 60, 16);
    set_label(&r[1], 110, 200, 60, 16);
    n = coop_middle_labels_deoverlap(r, 2, 0, 600);
    CHECK(n == 2 && !r[0].hidden && !r[1].hidden);
    CHECK(r[0].y == 200 && r[1].y == 200 - 16 - 1 && r[1].x == 110);
    CHECK(!label_overlap(&r[0], &r[1]));

    /* side by side: untouched */
    set_label(&r[0], 0, 100, 50, 16);
    set_label(&r[1], 60, 100, 50, 16);
    CHECK(coop_middle_labels_deoverlap(r, 2, 0, 600) == 2 && r[1].y == 100);

    /* a stack of 32 on one point: all pairwise apart, inside [min, max) */
    for (i = 0; i < COOP_LABELS_MAX; i++) {
        set_label(&r[i], 300 + (i % 3), 590, 80, 16);
    }
    n = coop_middle_labels_deoverlap(r, COOP_LABELS_MAX, 40, 600);
    CHECK(n > 0 && n <= COOP_LABELS_MAX);
    for (i = 0; i < COOP_LABELS_MAX; i++) {
        if (r[i].hidden) {
            continue;
        }
        CHECK(r[i].y >= 40 && r[i].y + r[i].h <= 600);
        for (j = 0; j < i; j++) {
            if (!r[j].hidden) {
                CHECK(!label_overlap(&r[i], &r[j]));
            }
        }
    }
    CHECK(!r[0].hidden && r[0].y == 600 - 16); /* clamped into the view */
    CHECK(n == COOP_LABELS_MAX); /* 33 rows of 17 px fit in 560 px: all 32 */
    for (i = 0; i < COOP_LABELS_MAX; i++) {
        set_label(&r[i], 300, 590, 80, 16);
    }
    n = coop_middle_labels_deoverlap(r, COOP_LABELS_MAX, 400, 600);
    CHECK(n == 11); /* y = 584 - 17 k >= 400: k = 0..10 */
    CHECK(!r[10].hidden && r[10].y == 584 - 170 && r[11].hidden && r[31].hidden);

    /* no room above: hidden */
    set_label(&r[0], 0, 0, 100, 16);
    set_label(&r[1], 0, 0, 100, 16);
    n = coop_middle_labels_deoverlap(r, 2, 0, 600);
    CHECK(n == 1 && !r[0].hidden && r[1].hidden);

    /* degenerate input */
    set_label(&r[0], 0, 10, 0, 16);
    set_label(&r[1], 0, 10, 10, 100);
    CHECK(coop_middle_labels_deoverlap(r, 2, 0, 50) == 0 && r[0].hidden && r[1].hidden);
    CHECK(coop_middle_labels_deoverlap(NULL, 3, 0, 50) == 0);
    CHECK(coop_middle_labels_deoverlap(r, 0, 0, 50) == 0);

    /* a label below the view is pulled up into it */
    set_label(&r[0], 0, 900, 40, 16);
    CHECK(coop_middle_labels_deoverlap(r, 1, 0, 600) == 1 && r[0].y == 584);
}

/* 10. M3 rules: the new active bits, keys, hp / xp */
static void test_m3_rules(void)
{
    CoopRules r;
    char text[128];
    char desc[320];
    int n;

    coop_rules_default(&r);
    CHECK((r.flags & COOP_RULE_CLIENT_PAUSE) == 0); /* host only by default */
    CHECK((r.flags & COOP_RULE_NO_FRIENDLY_FIRE_AOE) != 0); /* ff on by default */
    CHECK((COOP_RULE_ACTIVE_MASK & COOP_RULE_CLIENT_PAUSE) && (COOP_RULE_ACTIVE_MASK & COOP_RULE_NO_FRIENDLY_FIRE_AOE));
    CHECK((COOP_RULE_ACTIVE_MASK | COOP_RULE_LITE_MODE) == COOP_RULE_KNOWN_MASK);

    n = coop_rules_parse_env(&r, "pause=1,ff=0");
    CHECK(n == 2 && (r.flags & COOP_RULE_CLIENT_PAUSE) && !(r.flags & COOP_RULE_NO_FRIENDLY_FIRE_AOE));
    n = coop_rules_parse_env(&r, "client_pause=on,no_ff_aoe=off");
    CHECK(n == 2 && (r.flags & COOP_RULE_CLIENT_PAUSE) && !(r.flags & COOP_RULE_NO_FRIENDLY_FIRE_AOE));
    n = coop_rules_parse_env(&r, "noffaoe=0"); /* the M1-M2 spelling still reads */
    CHECK(n == 1 && !(r.flags & COOP_RULE_NO_FRIENDLY_FIRE_AOE));
    coop_rules_format_env(&r, text, sizeof(text));
    CHECK(strstr(text, "ff=0") != NULL && strstr(text, "pause=0") != NULL && strstr(text, "noffaoe") == NULL);

    /* hp / xp: keys, long aliases, clamps, launcher steps */
    n = coop_rules_parse_env(&r, "hp=200,xp=150");
    CHECK(n == 2 && r.enemy_hp_pct == 200 && r.xp_mult_pct == 150);
    n = coop_rules_parse_env(&r, "enemy_hp_pct=250,xp_mult_pct=125");
    CHECK(n == 2 && r.enemy_hp_pct == 250 && r.xp_mult_pct == 125);
    coop_rules_parse_env(&r, "hp=301,xp=251");
    CHECK(r.enemy_hp_pct == 300 && r.xp_mult_pct == 250);
    coop_rules_parse_env(&r, "hp=99,xp=0");
    CHECK(r.enemy_hp_pct == 100 && r.xp_mult_pct == 100);
    {
        static const int hp[] = { 100, 150, 200, 250, 300 };
        static const int xp[] = { 100, 125, 150, 200 };
        CoopRules back;
        int i;

        for (i = 0; i < 5; i++) {
            coop_rules_default(&r);
            r.enemy_hp_pct = (uint16_t)hp[i];
            r.xp_mult_pct = (uint16_t)xp[i % 4];
            r.flags |= COOP_RULE_CLIENT_PAUSE;
            coop_rules_format_env(&r, text, sizeof(text));
            CHECK(coop_rules_parse_env(&back, text) == 16);
            CHECK(coop_rules_equal(&r, &back));
        }
    }
    coop_rules_default(&r);
    r.flags |= COOP_RULE_CLIENT_PAUSE;
    coop_rules_describe(&r, false, desc, sizeof(desc));
    CHECK(strstr(desc, "client pause on") != NULL && strstr(desc, "no friendly fire on") != NULL);
    CHECK(strstr(desc, "reserved") == NULL);
    coop_rules_describe(&r, true, desc, sizeof(desc));
    CHECK(strstr(desc, "пауза клиентов вкл") != NULL);
}

/* 11. M3 enemy damage / XP scaling */
static void test_m3_scaling(void)
{
    int rem;
    int sum;
    int i;

    /* 100%: unchanged */
    rem = 0;
    CHECK(coop_middle_scale_enemy_damage(10, 100, &rem) == 10 && rem == 0);
    CHECK(coop_middle_scale_enemy_damage(1, 100, &rem) == 1);
    /* 150 / 200 / 300 */
    rem = 0;
    CHECK(coop_middle_scale_enemy_damage(30, 150, &rem) == 20 && rem == 0);
    rem = 0;
    CHECK(coop_middle_scale_enemy_damage(30, 200, &rem) == 15 && rem == 0);
    rem = 0;
    CHECK(coop_middle_scale_enemy_damage(30, 300, &rem) == 10 && rem == 0);
    /* the remainder carries: 10 at 150% three times = 6 + 7 + 7 = 20 */
    rem = 0;
    CHECK(coop_middle_scale_enemy_damage(10, 150, &rem) == 6 && rem == 100);
    CHECK(coop_middle_scale_enemy_damage(10, 150, &rem) == 7 && rem == 50);
    CHECK(coop_middle_scale_enemy_damage(10, 150, &rem) == 7 && rem == 0);
    /* a long series loses nothing: 7 x 300 hits of 7 at 300% = 7 * 100 */
    rem = 0;
    sum = 0;
    for (i = 0; i < 300; i++) {
        sum += coop_middle_scale_enemy_damage(7, 300, &rem);
    }
    CHECK(sum == 700);
    /* minimum 1, 0 for <= 0, bad remainders, NULL */
    rem = 0;
    CHECK(coop_middle_scale_enemy_damage(1, 300, &rem) == 1 && rem == 0);
    CHECK(coop_middle_scale_enemy_damage(2, 300, &rem) == 1);
    CHECK(coop_middle_scale_enemy_damage(0, 200, &rem) == 0);
    CHECK(coop_middle_scale_enemy_damage(-5, 200, &rem) == 0);
    rem = 99999;
    CHECK(coop_middle_scale_enemy_damage(30, 300, &rem) == 10);
    CHECK(coop_middle_scale_enemy_damage(30, 300, NULL) == 10);
    CHECK(coop_middle_scale_enemy_damage(30, 50, NULL) == 30); /* < 100 = 100 */
    CHECK(coop_middle_scale_enemy_damage(INT_MAX, 100, NULL) == INT_MAX);

    /* XP */
    CHECK(coop_middle_scale_xp(100, 100) == 100);
    CHECK(coop_middle_scale_xp(100, 125) == 125);
    CHECK(coop_middle_scale_xp(100, 150) == 150);
    CHECK(coop_middle_scale_xp(100, 250) == 250);
    CHECK(coop_middle_scale_xp(3, 125) == 3); /* floor, never below raw */
    CHECK(coop_middle_scale_xp(1, 100) == 1);
    CHECK(coop_middle_scale_xp(0, 250) == 0);
    CHECK(coop_middle_scale_xp(-4, 250) == -4);
    CHECK(coop_middle_scale_xp(INT_MAX, 250) == INT_MAX); /* overflow clamps */
    CHECK(coop_middle_scale_xp(INT_MAX / 2, 250) == INT_MAX);
    CHECK(coop_middle_scale_xp(1000000000, 200) == 2000000000 && coop_middle_scale_xp(1500000000, 150) == INT_MAX);
}

/* 12. M3 chat commands */
static void test_m3_cmds(void)
{
    char line[256];
    int r;

    CHECK(coop_middle_parse_cmd("/tp") == COOP_CMD_TP);
    CHECK(coop_middle_parse_cmd("/TP  ") == COOP_CMD_TP);
    CHECK(coop_middle_parse_cmd("  /Tp") == COOP_CMD_TP);
    CHECK(coop_middle_parse_cmd("/stuck") == COOP_CMD_STUCK);
    CHECK(coop_middle_parse_cmd("/STUCK\t") == COOP_CMD_STUCK);
    CHECK(coop_middle_parse_cmd("/gather") == COOP_CMD_GATHER);
    CHECK(coop_middle_parse_cmd("/tp all") == COOP_CMD_GATHER);
    CHECK(coop_middle_parse_cmd("/TP   ALL ") == COOP_CMD_GATHER);
    CHECK(coop_middle_parse_cmd("tp") == COOP_CMD_NONE);
    CHECK(coop_middle_parse_cmd("hello /tp") == COOP_CMD_NONE);
    CHECK(coop_middle_parse_cmd("/unknown") == COOP_CMD_NONE);
    CHECK(coop_middle_parse_cmd("/tp me") == COOP_CMD_NONE);
    CHECK(coop_middle_parse_cmd("/tp all now") == COOP_CMD_NONE);
    CHECK(coop_middle_parse_cmd("/tpx") == COOP_CMD_NONE);
    CHECK(coop_middle_parse_cmd("/") == COOP_CMD_NONE);
    CHECK(coop_middle_parse_cmd("") == COOP_CMD_NONE);
    CHECK(coop_middle_parse_cmd(NULL) == COOP_CMD_NONE);
    CHECK(coop_middle_parse_cmd("/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == COOP_CMD_NONE);
    CHECK(!coop_middle_try_chat_command(1, "/tp")); /* stub build: no host */

    /* cooldown 30 s */
    CHECK(coop_middle_cooldown_left_s(false, 0, 5000, COOP_CMD_COOLDOWN_MS) == 0);
    CHECK(coop_middle_cooldown_left_s(true, 1000, 1000, COOP_CMD_COOLDOWN_MS) == 30);
    CHECK(coop_middle_cooldown_left_s(true, 1000, 1001, COOP_CMD_COOLDOWN_MS) == 30);
    CHECK(coop_middle_cooldown_left_s(true, 1000, 30000, COOP_CMD_COOLDOWN_MS) == 1);
    CHECK(coop_middle_cooldown_left_s(true, 1000, 31000, COOP_CMD_COOLDOWN_MS) == 0);
    CHECK(coop_middle_cooldown_left_s(true, 0xFFFFF000u, 0x00001000u, COOP_CMD_COOLDOWN_MS) == 22); /* wrap */

    /* lines */
    coop_middle_feed_format(COOP_MID_CHAT_REGROUP, "Ann", "0", false, line, sizeof(line));
    CHECK(strcmp(line, "[Co-op] Ann regrouped to the host.") == 0);
    coop_middle_feed_format(COOP_MID_CHAT_REGROUP, "Ann", "0", true, line, sizeof(line));
    CHECK(strcmp(line, "[Кооп] Ann телепортировался к хосту.") == 0);
    coop_middle_feed_format(COOP_MID_CHAT_REGROUP, "Host", "1", false, line, sizeof(line));
    CHECK(strstr(line, "gathered") != NULL);
    coop_middle_feed_format(COOP_MID_CHAT_CMD_REFUSE, "", "5 12", false, line, sizeof(line));
    CHECK(strstr(line, "12 s") != NULL);
    for (r = 0; r <= COOP_TP_REFUSE_MAX + 1; r++) {
        char what[16];

        snprintf(what, sizeof(what), "%d 3", r);
        coop_middle_feed_format(COOP_MID_CHAT_CMD_REFUSE, "", what, false, line, sizeof(line));
        CHECK(line[0] != '\0');
        coop_middle_feed_format(COOP_MID_CHAT_CMD_REFUSE, "", what, true, line, sizeof(line));
        CHECK(line[0] != '\0');
    }
    coop_middle_feed_format(COOP_MID_CHAT_CMD_REFUSE, "", "garbage", false, line, sizeof(line));
    CHECK(line[0] != '\0');
    coop_middle_feed_format(COOP_MID_CHAT_PAUSE_DROP, "Bob", "-", false, line, sizeof(line));
    CHECK(strstr(line, "Bob") != NULL && strstr(line, "pause") != NULL);
    CHECK(COOP_MID_CHAT_PAUSE_DROP == 8 && COOP_MID_CHAT_KIND_MAX == COOP_MID_CHAT_AUTOSAVE);
}

/* 13. M3 pings: packet, role, validity, rate limit */
static void test_m3_ping(void)
{
    PacketCoopPing p;
    bool have = false;
    unsigned int last = 0;

    CHECK(PACKET_COOP_PING == 168 && PACKET_COOP_PING < PACKET_COOP_TYPE_LIMIT);
    CHECK(PACKET_COOP_TYPE_LIMIT == 176);
    CHECK(sizeof(PacketCoopPing) == 24);
    CHECK(offsetof(PacketCoopPing, kind) == 0x0C && offsetof(PacketCoopPing, loc) == 0x10);
    CHECK(coop_lite_coop_packet_min_size(PACKET_COOP_PING) == sizeof(PacketCoopPing));
    CHECK(coop_lite_mp_packet_role(PACKET_COOP_PING) == COOP_MP_PKT_BOTH);
    CHECK(coop_lite_packet_allowed(PACKET_COOP_PING, true, 3)); /* client -> host */
    CHECK(coop_lite_packet_allowed(PACKET_COOP_PING, false, 0)); /* host -> client */
    CHECK(!coop_lite_packet_allowed(PACKET_COOP_PING, false, 2)); /* never from another client */

    memset(&p, 0, sizeof(p));
    p.type = PACKET_COOP_PING;
    p.slot = 0;
    p.map = 5;
    p.kind = COOP_PING_KIND_LOOK;
    p.loc = ((int64_t)100 << 32) | 200;
    CHECK(coop_middle_ping_valid(&p, (int)sizeof(p)));
    CHECK(!coop_middle_ping_valid(&p, (int)sizeof(p) - 1));
    CHECK(!coop_middle_ping_valid(&p, (int)sizeof(p) + 8));
    CHECK(!coop_middle_ping_valid(NULL, (int)sizeof(p)));
    p.kind = COOP_PING_KIND_DANGER;
    CHECK(coop_middle_ping_valid(&p, (int)sizeof(p)));
    p.kind = 2;
    CHECK(!coop_middle_ping_valid(&p, (int)sizeof(p)));
    p.kind = -1;
    CHECK(!coop_middle_ping_valid(&p, (int)sizeof(p)));
    p.kind = 0;
    p.slot = 8;
    CHECK(!coop_middle_ping_valid(&p, (int)sizeof(p)));
    p.slot = 7;
    CHECK(coop_middle_ping_valid(&p, (int)sizeof(p)));
    p.type = PACKET_COOP_RULES;
    CHECK(!coop_middle_ping_valid(&p, (int)sizeof(p)));

    /* 1 ping / s per slot */
    CHECK(coop_middle_rate_admit(&have, &last, 5000, COOP_PING_RATE_MS));
    CHECK(!coop_middle_rate_admit(&have, &last, 5999, COOP_PING_RATE_MS));
    CHECK(last == 5000);
    CHECK(coop_middle_rate_admit(&have, &last, 6000, COOP_PING_RATE_MS));
    CHECK(!coop_middle_rate_admit(NULL, &last, 9000, COOP_PING_RATE_MS));
    CHECK(COOP_PING_MAX == 16 && COOP_PING_LIFE_MS == 4500u && COOP_PING_SOUND_RANGE == 40);
    {
        CoopPingView v[4];

        CHECK(coop_middle_pings_get(v, 4, 0) == 0); /* stub build */
    }
}

/* 14. M3 pause op in coop_lite_party_ctrl_valid, protocol 55 */
static void test_m3_pause(void)
{
    PacketCoopPartyCtrl c;
    bool have = false;
    unsigned int last = 0;
    int b;

    CHECK(TIG_NET_PROTOCOL >= 59);
    CHECK(COOP_CTRL_PAUSE == 8 && COOP_CTRL_PAUSE > COOP_CTRL_SLOT_STATUS);
    memset(&c, 0, sizeof(c));
    c.type = PACKET_COOP_PARTY_CTRL;
    c.op = COOP_CTRL_PAUSE;
    for (b = 0; b < 8; b++) {
        c.a = 1;
        c.b = b;
        CHECK(coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
        c.a = 0;
        CHECK(coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
    }
    c.a = 2;
    c.b = 0;
    CHECK(!coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
    c.a = -1;
    CHECK(!coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
    c.a = 1;
    c.b = 8;
    CHECK(!coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
    c.b = -1;
    CHECK(!coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
    c.b = 0;
    CHECK(!coop_lite_party_ctrl_valid(&c, (int)sizeof(c) - 4));

    /* the client's 2 s rate limit */
    CHECK(coop_middle_rate_admit(&have, &last, 100, COOP_PAUSE_RATE_MS));
    CHECK(!coop_middle_rate_admit(&have, &last, 2099, COOP_PAUSE_RATE_MS));
    CHECK(coop_middle_rate_admit(&have, &last, 2100, COOP_PAUSE_RATE_MS));

    /* stubs: never paused, orders never blocked, no-op hooks */
    CHECK(!coop_middle_is_paused() && coop_middle_paused_by() == -1);
    CHECK(!coop_middle_orders_blocked(true));
    CHECK(!coop_middle_pause_key());
    CHECK(coop_middle_host_enemy_damage(123, 40) == 40);
    CHECK(!coop_middle_host_ff_block(1, 2));
    CHECK(coop_middle_host_scale_xp(77) == 77);
}

/* 15. M4 rules: qitems / sfol, always in the string, aliases */
static void test_m4_rules(void)
{
    CoopRules r;
    CoopRules back;
    char text[160];
    char desc[1024];
    int n;

    CHECK(COOP_RULE_SHARED_QUEST_ITEMS == 0x40u && COOP_RULE_SHARED_FOLLOWERS == 0x80u);
    CHECK((COOP_RULE_ACTIVE_MASK & (COOP_RULE_SHARED_QUEST_ITEMS | COOP_RULE_SHARED_FOLLOWERS))
        == (COOP_RULE_SHARED_QUEST_ITEMS | COOP_RULE_SHARED_FOLLOWERS));
    CHECK(COOP_RULE_KNOWN_MASK == (COOP_RULE_ACTIVE_MASK | COOP_RULE_LITE_MODE));
    coop_rules_default(&r);
    r.flags &= ~(COOP_RULE_SHARED_QUEST_ITEMS | COOP_RULE_SHARED_FOLLOWERS);
    coop_rules_format_env(&r, text, sizeof(text));
    CHECK(strstr(text, "qitems=0") != NULL && strstr(text, "sfol=0") != NULL); /* off still listed */
    CHECK(coop_rules_parse_env(&back, text) == 16 && coop_rules_equal(&r, &back));
    n = coop_rules_parse_env(&r, "shared_quest_items=0,shared_followers=off");
    CHECK(n == 2 && (r.flags & (COOP_RULE_SHARED_QUEST_ITEMS | COOP_RULE_SHARED_FOLLOWERS)) == 0);
    n = coop_rules_parse_env(&r, "qitems=1;sfol=1");
    CHECK(n == 2 && (r.flags & COOP_RULE_SHARED_QUEST_ITEMS) != 0 && (r.flags & COOP_RULE_SHARED_FOLLOWERS) != 0);
    coop_rules_default(&r);
    coop_rules_describe(&r, false, desc, sizeof(desc));
    CHECK(strstr(desc, "shared quest items on") != NULL && strstr(desc, "shared followers on") != NULL);
    CHECK(strstr(desc, "XP 100%") != NULL); /* nothing cut off */
    coop_rules_describe(&r, true, desc, sizeof(desc));
    CHECK(strstr(desc, "общие квестовые предметы вкл") != NULL && strstr(desc, "общие спутники вкл") != NULL);
    CHECK(strstr(desc, "опыт 100%") != NULL);
}

/* 16. M4 shared quest items: who may be searched, pack order, chat line */
static void test_m4_quest_items(void)
{
    char line[256];
    int64_t owner = 123;

    CHECK(COOP_SHARED_QUEST_RADIUS == 30);
    CHECK(coop_middle_quest_pocket_eligible(true, true, 1, 0, true, false, true, 0));
    CHECK(coop_middle_quest_pocket_eligible(true, true, 0, 3, true, false, true, 30));
    CHECK(!coop_middle_quest_pocket_eligible(true, true, 0, 3, true, false, true, 31)); /* too far */
    CHECK(!coop_middle_quest_pocket_eligible(false, true, 0, 3, true, false, true, 1)); /* rule off */
    CHECK(!coop_middle_quest_pocket_eligible(true, false, 0, 3, true, false, true, 1)); /* not the host */
    CHECK(!coop_middle_quest_pocket_eligible(true, true, 2, 2, true, false, true, 0)); /* self */
    CHECK(!coop_middle_quest_pocket_eligible(true, true, 0, 3, false, false, true, 1)); /* disconnected */
    CHECK(!coop_middle_quest_pocket_eligible(true, true, 0, 3, true, true, true, 1)); /* dead */
    CHECK(!coop_middle_quest_pocket_eligible(true, true, 0, 3, true, false, false, 1)); /* other map */
    CHECK(!coop_middle_quest_pocket_eligible(true, true, 0, 3, true, false, true, -1)); /* unknown distance */
    CHECK(!coop_middle_quest_pocket_eligible(true, true, -1, 3, true, false, true, 1));
    CHECK(!coop_middle_quest_pocket_eligible(true, true, 0, 8, true, false, true, 1));
    CHECK(coop_middle_quest_item_rank(0) == 0 && coop_middle_quest_item_rank(999) == 0);
    CHECK(coop_middle_quest_item_rank(1000) == 1 && coop_middle_quest_item_rank(1008) == 1);
    CHECK(coop_middle_quest_item_rank(1009) == -1 && coop_middle_quest_item_rank(-1) == -1);
    coop_middle_feed_format(COOP_MID_CHAT_QUEST, "Bob", "Golden key", false, line, sizeof(line));
    CHECK(strcmp(line, "[Quest] Golden key handed over from Bob") == 0);
    coop_middle_feed_format(COOP_MID_CHAT_QUEST, "Bob", "Golden key", true, line, sizeof(line));
    CHECK(strcmp(line, "[Квест] Golden key передан из рюкзака Bob") == 0);
    /* test build: the game lookup is stubbed (no item, owner cleared) */
    CHECK(coop_middle_find_party_item(1, 5, COOP_QITEM_MATCH_NAME, &owner) == 0 && owner == 0);
}

/* 17. M4 town map fog: packet layout, validity, merge, signature */
static void test_m4_fog(void)
{
    static PacketCoopTownmapFog p;
    uint8_t dst[4] = { 0x01, 0x00, 0xF0, 0xFF };
    uint8_t src[4] = { 0x03, 0x80, 0x0F, 0x00 };
    uint8_t a[3] = { 1, 2, 3 };
    uint8_t b[3] = { 1, 2, 4 };

    CHECK(PACKET_COOP_TOWNMAP_FOG == 169 && PACKET_COOP_TOWNMAP_FOG < PACKET_COOP_TYPE_LIMIT);
    CHECK(sizeof(PacketCoopTownmapFog) == 0x410 && COOP_TOWNMAP_FOG_HDR == 0x10);
    CHECK(offsetof(PacketCoopTownmapFog, offset) == 8 && offsetof(PacketCoopTownmapFog, len) == 12
        && offsetof(PacketCoopTownmapFog, flags) == 14);
    CHECK(coop_lite_mp_packet_role(PACKET_COOP_TOWNMAP_FOG) == COOP_MP_PKT_BOTH);
    CHECK(coop_lite_coop_packet_min_size(PACKET_COOP_TOWNMAP_FOG) == (size_t)COOP_TOWNMAP_FOG_HDR + 1);
    memset(&p, 0, sizeof(p));
    p.type = PACKET_COOP_TOWNMAP_FOG;
    p.townmap_id = 7;
    p.offset = 0;
    p.len = 100;
    CHECK(coop_middle_townmap_fog_valid(&p, COOP_TOWNMAP_FOG_HDR + 100));
    CHECK(coop_middle_townmap_fog_valid(&p, (int)sizeof(p))); /* full struct is fine too */
    CHECK(!coop_middle_townmap_fog_valid(&p, COOP_TOWNMAP_FOG_HDR + 99)); /* short */
    CHECK(!coop_middle_townmap_fog_valid(&p, (int)sizeof(p) + 1));
    CHECK(!coop_middle_townmap_fog_valid(NULL, COOP_TOWNMAP_FOG_HDR + 100));
    p.flags = COOP_TOWNMAP_FOG_F_REPLACE;
    CHECK(coop_middle_townmap_fog_valid(&p, COOP_TOWNMAP_FOG_HDR + 100));
    p.flags = 0x2;
    CHECK(!coop_middle_townmap_fog_valid(&p, (int)sizeof(p))); /* unknown flag */
    p.flags = 0;
    p.townmap_id = 0;
    CHECK(!coop_middle_townmap_fog_valid(&p, (int)sizeof(p)));
    p.townmap_id = 7;
    p.len = 0;
    CHECK(!coop_middle_townmap_fog_valid(&p, (int)sizeof(p)));
    p.len = COOP_TOWNMAP_FOG_CHUNK + 1;
    CHECK(!coop_middle_townmap_fog_valid(&p, (int)sizeof(p)));
    p.len = COOP_TOWNMAP_FOG_CHUNK;
    p.offset = COOP_TOWNMAP_FOG_MAX_BYTES - COOP_TOWNMAP_FOG_CHUNK;
    CHECK(coop_middle_townmap_fog_valid(&p, (int)sizeof(p)));
    p.offset = 0xFFFFFFF0u; /* no wrap-around */
    CHECK(!coop_middle_townmap_fog_valid(&p, (int)sizeof(p)));
    p.type = PACKET_COOP_PING;
    p.offset = 0;
    CHECK(!coop_middle_townmap_fog_valid(&p, (int)sizeof(p)));

    CHECK(coop_middle_fog_merge(dst, src, 4, false) == 1 + 1 + 4); /* 0x02, 0x80, 0x0F */
    CHECK(dst[0] == 0x03 && dst[1] == 0x80 && dst[2] == 0xFF && dst[3] == 0xFF);
    CHECK(coop_middle_fog_merge(dst, src, 4, false) == 0); /* idempotent */
    CHECK(coop_middle_fog_merge(dst, src, 4, true) == 0); /* replace clears, opens nothing */
    CHECK(dst[0] == 0x03 && dst[2] == 0x0F && dst[3] == 0x00);
    CHECK(coop_middle_fog_merge(dst, b, 0, false) == 0 && coop_middle_fog_merge(NULL, src, 4, false) == 0);
    CHECK(coop_middle_fnv1a(a, 3) != coop_middle_fnv1a(b, 3));
    CHECK(coop_middle_fnv1a(a, 0) == 2166136261u && coop_middle_fnv1a(NULL, 5) == 2166136261u);
}

/* 18. M4 parallel dialogs: routing */
static void test_m4_dialog_route(void)
{
    bool act[8] = { false, false, false, false, false, false, false, false };

    CHECK(coop_middle_dialog_route(1, act, -1, -1) == -1);
    act[0] = true; /* the host talks */
    CHECK(coop_middle_dialog_route(1, act, -1, -1) == 0);
    CHECK(coop_middle_dialog_route(0, act, 3, 3) == 0); /* the host's own dialog first */
    act[2] = true; /* slot 2 talks in parallel */
    CHECK(coop_middle_dialog_route(2, act, -1, 2) == 2); /* own session */
    CHECK(coop_middle_dialog_route(2, act, 0, 2) == 2); /* own beats attached */
    CHECK(coop_middle_dialog_route(3, act, 2, 2) == 2); /* attached (NPC was busy) */
    CHECK(coop_middle_dialog_route(3, act, -1, 2) == 0); /* default: the host's */
    CHECK(coop_middle_dialog_route(3, act, 5, 2) == 0); /* attached session gone */
    act[0] = false;
    CHECK(coop_middle_dialog_route(3, act, -1, 2) == 2); /* else the most recent */
    CHECK(coop_middle_dialog_route(0, act, -1, 2) == 2); /* the host watches it */
    CHECK(coop_middle_dialog_route(3, act, -1, 0) == -1); /* recent must be a client */
    CHECK(coop_middle_dialog_route(8, act, -1, 2) == -1 && coop_middle_dialog_route(-1, act, -1, 2) == -1);
    CHECK(coop_middle_dialog_route(1, NULL, -1, 2) == -1);
}

/* 19. M4 shared followers: access, NPC stats packet with skills */
static void test_m4_followers(void)
{
    CHECK(COOP_SHARED_FOLLOWER_RANGE == 6);
    CHECK(coop_middle_follower_access_ok(false, true, true, true, 50)); /* the leader always */
    CHECK(coop_middle_follower_access_ok(true, false, true, true, 6));
    CHECK(!coop_middle_follower_access_ok(true, false, true, true, 7));
    CHECK(!coop_middle_follower_access_ok(false, false, true, true, 1)); /* rule off */
    CHECK(!coop_middle_follower_access_ok(true, false, false, true, 1)); /* not a party follower */
    CHECK(!coop_middle_follower_access_ok(true, true, true, false, 1)); /* dead */
    CHECK(!coop_middle_follower_access_ok(true, false, true, true, -1));
    CHECK(sizeof(PacketCoopNpcStats) == 0x120);
    CHECK(offsetof(PacketCoopNpcStats, basic_skills) == 0xA0 && offsetof(PacketCoopNpcStats, tech_skills) == 0xD0
        && offsetof(PacketCoopNpcStats, spell_tech) == 0xE0);
    CHECK(COOP_NPC_BASIC_SKILLS == 12 && COOP_NPC_TECH_SKILLS == 4 && COOP_NPC_SPELL_TECH == 16);
    {
        static PacketCoopNpcStats s;

        memset(&s, 0, sizeof(s));
        s.type = PACKET_COOP_NPC_STATS;
        s.stats[17] = 5;
        s.basic_skills[3] = 4;
        s.spell_tech[15] = 2;
        CHECK(coop_lite_npc_stats_valid(&s, (int)sizeof(s)));
        CHECK(!coop_lite_npc_stats_valid(&s, 0xA0)); /* the protocol-55 size is refused */
        CHECK(coop_lite_coop_packet_min_size(PACKET_COOP_NPC_STATS) == sizeof(PacketCoopNpcStats));
    }
}

#ifndef LOCATION_MAKE
#define LOCATION_MAKE(x, y) ((int64_t)((x) & 0xFFFFFFFF) | (((int64_t)(y) & 0xFFFFFFFF) << 32))
#endif
#ifndef SECTOR_MAKE
#define SECTOR_MAKE(a, b) ((int64_t)(a) | ((int64_t)(b) << 26))
#endif

/* 20. M5 rules: ow / usleep */
static void test_m5_rules(void)
{
    CoopRules r;
    CoopRules back;
    char text[200];
    char desc[1024];

    CHECK(COOP_RULE_FREE_OVERWORLD == 0x100u && COOP_RULE_UNIFIED_SLEEP == 0x200u);
    CHECK(COOP_RULE_KNOWN_MASK == (COOP_RULE_ACTIVE_MASK | COOP_RULE_LITE_MODE));
    coop_rules_default(&r);
    CHECK((r.flags & (COOP_RULE_FREE_OVERWORLD | COOP_RULE_UNIFIED_SLEEP))
        == (COOP_RULE_FREE_OVERWORLD | COOP_RULE_UNIFIED_SLEEP));
    r.flags &= ~(COOP_RULE_FREE_OVERWORLD | COOP_RULE_UNIFIED_SLEEP);
    coop_rules_format_env(&r, text, sizeof(text));
    CHECK(strstr(text, "ow=0") != NULL && strstr(text, "usleep=0") != NULL); /* off still listed */
    CHECK(coop_rules_parse_env(&back, text) == 16 && coop_rules_equal(&r, &back));
    CHECK(coop_rules_parse_env(&r, "free_overworld=0,unified_sleep=off") == 2);
    CHECK((r.flags & (COOP_RULE_FREE_OVERWORLD | COOP_RULE_UNIFIED_SLEEP)) == 0);
    coop_rules_default(&r);
    coop_rules_describe(&r, false, desc, sizeof(desc));
    CHECK(strstr(desc, "free world map on") != NULL && strstr(desc, "unified sleep on") != NULL);
    CHECK(strstr(desc, "XP 100%") != NULL);
    coop_rules_describe(&r, true, desc, sizeof(desc));
    CHECK(strstr(desc, "свободная карта мира вкл") != NULL && strstr(desc, "единый сон вкл") != NULL);
}

/* 21. M5 interest management: keep window, ranges, radii */
static void test_m5_interest(void)
{
    int64_t ids[9];
    int64_t base = LOCATION_MAKE(10 * 64 + 32, 7 * 64 + 32);
    int n;

    CHECK(COOP_SECTOR_CACHE_COOP == 128);
    CHECK(coop_middle_keep_sector_margin(0, 0) == 0);
    CHECK(coop_middle_keep_sector_margin(4, 0) == 0); /* few players: full 3x3 */
    CHECK(coop_middle_keep_sector_margin(5, 36) == 0); /* enough room */
    CHECK(coop_middle_keep_sector_margin(5, 35) == COOP_KEEP_MARGIN);
    CHECK(coop_middle_keep_sector_margin(7, -20) == COOP_KEEP_MARGIN);

    CHECK(coop_middle_keep_ids(base, 0, ids, 9) == 9); /* 3x3 */
    n = coop_middle_keep_ids(base, COOP_KEEP_MARGIN, ids, 9);
    CHECK(n == 4 && ids[0] == SECTOR_MAKE(10, 7) && ids[1] == SECTOR_MAKE(11, 7)); /* offset 32: the +1 side */
    n = coop_middle_keep_ids(LOCATION_MAKE(10 * 64 + 31, 7 * 64 + 31), COOP_KEEP_MARGIN, ids, 9);
    CHECK(n == 4 && ids[1] == SECTOR_MAKE(9, 7) && ids[3] == SECTOR_MAKE(9, 6)); /* offset 31: the -1 side */
    n = coop_middle_keep_ids(LOCATION_MAKE(10 * 64 + 31, 7 * 64 + 31), 8, ids, 9);
    CHECK(n == 1 && ids[0] == SECTOR_MAKE(10, 7)); /* narrow margin, sector middle: own only */
    n = coop_middle_keep_ids(LOCATION_MAKE(10 * 64 + 3, 7 * 64 + 60), COOP_KEEP_MARGIN, ids, 9);
    CHECK(n == 4); /* corner: 2x2 */
    CHECK(ids[0] == SECTOR_MAKE(10, 7) && ids[1] == SECTOR_MAKE(9, 7) && ids[2] == SECTOR_MAKE(10, 8)
        && ids[3] == SECTOR_MAKE(9, 8));
    n = coop_middle_keep_ids(LOCATION_MAKE(5, 5), COOP_KEEP_MARGIN, ids, 9);
    CHECK(n == 1 && ids[0] == SECTOR_MAKE(0, 0)); /* no negative sectors */
    CHECK(coop_middle_keep_ids(LOCATION_MAKE(64 * 10 + 3, 64 * 7 + 3), COOP_KEEP_MARGIN, ids, 2) == 2);
    CHECK(coop_middle_keep_ids(base, COOP_KEEP_MARGIN, NULL, 9) == 0);

    CHECK(coop_middle_loc_within(base, base, 0));
    CHECK(coop_middle_loc_within(base, LOCATION_MAKE(10 * 64 + 32 + 44, 7 * 64 + 32 - 44), 44));
    CHECK(!coop_middle_loc_within(base, LOCATION_MAKE(10 * 64 + 32 + 45, 7 * 64 + 32), 44));
    CHECK(!coop_middle_loc_within(0, base, 1000) && !coop_middle_loc_within(base, 0, 1000));

    CHECK(coop_middle_interest_radius(PACKET_COOP_NPC_POS, false) == 44);
    CHECK(coop_middle_interest_radius(PACKET_COOP_NPC_POS, true) == 44);
    CHECK(coop_middle_interest_radius(PACKET_COOP_DOOR_STATE, true) == 96);
    CHECK(coop_middle_interest_radius(PACKET_COOP_DOOR_STATE, false) == 0);
    CHECK(coop_middle_interest_radius(PACKET_COOP_TRAP_FX, true) == 56);
    CHECK(coop_middle_interest_radius(PACKET_COOP_PROJECTILE_FX, true) == 56);
    CHECK(coop_middle_interest_radius(106, true) == 56 && coop_middle_interest_radius(77, true) == 56);
    CHECK(coop_middle_interest_radius(106, false) == 0 && coop_middle_interest_radius(77, false) == 0);
    CHECK(coop_middle_interest_radius(PACKET_COOP_RULES, true) == 0);

    /* NPC_POS seen mask: a slot entering range forces a send */
    CHECK(coop_lite_npc_pos_force(0x02, 0x06, true));
    CHECK(!coop_lite_npc_pos_force(0x06, 0x06, true));
    CHECK(coop_lite_npc_pos_force(0x06, 0x06, false));
}

/* 22. M5 world map travel packet */
static void test_m5_overworld(void)
{
    PacketCoopOverworldTravel p;
    int x;
    int y;

    CHECK(TIG_NET_PROTOCOL >= 59);
    CHECK(PACKET_COOP_OVERWORLD_TRAVEL == 170 && PACKET_COOP_TYPE_LIMIT >= 172);
    CHECK(sizeof(PacketCoopOverworldTravel) == 0x20);
    CHECK(coop_lite_mp_packet_role(PACKET_COOP_OVERWORLD_TRAVEL) == COOP_MP_PKT_BOTH);
    CHECK(coop_lite_coop_packet_min_size(PACKET_COOP_OVERWORLD_TRAVEL) == sizeof(PacketCoopOverworldTravel));

    memset(&p, 0, sizeof(p));
    p.type = PACKET_COOP_OVERWORLD_TRAVEL;
    p.op = COOP_OWT_START;
    p.slot = 2;
    p.loc = LOCATION_MAKE(1000, 2000);
    CHECK(coop_middle_overworld_pkt_valid(&p, sizeof(p), false));
    CHECK(!coop_middle_overworld_pkt_valid(&p, sizeof(p), true)); /* START is client -> host */
    CHECK(!coop_middle_overworld_pkt_valid(&p, sizeof(p) - 1, false));
    CHECK(!coop_middle_overworld_pkt_valid(NULL, sizeof(p), false));
    p.flags = COOP_OWT_F_MOVING;
    CHECK(!coop_middle_overworld_pkt_valid(&p, sizeof(p), false));
    p.flags = 0;
    p.loc = 0;
    CHECK(!coop_middle_overworld_pkt_valid(&p, sizeof(p), false));
    p.op = COOP_OWT_CANCEL;
    CHECK(coop_middle_overworld_pkt_valid(&p, sizeof(p), false));
    CHECK(!coop_middle_overworld_pkt_valid(&p, sizeof(p), true));
    p.op = COOP_OWT_PROGRESS;
    p.flags = COOP_OWT_F_MOVING | COOP_OWT_F_ENCOUNTER;
    CHECK(coop_middle_overworld_pkt_valid(&p, sizeof(p), true));
    CHECK(!coop_middle_overworld_pkt_valid(&p, sizeof(p), false));
    p.flags = 4;
    CHECK(!coop_middle_overworld_pkt_valid(&p, sizeof(p), true));
    p.op = COOP_OWT_ARRIVED;
    p.flags = COOP_OWT_F_ENCOUNTER;
    p.loc = LOCATION_MAKE(5, 5);
    CHECK(coop_middle_overworld_pkt_valid(&p, sizeof(p), true));
    p.flags = COOP_OWT_F_MOVING;
    CHECK(!coop_middle_overworld_pkt_valid(&p, sizeof(p), true));
    p.op = COOP_OWT_REFUSED;
    p.flags = 0;
    p.world_x = COOP_OWT_REFUSE_COMBAT;
    CHECK(coop_middle_overworld_pkt_valid(&p, sizeof(p), true));
    p.world_x = COOP_OWT_REFUSE_MAX + 1;
    CHECK(!coop_middle_overworld_pkt_valid(&p, sizeof(p), true));
    p.op = 0;
    CHECK(!coop_middle_overworld_pkt_valid(&p, sizeof(p), true));
    p.op = COOP_OWT_PROGRESS;
    p.flags = 0;
    p.slot = 8;
    CHECK(!coop_middle_overworld_pkt_valid(&p, sizeof(p), true));

    (void)x;
    (void)y;
}

/* 24. Bugfix: client trips follow the plotted waypoints; Teleport spell ops */
static void test_m5_route_teleport(void)
{
    PacketCoopOverworldTravel p;
    CoopOwtRoute r;
    int64_t out[COOP_OWT_ROUTE_MAX];
    int64_t a = LOCATION_MAKE(100, 200);
    int64_t b = LOCATION_MAKE(300, 400);
    int64_t d = LOCATION_MAKE(500, 600);
    int i;

    coop_middle_owt_route_reset(&r);
    /* no waypoints: only the destination */
    CHECK(coop_middle_owt_route_finish(&r, 0, d, out, COOP_OWT_ROUTE_MAX) == 1 && out[0] == d);
    /* two waypoints in order, then the destination: all three, in order */
    CHECK(coop_middle_owt_route_add(&r, 0, 2, a));
    CHECK(coop_middle_owt_route_add(&r, 1, 2, b));
    CHECK(coop_middle_owt_route_finish(&r, 2, d, out, COOP_OWT_ROUTE_MAX) == 3);
    CHECK(out[0] == a && out[1] == b && out[2] == d);
    CHECK(r.count == 0); /* consumed */
    /* a lost waypoint: START falls back to its destination only */
    CHECK(coop_middle_owt_route_add(&r, 0, 3, a));
    CHECK(!coop_middle_owt_route_add(&r, 2, 3, b));
    CHECK(coop_middle_owt_route_finish(&r, 3, d, out, COOP_OWT_ROUTE_MAX) == 1 && out[0] == d);
    /* count mismatch with START */
    CHECK(coop_middle_owt_route_add(&r, 0, 1, a));
    CHECK(coop_middle_owt_route_finish(&r, 2, d, out, COOP_OWT_ROUTE_MAX) == 1);
    /* a restart (idx 0) replaces an old partial route */
    CHECK(coop_middle_owt_route_add(&r, 0, 2, b));
    CHECK(coop_middle_owt_route_add(&r, 0, 1, a));
    CHECK(coop_middle_owt_route_finish(&r, 1, d, out, COOP_OWT_ROUTE_MAX) == 2 && out[0] == a);
    /* limits */
    CHECK(!coop_middle_owt_route_add(&r, 0, COOP_OWT_ROUTE_MAX, a));
    CHECK(!coop_middle_owt_route_add(&r, 0, 2, 0));
    CHECK(!coop_middle_owt_route_add(NULL, 0, 2, a));
    for (i = 0; i < COOP_OWT_ROUTE_MAX - 1; i++) {
        CHECK(coop_middle_owt_route_add(&r, i, COOP_OWT_ROUTE_MAX - 1, a + i));
    }
    CHECK(coop_middle_owt_route_finish(&r, COOP_OWT_ROUTE_MAX - 1, d, out, COOP_OWT_ROUTE_MAX) == COOP_OWT_ROUTE_MAX);
    CHECK(out[COOP_OWT_ROUTE_MAX - 1] == d);
    CHECK(coop_middle_owt_route_finish(&r, 0, 0, out, COOP_OWT_ROUTE_MAX) == 0);
    CHECK(coop_middle_owt_route_finish(&r, 0, d, NULL, 4) == 0);

    memset(&p, 0, sizeof(p));
    p.type = PACKET_COOP_OVERWORLD_TRAVEL;
    p.slot = 1;
    p.loc = a;
    p.op = COOP_OWT_WAYPOINT;
    p.world_x = 0;
    p.world_y = 2;
    CHECK(coop_middle_overworld_pkt_valid(&p, sizeof(p), false));
    CHECK(!coop_middle_overworld_pkt_valid(&p, sizeof(p), true));
    p.world_x = 2;
    CHECK(!coop_middle_overworld_pkt_valid(&p, sizeof(p), false)); /* idx < count */
    p.world_x = 0;
    p.world_y = COOP_OWT_ROUTE_MAX;
    CHECK(!coop_middle_overworld_pkt_valid(&p, sizeof(p), false));
    p.op = COOP_OWT_START;
    p.world_y = 0;
    p.world_x = COOP_OWT_ROUTE_MAX - 1;
    CHECK(coop_middle_overworld_pkt_valid(&p, sizeof(p), false));
    p.world_x = COOP_OWT_ROUTE_MAX;
    CHECK(!coop_middle_overworld_pkt_valid(&p, sizeof(p), false));
    p.flags = COOP_OWT_F_TELEPORT; /* spell id */
    CHECK(coop_middle_overworld_pkt_valid(&p, sizeof(p), false));
    p.world_x = COOP_OWT_SPELL_MAX;
    CHECK(!coop_middle_overworld_pkt_valid(&p, sizeof(p), false));
    p.world_x = 5;
    p.flags = COOP_OWT_F_TELEPORT | COOP_OWT_F_MOVING;
    CHECK(!coop_middle_overworld_pkt_valid(&p, sizeof(p), false));
    p.op = COOP_OWT_SELECT;
    p.flags = 0;
    p.loc = 0;
    CHECK(coop_middle_overworld_pkt_valid(&p, sizeof(p), true));
    CHECK(!coop_middle_overworld_pkt_valid(&p, sizeof(p), false));
    p.world_x = -1;
    CHECK(!coop_middle_overworld_pkt_valid(&p, sizeof(p), true));
    p.op = COOP_OWT_ARRIVED;
    p.world_x = 0;
    p.loc = d;
    p.flags = COOP_OWT_F_TELEPORT;
    CHECK(!coop_middle_overworld_pkt_valid(&p, sizeof(p), true));
    p.op = COOP_OWT_REFUSED;
    p.flags = 0;
    p.world_x = COOP_OWT_REFUSE_BLOCKED;
    CHECK(coop_middle_overworld_pkt_valid(&p, sizeof(p), true));
}

/* 23. M5 unified sleep: vote round for 1..8 players, times, ctrl ops */
static void test_m5_sleep(void)
{
    CoopSleepState st;
    PacketCoopPartyCtrl c;
    char text[192];
    int players;

    coop_middle_sleep_reset(&st);
    CHECK(st.ready_mask == 0 && st.initiator == -1);
    CHECK(coop_middle_sleep_step(&st, 0x01, COOP_SLEEP_EV_VOTE, 0, COOP_SLEEP_MODE_8H) == COOP_SLEEP_RES_SOLO);
    CHECK(st.initiator == -1);
    for (players = 2; players <= 8; players++) {
        uint8_t living = (uint8_t)((1u << players) - 1u);
        int s;

        coop_middle_sleep_reset(&st);
        CHECK(coop_middle_sleep_step(&st, living, COOP_SLEEP_EV_VOTE, players - 1, COOP_SLEEP_MODE_MORNING)
            == COOP_SLEEP_RES_WAIT);
        CHECK(st.initiator == players - 1 && st.mode == COOP_SLEEP_MODE_MORNING);
        for (s = 0; s < players - 2; s++) {
            CHECK(coop_middle_sleep_step(&st, living, COOP_SLEEP_EV_VOTE, s, COOP_SLEEP_MODE_1H) == COOP_SLEEP_RES_WAIT);
            CHECK(st.mode == COOP_SLEEP_MODE_MORNING); /* a later vote keeps the mode */
        }
        CHECK(coop_middle_sleep_step(&st, living, COOP_SLEEP_EV_CHECK, -1, 0) == COOP_SLEEP_RES_NONE);
        CHECK(coop_middle_sleep_step(&st, living, COOP_SLEEP_EV_VOTE, players - 2, COOP_SLEEP_MODE_1H)
            == COOP_SLEEP_RES_EXECUTE);
    }
    /* a dead / gone player does not block */
    coop_middle_sleep_reset(&st);
    CHECK(coop_middle_sleep_step(&st, 0x07, COOP_SLEEP_EV_VOTE, 0, COOP_SLEEP_MODE_8H) == COOP_SLEEP_RES_WAIT);
    CHECK(coop_middle_sleep_step(&st, 0x07, COOP_SLEEP_EV_VOTE, 1, COOP_SLEEP_MODE_8H) == COOP_SLEEP_RES_WAIT);
    CHECK(coop_middle_sleep_step(&st, 0x03, COOP_SLEEP_EV_CHECK, -1, 0) == COOP_SLEEP_RES_EXECUTE);
    /* the initiator cancels: the lowest ready slot leads, mode kept */
    coop_middle_sleep_reset(&st);
    coop_middle_sleep_step(&st, 0x0F, COOP_SLEEP_EV_VOTE, 2, COOP_SLEEP_MODE_HEALED);
    coop_middle_sleep_step(&st, 0x0F, COOP_SLEEP_EV_VOTE, 3, COOP_SLEEP_MODE_1H);
    CHECK(coop_middle_sleep_step(&st, 0x0F, COOP_SLEEP_EV_CANCEL, 2, 0) == COOP_SLEEP_RES_WAIT);
    CHECK(st.initiator == 3 && st.mode == COOP_SLEEP_MODE_HEALED && st.ready_mask == 0x08);
    CHECK(coop_middle_sleep_step(&st, 0x0F, COOP_SLEEP_EV_CANCEL, 3, 0) == COOP_SLEEP_RES_CLEARED);
    CHECK(st.initiator == -1 && st.ready_mask == 0);
    CHECK(coop_middle_sleep_step(&st, 0x0F, COOP_SLEEP_EV_CANCEL, 3, 0) == COOP_SLEEP_RES_NONE);
    /* the only ready slot dies: the round is cleared */
    coop_middle_sleep_step(&st, 0x0F, COOP_SLEEP_EV_VOTE, 1, COOP_SLEEP_MODE_2H);
    CHECK(coop_middle_sleep_step(&st, 0x0D, COOP_SLEEP_EV_CHECK, -1, 0) == COOP_SLEEP_RES_CLEARED);
    /* refusals */
    CHECK(coop_middle_sleep_step(&st, 0x03, COOP_SLEEP_EV_VOTE, 2, COOP_SLEEP_MODE_8H) == COOP_SLEEP_RES_REFUSED);
    CHECK(coop_middle_sleep_step(&st, 0x03, COOP_SLEEP_EV_VOTE, 1, 0) == COOP_SLEEP_RES_REFUSED);
    CHECK(coop_middle_sleep_step(&st, 0x03, COOP_SLEEP_EV_VOTE, 1, COOP_SLEEP_MODE_MAX + 1) == COOP_SLEEP_RES_REFUSED);
    CHECK(coop_middle_sleep_step(&st, 0x03, COOP_SLEEP_EV_VOTE, 9, COOP_SLEEP_MODE_8H) == COOP_SLEEP_RES_REFUSED);
    CHECK(coop_middle_sleep_step(NULL, 0x03, COOP_SLEEP_EV_VOTE, 1, 1) == COOP_SLEEP_RES_NONE);

    /* minutes: morning 08:00, evening 20:00, across midnight */
    CHECK(coop_middle_sleep_minutes(COOP_SLEEP_MODE_MORNING, 22, 30) == 9 * 60 + 30);
    CHECK(coop_middle_sleep_minutes(COOP_SLEEP_MODE_MORNING, 7, 0) == 60);
    CHECK(coop_middle_sleep_minutes(COOP_SLEEP_MODE_MORNING, 8, 0) == 24 * 60);
    CHECK(coop_middle_sleep_minutes(COOP_SLEEP_MODE_EVENING, 21, 0) == 23 * 60);
    CHECK(coop_middle_sleep_minutes(COOP_SLEEP_MODE_EVENING, 0, 15) == 19 * 60 + 45);
    CHECK(coop_middle_sleep_minutes(COOP_SLEEP_MODE_8H, 3, 3) == 480);
    CHECK(coop_middle_sleep_minutes(COOP_SLEEP_MODE_24H, 3, 3) == 1440);
    CHECK(coop_middle_sleep_minutes(COOP_SLEEP_MODE_HEALED, 3, 3) == -1);
    CHECK(coop_middle_sleep_minutes(COOP_SLEEP_MODE_8H, 24, 0) == -1);
    CHECK(coop_middle_sleep_mode_from_ui(1) == COOP_SLEEP_MODE_1H && coop_middle_sleep_mode_from_ui(2) == COOP_SLEEP_MODE_2H);
    CHECK(coop_middle_sleep_mode_from_ui(4) == COOP_SLEEP_MODE_4H && coop_middle_sleep_mode_from_ui(8) == COOP_SLEEP_MODE_8H);
    CHECK(coop_middle_sleep_mode_from_ui(24) == COOP_SLEEP_MODE_24H);
    CHECK(coop_middle_sleep_mode_from_ui(-1) == COOP_SLEEP_MODE_MORNING);
    CHECK(coop_middle_sleep_mode_from_ui(-2) == COOP_SLEEP_MODE_EVENING);
    CHECK(coop_middle_sleep_mode_from_ui(-3) == COOP_SLEEP_MODE_HEALED);
    CHECK(coop_middle_sleep_mode_from_ui(3) == 0);
    CHECK(coop_middle_sleep_count(0xFF) == 8 && coop_middle_sleep_count(0) == 0);

    coop_middle_sleep_banner_text(text, sizeof(text), 2, 3, "Bob", COOP_SLEEP_MODE_MORNING, false);
    CHECK(strcmp(text, "Sleep: 2/3 ready (Bob: until morning). Y = join sleep, N = cancel") == 0);
    coop_middle_sleep_banner_text(text, sizeof(text), 2, 3, "Bob", COOP_SLEEP_MODE_HEALED, true);
    CHECK(strstr(text, "готовы 2/3") != NULL && strstr(text, "до исцеления") != NULL);
    coop_middle_sleep_banner_text(text, 8, 2, 3, "Bob", 1, false);
    CHECK(strlen(text) < 8);

    /* ctrl ops */
    memset(&c, 0, sizeof(c));
    c.type = PACKET_COOP_PARTY_CTRL;
    c.op = COOP_CTRL_SLEEP_VOTE;
    c.a = COOP_SLEEP_MODE_24H;
    CHECK(coop_middle_party_ctrl_valid(&c));
    c.a = 0;
    CHECK(coop_middle_party_ctrl_valid(&c)); /* cancel */
    c.a = COOP_SLEEP_MODE_MAX + 1;
    CHECK(!coop_middle_party_ctrl_valid(&c));
    c.a = 1;
    c.b = 1;
    CHECK(!coop_middle_party_ctrl_valid(&c));
    c.op = COOP_CTRL_SLEEP_STATE;
    c.a = 0x03 | (COOP_SLEEP_MODE_MORNING << 8);
    c.b = 1 | (3 << 8);
    CHECK(coop_middle_party_ctrl_valid(&c));
    c.b = 0xFF;
    CHECK(coop_middle_party_ctrl_valid(&c)); /* no round */
    c.b = 9;
    CHECK(!coop_middle_party_ctrl_valid(&c));
    c.b = 1 | (9 << 8);
    CHECK(!coop_middle_party_ctrl_valid(&c));
    c.b = 1;
    c.a = 0x03 | (9 << 8);
    CHECK(!coop_middle_party_ctrl_valid(&c));
    c.a = -1;
    CHECK(!coop_middle_party_ctrl_valid(&c));
}

/* ---- M6: solo sub-instances ---- */

static void test_m6_filenames(void)
{
    char longname[80];

    CHECK(coop_subinst_filename_safe("12345.dif"));
    CHECK(coop_subinst_filename_safe("mobile.mdy"));
    CHECK(coop_subinst_filename_safe("MOBILE.MD"));
    CHECK(coop_subinst_filename_safe("mobile.des"));
    CHECK(coop_subinst_filename_safe("a-b_c.DIF"));
    CHECK(!coop_subinst_filename_safe(NULL));
    CHECK(!coop_subinst_filename_safe(""));
    CHECK(!coop_subinst_filename_safe(".dif"));
    CHECK(!coop_subinst_filename_safe("../x.dif"));
    CHECK(!coop_subinst_filename_safe("..dif"));
    CHECK(!coop_subinst_filename_safe("a..b.dif"));
    CHECK(!coop_subinst_filename_safe("dir/x.dif"));
    CHECK(!coop_subinst_filename_safe("dir\\x.dif"));
    CHECK(!coop_subinst_filename_safe("c:x.dif"));
    CHECK(!coop_subinst_filename_safe("x.sav"));
    CHECK(!coop_subinst_filename_safe("x.dif.exe"));
    CHECK(!coop_subinst_filename_safe("x y.dif"));
    memset(longname, 'a', sizeof(longname));
    strcpy(longname + 59, ".dif"); /* 63 chars */
    CHECK(coop_subinst_filename_safe(longname));
    memset(longname, 'a', sizeof(longname));
    strcpy(longname + 60, ".dif"); /* 64 chars */
    CHECK(!coop_subinst_filename_safe(longname));
}

static int m6_roundtrip(int nfiles)
{
    static uint8_t wire[COOP_SUBINST_MAX_WIRE];
    static uint8_t raw[COOP_SUBINST_MAX_RAW];
    static char names[COOP_SUBINST_MAX_FILES][COOP_SUBINST_NAME_MAX + 1];
    CoopSubinstFile in[32];
    CoopSubinstFile out[COOP_SUBINST_MAX_FILES];
    char nbuf[32][32];
    uint8_t data[32][300];
    size_t wire_len = 0;
    uint32_t map_id = 0;
    int count = -1;
    int i;
    int rc;

    for (i = 0; i < nfiles; i++) {
        size_t k;

        snprintf(nbuf[i], sizeof(nbuf[i]), "%d.dif", 1000 + i);
        for (k = 0; k < sizeof(data[i]); k++) {
            data[i][k] = (uint8_t)(i * 7 + k);
        }
        in[i].name = nbuf[i];
        in[i].data = data[i];
        in[i].len = (uint32_t)(i * 13 % 300);
    }
    rc = coop_subinst_pack_buf(4242u, in, nfiles, wire, sizeof(wire), &wire_len);
    if (rc != COOP_SUBINST_OK) {
        return 1;
    }
    rc = coop_subinst_unpack_buf(wire, wire_len, raw, sizeof(raw), &map_id, out, names, COOP_SUBINST_MAX_FILES,
        &count);
    if (rc != COOP_SUBINST_OK || map_id != 4242u || count != nfiles) {
        return 2;
    }
    for (i = 0; i < nfiles; i++) {
        if (strcmp(out[i].name, nbuf[i]) != 0 || out[i].len != in[i].len
            || (in[i].len > 0 && memcmp(out[i].data, in[i].data, in[i].len) != 0)) {
            return 3;
        }
    }
    return 0;
}

static void test_m6_archive(void)
{
    static uint8_t wire[COOP_SUBINST_MAX_WIRE];
    static uint8_t raw[COOP_SUBINST_MAX_RAW];
    static char names[COOP_SUBINST_MAX_FILES][COOP_SUBINST_NAME_MAX + 1];
    CoopSubinstFile out[COOP_SUBINST_MAX_FILES];
    CoopSubinstFile in[3];
    static const uint8_t body[] = "map differences";
    size_t wire_len = 0;
    uint32_t map_id = 0;
    int count = 0;

    CHECK(m6_roundtrip(0) == 0);
    CHECK(m6_roundtrip(1) == 0);
    CHECK(m6_roundtrip(20) == 0);

    in[0].name = "mobile.md";
    in[0].data = body;
    in[0].len = sizeof(body);
    in[1].name = "../evil.dif";
    in[1].data = body;
    in[1].len = sizeof(body);
    CHECK(coop_subinst_pack_buf(7u, in, 2, wire, sizeof(wire), &wire_len) == COOP_SUBINST_E_NAME);
    in[1].name = "MOBILE.md"; /* duplicate, case-insensitive */
    CHECK(coop_subinst_pack_buf(7u, in, 2, wire, sizeof(wire), &wire_len) != COOP_SUBINST_OK
        || coop_subinst_unpack_buf(wire, wire_len, raw, sizeof(raw), &map_id, out, names, COOP_SUBINST_MAX_FILES,
               &count)
            != COOP_SUBINST_OK);
    CHECK(coop_subinst_pack_buf(7u, in, COOP_SUBINST_MAX_FILES + 1, wire, sizeof(wire), &wire_len)
        != COOP_SUBINST_OK);

    CHECK(coop_subinst_pack_buf(7u, in, 1, wire, sizeof(wire), &wire_len) == COOP_SUBINST_OK);
    CHECK(coop_subinst_unpack_buf(wire, wire_len, raw, sizeof(raw), &map_id, out, names, COOP_SUBINST_MAX_FILES,
              &count)
            == COOP_SUBINST_OK
        && map_id == 7u && count == 1 && strcmp(out[0].name, "mobile.md") == 0);
    /* truncated */
    CHECK(coop_subinst_unpack_buf(wire, wire_len - 1, raw, sizeof(raw), &map_id, out, names,
              COOP_SUBINST_MAX_FILES, &count)
        != COOP_SUBINST_OK);
    CHECK(coop_subinst_unpack_buf(wire, 3, raw, sizeof(raw), &map_id, out, names, COOP_SUBINST_MAX_FILES, &count)
        != COOP_SUBINST_OK);
    /* raw size field over the limit */
    {
        uint8_t bad[64];

        memcpy(bad, wire, wire_len < sizeof(bad) ? wire_len : sizeof(bad));
        bad[0] = 0xFF;
        bad[1] = 0xFF;
        bad[2] = 0xFF;
        bad[3] = 0x7F;
        CHECK(coop_subinst_unpack_buf(bad, wire_len < sizeof(bad) ? wire_len : sizeof(bad), raw, sizeof(raw),
                  &map_id, out, names, COOP_SUBINST_MAX_FILES, &count)
            != COOP_SUBINST_OK);
    }
    /* trailing garbage */
    {
        uint8_t* big = (uint8_t*)malloc(wire_len + 4);

        memcpy(big, wire, wire_len);
        memset(big + wire_len, 0, 4);
        CHECK(coop_subinst_unpack_buf(big, wire_len + 4, raw, sizeof(raw), &map_id, out, names,
                  COOP_SUBINST_MAX_FILES, &count)
            != COOP_SUBINST_OK);
        free(big);
    }
    /* corrupted zlib stream / CRC */
    {
        uint8_t* bad = (uint8_t*)malloc(wire_len);
        size_t i;
        int fails = 0;

        for (i = 4; i < wire_len; i++) {
            memcpy(bad, wire, wire_len);
            bad[i] ^= 0x5A;
            if (coop_subinst_unpack_buf(bad, wire_len, raw, sizeof(raw), &map_id, out, names,
                    COOP_SUBINST_MAX_FILES, &count)
                != COOP_SUBINST_OK) {
                fails++;
            }
        }
        CHECK(fails == (int)(wire_len - 4));
        free(bad);
    }
    /* raw buffer too small */
    CHECK(coop_subinst_unpack_buf(wire, wire_len, raw, 8, &map_id, out, names, COOP_SUBINST_MAX_FILES, &count)
        != COOP_SUBINST_OK);
}

static void test_m6_payload(void)
{
    uint8_t buf[512];
    uint8_t qbuf[4 + 3 * COOP_SUBINST_QTY_ENTRY];
    CoopSubinstSection a = { (const uint8_t*)"CMAPDATA", 8 };
    CoopSubinstSection b = { (const uint8_t*)"BUNDLE", 6 };
    CoopSubinstSection q = { NULL, 0 };
    CoopSubinstSection ca;
    CoopSubinstSection cb;
    CoopSubinstSection cq;
    CoopSubinstQty items[3];
    CoopSubinstQty back[3];
    size_t len = 0;
    size_t qlen;

    memset(items, 0, sizeof(items));
    items[0].oid[0] = 1;
    items[0].qty = 500;
    items[1].oid[23] = 9;
    items[1].qty = 0;
    qlen = coop_subinst_qty_build(items, 2, qbuf, sizeof(qbuf));
    CHECK(qlen == 4 + 2 * COOP_SUBINST_QTY_ENTRY);
    CHECK(coop_subinst_qty_parse(qbuf, qlen, back, 3) == 2 && back[0].qty == 500 && back[1].oid[23] == 9);
    CHECK(coop_subinst_qty_parse(qbuf, qlen - 1, back, 3) < 0);
    CHECK(coop_subinst_qty_parse(qbuf, qlen, back, 1) < 0);
    q.data = qbuf;
    q.len = qlen;
    CHECK(coop_subinst_payload_size(8, 6, qlen) == 12 + 8 + 6 + qlen);
    CHECK(coop_subinst_payload_build(a, b, q, buf, sizeof(buf), &len) && len == 12 + 8 + 6 + qlen);
    CHECK(coop_subinst_payload_split(buf, len, &ca, &cb, &cq) && ca.len == 8 && memcmp(ca.data, "CMAPDATA", 8) == 0
        && cb.len == 6 && cq.len == qlen);
    CHECK(!coop_subinst_payload_split(buf, len - 1, &ca, &cb, &cq));
    CHECK(!coop_subinst_payload_build(a, b, q, buf, 10, &len));
    b.len = 0;
    CHECK(coop_subinst_payload_build(a, b, q, buf, sizeof(buf), &len)
        && coop_subinst_payload_split(buf, len, &ca, &cb, &cq) && cb.len == 0);
}

static void test_m6_chunks(void)
{
    CoopSubinstRx rx;
    uint8_t data[9000];
    size_t i;

    memset(&rx, 0, sizeof(rx));
    for (i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)i;
    }
    CHECK(coop_subinst_chunk_count(0) == 0);
    CHECK(coop_subinst_chunk_count(1) == 1);
    CHECK(coop_subinst_chunk_count(4096) == 1);
    CHECK(coop_subinst_chunk_count(4097) == 2);
    CHECK(coop_subinst_chunk_count(COOP_SUBINST_MAX_PAYLOAD + 1) == 0);
    CHECK(!coop_subinst_rx_begin(&rx, 0, 3, sizeof(data), COOP_SUBINST_MAX_WIRE));
    CHECK(!coop_subinst_rx_begin(&rx, 5, 2, sizeof(data), COOP_SUBINST_MAX_WIRE));
    CHECK(!coop_subinst_rx_begin(&rx, 5, 3, sizeof(data), 100));
    CHECK(coop_subinst_rx_begin(&rx, 5, 3, sizeof(data), COOP_SUBINST_MAX_WIRE));
    CHECK(coop_subinst_rx_accept(&rx, 6, 0, 3, 4096, data) == COOP_SUBINST_RX_STALE);
    CHECK(coop_subinst_rx_accept(&rx, 5, 0, 3, 4096, data) == COOP_SUBINST_RX_MORE);
    CHECK(coop_subinst_rx_accept(&rx, 5, 1, 3, 4096, data + 4096) == COOP_SUBINST_RX_MORE);
    CHECK(coop_subinst_rx_accept(&rx, 5, 2, 3, 9000 - 8192, data + 8192) == COOP_SUBINST_RX_DONE);
    CHECK(rx.total == sizeof(data) && memcmp(rx.data, data, sizeof(data)) == 0);
    coop_subinst_rx_free(&rx);
    /* out of order / wrong size = broken stream */
    CHECK(coop_subinst_rx_begin(&rx, 5, 3, sizeof(data), COOP_SUBINST_MAX_WIRE));
    CHECK(coop_subinst_rx_accept(&rx, 5, 1, 3, 4096, data) == COOP_SUBINST_RX_BAD);
    CHECK(!rx.active && rx.data == NULL);
    CHECK(coop_subinst_rx_begin(&rx, 5, 3, sizeof(data), COOP_SUBINST_MAX_WIRE));
    CHECK(coop_subinst_rx_accept(&rx, 5, 0, 3, 100, data) == COOP_SUBINST_RX_BAD);
    CHECK(coop_subinst_rx_begin(&rx, 5, 3, sizeof(data), COOP_SUBINST_MAX_WIRE));
    CHECK(coop_subinst_rx_accept(&rx, 5, 0, 4, 4096, data) == COOP_SUBINST_RX_BAD);
    coop_subinst_rx_free(&rx);
}

static void test_m6_lease(void)
{
    CoopSubinstLease leases[8];
    int other = 0;

    memset(leases, 0, sizeof(leases));
    /* not sub-instance cases */
    CHECK(coop_subinst_can_lease(10, 10, 99, 1, leases, false, false, &other) == COOP_SUBINST_LEASE_NORMAL);
    CHECK(coop_subinst_can_lease(10, 99, 99, 1, leases, false, false, &other) == COOP_SUBINST_LEASE_NORMAL);
    CHECK(coop_subinst_can_lease(10, 0, 99, 1, leases, false, false, &other) == COOP_SUBINST_LEASE_NORMAL);
    CHECK(coop_subinst_can_lease(10, 20, 99, 0, leases, false, false, &other) == COOP_SUBINST_LEASE_NORMAL);
    CHECK(coop_subinst_can_lease(10, 20, 99, 1, leases, false, false, &other) == COOP_SUBINST_LEASE_OK
        && other == -1);
    CHECK(coop_subinst_can_lease(10, 20, 99, 1, leases, true, false, &other) == COOP_SUBINST_LEASE_DENY_STATE);
    CHECK(coop_subinst_can_lease(10, 20, 99, 1, leases, false, true, &other) == COOP_SUBINST_LEASE_DENY_STATE);
    leases[2].active = true;
    leases[2].map_id = 20;
    leases[2].lease_id = 7;
    CHECK(coop_subinst_lease_owner(leases, 20) == 2);
    CHECK(coop_subinst_lease_owner(leases, 21) == -1);
    CHECK(coop_subinst_can_lease(10, 20, 99, 1, leases, false, false, &other) == COOP_SUBINST_LEASE_DENY_BUSY
        && other == 2);
    CHECK(coop_subinst_can_lease(10, 20, 99, 2, leases, false, false, &other) == COOP_SUBINST_LEASE_OK);
    CHECK(coop_subinst_can_lease(10, 21, 99, 1, leases, false, false, &other) == COOP_SUBINST_LEASE_OK);
}

static void test_m6_state_machine(void)
{
    int s = COOP_SUBINST_ST_IDLE;

    s = coop_subinst_client_next(s, COOP_SUBINST_EV_REQUEST);
    CHECK(s == COOP_SUBINST_ST_REQUESTED);
    CHECK(coop_subinst_client_next(s, COOP_SUBINST_EV_DENY) == COOP_SUBINST_ST_IDLE);
    s = coop_subinst_client_next(s, COOP_SUBINST_EV_GRANT);
    CHECK(s == COOP_SUBINST_ST_RECEIVING);
    CHECK(coop_subinst_client_next(s, COOP_SUBINST_EV_ARCHIVE_BAD) == COOP_SUBINST_ST_IDLE);
    CHECK(coop_subinst_client_next(s, COOP_SUBINST_EV_COMMIT) == -1);
    s = coop_subinst_client_next(s, COOP_SUBINST_EV_ARCHIVE_OK);
    CHECK(s == COOP_SUBINST_ST_ACTIVE);
    CHECK(coop_subinst_client_next(s, COOP_SUBINST_EV_GRANT) == -1);
    CHECK(coop_subinst_client_next(s, COOP_SUBINST_EV_ABORT) == COOP_SUBINST_ST_IDLE);
    s = coop_subinst_client_next(s, COOP_SUBINST_EV_COMMIT);
    CHECK(s == COOP_SUBINST_ST_COMMITTING);
    CHECK(coop_subinst_client_next(s, COOP_SUBINST_EV_ACK) == COOP_SUBINST_ST_IDLE);
    CHECK(coop_subinst_client_next(s, COOP_SUBINST_EV_ACK_NEXT) == COOP_SUBINST_ST_REQUESTED);
    CHECK(coop_subinst_client_next(s, COOP_SUBINST_EV_RESET) == COOP_SUBINST_ST_IDLE);
    CHECK(coop_subinst_client_next(COOP_SUBINST_ST_IDLE, COOP_SUBINST_EV_ACK) == -1);
}

static void test_m6_packets(void)
{
    PacketCoopSubinstCtrl c;
    PacketCoopSubinstChunk k;
    PacketCoopSubinstDelta d;

    CHECK(TIG_NET_PROTOCOL >= 59);
    CHECK(PACKET_COOP_SUBINST_CTRL == 171 && PACKET_COOP_SUBINST_CHUNK == 172 && PACKET_COOP_SUBINST_DELTA == 173);
    CHECK(PACKET_COOP_SUBINST_DELTA < PACKET_COOP_TYPE_LIMIT);
    CHECK(sizeof(PacketCoopSubinstCtrl) == 0x40 && sizeof(PacketCoopSubinstDelta) == 0x18);

    memset(&c, 0, sizeof(c));
    c.type = PACKET_COOP_SUBINST_CTRL;
    c.op = COOP_SUBINST_OP_LEASE_GRANT;
    c.lease_id = 3;
    c.map_id = 12;
    c.total_bytes = 5000;
    c.total_chunks = 2;
    CHECK(coop_subinst_ctrl_valid(&c, sizeof(c), true));
    CHECK(!coop_subinst_ctrl_valid(&c, sizeof(c), false)); /* GRANT from a client */
    CHECK(!coop_subinst_ctrl_valid(&c, sizeof(c) - 1, true));
    c.total_chunks = 3;
    CHECK(!coop_subinst_ctrl_valid(&c, sizeof(c), true));
    c.total_chunks = 2;
    c.total_bytes = COOP_SUBINST_MAX_WIRE + 1;
    c.total_chunks = coop_subinst_chunk_count((size_t)c.total_bytes);
    CHECK(!coop_subinst_ctrl_valid(&c, sizeof(c), true));
    c.op = COOP_SUBINST_OP_COMMIT_BEGIN;
    c.total_bytes = 5000;
    c.total_chunks = 2;
    CHECK(coop_subinst_ctrl_valid(&c, sizeof(c), false));
    CHECK(!coop_subinst_ctrl_valid(&c, sizeof(c), true));
    c.flags = COOP_SUBINST_F_DEAD;
    CHECK(coop_subinst_ctrl_valid(&c, sizeof(c), false));
    c.flags = COOP_SUBINST_F_NEXT;
    CHECK(!coop_subinst_ctrl_valid(&c, sizeof(c), false));
    c.flags = 0x10;
    CHECK(!coop_subinst_ctrl_valid(&c, sizeof(c), false));
    c.flags = 0;
    c.hp_damage = -1;
    CHECK(!coop_subinst_ctrl_valid(&c, sizeof(c), false));
    c.hp_damage = 0;
    c.lease_id = 0;
    CHECK(!coop_subinst_ctrl_valid(&c, sizeof(c), false));
    memset(&c, 0, sizeof(c));
    c.type = PACKET_COOP_SUBINST_CTRL;
    c.op = COOP_SUBINST_OP_LEASE_DENY;
    c.reason = COOP_SUBINST_DENY_BUSY;
    c.slot = 2;
    CHECK(coop_subinst_ctrl_valid(&c, sizeof(c), true));
    c.reason = 9;
    CHECK(!coop_subinst_ctrl_valid(&c, sizeof(c), true));
    c.op = COOP_SUBINST_OP_STATE;
    c.slot = 0;
    CHECK(!coop_subinst_ctrl_valid(&c, sizeof(c), true));
    c.slot = 3;
    CHECK(coop_subinst_ctrl_valid(&c, sizeof(c), true));
    c.op = 99;
    CHECK(!coop_subinst_ctrl_valid(&c, sizeof(c), true));

    memset(&k, 0, sizeof(k));
    k.type = PACKET_COOP_SUBINST_CHUNK;
    k.lease_id = 3;
    k.chunk_count = 2;
    k.chunk_idx = 1;
    k.data_len = 10;
    CHECK(coop_subinst_chunk_valid(&k, COOP_SUBINST_CHUNK_HDR + 10));
    CHECK(!coop_subinst_chunk_valid(&k, COOP_SUBINST_CHUNK_HDR + 11));
    k.chunk_idx = 2;
    CHECK(!coop_subinst_chunk_valid(&k, COOP_SUBINST_CHUNK_HDR + 10));
    k.chunk_idx = 0;
    k.data_len = COOP_SUBINST_CHUNK + 1;
    CHECK(!coop_subinst_chunk_valid(&k, COOP_SUBINST_CHUNK_HDR + COOP_SUBINST_CHUNK + 1));
    k.data_len = 0;
    CHECK(!coop_subinst_chunk_valid(&k, COOP_SUBINST_CHUNK_HDR));

    memset(&d, 0, sizeof(d));
    d.type = PACKET_COOP_SUBINST_DELTA;
    d.lease_id = 3;
    d.kind = COOP_SUBINST_DELTA_GF;
    d.index = 5;
    d.val = 1;
    CHECK(coop_subinst_delta_valid(&d, sizeof(d)));
    d.val = 2;
    CHECK(!coop_subinst_delta_valid(&d, sizeof(d)));
    d.kind = COOP_SUBINST_DELTA_GV;
    d.index = COOP_SUBINST_GV_MAX;
    CHECK(!coop_subinst_delta_valid(&d, sizeof(d)));
    d.kind = COOP_SUBINST_DELTA_XP;
    d.index = 0;
    d.val = 150;
    CHECK(coop_subinst_delta_valid(&d, sizeof(d)));
    d.val = -5;
    CHECK(!coop_subinst_delta_valid(&d, sizeof(d)));
    d.kind = COOP_SUBINST_DELTA_REP;
    d.index = 4;
    d.val = -1;
    CHECK(coop_subinst_delta_valid(&d, sizeof(d)));
    d.kind = COOP_SUBINST_DELTA_FATE;
    d.index = COOP_SUBINST_FATE_MAX;
    d.val = 1;
    CHECK(!coop_subinst_delta_valid(&d, sizeof(d)));
    d.kind = 0;
    CHECK(!coop_subinst_delta_valid(&d, sizeof(d)));
    d.kind = COOP_SUBINST_DELTA_GF;
    d.index = 1;
    d.val = 1;
    d.lease_id = 0;
    CHECK(!coop_subinst_delta_valid(&d, sizeof(d)));
}

/* M7: off-screen arrows, collapse notice, autosave gate / rotation, asave
   rule, SLEEP_APPLY, session reset. */
static bool m7_on_border(int x, int y, int w, int h, int pad)
{
    bool in_x = x >= pad && x <= w - pad;
    bool in_y = y >= pad && y <= h - pad;

    return in_x && in_y && (x == pad || x == w - pad || y == pad || y == h - pad);
}

/* t391u: Coop Lite mode (rule lite) and the party dialog gate. */
static void test_lite_mode(void)
{
    CoopRules r;
    CoopRules e;
    PacketCoopRules pkt;
    char text[160];
    char desc[1024];
    const uint32_t middle = COOP_RULE_GOLD_SPLIT | COOP_RULE_REST_VOTE | COOP_RULE_DIRECT_TRADE
        | COOP_RULE_CLIENT_PAUSE | COOP_RULE_NO_FRIENDLY_FIRE_AOE | COOP_RULE_SHARED_QUEST_ITEMS
        | COOP_RULE_SHARED_FOLLOWERS | COOP_RULE_FREE_OVERWORLD | COOP_RULE_UNIFIED_SLEEP | COOP_RULE_SUBINSTANCES
        | COOP_RULE_FOLLOWER_LEVEL | COOP_RULE_PARTY_LEVEL;

    /* default: off, and not in the env string (older strings unchanged) */
    coop_rules_default(&r);
    CHECK(!coop_rules_is_lite(&r));
    CHECK((COOP_RULE_DEFAULT_FLAGS & COOP_RULE_LITE_MODE) == 0);
    coop_rules_format_env(&r, text, sizeof(text));
    CHECK(strstr(text, "lite") == NULL);
    e = r;
    coop_rules_effective(&e);
    CHECK(coop_rules_equal(&r, &e)); /* lite=0: the rules as set */
    /* 2026-11-27: no ow box - the free world map is always on in Middle */
    coop_rules_parse_env(&r, "ow=0");
    CHECK((r.flags & COOP_RULE_FREE_OVERWORLD) == 0); /* still parsed */
    coop_rules_effective(&r);
    CHECK((r.flags & COOP_RULE_FREE_OVERWORLD) != 0);
    coop_rules_parse_env(&r, "ow=0,lite=1");
    coop_rules_effective(&r);
    CHECK((r.flags & COOP_RULE_FREE_OVERWORLD) == 0);

    /* lite=1 with everything on: the Coop Lite 1.0 set */
    CHECK(coop_rules_parse_env(&r, "gold=1,loot=1,rest=1,trade=1,pause=1,ff=1,qitems=1,sfol=1,ow=1,usleep=1,"
                                   "subinst=1,hp=300,xp=250,asave=30,lite=1")
        == 15);
    CHECK(coop_rules_is_lite(&r));
    CHECK((r.flags & middle) == middle); /* parse keeps the boxes; effective decides */
    coop_rules_format_env(&r, text, sizeof(text));
    CHECK(strstr(text, ",lite=1") != NULL);
    coop_rules_effective(&r);
    CHECK(r.flags == (COOP_RULE_LITE_MODE | COOP_RULE_LOOT_FEED));
    CHECK((r.flags & middle) == 0);
    CHECK(r.enemy_hp_pct == 100 && r.xp_mult_pct == 100);
    CHECK(r.autosave_min == 30); /* kept */
    e = r;
    coop_rules_effective(&e);
    CHECK(coop_rules_equal(&r, &e)); /* idempotent */
    CHECK(coop_rules_parse_env(&r, "loot=0,lite=on") == 2);
    coop_rules_effective(&r);
    CHECK(r.flags == COOP_RULE_LITE_MODE);
    /* aliases (launcher ini name, spelled out) */
    coop_rules_parse_env(&r, "lite_mode=1");
    CHECK(coop_rules_is_lite(&r));
    coop_rules_parse_env(&r, "coop_lite=yes");
    CHECK(coop_rules_is_lite(&r));
    coop_rules_parse_env(&r, "lite=0");
    CHECK(!coop_rules_is_lite(&r));
    /* the wire: the host sends the effective rules, the bit survives */
    coop_rules_parse_env(&r, "lite=1");
    coop_rules_effective(&r);
    coop_rules_packet_fill(&pkt, &r);
    CHECK(pkt.rules.flags == (COOP_RULE_LITE_MODE | COOP_RULE_LOOT_FEED));
    /* an earlier protocol-59 peer drops the unknown bit and still sees no Middle rule */
    CHECK((pkt.rules.flags & COOP_RULE_ACTIVE_MASK & middle) == 0);
    /* a client that gets lite with Middle bits still (bad host) plays Lite */
    r.flags = COOP_RULE_LITE_MODE | middle;
    r.enemy_hp_pct = 200;
    coop_rules_effective(&r);
    CHECK(r.flags == COOP_RULE_LITE_MODE && r.enemy_hp_pct == 100);
    coop_rules_describe(&r, false, desc, sizeof(desc));
    CHECK(strncmp(desc, "Coop Lite mode (as release 1.0). Rules:", 39) == 0);
    CHECK(strstr(desc, "free world map off") != NULL);
    coop_rules_describe(&r, true, desc, sizeof(desc));
    CHECK(strstr(desc, "Coop Lite") != NULL);
    coop_rules_effective(NULL);
    CHECK(!coop_rules_is_lite(NULL));
    CHECK(sizeof(CoopRules) == 28 && sizeof(PacketCoopRules) == 32);

    /* party dialog gate: Middle = anywhere on the map, parallel dialogs */
    CHECK(coop_lite_party_talk_gate(2, 1, 100000, false, false, false, false, false) == 0);
    CHECK(coop_lite_party_talk_gate(2, 1, -1, false, false, false, false, false) == 0);
    CHECK(coop_lite_party_talk_gate(2, 1, COOP_PARTY_TALK_MAX_HOST_DIST + 1, false, true, true, false, false) == 0);
    CHECK(coop_lite_party_talk_gate(2, COOP_PARTY_TALK_MAX_DIST + 1, 1, false, false, false, false, false) == 2);
    CHECK(coop_lite_party_talk_gate(2, 1, 1, true, false, false, false, false) == 4);
    CHECK(coop_lite_party_talk_gate(2, 1, 1, false, false, false, true, false) == 4); /* own fight */
    CHECK(coop_lite_party_talk_gate(0, 1, 1, false, false, false, false, false) == 1);
    /* Lite mode = PT41 as released */
    CHECK(coop_lite_party_talk_gate(2, 1, 10, false, false, false, false, true) == 0);
    CHECK(coop_lite_party_talk_gate(2, 1, COOP_PARTY_TALK_MAX_HOST_DIST + 1, false, false, false, false, true) == 3);
    CHECK(coop_lite_party_talk_gate(2, 1, -1, false, false, false, false, true) == 3);
    CHECK(coop_lite_party_talk_gate(2, 1, 10, false, true, false, false, true) == 4); /* one at a time */
    CHECK(coop_lite_party_talk_gate(2, 1, 10, false, false, true, false, true) == 4); /* host fights */
    CHECK(coop_lite_party_talk_gate(2, 1, 10, false, false, false, true, true) == 0);
    CHECK(coop_lite_party_talk_gate(2, COOP_PARTY_TALK_MAX_DIST + 1, 10, false, false, false, false, true) == 2);
    CHECK(coop_lite_party_talk_gate(2, 1, 10, false, false, false, false, true)
        == coop_lite_party_talk_check(2, 1, 10, false));
    /* the stubs play Middle by default */
    CHECK(!coop_middle_lite_mode());
}

/* After M7: auto-levelling rules (flvl / plvl) and their pure helpers. */
/* t402u / t403u: same-map hops solo in Middle; client follower lists. */
static void test_hop_and_followers(void)
{
    int64_t have[6];
    bool ok[6];
    int64_t want[4];
    int64_t out[8];
    bool changed = false;
    int n;

    /* Middle: same map (or -1 = same) is solo; a map change is not; Lite never. */
    CHECK(coop_middle_solo_hop_rule(true, 7, 7));
    CHECK(coop_middle_solo_hop_rule(true, 7, -1));
    CHECK(coop_middle_solo_hop_rule(true, 1, 1));
    CHECK(!coop_middle_solo_hop_rule(true, 7, 8));
    CHECK(!coop_middle_solo_hop_rule(false, 7, 7));
    CHECK(!coop_middle_solo_hop_rule(false, 1, 1));
    CHECK(!coop_middle_solo_hop_rule(true, 0, 0));
    CHECK(!coop_middle_solo_hop_rule(true, -1, -1));
    /* the stub (no session) never hops solo */
    CHECK(!coop_middle_host_solo_hop(7));

    /* consistent list: unchanged */
    have[0] = 0x101; ok[0] = true;
    have[1] = 0x102; ok[1] = true;
    want[0] = 0x102; want[1] = 0x101;
    n = coop_middle_follower_list_plan(have, ok, 2, want, 2, out, 8, &changed);
    CHECK(n == 2 && !changed && out[0] == 0x101 && out[1] == 0x102);

    /* t403u: null / stale entries dropped, missing table followers added */
    have[0] = 0;     ok[0] = false;
    have[1] = 0x201; ok[1] = true;
    have[2] = 0x999; ok[2] = false; /* freed handle */
    have[3] = 0x201; ok[3] = true;  /* duplicate */
    want[0] = 0x202; want[1] = 0x201; want[2] = 0; want[3] = 0x203;
    n = coop_middle_follower_list_plan(have, ok, 4, want, 4, out, 8, &changed);
    CHECK(changed && n == 3 && out[0] == 0x201 && out[1] == 0x202 && out[2] == 0x203);

    /* t402u: leader field set, list empty (world state / load): added */
    n = coop_middle_follower_list_plan(have, ok, 0, want, 1, out, 8, &changed);
    CHECK(changed && n == 1 && out[0] == 0x202);

    /* capacity respected */
    have[0] = 1; have[1] = 2; have[2] = 3; ok[0] = ok[1] = ok[2] = true;
    want[0] = 4; want[1] = 5;
    n = coop_middle_follower_list_plan(have, ok, 3, want, 2, out, 4, &changed);
    CHECK(n == 4 && changed && out[3] == 4);
    n = coop_middle_follower_list_plan(NULL, NULL, 0, NULL, 0, out, 4, &changed);
    CHECK(n == 0 && !changed);
    printf("PASS: same-map solo hop (Middle) / client follower list repair\n");
}

static void test_autolevel(void)
{
    CoopRules r;
    char text[200];
    char desc[1024];
    int xp[4] = { 2100, 450000, 0, 450000 };

    coop_rules_default(&r);
    CHECK((r.flags & COOP_RULE_FOLLOWER_LEVEL) != 0 && (r.flags & COOP_RULE_PARTY_LEVEL) != 0); /* on by default */
    CHECK((COOP_RULE_ACTIVE_MASK & (COOP_RULE_FOLLOWER_LEVEL | COOP_RULE_PARTY_LEVEL))
        == (COOP_RULE_FOLLOWER_LEVEL | COOP_RULE_PARTY_LEVEL));
    CHECK(coop_rules_parse_env(&r, "flvl=0,plvl=0") == 2);
    CHECK((r.flags & (COOP_RULE_FOLLOWER_LEVEL | COOP_RULE_PARTY_LEVEL)) == 0);
    coop_rules_format_env(&r, text, sizeof(text));
    CHECK(strstr(text, "flvl=0,plvl=0") != NULL);
    CHECK(coop_rules_parse_env(&r, "follower_autolevel=1,party_level=1") == 2);
    CHECK((r.flags & (COOP_RULE_FOLLOWER_LEVEL | COOP_RULE_PARTY_LEVEL))
        == (COOP_RULE_FOLLOWER_LEVEL | COOP_RULE_PARTY_LEVEL));
    coop_rules_effective(&r); /* Middle: kept */
    CHECK((r.flags & (COOP_RULE_FOLLOWER_LEVEL | COOP_RULE_PARTY_LEVEL))
        == (COOP_RULE_FOLLOWER_LEVEL | COOP_RULE_PARTY_LEVEL));
    coop_rules_parse_env(&r, "flvl=1,plvl=1,lite=1");
    coop_rules_effective(&r); /* Lite mode: off */
    CHECK((r.flags & (COOP_RULE_FOLLOWER_LEVEL | COOP_RULE_PARTY_LEVEL)) == 0);
    coop_rules_default(&r);
    coop_rules_describe(&r, false, desc, sizeof(desc));
    CHECK(strstr(desc, "follower auto-level on, party level on") != NULL);
    CHECK(strstr(desc, "autosave 10 min") != NULL); /* not cut off */
    coop_rules_describe(&r, true, desc, sizeof(desc));
    CHECK(strstr(desc, "автоуровень спутников вкл, общий уровень игроков вкл") != NULL);
    CHECK(strstr(desc, "автосейв 10 мин") != NULL);
    CHECK(strlen(desc) < 1000);
    CHECK(sizeof(CoopRules) == 28);

    /* follower: up to the leader's level, never down, capped below max */
    CHECK(coop_middle_follower_level_target(5, 35, 51) == 35); /* the late-hired dog */
    CHECK(coop_middle_follower_level_target(35, 35, 51) == 0);
    CHECK(coop_middle_follower_level_target(40, 35, 51) == 0); /* higher: left alone */
    CHECK(coop_middle_follower_level_target(5, 60, 51) == 50);
    CHECK(coop_middle_follower_level_target(50, 60, 51) == 0);
    CHECK(coop_middle_follower_level_target(1, 20, 0) == 20); /* no max known */
    CHECK(coop_middle_follower_level_target(0, 0, 51) == 0);
    /* party: everybody follows the highest XP */
    CHECK(coop_middle_party_xp_target(xp, 4) == 450000);
    CHECK(coop_middle_party_xp_target(xp, 1) == 2100);
    CHECK(coop_middle_party_xp_target(xp, 0) == -1);
    CHECK(coop_middle_party_xp_target(NULL, 3) == -1);
    CHECK(TIG_NET_PROTOCOL == 60);
}

/* M7 review: the subinst client's timed checkpoint clock. */
static void test_m7_ckpt_clock(void)
{
    unsigned int ck = 1000;
    unsigned int ping = 0;
    const unsigned int P = COOP_SUBINST_CHECKPOINT_MS;

    /* first call after entry: no delta, not due yet */
    CHECK(!coop_subinst_ckpt_due(&ck, &ping, 1000, false, false));
    CHECK(ping == 1000);
    CHECK(!coop_subinst_ckpt_due(&ck, &ping, 1000 + P - 1, false, false));
    CHECK(coop_subinst_ckpt_due(&ck, &ping, 1000 + P, false, false));
    /* a pause of 30 s neither fires nor counts */
    ck = 1000;
    ping = 0;
    CHECK(!coop_subinst_ckpt_due(&ck, &ping, 2000, false, false));
    CHECK(!coop_subinst_ckpt_due(&ck, &ping, 2000 + P, true, false)); /* paused: skipped */
    CHECK(ck == 1000 + P);
    CHECK(!coop_subinst_ckpt_due(&ck, &ping, 2000 + P + 500, false, false));
    CHECK(coop_subinst_ckpt_due(&ck, &ping, 1000 + 2 * P, false, false));
    /* an explicit request still goes while paused */
    CHECK(coop_subinst_ckpt_due(&ck, &ping, 1000 + 2 * P + 1, true, true));
    /* the tick counter wraps: still due exactly one period later */
    ck = 0xFFFFF000u;
    ping = 0;
    CHECK(!coop_subinst_ckpt_due(&ck, &ping, 0xFFFFF000u, false, false));
    CHECK(!coop_subinst_ckpt_due(&ck, &ping, 0xFFFFF000u + P - 1, false, false));
    CHECK(coop_subinst_ckpt_due(&ck, &ping, 0xFFFFF000u + P, false, false));
    /* a checkpoint stamp ahead of now (stale / wrapped) is pulled back, no storm */
    ck = 50000;
    ping = 0;
    CHECK(!coop_subinst_ckpt_due(&ck, &ping, 10000, false, false));
    CHECK(ck == 10000);
    CHECK(!coop_subinst_ckpt_due(&ck, &ping, 10000 + P - 1, false, false));
    /* a reset ping (leave / entry) gives no stale delta: a long gap while
       paused does not push the clock */
    ck = 5000;
    ping = 0;
    CHECK(!coop_subinst_ckpt_due(&ck, &ping, 900000, true, false));
    CHECK(ck == 5000);
    CHECK(coop_subinst_ckpt_due(&ck, &ping, 900001, false, false));
    /* now == 0 never re-arms the "first call" sentinel */
    ping = 0;
    ck = 0;
    (void)coop_subinst_ckpt_due(&ck, &ping, 0, false, false);
    CHECK(ping != 0);
    CHECK(!coop_subinst_ckpt_due(NULL, &ping, 1, false, false));
    CHECK(coop_subinst_ckpt_due(&ck, NULL, 1, false, true));
}

static void test_m7(void)
{
    int x = -1;
    int y = -1;
    int oct = -1;
    CoopCollapseTrack ct;
    CoopAutosaveGate g;
    CoopRules r;
    CoopSleepState st;
    PacketCoopPartyCtrl c;
    char line[512];
    int i;

    /* edge indicator: inside (border included) = false */
    CHECK(!coop_middle_edge_indicator(400, 300, 800, 600, 20, &x, &y, &oct));
    CHECK(!coop_middle_edge_indicator(20, 20, 800, 600, 20, &x, &y, &oct));
    CHECK(!coop_middle_edge_indicator(780, 580, 800, 600, 20, &x, &y, &oct));
    CHECK(!coop_middle_edge_indicator(10, 10, 0, 600, 20, &x, &y, &oct));
    /* the four sides */
    CHECK(coop_middle_edge_indicator(400, -100, 800, 600, 20, &x, &y, &oct) && x == 400 && y == 20 && oct == 2);
    CHECK(coop_middle_edge_indicator(400, 900, 800, 600, 20, &x, &y, &oct) && x == 400 && y == 580 && oct == 6);
    CHECK(coop_middle_edge_indicator(-100, 300, 800, 600, 20, &x, &y, &oct) && x == 20 && y == 300 && oct == 4);
    CHECK(coop_middle_edge_indicator(1000, 300, 800, 600, 20, &x, &y, &oct) && x == 780 && y == 300 && oct == 0);
    /* the four corners (square view: the ray hits the corner itself) */
    CHECK(coop_middle_edge_indicator(-300, -300, 600, 600, 20, &x, &y, &oct) && x == 20 && y == 20 && oct == 3);
    CHECK(coop_middle_edge_indicator(900, -300, 600, 600, 20, &x, &y, &oct) && x == 580 && y == 20 && oct == 1);
    CHECK(coop_middle_edge_indicator(-300, 900, 600, 600, 20, &x, &y, &oct) && x == 20 && y == 580 && oct == 5);
    CHECK(coop_middle_edge_indicator(900, 900, 600, 600, 20, &x, &y, &oct) && x == 580 && y == 580 && oct == 7);
    /* off-axis points land on the pad border */
    CHECK(coop_middle_edge_indicator(-1000, -1000, 800, 600, 20, &x, &y, &oct) && m7_on_border(x, y, 800, 600, 20)
        && y == 20 && oct == 3);
    CHECK(coop_middle_edge_indicator(5000, 310, 800, 600, 20, &x, &y, &oct) && x == 780 && m7_on_border(x, y, 800, 600, 20)
        && oct == 0);
    CHECK(coop_middle_edge_indicator(-7, 3000, 800, 600, 24, &x, &y, &oct) && y == 576 && m7_on_border(x, y, 800, 600, 24)
        && oct == 6);
    for (i = -2000; i <= 2000; i += 97) {
        CHECK(coop_middle_edge_indicator(i, -50, 800, 600, 28, &x, &y, &oct) && m7_on_border(x, y, 800, 600, 28));
        CHECK(coop_middle_edge_indicator(-60, i, 800, 600, 28, &x, &y, &oct) && m7_on_border(x, y, 800, 600, 28));
        CHECK(oct >= 0 && oct <= 7);
    }

    /* collapse notice: once per fall, 10 s per slot */
    memset(&ct, 0, sizeof(ct));
    CHECK(coop_middle_collapse_step(&ct, 2, true, 1000));
    CHECK(!coop_middle_collapse_step(&ct, 2, true, 1500));
    CHECK(!coop_middle_collapse_step(&ct, 2, false, 2000));
    CHECK(!coop_middle_collapse_step(&ct, 2, true, 6000)); /* within 10 s */
    CHECK(coop_middle_collapse_step(&ct, 3, true, 6000)); /* another slot */
    CHECK(!coop_middle_collapse_step(&ct, 2, false, 7000));
    CHECK(coop_middle_collapse_step(&ct, 2, true, 1000 + COOP_COLLAPSE_SPAM_MS));
    CHECK(!coop_middle_collapse_step(&ct, 8, true, 0) && !coop_middle_collapse_step(NULL, 1, true, 0));
    coop_middle_feed_format(COOP_MID_CHAT_COLLAPSE, "Bob", "-", false, line, sizeof(line));
    CHECK(strcmp(line, "[Party] Bob collapsed from exhaustion!") == 0);
    coop_middle_feed_format(COOP_MID_CHAT_COLLAPSE, "Bob", "-", true, line, sizeof(line));
    CHECK(strcmp(line, "[Партия] Bob упал без сознания от усталости!") == 0);
    coop_middle_feed_format(COOP_MID_CHAT_AUTOSAVE, "", "3", false, line, sizeof(line));
    CHECK(strcmp(line, "[AutoSave] World saved (AutoSave 3).") == 0);
    coop_middle_feed_format(COOP_MID_CHAT_AUTOSAVE, "", "3", true, line, sizeof(line));
    CHECK(strcmp(line, "[Автосейв] Мир сохранён (AutoSave 3).") == 0);

    /* autosave gate */
    memset(&g, 0, sizeof(g));
    g.in_world = true;
    CHECK(coop_middle_autosave_ready(10, 600000u, 0u, &g));
    CHECK(!coop_middle_autosave_ready(10, 599999u, 0u, &g)); /* interval not over */
    CHECK(coop_middle_autosave_ready(10, 100000u, 100000u - 600000u, &g)); /* clock wrap */
    CHECK(!coop_middle_autosave_ready(0, 600000u, 0u, &g)); /* interval 0 = off */
    CHECK(!coop_middle_autosave_ready(10, 600000u, 0u, NULL));
    g.paused = true;
    CHECK(!coop_middle_autosave_ready(10, 600000u, 0u, &g));
    g.paused = false;
    g.combat = true;
    CHECK(!coop_middle_autosave_ready(10, 600000u, 0u, &g));
    g.combat = false;
    g.dialog = true;
    CHECK(!coop_middle_autosave_ready(10, 600000u, 0u, &g));
    g.dialog = false;
    g.world_map = true;
    CHECK(!coop_middle_autosave_ready(10, 600000u, 0u, &g));
    g.world_map = false;
    g.commit = true;
    CHECK(!coop_middle_autosave_ready(10, 600000u, 0u, &g));
    g.commit = false;
    g.teleport = true;
    CHECK(!coop_middle_autosave_ready(10, 600000u, 0u, &g));
    g.teleport = false;
    g.in_world = false;
    CHECK(!coop_middle_autosave_ready(10, 600000u, 0u, &g));
    /* rotation Slot9690..9694, round robin, never a client slot */
    for (i = 0; i < 12; i++) {
        CHECK(coop_middle_autosave_slot(i) == 9690 + i % 5);
    }
    CHECK(coop_middle_autosave_slot(-1) == 9694);
    CHECK(coop_middle_autosave_next_index(9690) == 1 && coop_middle_autosave_next_index(9694) == 0);
    CHECK(coop_middle_autosave_next_index(9700) == 0 && coop_middle_autosave_next_index(1) == 0);
    CHECK(coop_middle_autosave_is_slot(9690) && coop_middle_autosave_is_slot(9694));
    CHECK(!coop_middle_autosave_is_slot(9689) && !coop_middle_autosave_is_slot(9695));
    CHECK(!coop_middle_autosave_is_slot(COOP_SREQ_SLOT_FIRST) && !coop_middle_autosave_is_slot(COOP_SREQ_SLOT_LAST));
    CHECK(COOP_AUTOSAVE_SLOT_FIRST + COOP_AUTOSAVE_SLOT_COUNT <= COOP_SREQ_SLOT_FIRST);

    /* asave rule */
    CHECK(sizeof(CoopRules) == 0x1C);
    coop_rules_default(&r);
    CHECK(r.autosave_min == COOP_RULES_ASAVE_DEFAULT && r.autosave_min == 10);
    CHECK(coop_rules_parse_env(&r, "asave=0") == 1 && r.autosave_min == 0);
    CHECK(coop_rules_parse_env(&r, "asave=10") == 1 && r.autosave_min == 10);
    CHECK(coop_rules_parse_env(&r, "asave=1") == 1 && r.autosave_min == 5);
    CHECK(coop_rules_parse_env(&r, "asave=999") == 1 && r.autosave_min == 60);
    CHECK(coop_rules_parse_env(&r, "autosave_min=30") == 1 && r.autosave_min == 30);
    coop_rules_describe(&r, false, line, sizeof(line));
    CHECK(strstr(line, "autosave 30 min") != NULL);
    r.autosave_min = 0;
    coop_rules_describe(&r, false, line, sizeof(line));
    CHECK(strstr(line, "autosave off") != NULL);

    /* unified sleep with a player inside a solo instance */
    coop_middle_sleep_reset(&st);
    CHECK(coop_middle_sleep_step(&st, 0x07, COOP_SLEEP_EV_VOTE, 0, COOP_SLEEP_MODE_8H) == COOP_SLEEP_RES_WAIT);
    CHECK(coop_middle_sleep_step(&st, 0x07, COOP_SLEEP_EV_VOTE, 1, COOP_SLEEP_MODE_8H) == COOP_SLEEP_RES_WAIT);
    CHECK(coop_middle_sleep_step(&st, 0x07, COOP_SLEEP_EV_VOTE, 2, COOP_SLEEP_MODE_1H) == COOP_SLEEP_RES_EXECUTE);
    CHECK(st.mode == COOP_SLEEP_MODE_8H);
    CHECK(coop_middle_sleep_apply_mask(0x07, 0x04) == 0x04); /* slot 2 inside: gets SLEEP_APPLY */
    CHECK(coop_middle_sleep_apply_mask(0x03, 0x05) == 0x00); /* never the host, never a dead slot */
    CHECK(coop_middle_sleep_apply_mask(0xFF, 0xFF) == 0xFE);
    memset(&c, 0, sizeof(c));
    c.type = PACKET_COOP_PARTY_CTRL;
    c.op = COOP_CTRL_SLEEP_APPLY;
    CHECK(COOP_CTRL_SLEEP_APPLY == 11);
    c.a = 1;
    CHECK(coop_middle_party_ctrl_valid(&c) && coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
    c.a = COOP_SLEEP_APPLY_MAX_HOURS;
    CHECK(coop_middle_party_ctrl_valid(&c) && coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
    c.a = 0;
    CHECK(!coop_middle_party_ctrl_valid(&c));
    c.a = COOP_SLEEP_APPLY_MAX_HOURS + 1;
    CHECK(!coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
    c.a = 8;
    c.b = 1;
    CHECK(!coop_lite_party_ctrl_valid(&c, (int)sizeof(c)));
    c.b = 0;
    CHECK(!coop_lite_party_ctrl_valid(&c, (int)sizeof(c) - 1));

    /* repeated session resets are idempotent (LSan: nothing left behind) */
    for (i = 0; i < 3; i++) {
        coop_middle_session_reset();
        coop_lite_session_reset();
        CHECK(coop_middle_rules()->autosave_min == COOP_RULES_ASAVE_DEFAULT);
        CHECK(!coop_middle_is_paused());
    }
    CHECK(TIG_NET_PROTOCOL >= 59);
}

static void test_m6_review(void)
{
    uint8_t des[24 * 4];
    uint8_t ours[2][24];
    uint8_t qbuf[4 + COOP_SUBINST_QTY_ENTRY];
    CoopSubinstQty it;
    CoopSubinstQty back;
    PacketCoopSubinstCtrl c;
    int s;
    int i;

    /* mobile.des: only our party's ids leave the local copy. */
    for (i = 0; i < 4; i++) {
        memset(des + i * 24, 0x10 + i, 24);
    }
    memset(ours[0], 0x11, 24);
    memset(ours[1], 0x13, 24);
    CHECK(coop_subinst_des_filter(des, sizeof(des), (const uint8_t(*)[24])ours, 2) == 48);
    CHECK(des[0] == 0x10 && des[24] == 0x12);
    CHECK(coop_subinst_des_filter(des, 48 + 5, NULL, 0) == 48); /* partial record dropped */

    /* item state table: every item, -1 = no quantity */
    memset(&it, 0, sizeof(it));
    it.oid[0] = 7;
    it.qty = COOP_SUBINST_QTY_NONE;
    it.hp_damage = 12;
    it.charges = 3;
    it.mana = -1;
    it.inv_loc = 1003;
    CHECK(coop_subinst_qty_build(&it, 1, qbuf, sizeof(qbuf)) == sizeof(qbuf));
    CHECK(coop_subinst_qty_parse(qbuf, sizeof(qbuf), &back, 1) == 1 && back.qty == -1 && back.hp_damage == 12
        && back.charges == 3 && back.mana == -1 && back.inv_loc == 1003);
    it.qty = -2;
    coop_subinst_qty_build(&it, 1, qbuf, sizeof(qbuf));
    CHECK(coop_subinst_qty_parse(qbuf, sizeof(qbuf), &back, 1) < 0);
    it.qty = 5;
    it.hp_damage = -1;
    coop_subinst_qty_build(&it, 1, qbuf, sizeof(qbuf));
    CHECK(coop_subinst_qty_parse(qbuf, sizeof(qbuf), &back, 1) < 0);

    /* sub -> sub refused: never an ACK(F_NEXT) without a GRANT; the client
       stays COMMITTING through the DENY and the ACK(0) ends it. */
    CHECK(coop_subinst_sub_next_plan(COOP_SUBINST_LEASE_OK, true) == COOP_SUBINST_NEXT_GRANT);
    CHECK(coop_subinst_sub_next_plan(COOP_SUBINST_LEASE_OK, false) == COOP_SUBINST_NEXT_BACK);
    CHECK(coop_subinst_sub_next_plan(COOP_SUBINST_LEASE_DENY_BUSY, true) == COOP_SUBINST_NEXT_BACK);
    CHECK(coop_subinst_sub_next_plan(COOP_SUBINST_LEASE_DENY_STATE, true) == COOP_SUBINST_NEXT_BACK);
    CHECK(coop_subinst_sub_next_plan(COOP_SUBINST_LEASE_NORMAL, true) == COOP_SUBINST_NEXT_BACK);
    s = COOP_SUBINST_ST_COMMITTING;
    s = coop_subinst_client_next(s, COOP_SUBINST_EV_DENY);
    CHECK(s == COOP_SUBINST_ST_COMMITTING);
    s = coop_subinst_client_next(s, COOP_SUBINST_EV_ACK);
    CHECK(s == COOP_SUBINST_ST_IDLE);
    /* ACK(F_NEXT) then a failed grant: ABORT frees REQUESTED */
    s = coop_subinst_client_next(COOP_SUBINST_ST_COMMITTING, COOP_SUBINST_EV_ACK_NEXT);
    CHECK(s == COOP_SUBINST_ST_REQUESTED);
    CHECK(coop_subinst_client_next(s, COOP_SUBINST_EV_ABORT) == COOP_SUBINST_ST_IDLE);
    /* and if nothing comes at all, the stall timer gives the lease back */
    CHECK(!coop_subinst_client_stalled(COOP_SUBINST_ST_REQUESTED, true, 1000, 1000 + COOP_SUBINST_STALL_MS - 1));
    CHECK(coop_subinst_client_stalled(COOP_SUBINST_ST_REQUESTED, true, 1000, 1000 + COOP_SUBINST_STALL_MS));
    CHECK(coop_subinst_client_stalled(COOP_SUBINST_ST_RECEIVING, true, 0xFFFFFF00u, 0xFFFFFF00u + COOP_SUBINST_STALL_MS));
    CHECK(!coop_subinst_client_stalled(COOP_SUBINST_ST_REQUESTED, false, 0, 1000000));
    CHECK(!coop_subinst_client_stalled(COOP_SUBINST_ST_ACTIVE, true, 0, 1000000));
    CHECK(!coop_subinst_client_stalled(COOP_SUBINST_ST_COMMITTING, true, 0, 1000000));

    /* CANCEL (client -> host) */
    memset(&c, 0, sizeof(c));
    c.type = PACKET_COOP_SUBINST_CTRL;
    c.op = COOP_SUBINST_OP_CANCEL;
    c.lease_id = 9;
    CHECK(coop_subinst_ctrl_valid(&c, sizeof(c), false));
    CHECK(!coop_subinst_ctrl_valid(&c, sizeof(c), true));
    c.lease_id = 0;
    CHECK(!coop_subinst_ctrl_valid(&c, sizeof(c), false));

    /* client role inside a solo instance: full local authority */
    tig_net_test_force_role(true, true, false);
    CHECK(tig_net_is_coop_client() && coop_lite_is_client_role());
    tig_net_local_suspend_begin();
    CHECK(coop_lite_is_client_role()); /* PT7: the client's own suspended talk is still a client */
    tig_net_set_local_authority(true);
    CHECK(tig_net_has_local_authority());
    CHECK(!coop_lite_is_client_role());
    CHECK(!tig_net_is_coop_client());
    CHECK(!coop_lite_story_write_blocked());
    CHECK(!coop_lite_client_local_loot(1));
    {
        int depth = tig_net_local_suspend_pause(); /* pumping: a client again */

        CHECK(!tig_net_has_local_authority() && tig_net_is_coop_client() && coop_lite_is_client_role());
        tig_net_local_suspend_resume(depth);
    }
    CHECK(!coop_lite_is_client_role());
    tig_net_set_local_authority(false);
    CHECK(coop_lite_is_client_role());
    tig_net_local_suspend_end();
    CHECK(coop_lite_story_write_blocked()); /* outside: the host writes the story */
    tig_net_test_force_role(false, false, false);
    CHECK(!coop_lite_is_client_role() && !tig_net_is_coop_client());
}

static void test_m6_ui(void)
{
    char tag[4];
    char line[256];
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    CHECK(coop_middle_status_badge_ex(false, false, true, COOP_STATUS_COMBAT, tag, &r, &g, &b) && strcmp(tag, "M") == 0
        && r == 120 && g == 160 && b == 255);
    CHECK(coop_middle_status_badge_ex(false, true, true, 0, tag, &r, &g, &b) && strcmp(tag, "Z") == 0);
    CHECK(coop_middle_status_badge_ex(true, false, true, 0, tag, &r, &g, &b) && strcmp(tag, "X") == 0);
    CHECK(coop_middle_status_badge_ex(false, false, false, 0xF, tag, &r, &g, &b) && strcmp(tag, "P") == 0);
    coop_middle_feed_format(COOP_MID_CHAT_SUBINST, "Bob", "1|Tarant Sewers", false, line, sizeof(line));
    CHECK(strcmp(line, "[Co-op] Bob entered Tarant Sewers (solo instance).") == 0);
    coop_middle_feed_format(COOP_MID_CHAT_SUBINST, "Bob", "1|Tarant Sewers", true, line, sizeof(line));
    CHECK(strcmp(line, "[Кооп] Bob вошёл в Tarant Sewers (отдельная локация).") == 0);
    coop_middle_feed_format(COOP_MID_CHAT_SUBINST, "Bob", "0|Tarant Sewers", false, line, sizeof(line));
    CHECK(strstr(line, "left Tarant Sewers") != NULL);
    CHECK((COOP_RULE_DEFAULT_FLAGS & COOP_RULE_SUBINSTANCES) != 0);
}

int main(void)
{
    test_rules_default_sanitize();
    test_rules_env();
    test_rules_packet();
    test_split_gold();
    test_loot_feed();
    test_rest_ops();
    test_trade();
    test_status();
    test_labels();
    test_m3_rules();
    test_m3_scaling();
    test_m3_cmds();
    test_m3_ping();
    test_m3_pause();
    test_m4_rules();
    test_m4_quest_items();
    test_m4_fog();
    test_m4_dialog_route();
    test_m4_followers();
    test_m5_rules();
    test_m5_interest();
    test_m5_overworld();
    test_m5_sleep();
    test_m5_route_teleport();
    test_m6_filenames();
    test_m6_archive();
    test_m6_payload();
    test_m6_chunks();
    test_m6_lease();
    test_m6_state_machine();
    test_m6_packets();
    test_m6_ui();
    test_m6_review();
    test_m7();
    test_m7_ckpt_clock();
    test_lite_mode();
    test_autolevel();
    test_hop_and_followers();
    if (g_fail != 0) {
        fprintf(stderr, "coop_middle_test: %d check(s) failed\n", g_fail);
        return 1;
    }
    printf("PASS: coop_middle (rules contract, PACKET_COOP_RULES, gold split, loot feed, rest ops, "
           "trade, status badges, label deoverlap, M3 rules / scaling / commands / pings / pause, "
           "M4 rules / quest items / town map fog / dialog routing / followers, "
           "M5 rules / interest / world map travel / unified sleep, "
           "M6 CMAP1 archive / payload / chunks / leases / client states / packets 171-173 / M badge, "
           "M7 edge arrows / collapse / autosave / asave / SLEEP_APPLY / reset, Lite mode, auto-levelling, protocol 60, solo hops, follower lists)\n");
    return 0;
}
