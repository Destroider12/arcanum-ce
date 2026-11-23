/*
 * Coop Middle M1 (protocol 53): session rules contract, PACKET_COOP_RULES,
 * gold split, loot feed filter / limiter, REST_ASK / REST_REFUSE ops.
 * Headless (no engine); also run under ASan/UBSan (nbuild-asan).
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/coop_lite.h"
#include "game/coop_middle.h"
#include "game/coop_rules.h"

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
    CHECK(r.flags == (COOP_RULE_LOOT_FEED | COOP_RULE_REST_VOTE | COOP_RULE_DIRECT_TRADE));
    CHECK((r.flags & COOP_RULE_GOLD_SPLIT) == 0);
    CHECK(r.enemy_hp_pct == 100 && r.xp_mult_pct == 100);
    CHECK(r.reserved[0] == 0 && r.reserved[1] == 0 && r.reserved[2] == 0 && r.reserved[3] == 0);

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
    r.reserved[3] = 0xDEADBEEFu;
    r.version = 99;
    coop_rules_sanitize(&r);
    CHECK(r.flags == COOP_RULE_KNOWN_MASK);
    CHECK((r.flags & COOP_RULE_DIRECT_TRADE) && (r.flags & COOP_RULE_CLIENT_PAUSE)
        && (r.flags & COOP_RULE_NO_FRIENDLY_FIRE_AOE));
    CHECK(r.reserved[0] == 0 && r.reserved[3] == 0);
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
    char desc[256];
    unsigned int mask;
    int n;

    coop_rules_default(&r);
    coop_rules_format_env(&r, text, sizeof(text));
    CHECK(strcmp(text, "gold=0,loot=1,rest=1,trade=1,hp=100,xp=100") == 0);

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
    CHECK(r.flags == (COOP_RULE_GOLD_SPLIT | COOP_RULE_REST_VOTE | COOP_RULE_DIRECT_TRADE));

    /* partial: missing keys keep defaults */
    n = coop_rules_parse_env(&r, "gold=1");
    CHECK(n == 1 && r.flags == (COOP_RULE_GOLD_SPLIT | COOP_RULE_LOOT_FEED | COOP_RULE_REST_VOTE | COOP_RULE_DIRECT_TRADE));
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

    CHECK(TIG_NET_PROTOCOL == 54);
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
    c.op = 9;
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
    CHECK(COOP_MID_CHAT_KIND_MAX == COOP_MID_CHAT_TRADE_GOLD);

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
    if (g_fail != 0) {
        fprintf(stderr, "coop_middle_test: %d check(s) failed\n", g_fail);
        return 1;
    }
    printf("PASS: coop_middle (rules contract, PACKET_COOP_RULES, gold split, loot feed, rest ops)\n");
    return 0;
}
