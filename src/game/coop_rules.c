/*
 * Coop Middle M1: session rules (pure; see coop_rules.h).
 */

#include "game/coop_rules.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void coop_rules_default(CoopRules* r)
{
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof(*r));
    r->version = COOP_RULES_VERSION;
    r->flags = COOP_RULE_DEFAULT_FLAGS;
    r->enemy_hp_pct = 100;
    r->xp_mult_pct = 100;
    r->autosave_min = COOP_RULES_ASAVE_DEFAULT;
}

void coop_rules_sanitize(CoopRules* r)
{
    if (r == NULL) {
        return;
    }
    r->version = COOP_RULES_VERSION;
    r->flags &= COOP_RULE_KNOWN_MASK;
    if (r->enemy_hp_pct < COOP_RULES_HP_MIN) {
        r->enemy_hp_pct = COOP_RULES_HP_MIN;
    } else if (r->enemy_hp_pct > COOP_RULES_HP_MAX) {
        r->enemy_hp_pct = COOP_RULES_HP_MAX;
    }
    if (r->xp_mult_pct < COOP_RULES_XP_MIN) {
        r->xp_mult_pct = COOP_RULES_XP_MIN;
    } else if (r->xp_mult_pct > COOP_RULES_XP_MAX) {
        r->xp_mult_pct = COOP_RULES_XP_MAX;
    }
    if (r->autosave_min != 0 && r->autosave_min < COOP_RULES_ASAVE_MIN) {
        r->autosave_min = COOP_RULES_ASAVE_MIN;
    } else if (r->autosave_min > COOP_RULES_ASAVE_MAX) {
        r->autosave_min = COOP_RULES_ASAVE_MAX;
    }
    r->reserved16 = 0;
    memset(r->reserved, 0, sizeof(r->reserved));
}

static const struct {
    const char* key;
    const char* alias; /* launcher ini name */
    const char* legacy; /* older spelling, still read */
    uint32_t flag;
} coop_rules_keys[] = {
    { "gold", "gold_split", NULL, COOP_RULE_GOLD_SPLIT },
    { "loot", "loot_feed", NULL, COOP_RULE_LOOT_FEED },
    { "rest", "rest_vote", NULL, COOP_RULE_REST_VOTE },
    { "trade", "direct_trade", NULL, COOP_RULE_DIRECT_TRADE },
    { "pause", "client_pause", NULL, COOP_RULE_CLIENT_PAUSE },
    { "ff", "no_ff_aoe", "noffaoe", COOP_RULE_NO_FRIENDLY_FIRE_AOE }, /* M1-M2 wrote "noffaoe" */
    { "qitems", "shared_quest_items", NULL, COOP_RULE_SHARED_QUEST_ITEMS },
    { "sfol", "shared_followers", NULL, COOP_RULE_SHARED_FOLLOWERS },
    { "ow", "free_overworld", NULL, COOP_RULE_FREE_OVERWORLD },
    { "usleep", "unified_sleep", NULL, COOP_RULE_UNIFIED_SLEEP },
    { "subinst", "solo_instances", "subinstances", COOP_RULE_SUBINSTANCES },
    { "flvl", "follower_autolevel", NULL, COOP_RULE_FOLLOWER_LEVEL },
    { "plvl", "party_level", NULL, COOP_RULE_PARTY_LEVEL },
    { "dscale", "dynamic_scale", NULL, COOP_RULE_DYNAMIC_SCALE },
    { "lite", "lite_mode", "coop_lite", COOP_RULE_LITE_MODE },
};

#define COOP_RULES_KEY_COUNT ((int)(sizeof(coop_rules_keys) / sizeof(coop_rules_keys[0])))

/* A whole token as a number (no trailing garbage), or on/off words. */
static bool coop_rules_value(const char* s, long* out)
{
    char* end = NULL;
    long v;

    if (strcmp(s, "on") == 0 || strcmp(s, "yes") == 0 || strcmp(s, "true") == 0) {
        *out = 1;
        return true;
    }
    if (strcmp(s, "off") == 0 || strcmp(s, "no") == 0 || strcmp(s, "false") == 0) {
        *out = 0;
        return true;
    }
    if (s[0] == '\0') {
        return false;
    }
    v = strtol(s, &end, 10);
    if (end == s || *end != '\0') {
        return false;
    }
    if (v < -100000) {
        v = -100000;
    } else if (v > 100000) {
        v = 100000;
    }
    *out = v;
    return true;
}

