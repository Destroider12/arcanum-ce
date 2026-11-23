#ifndef ARCANUM_GAME_COOP_RULES_H_
#define ARCANUM_GAME_COOP_RULES_H_

/*
 * Coop Middle M1 (protocol 53): the session rules contract.
 *
 * The host reads its rules from ARCANUM_COOP_RULES (the launcher writes it),
 * sends them to each client right after SESSION_SYNC (join and rejoin) as
 * PACKET_COOP_RULES, and clients keep them until the session resets.
 *
 * Everything here is pure C (no engine headers): the launcher and the
 * headless tests link this file as is.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PACKET_COOP_RULES 167 /* Middle M1: host -> client session rules */

#define COOP_RULES_VERSION 1u

/* M1 */
#define COOP_RULE_GOLD_SPLIT 0x00000001u /* split picked-up gold among living players */
#define COOP_RULE_LOOT_FEED 0x00000002u /* [Loot] chat lines for notable items */
#define COOP_RULE_REST_VOTE 0x00000004u /* clients may ask the host to rest (Y / N) */
/* M2 */
#define COOP_RULE_DIRECT_TRADE 0x00000008u /* free give between players (trade window) */
/* M3 */
#define COOP_RULE_CLIENT_PAUSE 0x00000010u /* clients may toggle the shared pause (host always may) */
#define COOP_RULE_NO_FRIENDLY_FIRE_AOE 0x00000020u /* party never damages party (AoE, stray hits) */
/* M4 */
#define COOP_RULE_SHARED_QUEST_ITEMS 0x00000040u /* quest item checks also see nearby players' packs */
#define COOP_RULE_SHARED_FOLLOWERS 0x00000080u /* any party player manages / barters with any follower */

#define COOP_RULE_M1_MASK (COOP_RULE_GOLD_SPLIT | COOP_RULE_LOOT_FEED | COOP_RULE_REST_VOTE)
/* Rules that do something (always in the env string / description). */
#define COOP_RULE_ACTIVE_MASK \
    (COOP_RULE_M1_MASK | COOP_RULE_DIRECT_TRADE | COOP_RULE_CLIENT_PAUSE | COOP_RULE_NO_FRIENDLY_FIRE_AOE \
        | COOP_RULE_SHARED_QUEST_ITEMS | COOP_RULE_SHARED_FOLLOWERS)
#define COOP_RULE_KNOWN_MASK COOP_RULE_ACTIVE_MASK
#define COOP_RULE_DEFAULT_FLAGS \
    (COOP_RULE_LOOT_FEED | COOP_RULE_REST_VOTE | COOP_RULE_DIRECT_TRADE | COOP_RULE_NO_FRIENDLY_FIRE_AOE \
        | COOP_RULE_SHARED_QUEST_ITEMS | COOP_RULE_SHARED_FOLLOWERS)

#define COOP_RULES_HP_MIN 100
#define COOP_RULES_HP_MAX 300
#define COOP_RULES_XP_MIN 100
#define COOP_RULES_XP_MAX 250

typedef struct CoopRules {
    /* 0000 */ uint32_t version; /* COOP_RULES_VERSION */
    /* 0004 */ uint32_t flags; /* COOP_RULE_* */
    /* 0008 */ uint16_t enemy_hp_pct; /* 100..300: damage to enemies / pct (M3, key hp) */
    /* 000A */ uint16_t xp_mult_pct; /* 100..250: party XP * pct (M3, key xp) */
    /* 000C */ uint32_t reserved[4]; /* zero; room to grow without a protocol bump */
} CoopRules;

_Static_assert(sizeof(CoopRules) == 0x1C, "CoopRules size");
_Static_assert(_Alignof(CoopRules) == 4, "CoopRules alignment");
_Static_assert(offsetof(CoopRules, enemy_hp_pct) == 0x08, "CoopRules.enemy_hp_pct");
_Static_assert(offsetof(CoopRules, reserved) == 0x0C, "CoopRules.reserved");

typedef struct PacketCoopRules {
    /* 0000 */ int type; /* PACKET_COOP_RULES */
    /* 0004 */ CoopRules rules;
} PacketCoopRules;

_Static_assert(sizeof(PacketCoopRules) == 0x20, "PacketCoopRules size");

void coop_rules_default(CoopRules* r);
/* Clamp the percentages, drop unknown flag bits, zero reserved, fix version. */
void coop_rules_sanitize(CoopRules* r);
/* "gold=1,loot=1,rest=1,trade=1,pause=0,ff=1,hp=100,xp=100" (defaults first, unknown keys and
   bad values ignored, result sanitized). Returns how many keys were used. */
int coop_rules_parse_env(CoopRules* r, const char* env_str);
/* The same compact form; always NUL-terminated. Returns the length. */
int coop_rules_format_env(const CoopRules* r, char* out, size_t sz);
/* One readable line for the log and the chat. */
void coop_rules_describe(const CoopRules* r, bool ru, char* out, size_t sz);
bool coop_rules_equal(const CoopRules* a, const CoopRules* b);

/* Wire check: exact size, type, version >= 1. Direction is the packet
   role table (host -> client only). */
bool coop_rules_packet_valid(const PacketCoopRules* pkt, int size);
void coop_rules_packet_fill(PacketCoopRules* pkt, const CoopRules* r);

#ifdef __cplusplus
}
#endif

#endif /* ARCANUM_GAME_COOP_RULES_H_ */
