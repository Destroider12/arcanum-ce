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
/* M5 */
#define COOP_RULE_FREE_OVERWORLD 0x00000100u /* map 1: everyone travels the world map alone, no clock jump */
#define COOP_RULE_UNIFIED_SLEEP 0x00000200u /* sleep / wait needs every living player ready (consensus) */
/* Coop Middle M6 */
#define COOP_RULE_SUBINSTANCES 0x00000400u /* a client may enter an interior / dungeon alone (solo instance) */
/* Coop Lite mode (key lite, default off): the session plays as the Coop Lite
   1.0 release. coop_rules_effective turns every Middle gameplay rule off
   (and hp / xp to 100); only the bits in COOP_RULE_LITE_KEEP_MASK stay.
   The game also reads it for the gates that are not a rule bit (party
   dialog = one at a time, leader nearby; no pause; no /tp /stuck /gather). */
#define COOP_RULE_LITE_MODE 0x00000800u

#define COOP_RULE_M1_MASK (COOP_RULE_GOLD_SPLIT | COOP_RULE_LOOT_FEED | COOP_RULE_REST_VOTE)
/* Rules that do something (always in the env string / description). */
#define COOP_RULE_ACTIVE_MASK \
    (COOP_RULE_M1_MASK | COOP_RULE_DIRECT_TRADE | COOP_RULE_CLIENT_PAUSE | COOP_RULE_NO_FRIENDLY_FIRE_AOE \
        | COOP_RULE_SHARED_QUEST_ITEMS | COOP_RULE_SHARED_FOLLOWERS | COOP_RULE_FREE_OVERWORLD \
        | COOP_RULE_UNIFIED_SLEEP | COOP_RULE_SUBINSTANCES)
/* lite is written to the env string only when it is on (older strings stay the same). */
#define COOP_RULE_KNOWN_MASK (COOP_RULE_ACTIVE_MASK | COOP_RULE_LITE_MODE)
/* What Lite mode keeps: the mode bit and the [Loot] chat feed (a UI line). */
#define COOP_RULE_LITE_KEEP_MASK (COOP_RULE_LITE_MODE | COOP_RULE_LOOT_FEED)
#define COOP_RULE_DEFAULT_FLAGS \
    (COOP_RULE_LOOT_FEED | COOP_RULE_REST_VOTE | COOP_RULE_DIRECT_TRADE | COOP_RULE_NO_FRIENDLY_FIRE_AOE \
        | COOP_RULE_SHARED_QUEST_ITEMS | COOP_RULE_SHARED_FOLLOWERS | COOP_RULE_FREE_OVERWORLD \
        | COOP_RULE_UNIFIED_SLEEP | COOP_RULE_SUBINSTANCES)

#define COOP_RULES_HP_MIN 100
#define COOP_RULES_HP_MAX 300
#define COOP_RULES_XP_MIN 100
#define COOP_RULES_XP_MAX 250
#define COOP_RULES_ASAVE_MIN 5 /* M7: minutes (0 = off) */
#define COOP_RULES_ASAVE_MAX 60
#define COOP_RULES_ASAVE_DEFAULT 10

typedef struct CoopRules {
    /* 0000 */ uint32_t version; /* COOP_RULES_VERSION */
    /* 0004 */ uint32_t flags; /* COOP_RULE_* */
    /* 0008 */ uint16_t enemy_hp_pct; /* 100..300: damage to enemies / pct (M3, key hp) */
    /* 000A */ uint16_t xp_mult_pct; /* 100..250: party XP * pct (M3, key xp) */
    /* 000C */ uint16_t autosave_min; /* M7: host rotating autosave, 0 = off or 5..60 minutes (key asave) */
    /* 000E */ uint16_t reserved16; /* zero */
    /* 0010 */ uint32_t reserved[3]; /* zero; room to grow without a protocol bump */
} CoopRules;

_Static_assert(sizeof(CoopRules) == 0x1C, "CoopRules size");
_Static_assert(_Alignof(CoopRules) == 4, "CoopRules alignment");
_Static_assert(offsetof(CoopRules, enemy_hp_pct) == 0x08, "CoopRules.enemy_hp_pct");
_Static_assert(offsetof(CoopRules, autosave_min) == 0x0C, "CoopRules.autosave_min");
_Static_assert(offsetof(CoopRules, reserved) == 0x10, "CoopRules.reserved");

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
/* The rules the game plays by: sanitized, and with COOP_RULE_LITE_MODE the
   Coop Lite 1.0 set (flags & COOP_RULE_LITE_KEEP_MASK, hp 100, xp 100; the
   autosave interval is kept). Idempotent. The host applies it before it
   uses or sends its rules, a client to what it receives. */
void coop_rules_effective(CoopRules* r);
bool coop_rules_is_lite(const CoopRules* r);

/* Wire check: exact size, type, version >= 1. Direction is the packet
   role table (host -> client only). */
bool coop_rules_packet_valid(const PacketCoopRules* pkt, int size);
void coop_rules_packet_fill(PacketCoopRules* pkt, const CoopRules* r);

#ifdef __cplusplus
}
#endif

#endif /* ARCANUM_GAME_COOP_RULES_H_ */