int coop_rules_parse_env(CoopRules* r, const char* env_str)
{
    const char* p = env_str;
    int used = 0;

    if (r == NULL) {
        return 0;
    }
    coop_rules_default(r);
    if (p == NULL) {
        return 0;
    }
    while (*p != '\0') {
        char key[24];
        char val[24];
        size_t kn = 0;
        size_t vn = 0;
        long v;
        int i;

        while (*p == ',' || *p == ';' || isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        while (*p != '\0' && *p != '=' && *p != ',' && *p != ';') {
            if (kn + 1 < sizeof(key) && !isspace((unsigned char)*p)) {
                key[kn++] = (char)tolower((unsigned char)*p);
            }
            p++;
        }
        key[kn] = '\0';
        if (*p != '=') {
            continue; /* bare word: ignored */
        }
        p++;
        while (*p != '\0' && *p != ',' && *p != ';') {
            if (vn + 1 < sizeof(val) && !isspace((unsigned char)*p)) {
                val[vn++] = (char)tolower((unsigned char)*p);
            }
            p++;
        }
        val[vn] = '\0';
        if (!coop_rules_value(val, &v)) {
            continue;
        }
        if (strcmp(key, "hp") == 0 || strcmp(key, "enemy_hp_pct") == 0) {
            r->enemy_hp_pct = (uint16_t)(v < 0 ? 0 : (v > 65535 ? 65535 : v));
            used++;
            continue;
        }
        if (strcmp(key, "xp") == 0 || strcmp(key, "xp_mult_pct") == 0) {
            r->xp_mult_pct = (uint16_t)(v < 0 ? 0 : (v > 65535 ? 65535 : v));
            used++;
            continue;
        }
        if (strcmp(key, "asave") == 0 || strcmp(key, "autosave_min") == 0 || strcmp(key, "autosave") == 0) {
            r->autosave_min = (uint16_t)(v < 0 ? 0 : (v > 65535 ? 65535 : v)); /* clamped by sanitize */
            used++;
            continue;
        }
        for (i = 0; i < COOP_RULES_KEY_COUNT; i++) {
            if (strcmp(key, coop_rules_keys[i].key) == 0 || strcmp(key, coop_rules_keys[i].alias) == 0
                || (coop_rules_keys[i].legacy != NULL && strcmp(key, coop_rules_keys[i].legacy) == 0)) {
                if (v != 0) {
                    r->flags |= coop_rules_keys[i].flag;
                } else {
                    r->flags &= ~coop_rules_keys[i].flag;
                }
                used++;
                break;
            }
        }
    }
    coop_rules_sanitize(r);
    return used;
}

int coop_rules_format_env(const CoopRules* r, char* out, size_t sz)
{
    CoopRules s;
    size_t len = 0;
    int i;
    int n;

    if (out == NULL || sz == 0) {
        return 0;
    }
    out[0] = '\0';
    if (r == NULL) {
        coop_rules_default(&s);
    } else {
        s = *r;
        coop_rules_sanitize(&s);
    }
    for (i = 0; i < COOP_RULES_KEY_COUNT; i++) {
        /* Active keys always; future reserved keys only when set (compact). */
        if ((coop_rules_keys[i].flag & COOP_RULE_ACTIVE_MASK) == 0 && (s.flags & coop_rules_keys[i].flag) == 0) {
            continue;
        }
        n = snprintf(out + len, sz - len, "%s%s=%d", len > 0 ? "," : "", coop_rules_keys[i].key,
            (s.flags & coop_rules_keys[i].flag) != 0 ? 1 : 0);
        if (n < 0 || (size_t)n >= sz - len) {
            out[sz - 1] = '\0';
            return (int)strlen(out);
        }
        len += (size_t)n;
    }
    n = snprintf(out + len, sz - len, ",hp=%u,xp=%u,asave=%u", (unsigned)s.enemy_hp_pct, (unsigned)s.xp_mult_pct,
        (unsigned)s.autosave_min);
    if (n < 0 || (size_t)n >= sz - len) {
        out[sz - 1] = '\0';
        return (int)strlen(out);
    }
    return (int)(len + (size_t)n);
}

void coop_rules_describe(const CoopRules* r, bool ru, char* out, size_t sz)
{
    CoopRules s;
    const char* on = ru ? "вкл" : "on";
    const char* off = ru ? "выкл" : "off";
    char asave[32];

    if (out == NULL || sz == 0) {
        return;
    }
    if (r == NULL) {
        coop_rules_default(&s);
    } else {
        s = *r;
        coop_rules_sanitize(&s);
    }
    if ((s.flags & COOP_RULE_LITE_MODE) != 0) {
        size_t n;

        snprintf(out, sz, "%s", ru ? "Режим Coop Lite (как релиз 1.0). " : "Coop Lite mode (as release 1.0). ");
        n = strlen(out);
        if (n + 1 < sz) {
            CoopRules e = s;

            e.flags &= ~COOP_RULE_LITE_MODE;
            coop_rules_describe(&e, ru, out + n, sz - n);
        }
        return;
    }
    if (s.autosave_min == 0) {
        snprintf(asave, sizeof(asave), "%s", off);
    } else {
        snprintf(asave, sizeof(asave), ru ? "%u мин" : "%u min", (unsigned)s.autosave_min);
    }
    if (ru) {
        snprintf(out, sz,
            "Правила: делёж золота %s, лут в чат %s, отдых по запросу %s, обмен %s, пауза клиентов %s, "
            "без огня по своим %s, общие квестовые предметы %s, общие спутники %s, свободная карта мира %s, "
            "единый сон %s, отдельные локации %s, автоуровень спутников %s, общий уровень игроков %s, "
            "динамическая сложность %s, HP врагов %u%%, опыт %u%%, автосейв %s",
            (s.flags & COOP_RULE_GOLD_SPLIT) != 0 ? on : off,
            (s.flags & COOP_RULE_LOOT_FEED) != 0 ? on : off,
            (s.flags & COOP_RULE_REST_VOTE) != 0 ? on : off,
            (s.flags & COOP_RULE_DIRECT_TRADE) != 0 ? on : off,
            (s.flags & COOP_RULE_CLIENT_PAUSE) != 0 ? on : off,
            (s.flags & COOP_RULE_NO_FRIENDLY_FIRE_AOE) != 0 ? on : off,
            (s.flags & COOP_RULE_SHARED_QUEST_ITEMS) != 0 ? on : off,
            (s.flags & COOP_RULE_SHARED_FOLLOWERS) != 0 ? on : off,
            (s.flags & COOP_RULE_FREE_OVERWORLD) != 0 ? on : off,
            (s.flags & COOP_RULE_UNIFIED_SLEEP) != 0 ? on : off,
            (s.flags & COOP_RULE_SUBINSTANCES) != 0 ? on : off,
            (s.flags & COOP_RULE_FOLLOWER_LEVEL) != 0 ? on : off,
            (s.flags & COOP_RULE_PARTY_LEVEL) != 0 ? on : off,
            (s.flags & COOP_RULE_DYNAMIC_SCALE) != 0 ? on : off,
            (unsigned)s.enemy_hp_pct, (unsigned)s.xp_mult_pct, asave);
    } else {
        snprintf(out, sz,
            "Rules: gold split %s, loot feed %s, rest vote %s, trade %s, client pause %s, no friendly fire %s, "
            "shared quest items %s, shared followers %s, free world map %s, unified sleep %s, solo interiors %s, "
            "follower auto-level %s, party level %s, dynamic difficulty %s, enemy HP %u%%, XP %u%%, autosave %s",
            (s.flags & COOP_RULE_GOLD_SPLIT) != 0 ? on : off,
            (s.flags & COOP_RULE_LOOT_FEED) != 0 ? on : off,
            (s.flags & COOP_RULE_REST_VOTE) != 0 ? on : off,
            (s.flags & COOP_RULE_DIRECT_TRADE) != 0 ? on : off,
            (s.flags & COOP_RULE_CLIENT_PAUSE) != 0 ? on : off,
            (s.flags & COOP_RULE_NO_FRIENDLY_FIRE_AOE) != 0 ? on : off,
            (s.flags & COOP_RULE_SHARED_QUEST_ITEMS) != 0 ? on : off,
            (s.flags & COOP_RULE_SHARED_FOLLOWERS) != 0 ? on : off,
            (s.flags & COOP_RULE_FREE_OVERWORLD) != 0 ? on : off,
            (s.flags & COOP_RULE_UNIFIED_SLEEP) != 0 ? on : off,
            (s.flags & COOP_RULE_SUBINSTANCES) != 0 ? on : off,
            (s.flags & COOP_RULE_FOLLOWER_LEVEL) != 0 ? on : off,
            (s.flags & COOP_RULE_PARTY_LEVEL) != 0 ? on : off,
            (s.flags & COOP_RULE_DYNAMIC_SCALE) != 0 ? on : off,
            (unsigned)s.enemy_hp_pct, (unsigned)s.xp_mult_pct, asave);
    }
}

void coop_rules_effective(CoopRules* r)
{
    if (r == NULL) {
        return;
    }
    coop_rules_sanitize(r);
    if ((r->flags & COOP_RULE_LITE_MODE) != 0) {
        r->flags &= COOP_RULE_LITE_KEEP_MASK;
        r->enemy_hp_pct = 100;
        r->xp_mult_pct = 100;
    } else {
        /* 2026-11-27: the free world map is always on in Middle (no
           launcher box any more; an old ow=0 is ignored). */
        r->flags |= COOP_RULE_FREE_OVERWORLD;
    }
}

bool coop_rules_is_lite(const CoopRules* r)
{
    return r != NULL && (r->flags & COOP_RULE_LITE_MODE) != 0;
}

bool coop_rules_equal(const CoopRules* a, const CoopRules* b)
{
    return a != NULL && b != NULL && memcmp(a, b, sizeof(*a)) == 0;
}

bool coop_rules_packet_valid(const PacketCoopRules* pkt, int size)
{
    return pkt != NULL && size == (int)sizeof(PacketCoopRules) && pkt->type == PACKET_COOP_RULES
        && pkt->rules.version >= 1u;
}

void coop_rules_packet_fill(PacketCoopRules* pkt, const CoopRules* r)
{
    if (pkt == NULL) {
        return;
    }
    memset(pkt, 0, sizeof(*pkt));
    pkt->type = PACKET_COOP_RULES;
    if (r != NULL) {
        pkt->rules = *r;
    } else {
        coop_rules_default(&pkt->rules);
    }
    coop_rules_sanitize(&pkt->rules);
}

void coop_rules_apply_preset(CoopRules* r, int preset)
{
    if (r == NULL || preset == COOP_PRESET_CUSTOM) {
        return;
    }
    memset(r, 0, sizeof(*r));
    r->version = COOP_RULES_VERSION;
    r->xp_mult_pct = 100;
    if (preset == COOP_PRESET_LITE) {
        /* lite=1 is the master switch (effective() strips Middle flags).
           Loot feed stays: it is in LITE_KEEP_MASK and is Lite 1.0 UI. */
        r->flags = COOP_RULE_LITE_KEEP_MASK;
        r->enemy_hp_pct = 100;
        r->autosave_min = 0;
    } else if (preset == COOP_PRESET_HARDCORE) {
        r->flags = COOP_RULE_DEFAULT_FLAGS & ~COOP_RULE_NO_FRIENDLY_FIRE_AOE;
        r->enemy_hp_pct = 150;
        r->autosave_min = COOP_RULES_ASAVE_DEFAULT;
    } else {
        /* MIDDLE, or an unknown id: Standard Middle. */
        r->flags = COOP_RULE_DEFAULT_FLAGS;
        r->enemy_hp_pct = 100;
        r->autosave_min = COOP_RULES_ASAVE_DEFAULT;
    }
    coop_rules_sanitize(r);
}

int coop_rules_detect_preset(const CoopRules* r)
{
    CoopRules have;
    CoopRules want;
    int p;

    if (r == NULL) {
        return COOP_PRESET_CUSTOM;
    }
    have = *r;
    coop_rules_sanitize(&have);
    for (p = COOP_PRESET_LITE; p <= COOP_PRESET_HARDCORE; p++) {
        coop_rules_apply_preset(&want, p);
        if (coop_rules_equal(&have, &want)) {
            return p;
        }
    }
    return COOP_PRESET_CUSTOM;
}
