/*
 * Coop Middle M1 (protocol 53): gold split, loot feed, client rest request,
 * and the host -> client session rules (PACKET_COOP_RULES).
 * Coop Middle M2 (protocol 54): direct trade, status badges.
 *
 * Pure helpers first (headless tests), then the game side, compiled out
 * under COOP_LITE_TEST_STUBS (no-op stubs at the end).
 */

#include "game/coop_middle.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/tig_net.h"
#ifndef COOP_LITE_TEST_STUBS
#include "game/location.h" /* M5: LOCATION_GET_X / SECTOR_MAKE in the pure part */
#include "game/sector.h"
#else
#define LOCATION_MAKE(x, y) ((int64_t)((x) & 0xFFFFFFFF) | (((int64_t)(y) & 0xFFFFFFFF) << 32))
#define LOCATION_GET_X(l) ((int64_t)((l) & 0xFFFFFFFF))
#define LOCATION_GET_Y(l) ((int64_t)(((l) >> 32) & 0xFFFFFFFF))
#define SECTOR_MAKE(a, b) ((a) | ((b) << 26))
#endif

_Static_assert(PACKET_COOP_RULES < PACKET_COOP_TYPE_LIMIT, "PACKET_COOP_RULES inside the coop range");
_Static_assert(PACKET_COOP_RULES > PACKET_COOP_NPC_STATS, "PACKET_COOP_RULES after the Lite types");

/* ---- pure ---------------------------------------------------------------- */

int coop_middle_split_gold(int amount, int living_count, int* carry_inout, int* each_out)
{
    long long total = amount > 0 ? amount : 0;
    int n = living_count < 1 ? 1 : (living_count > 8 ? 8 : living_count);
    int each;

    if (carry_inout != NULL) {
        if (*carry_inout > 0) {
            total += *carry_inout;
        }
        *carry_inout = 0;
    }
    if (total > INT_MAX) {
        total = INT_MAX; /* saturate (never more gold than exists) */
    }
    each = (int)(total / n);
    if (each_out != NULL) {
        *each_out = each;
    }
    return (int)(total - (long long)each * n);
}

bool coop_middle_loot_feed_worthy(int obj_type, int item_flags, int base_cost, bool is_quest_or_key)
{
    if (obj_type == COOP_MID_T_GOLD || obj_type == COOP_MID_T_AMMO) {
        return false; /* gold has its own line (split); ammo never */
    }
    if (obj_type < COOP_MID_T_WEAPON || obj_type > COOP_MID_T_GENERIC) {
        return false; /* not an item */
    }
    if (is_quest_or_key || obj_type == COOP_MID_T_KEY || obj_type == COOP_MID_T_KEY_RING) {
        return true;
    }
    if (obj_type == COOP_MID_T_SCROLL || obj_type == COOP_MID_T_WRITTEN) {
        return true; /* scrolls, notes, books, schematics */
    }
    if ((item_flags & COOP_MID_OIF_UBER) != 0) {
        return true; /* unique */
    }
    return base_cost >= COOP_LOOT_FEED_COST;
}

void coop_middle_feed_reset(CoopFeedLimiter* l)
{
    if (l != NULL) {
        memset(l, 0, sizeof(*l));
    }
}

static void coop_feed_roll(CoopFeedLimiter* l, unsigned int now_ms)
{
    if (!l->started || (unsigned int)(now_ms - l->window_ms) >= 1000u) {
        l->started = true;
        l->window_ms = now_ms;
        l->count = 0;
    }
}

bool coop_middle_feed_admit(CoopFeedLimiter* l, unsigned int now_ms)
{
    if (l == NULL) {
        return false;
    }
    if (l->suppressed > 0 && l->started && (unsigned int)(now_ms - l->window_ms) < 1000u) {
        l->suppressed++; /* keep collapsing until the summary went out */
        return false;
    }
    coop_feed_roll(l, now_ms);
    if (l->suppressed > 0) {
        l->suppressed++;
        return false;
    }
    if (l->count < COOP_LOOT_FEED_PER_SEC) {
        l->count++;
        return true;
    }
    l->suppressed = 1;
    return false;
}

int coop_middle_feed_take_suppressed(CoopFeedLimiter* l, unsigned int now_ms)
{
    int n;

    if (l == NULL || l->suppressed <= 0 || !l->started || (unsigned int)(now_ms - l->window_ms) < 1000u) {
        return 0;
    }
    n = l->suppressed;
    l->suppressed = 0;
    l->started = true;
    l->window_ms = now_ms;
    l->count = 1; /* the summary line itself */
    return n;
}

void coop_middle_feed_format(int kind, const char* who, const char* what, bool ru, char* out, size_t sz)
{
    if (out == NULL || sz == 0) {
        return;
    }
    if (who == NULL || who[0] == '\0') {
        who = ru ? "Игрок" : "Player";
    }
    if (what == NULL) {
        what = "";
    }
    switch (kind) {
    case COOP_MID_CHAT_LOOT:
        snprintf(out, sz, ru ? "[Лут] %s подобрал: %s" : "[Loot] %s picked up: %s", who, what);
        break;
    case COOP_MID_CHAT_GOLD: {
        int total = 0;
        int each = 0;

        if (sscanf(what, "%d %d", &total, &each) != 2) {
            total = atoi(what);
            each = 0;
        }
        snprintf(out, sz, ru ? "[Лут] %s нашёл %d золота: поровну по %d" : "[Loot] %s found %d gold: split, %d each",
            who, total, each);
        break;
    }
    case COOP_MID_CHAT_MORE:
        snprintf(out, sz, ru ? "[Лут] ... и ещё %s" : "[Loot] ... and %s more", what);
        break;
    case COOP_MID_CHAT_TRADE:
    case COOP_MID_CHAT_TRADE_GOLD: {
        /* what = "receiver|item" (or "receiver|amount") */
        char to[64];
        const char* bar = strchr(what, '|');
        const char* thing = bar != NULL ? bar + 1 : what;
        size_t n = bar != NULL ? (size_t)(bar - what) : 0;

        if (n >= sizeof(to)) {
            n = sizeof(to) - 1;
        }
        memcpy(to, what, n);
        to[n] = '\0';
        if (to[0] == '\0') {
            snprintf(to, sizeof(to), "%s", ru ? "игрок" : "player");
        }
        if (kind == COOP_MID_CHAT_TRADE_GOLD) {
            snprintf(out, sz, ru ? "[Обмен] %s передал %d золота игроку %s" : "[Trade] %s gave %d gold to %s",
                who, atoi(thing), to);
        } else {
            snprintf(out, sz, ru ? "[Обмен] %s передал %s игроку %s" : "[Trade] %s gave %s to %s",
                who, thing[0] != '\0' ? thing : "?", to);
        }
        break;
    }
    case COOP_MID_CHAT_REGROUP:
        if (atoi(what) == 1) {
            snprintf(out, sz, ru ? "[Кооп] %s собрал отряд вокруг себя." : "[Co-op] %s gathered the party.", who);
        } else {
            snprintf(out, sz, ru ? "[Кооп] %s телепортировался к хосту." : "[Co-op] %s regrouped to the host.", who);
        }
        break;
    case COOP_MID_CHAT_CMD_REFUSE: {
        int reason = 0;
        int secs = 0;

        if (sscanf(what, "%d %d", &reason, &secs) < 1) {
            reason = 0;
        }
        switch (reason) {
        case COOP_TP_REFUSE_DEAD:
            snprintf(out, sz, ru ? "[Кооп] Телепорт невозможен: вы или хост мертвы." : "[Co-op] Can't regroup: you or the host is dead.");
            break;
        case COOP_TP_REFUSE_MAP:
            snprintf(out, sz, ru ? "[Кооп] Телепорт невозможен: вы не на карте хоста." : "[Co-op] Can't regroup: you are not on the host's map.");
            break;
        case COOP_TP_REFUSE_COMBAT:
            snprintf(out, sz, ru ? "[Кооп] Телепорт невозможен: идёт бой." : "[Co-op] Can't regroup: combat mode is on.");
            break;
        case COOP_TP_REFUSE_TARGETED:
            snprintf(out, sz, ru ? "[Кооп] Телепорт невозможен: рядом враг, который атакует вас." : "[Co-op] Can't regroup: an enemy nearby is fighting you.");
            break;
        case COOP_TP_REFUSE_COOLDOWN:
            snprintf(out, sz, ru ? "[Кооп] Команда будет доступна через %d с." : "[Co-op] Command available again in %d s.", secs);
            break;
        case COOP_TP_REFUSE_HOST_ONLY:
            snprintf(out, sz, ru ? "[Кооп] /gather может использовать только хост." : "[Co-op] Only the host can use /gather.");
            break;
        case COOP_TP_REFUSE_NOBODY:
            snprintf(out, sz, ru ? "[Кооп] Некого собирать: никто не может телепортироваться сейчас." : "[Co-op] Nobody to gather: no player can be moved now.");
            break;
        case COOP_TP_REFUSE_SELF:
            snprintf(out, sz, ru ? "[Кооп] Хост собирает отряд командой /gather." : "[Co-op] The host uses /gather to pull the party.");
            break;
        default:
            snprintf(out, sz, ru ? "[Кооп] Команда отклонена." : "[Co-op] Command refused.");
            break;
        }
        break;
    }
    case COOP_MID_CHAT_PAUSE_DROP:
        snprintf(out, sz, ru ? "[Кооп] %s отключился - пауза снята." : "[Co-op] %s left - pause lifted.", who);
        break;
    case COOP_MID_CHAT_QUEST:
        snprintf(out, sz, ru ? "[Квест] %s передан из рюкзака %s" : "[Quest] %s handed over from %s", what, who);
        break;
    case COOP_MID_CHAT_SUBINST: {
        const char* map = strchr(what, '|');

        map = map != NULL && map[1] != '\0' ? map + 1 : (ru ? "локацию" : "a location");
        if (what[0] == '1') {
            snprintf(out, sz, ru ? "[Кооп] %s вошёл в %s (отдельная локация)." : "[Co-op] %s entered %s (solo instance).",
                who, map);
        } else {
            snprintf(out, sz, ru ? "[Кооп] %s вышел из %s." : "[Co-op] %s left %s.", who, map);
        }
        break;
    }
    case COOP_MID_CHAT_COLLAPSE:
        snprintf(out, sz, ru ? "[Партия] %s упал без сознания от усталости!" : "[Party] %s collapsed from exhaustion!",
            who);
        break;
    case COOP_MID_CHAT_AUTOSAVE:
        snprintf(out, sz, ru ? "[Автосейв] Мир сохранён (AutoSave %d)." : "[AutoSave] World saved (AutoSave %d).",
            atoi(what));
        break;
    default:
        out[0] = '\0';
        break;
    }
}

const char* coop_middle_rest_refuse_text(int reason, bool ru)
{
    switch (reason) {
    case COOP_REST_REFUSE_COMBAT:
        return ru ? "Хост в бою - отдых отклонён." : "The host is in combat - rest refused.";
    case COOP_REST_REFUSE_BUSY:
        return ru ? "Хост занят другим запросом - попробуйте позже." : "The host is busy with another request - try again later.";
    case COOP_REST_REFUSE_RULE_OFF:
        return ru ? "Запросы на отдых выключены хостом." : "The host has rest requests turned off.";
    case COOP_REST_REFUSE_CANCELLED:
        return ru ? "Запрос на отдых отменён." : "Rest request cancelled.";
    case COOP_REST_REFUSE_CANT_REST:
        return ru ? "Здесь отдохнуть нельзя." : "The party cannot rest here.";
    case COOP_REST_REFUSE_TIMEOUT:
        return ru ? "Хост не ответил на запрос отдыха." : "The host did not answer the rest request.";
    case COOP_REST_REFUSE_HOST:
    default:
        return ru ? "Хост отказал в отдыхе." : "The host refused to rest.";
    }
}

bool coop_middle_party_ctrl_valid(const PacketCoopPartyCtrl* pkt)
{
    if (pkt == NULL || pkt->type != PACKET_COOP_PARTY_CTRL) {
        return false;
    }
    if (pkt->op == COOP_CTRL_REST_ASK) {
        return (pkt->a == COOP_REST_MODE_NIGHT || pkt->a == COOP_REST_MODE_HEALED) && pkt->b == 0;
    }
    if (pkt->op == COOP_CTRL_REST_REFUSE) {
        return pkt->a >= 0 && pkt->a <= COOP_REST_REFUSE_MAX && pkt->b == 0;
    }
    if (pkt->op == COOP_CTRL_SLOT_STATUS) {
        return pkt->b == 0; /* a: any packing (the handler masks a client's) */
    }
    if (pkt->op == COOP_CTRL_PAUSE) {
        return (pkt->a == 0 || pkt->a == 1) && pkt->b >= 0 && pkt->b < 8;
    }
    if (pkt->op == COOP_CTRL_SLEEP_VOTE) { /* M5 */
        return pkt->a >= COOP_SLEEP_MODE_CANCEL && pkt->a <= COOP_SLEEP_MODE_MAX && pkt->b == 0;
    }
    if (pkt->op == COOP_CTRL_SLEEP_APPLY) { /* M7 */
        return pkt->a >= 1 && pkt->a <= COOP_SLEEP_APPLY_MAX_HOURS && pkt->b == 0;
    }
    if (pkt->op == COOP_CTRL_SLEEP_STATE) {
        int mode = (pkt->a >> 8) & 0xFF;
        int initiator = pkt->b & 0xFF;
        int living = (pkt->b >> 8) & 0xFF;

        return pkt->a >= 0 && (pkt->a >> 16) == 0 && mode <= COOP_SLEEP_MODE_MAX && pkt->b >= 0
            && (pkt->b >> 16) == 0 && (initiator < 8 || initiator == 0xFF) && living <= 8;
    }
    return false;
}

bool coop_middle_rest_ask_allowed(bool have_last, unsigned int last_ms, unsigned int now_ms)
{
    return !have_last || (unsigned int)(now_ms - last_ms) >= COOP_REST_ASK_INTERVAL_MS;
}

uint32_t coop_middle_status_pack(const uint8_t bits[8])
{
    uint32_t packed = 0;
    int slot;

    if (bits == NULL) {
        return 0;
    }
    for (slot = 0; slot < 8; slot++) {
        packed |= (uint32_t)(bits[slot] & 0x0Fu) << (slot * 4);
    }
    return packed;
}

uint8_t coop_middle_status_get(uint32_t packed, int slot)
{
    if (slot < 0 || slot >= 8) {
        return 0;
    }
    return (uint8_t)((packed >> (slot * 4)) & 0x0Fu);
}

bool coop_middle_status_badge(bool dead, bool unconscious, uint8_t bits, char* tag, uint8_t* r, uint8_t* g,
    uint8_t* b)
{
    return coop_middle_status_badge_ex(dead, unconscious, false, bits, tag, r, g, b);
}

bool coop_middle_status_badge_ex(bool dead, bool unconscious, bool solo_instance, uint8_t bits, char* tag, uint8_t* r,
    uint8_t* g, uint8_t* b)
{
    char c;
    uint8_t cr;
    uint8_t cg;
    uint8_t cb;

    if (dead) {
        c = 'X', cr = 220, cg = 60, cb = 60;
    } else if (unconscious) {
        c = 'Z', cr = 180, cg = 130, cb = 230;
    } else if (solo_instance) {
        c = 'M', cr = 120, cg = 160, cb = 255; /* M6: in a solo instance */
    } else if ((bits & COOP_STATUS_POISON) != 0) {
        c = 'P', cr = 90, cg = 220, cb = 90;
    } else if ((bits & COOP_STATUS_DIALOG) != 0) {
        c = 'D', cr = 240, cg = 200, cb = 80;
    } else if ((bits & COOP_STATUS_INVEN) != 0) {
        c = 'I', cr = 100, cg = 210, cb = 240;
    } else if ((bits & COOP_STATUS_COMBAT) != 0) {
        c = 'C', cr = 240, cg = 140, cb = 60;
    } else {
        if (tag != NULL) {
            tag[0] = '\0';
        }
        return false;
    }
    if (tag != NULL) {
        tag[0] = c;
        tag[1] = '\0';
    }
    if (r != NULL) {
        *r = cr;
    }
    if (g != NULL) {
        *g = cg;
    }
    if (b != NULL) {
        *b = cb;
    }
    return true;
}

static bool coop_label_overlap(const CoopLabelRect* a, const CoopLabelRect* b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

int coop_middle_labels_deoverlap(CoopLabelRect* rects, int count, int min_y, int max_y)
{
    int visible = 0;
    int i;
    int j;

    if (rects == NULL || count <= 0) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        CoopLabelRect* r = &rects[i];
        bool moved = true;
        int guard = 2 * count + 2;

        r->hidden = r->w <= 0 || r->h <= 0 || r->h > max_y - min_y;
        if (r->hidden) {
            continue;
        }
        if (r->y + r->h > max_y) {
            r->y = max_y - r->h;
        }
        if (r->y < min_y) {
            r->y = min_y;
        }
        /* Each move goes strictly up, so this ends. */
        while (moved && !r->hidden) {
            moved = false;
            for (j = 0; j < i; j++) {
                if (!rects[j].hidden && coop_label_overlap(r, &rects[j])) {
                    r->y = rects[j].y - r->h - 1;
                    moved = true;
                    if (r->y < min_y || --guard <= 0) {
                        r->hidden = true;
                    }
                    break;
                }
            }
        }
        if (!r->hidden) {
            visible++;
        }
    }
    return visible;
}

bool coop_middle_trade_partner_ok(bool rule_on, int self_slot, int partner_slot, bool partner_is_pc,
    bool partner_active, bool self_dead, bool partner_dead, int dist)
{
    return rule_on && partner_is_pc && partner_active
        && self_slot >= 0 && self_slot < 8 && partner_slot >= 0 && partner_slot < 8 && partner_slot != self_slot
        && !self_dead && !partner_dead
        && dist >= 0 && dist <= COOP_TRADE_RANGE;
}

bool coop_middle_trade_transfer_ok(int kind, unsigned int flags, bool from_target)
{
    if (kind != COOP_TARGET_TRADE || from_target) {
        return false;
    }
    return flags == 0x01u || flags == (0x01u | COOP_XFER_PLANT);
}

/* ---- game side ------------------------------------------------------------ */

/* ---- M3 pure ---- */

int coop_middle_scale_enemy_damage(int raw_dmg, int enemy_hp_pct, int* frac_rem_inout)
{
    int64_t total;
    int64_t scaled;
    int rem = frac_rem_inout != NULL ? *frac_rem_inout : 0;

    if (raw_dmg <= 0) {
        return 0;
    }
    if (enemy_hp_pct < 100) {
        enemy_hp_pct = 100;
    }
    if (rem < 0 || rem >= enemy_hp_pct) {
        rem = 0;
    }
    total = (int64_t)raw_dmg * 100 + rem;
    scaled = total / enemy_hp_pct;
    rem = (int)(total % enemy_hp_pct);
    if (scaled < 1) {
        scaled = 1; /* a hit always hurts; the fraction is used up */
        rem = 0;
    }
    if (frac_rem_inout != NULL) {
        *frac_rem_inout = rem;
    }
    return scaled > INT_MAX ? INT_MAX : (int)scaled;
}

int coop_middle_scale_xp(int raw_xp, int pct)
{
    int64_t v;

    if (raw_xp <= 0 || pct <= 0) {
        return raw_xp;
    }
    v = ((int64_t)raw_xp * pct) / 100;
    if (v > INT_MAX) {
        v = INT_MAX;
    }
    if (pct >= 100 && v < raw_xp) {
        v = raw_xp;
    }
    return (int)v;
}

int coop_middle_parse_cmd(const char* text)
{
    char word[16];
    char arg[16];
    size_t n = 0;
    size_t m = 0;
    const char* p = text;

    if (p == NULL) {
        return COOP_CMD_NONE;
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '/') {
        return COOP_CMD_NONE;
    }
    p++;
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        if (n + 1 >= sizeof(word)) {
            return COOP_CMD_NONE;
        }
        word[n++] = (char)tolower((unsigned char)*p++);
    }
    word[n] = '\0';
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        if (m + 1 >= sizeof(arg)) {
            return COOP_CMD_NONE;
        }
        arg[m++] = (char)tolower((unsigned char)*p++);
    }
    arg[m] = '\0';
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '\0') {
        return COOP_CMD_NONE; /* more words: plain text */
    }
    if (strcmp(word, "tp") == 0) {
        if (m == 0) {
            return COOP_CMD_TP;
        }
        return strcmp(arg, "all") == 0 ? COOP_CMD_GATHER : COOP_CMD_NONE;
    }
    if (m != 0) {
        return COOP_CMD_NONE;
    }
    if (strcmp(word, "stuck") == 0) {
        return COOP_CMD_STUCK;
    }
    if (strcmp(word, "gather") == 0) {
        return COOP_CMD_GATHER;
    }
    return COOP_CMD_NONE;
}

bool coop_middle_rate_admit(bool* have_last, unsigned int* last_ms, unsigned int now_ms, unsigned int interval_ms)
{
    if (have_last == NULL || last_ms == NULL) {
        return false;
    }
    if (*have_last && (unsigned int)(now_ms - *last_ms) < interval_ms) {
        return false;
    }
    *have_last = true;
    *last_ms = now_ms;
    return true;
}

int coop_middle_cooldown_left_s(bool have_last, unsigned int last_ms, unsigned int now_ms, unsigned int interval_ms)
{
    unsigned int passed = now_ms - last_ms;

    if (!have_last || passed >= interval_ms) {
        return 0;
    }
    return (int)((interval_ms - passed + 999u) / 1000u);
}

bool coop_middle_ping_valid(const PacketCoopPing* pkt, int size)
{
    return pkt != NULL && size == (int)sizeof(PacketCoopPing) && pkt->type == PACKET_COOP_PING
        && pkt->kind >= 0 && pkt->kind <= COOP_PING_KIND_MAX && pkt->slot >= 0 && pkt->slot < 8;
}

/* ---- M4 pure ---- */

bool coop_middle_quest_pocket_eligible(bool rule_on, bool is_host, int asker_slot, int ally_slot, bool ally_active,
    bool ally_dead, bool same_map, int dist)
{
    return rule_on && is_host && asker_slot >= 0 && asker_slot < 8 && ally_slot >= 0 && ally_slot < 8
        && ally_slot != asker_slot && ally_active && !ally_dead && same_map && dist >= 0
        && dist <= COOP_SHARED_QUEST_RADIUS;
}

int coop_middle_quest_item_rank(int inventory_location)
{
    if (inventory_location >= 0 && inventory_location < 1000) {
        return 0;
    }
    if (inventory_location >= 1000 && inventory_location <= 1008) {
        return 1;
    }
    return -1;
}

bool coop_middle_townmap_fog_valid(const PacketCoopTownmapFog* pkt, int size)
{
    if (pkt == NULL || size < COOP_TOWNMAP_FOG_HDR || size > (int)sizeof(PacketCoopTownmapFog)
        || pkt->type != PACKET_COOP_TOWNMAP_FOG) {
        return false;
    }
    if (pkt->townmap_id <= 0 || pkt->len == 0 || pkt->len > COOP_TOWNMAP_FOG_CHUNK
        || (pkt->flags & ~COOP_TOWNMAP_FOG_F_REPLACE) != 0) {
        return false;
    }
    if (size < COOP_TOWNMAP_FOG_HDR + (int)pkt->len) {
        return false;
    }
    return (uint64_t)pkt->offset + pkt->len <= COOP_TOWNMAP_FOG_MAX_BYTES;
}

int coop_middle_fog_merge(uint8_t* dst, const uint8_t* src, size_t len, bool replace)
{
    int opened = 0;
    size_t i;

    if (dst == NULL || src == NULL) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        uint8_t now = replace ? src[i] : (uint8_t)(dst[i] | src[i]);
        uint8_t fresh = (uint8_t)(now & ~dst[i]);

        while (fresh != 0) {
            opened += fresh & 1u;
            fresh >>= 1;
        }
        dst[i] = now;
    }
    return opened;
}

uint32_t coop_middle_fnv1a(const uint8_t* data, size_t len)
{
    uint32_t h = 2166136261u;
    size_t i;

    for (i = 0; data != NULL && i < len; i++) {
        h = (h ^ data[i]) * 16777619u;
    }
    return h;
}

int coop_middle_dialog_route(int viewer_slot, const bool active[8], int watch_slot, int recent_slot)
{
    if (active == NULL || viewer_slot < 0 || viewer_slot >= 8) {
        return -1;
    }
    if (active[viewer_slot]) {
        return viewer_slot;
    }
    if (watch_slot >= 0 && watch_slot < 8 && active[watch_slot]) {
        return watch_slot;
    }
    if (active[0]) {
        return 0;
    }
    if (recent_slot >= 1 && recent_slot < 8 && active[recent_slot]) {
        return recent_slot;
    }
    return -1;
}

bool coop_middle_follower_access_ok(bool rule_on, bool is_leader, bool leader_in_party, bool alive, int dist)
{
    if (!alive) {
        return false;
    }
    if (is_leader) {
        return true;
    }
    return rule_on && leader_in_party && dist >= 0 && dist <= COOP_SHARED_FOLLOWER_RANGE;
}

/* ---- M5 pure ---- */

int coop_middle_keep_sector_margin(int active_remote_slots, int free_cache_slots)
{
    if (active_remote_slots <= COOP_KEEP_FULL_MAX_REMOTE || free_cache_slots >= COOP_KEEP_FREE_MIN) {
        return 0;
    }
    return COOP_KEEP_MARGIN;
}

int coop_middle_keep_ids(int64_t loc, int margin, int64_t* ids, int max)
{
    int64_t x;
    int64_t y;
    int64_t sx;
    int64_t sy;
    int64_t ox;
    int64_t oy;
    int dxs[2];
    int dys[2];
    int nx = 1;
    int ny = 1;
    int i;
    int j;
    int n = 0;

    if (margin <= 0) {
        return coop_lite_keep_ids(loc, ids, max);
    }
    if (ids == NULL || max <= 0) {
        return 0;
    }
    x = LOCATION_GET_X(loc);
    y = LOCATION_GET_Y(loc);
    sx = x >> 6;
    sy = y >> 6;
    ox = x & 63;
    oy = y & 63;
    dxs[0] = 0;
    dys[0] = 0;
    if (ox < margin) {
        dxs[nx++] = -1;
    } else if (63 - ox < margin) {
        dxs[nx++] = 1;
    }
    if (oy < margin) {
        dys[ny++] = -1;
    } else if (63 - oy < margin) {
        dys[ny++] = 1;
    }
    for (j = 0; j < ny; j++) {
        for (i = 0; i < nx; i++) {
            int64_t cx = sx + dxs[i];
            int64_t cy = sy + dys[j];

            if (cx < 0 || cy < 0) {
                continue;
            }
            if (n >= max) {
                return n;
            }
            ids[n++] = SECTOR_MAKE(cx, cy);
        }
    }
    return n;
}

bool coop_middle_loc_within(int64_t a, int64_t b, int radius)
{
    int64_t dx;
    int64_t dy;

    if (a == 0 || b == 0 || radius < 0) {
        return false;
    }
    dx = (int64_t)LOCATION_GET_X(a) - (int64_t)LOCATION_GET_X(b);
    dy = (int64_t)LOCATION_GET_Y(a) - (int64_t)LOCATION_GET_Y(b);
    return dx >= -radius && dx <= radius && dy >= -radius && dy <= radius;
}

int coop_middle_interest_radius(int packet_type, bool start_map)
{
    if (packet_type == PACKET_COOP_NPC_POS) {
        return COOP_IM_NPC_RADIUS;
    }
    if (!start_map) {
        return 0;
    }
    switch (packet_type) {
    case PACKET_COOP_DOOR_STATE:
        return COOP_IM_DOOR_RADIUS;
    case PACKET_COOP_TRAP_FX:
    case PACKET_COOP_PROJECTILE_FX:
    case 106: /* sounds */
    case 77: /* spell eye candy */
        return COOP_IM_FX_RADIUS;
    default:
        return 0;
    }
}

bool coop_middle_overworld_pkt_valid(const PacketCoopOverworldTravel* pkt, int size, bool from_host)
{
    if (pkt == NULL || size != (int)sizeof(PacketCoopOverworldTravel) || pkt->type != PACKET_COOP_OVERWORLD_TRAVEL
        || pkt->slot < 0 || pkt->slot >= 8 || (pkt->flags & ~COOP_OWT_F_MASK) != 0) {
        return false;
    }
    switch (pkt->op) {
    case COOP_OWT_START:
        if (from_host || pkt->loc == 0 || (pkt->flags & ~COOP_OWT_F_TELEPORT) != 0) {
            return false;
        }
        return (pkt->flags & COOP_OWT_F_TELEPORT) != 0
            ? pkt->world_x >= 0 && pkt->world_x < COOP_OWT_SPELL_MAX
            : pkt->world_x >= 0 && pkt->world_x < COOP_OWT_ROUTE_MAX;
    case COOP_OWT_WAYPOINT:
        return !from_host && pkt->loc != 0 && pkt->flags == 0 && pkt->world_y >= 1
            && pkt->world_y < COOP_OWT_ROUTE_MAX && pkt->world_x >= 0 && pkt->world_x < pkt->world_y;
    case COOP_OWT_SELECT:
        return from_host && pkt->flags == 0 && pkt->world_x >= 0 && pkt->world_x < COOP_OWT_SPELL_MAX;
    case COOP_OWT_CANCEL:
        return !from_host && pkt->flags == 0;
    case COOP_OWT_PROGRESS:
        return from_host && (pkt->flags & COOP_OWT_F_TELEPORT) == 0;
    case COOP_OWT_ARRIVED:
        return from_host && pkt->loc != 0 && (pkt->flags & (COOP_OWT_F_MOVING | COOP_OWT_F_TELEPORT)) == 0;
    case COOP_OWT_REFUSED:
        return from_host && pkt->world_x >= 1 && pkt->world_x <= COOP_OWT_REFUSE_MAX && pkt->flags == 0;
    default:
        return false;
    }
}

void coop_middle_owt_route_reset(CoopOwtRoute* r)
{
    if (r != NULL) {
        r->count = 0;
        r->expect = 0;
    }
}

bool coop_middle_owt_route_add(CoopOwtRoute* r, int idx, int count, int64_t loc)
{
    if (r == NULL || loc == 0 || count < 1 || count >= COOP_OWT_ROUTE_MAX || idx < 0 || idx >= count) {
        return false;
    }
    if (idx == 0) {
        r->count = 0;
        r->expect = count;
    }
    if (idx != r->count || count != r->expect) {
        coop_middle_owt_route_reset(r); /* lost or reordered: START falls back to its own dest */
        return false;
    }
    r->locs[r->count++] = loc;
    return true;
}

int coop_middle_owt_route_finish(CoopOwtRoute* r, int before, int64_t dest, int64_t* out, int max)
{
    int n = 0;
    int i;

    if (out == NULL || max < 1 || dest == 0) {
        if (r != NULL) {
            coop_middle_owt_route_reset(r);
        }
        return 0;
    }
    if (r != NULL && before > 0 && r->count == before && r->expect == before && before + 1 <= max) {
        for (i = 0; i < before; i++) {
            out[n++] = r->locs[i];
        }
    }
    out[n++] = dest;
    if (r != NULL) {
        coop_middle_owt_route_reset(r);
    }
    return n;
}

void coop_middle_sleep_reset(CoopSleepState* st)
{
    if (st != NULL) {
        st->ready_mask = 0;
        st->mode = 0;
        st->initiator = -1;
    }
}

int coop_middle_sleep_count(uint8_t mask)
{
    int n = 0;

    while (mask != 0) {
        n += mask & 1u;
        mask >>= 1;
    }
    return n;
}

static int coop_sleep_lowest(uint8_t mask)
{
    int s;

    for (s = 0; s < 8; s++) {
        if ((mask & (1u << s)) != 0) {
            return s;
        }
    }
    return -1;
}

int coop_middle_sleep_step(CoopSleepState* st, uint8_t living_mask, int ev, int slot, int mode)
{
    uint8_t bit;
    uint8_t before;

    if (st == NULL) {
        return COOP_SLEEP_RES_NONE;
    }
    bit = slot >= 0 && slot < 8 ? (uint8_t)(1u << slot) : 0;
    before = st->ready_mask;
    switch (ev) {
    case COOP_SLEEP_EV_VOTE:
        if (bit == 0 || (living_mask & bit) == 0 || mode < 1 || mode > COOP_SLEEP_MODE_MAX) {
            return COOP_SLEEP_RES_REFUSED;
        }
        if (coop_middle_sleep_count(living_mask) == 1) {
            coop_middle_sleep_reset(st);
            return COOP_SLEEP_RES_SOLO;
        }
        if (st->initiator < 0 || st->ready_mask == 0) {
            st->initiator = slot;
            st->mode = (uint8_t)mode;
        }
        st->ready_mask |= bit;
        break;
    case COOP_SLEEP_EV_CANCEL:
        st->ready_mask &= (uint8_t)~bit;
        break;
    case COOP_SLEEP_EV_CHECK:
        break;
    default:
        return COOP_SLEEP_RES_NONE;
    }
    st->ready_mask &= living_mask;
    if (st->ready_mask == 0) {
        if (st->initiator >= 0) {
            coop_middle_sleep_reset(st);
            return COOP_SLEEP_RES_CLEARED;
        }
        return COOP_SLEEP_RES_NONE;
    }
    if (st->initiator < 0 || (st->ready_mask & (1u << st->initiator)) == 0) {
        st->initiator = coop_sleep_lowest(st->ready_mask); /* the mode stays */
    }
    if ((st->ready_mask & living_mask) == living_mask) {
        return COOP_SLEEP_RES_EXECUTE;
    }
    return st->ready_mask != before || ev == COOP_SLEEP_EV_VOTE ? COOP_SLEEP_RES_WAIT : COOP_SLEEP_RES_NONE;
}

int coop_middle_sleep_minutes(int mode, int hour, int minute)
{
    int now;
    int target;
    int d;

    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return -1;
    }
    now = hour * 60 + minute;
    switch (mode) {
    case COOP_SLEEP_MODE_MORNING:
    case COOP_SLEEP_MODE_EVENING:
        target = mode == COOP_SLEEP_MODE_MORNING ? 8 * 60 : 20 * 60;
        d = (target - now + 24 * 60) % (24 * 60);
        return d == 0 ? 24 * 60 : d;
    case COOP_SLEEP_MODE_8H:
        return 8 * 60;
    case COOP_SLEEP_MODE_1H:
        return 60;
    case COOP_SLEEP_MODE_2H:
        return 2 * 60;
    case COOP_SLEEP_MODE_4H:
        return 4 * 60;
    case COOP_SLEEP_MODE_24H:
        return 24 * 60;
    default:
        return -1;
    }
}

int coop_middle_sleep_mode_from_ui(int hours_value)
{
    switch (hours_value) {
    case 1:
        return COOP_SLEEP_MODE_1H;
    case 2:
        return COOP_SLEEP_MODE_2H;
    case 4:
        return COOP_SLEEP_MODE_4H;
    case 8:
        return COOP_SLEEP_MODE_8H;
    case 24:
        return COOP_SLEEP_MODE_24H;
    case -1:
        return COOP_SLEEP_MODE_MORNING;
    case -2:
        return COOP_SLEEP_MODE_EVENING;
    case -3:
        return COOP_SLEEP_MODE_HEALED;
    default:
        return 0;
    }
}

static const char* coop_sleep_mode_name(int mode, bool ru)
{
    switch (mode) {
    case COOP_SLEEP_MODE_MORNING:
        return ru ? "до утра" : "until morning";
    case COOP_SLEEP_MODE_EVENING:
        return ru ? "до вечера" : "until evening";
    case COOP_SLEEP_MODE_HEALED:
        return ru ? "до исцеления" : "until healed";
    case COOP_SLEEP_MODE_8H:
        return ru ? "8 часов" : "8 hours";
    case COOP_SLEEP_MODE_1H:
        return ru ? "1 час" : "1 hour";
    case COOP_SLEEP_MODE_2H:
        return ru ? "2 часа" : "2 hours";
    case COOP_SLEEP_MODE_4H:
        return ru ? "4 часа" : "4 hours";
    case COOP_SLEEP_MODE_24H:
        return ru ? "сутки" : "24 hours";
    default:
        return "?";
    }
}

const char* coop_middle_sleep_banner_text(char* out, size_t sz, int ready, int living, const char* name, int mode,
    bool ru)
{
    if (out == NULL || sz == 0) {
        return out;
    }
    if (ru) {
        snprintf(out, sz, "Сон: готовы %d/%d (%s: %s). Y - спать вместе, N - отмена", ready, living,
            name != NULL ? name : "?", coop_sleep_mode_name(mode, true));
    } else {
        snprintf(out, sz, "Sleep: %d/%d ready (%s: %s). Y = join sleep, N = cancel", ready, living,
            name != NULL ? name : "?", coop_sleep_mode_name(mode, false));
    }
    return out;
}

/* ---- M7 pure helpers ---- */

uint8_t coop_middle_sleep_apply_mask(uint8_t living_mask, uint8_t subinst_mask)
{
    return (uint8_t)(living_mask & subinst_mask & 0xFEu);
}

bool coop_middle_edge_indicator(int sx, int sy, int view_w, int view_h, int pad, int* out_x, int* out_y,
    int* octant_out)
{
    int left;
    int right;
    int top;
    int bottom;
    double cx;
    double cy;
    double dx;
    double dy;
    double ax;
    double ay;
    double k = 0.0;
    int side = -1;
    int x;
    int y;
    int oct;

    if (view_w <= 0 || view_h <= 0) {
        return false;
    }
    if (pad < 0) {
        pad = 0;
    }
    if (pad * 2 > view_w) {
        pad = view_w / 2;
    }
    if (pad * 2 > view_h) {
        pad = view_h / 2;
    }
    left = pad;
    right = view_w - pad;
    top = pad;
    bottom = view_h - pad;
    if (sx >= left && sx <= right && sy >= top && sy <= bottom) {
        return false;
    }
    cx = view_w / 2.0;
    cy = view_h / 2.0;
    dx = (double)sx - cx;
    dy = (double)sy - cy;
    /* the first pad border the ray from the centre crosses */
    if (dx > 0.0 && (side < 0 || (right - cx) / dx < k)) {
        k = (right - cx) / dx;
        side = 0;
    }
    if (dx < 0.0 && (side < 0 || (left - cx) / dx < k)) {
        k = (left - cx) / dx;
        side = 1;
    }
    if (dy < 0.0 && (side < 0 || (top - cy) / dy < k)) {
        k = (top - cy) / dy;
        side = 2;
    }
    if (dy > 0.0 && (side < 0 || (bottom - cy) / dy < k)) {
        k = (bottom - cy) / dy;
        side = 3;
    }
    if (side < 0) {
        return false;
    }
    x = (int)(cx + dx * k + (dx * k >= 0.0 ? 0.5 : -0.5));
    y = (int)(cy + dy * k + (dy * k >= 0.0 ? 0.5 : -0.5));
    switch (side) {
    case 0:
        x = right;
        break;
    case 1:
        x = left;
        break;
    case 2:
        y = top;
        break;
    default:
        y = bottom;
        break;
    }
    x = x < left ? left : (x > right ? right : x);
    y = y < top ? top : (y > bottom ? bottom : y);
    /* octants without libm: tan(22.5 deg) = 0.41421356 */
    ax = dx < 0.0 ? -dx : dx;
    ay = dy < 0.0 ? -dy : dy;
    if (ay <= ax * 0.41421356) {
        oct = dx > 0.0 ? 0 : 4;
    } else if (ax <= ay * 0.41421356) {
        oct = dy < 0.0 ? 2 : 6;
    } else if (dx > 0.0) {
        oct = dy < 0.0 ? 1 : 7;
    } else {
        oct = dy < 0.0 ? 3 : 5;
    }
    if (out_x != NULL) {
        *out_x = x;
    }
    if (out_y != NULL) {
        *out_y = y;
    }
    if (octant_out != NULL) {
        *octant_out = oct;
    }
    return true;
}

bool coop_middle_collapse_step(CoopCollapseTrack* t, int slot, bool down, unsigned int now_ms)
{
    uint8_t bit;
    bool was;

    if (t == NULL || slot < 0 || slot >= 8) {
        return false;
    }
    bit = (uint8_t)(1u << slot);
    was = (t->down_mask & bit) != 0;
    if (!down) {
        t->down_mask &= (uint8_t)~bit;
        return false;
    }
    t->down_mask |= bit;
    if (was) {
        return false;
    }
    if ((t->sent_mask & bit) != 0 && (unsigned int)(now_ms - t->sent_ms[slot]) < COOP_COLLAPSE_SPAM_MS) {
        return false;
    }
    t->sent_mask |= bit;
    t->sent_ms[slot] = now_ms;
    return true;
}

bool coop_middle_autosave_ready(int autosave_min, unsigned int now_ms, unsigned int last_save_ms,
    const CoopAutosaveGate* gate)
{
    if (autosave_min <= 0 || gate == NULL) {
        return false;
    }
    if ((unsigned int)(now_ms - last_save_ms) < (unsigned int)autosave_min * 60000u) {
        return false;
    }
    return gate->in_world && !gate->paused && !gate->combat && !gate->dialog && !gate->world_map && !gate->teleport
        && !gate->commit;
}

int coop_middle_autosave_slot(int index)
{
    return COOP_AUTOSAVE_SLOT_FIRST + ((index % COOP_AUTOSAVE_SLOT_COUNT) + COOP_AUTOSAVE_SLOT_COUNT) % COOP_AUTOSAVE_SLOT_COUNT;
}

bool coop_middle_autosave_is_slot(int slot_num)
{
    return slot_num >= COOP_AUTOSAVE_SLOT_FIRST && slot_num < COOP_AUTOSAVE_SLOT_FIRST + COOP_AUTOSAVE_SLOT_COUNT;
}

int coop_middle_autosave_next_index(int newest_slot_num)
{
    return coop_middle_autosave_is_slot(newest_slot_num)
        ? (newest_slot_num - COOP_AUTOSAVE_SLOT_FIRST + 1) % COOP_AUTOSAVE_SLOT_COUNT
        : 0;
}


#ifndef COOP_LITE_TEST_STUBS

#include <SDL3/SDL.h>

#include "coop_paths.h"
#include "game/anim.h"
#include "game/antiteleport.h"
#include "game/area.h"
#include "game/magictech.h"
#include "game/path.h"
#include "game/ui.h"
#include "game/gfade.h"
#include "game/obj_pool.h"
#include "game/random.h"
#include "game/timeevent.h"
#include "ui/wmap_rnd.h"
#include "game/combat.h"
#include "game/coop_subinst.h"
#include "game/critter.h"
#include "game/gsound.h"
#include "game/item.h"
#include "game/location.h"
#include "game/map.h"
#include "game/obj.h"
#include "game/obj_flags.h"
#include "game/multiplayer.h"
#include "game/obj_id.h"
#include "game/object.h"
#include "game/player.h"
#include "game/proto.h"
#include "game/sector.h"
#include "game/stat.h"
#include "game/townmap.h"
#include "game/gamelib.h"
#include "game/teleport.h"
#include "tig/debug.h"
#include "tig/memory.h"
#include "ui/mainmenu_ui.h"
#include "ui/coop_middle_ui.h"
#include "ui/dialog_ui.h"
#include "ui/sleep_ui.h"
#include "ui/wmap_ui.h"

_Static_assert(COOP_MID_T_WEAPON == OBJ_TYPE_WEAPON && COOP_MID_T_AMMO == OBJ_TYPE_AMMO
        && COOP_MID_T_ARMOR == OBJ_TYPE_ARMOR && COOP_MID_T_GOLD == OBJ_TYPE_GOLD
        && COOP_MID_T_FOOD == OBJ_TYPE_FOOD && COOP_MID_T_SCROLL == OBJ_TYPE_SCROLL
        && COOP_MID_T_KEY == OBJ_TYPE_KEY && COOP_MID_T_KEY_RING == OBJ_TYPE_KEY_RING
        && COOP_MID_T_WRITTEN == OBJ_TYPE_WRITTEN && COOP_MID_T_GENERIC == OBJ_TYPE_GENERIC
        && COOP_MID_T_NPC == OBJ_TYPE_NPC,
    "coop_middle.h item types mirror obj.h");
_Static_assert(COOP_MID_OIF_WONT_SELL == (int)OIF_WONT_SELL && COOP_MID_OIF_UBER == (int)OIF_UBER,
    "coop_middle.h item flags mirror obj_flags.h");

/* Rules: the host's from ARCANUM_COOP_RULES (read once), a client's from
   the host's packet (defaults until it arrives, again after a reset). */
static CoopRules coop_mid_host_rules;
static bool coop_mid_host_rules_ready;
static CoopRules coop_mid_rx_rules;
static bool coop_mid_rx_valid;
static unsigned int coop_mid_now_ms;
static unsigned int coop_fog_slot_pending; /* M4 host: town map fog REPLACE snapshot to these slots */

/* Host: one rest request at a time. */
static bool coop_rest_active;
static int coop_rest_slot;
static int coop_rest_mode;
static unsigned int coop_rest_since_ms;
/* Client: last ask. */
static bool coop_rest_asked;
static unsigned int coop_rest_asked_ms;

static CoopFeedLimiter coop_feed;

static bool coop_status_handle(const PacketCoopPartyCtrl* pkt); /* M2 */
static bool coop_sleep_handle(const PacketCoopPartyCtrl* pkt); /* M5 */
static void coop_status_host_tick(unsigned int now_ms);
static void coop_status_client_tick(unsigned int now_ms);
static bool coop_pause_handle(const PacketCoopPartyCtrl* pkt); /* M3 */
static void coop_pause_set(bool on, int by, const char* why);
static void coop_pause_send(int to_slot);
static void coop_pause_clear(void);
static void coop_pings_clear(void);
static bool coop_pause_on;
static int coop_pause_by;

/* Gold a player dropped (ground piles), never split when picked up. */
#define COOP_MID_DROPS 32
static ObjectID coop_mid_drops[COOP_MID_DROPS];
static int coop_mid_drops_next;

/* Gold a player put into a container: taking it back is not a find. */
#define COOP_MID_STASH 16
static struct {
    ObjectID container;
    int64_t amount;
} coop_mid_stash[COOP_MID_STASH];
static int coop_mid_stash_next;

static bool coop_mid_session_host(void)
{
    return tig_net_is_active() && tig_net_is_host() && tig_net_is_coop_enabled();
}

const CoopRules* coop_middle_rules(void)
{
    /* M7: inside a solo instance the net is locally suspended (is_active
       false) but this is still the client: the host's rules apply. */
    if ((tig_net_is_active() && !tig_net_is_host()) || coop_subinst_client_active()) {
        if (!coop_mid_rx_valid) {
            coop_rules_default(&coop_mid_rx_rules);
        }
        return &coop_mid_rx_rules;
    }
    if (!coop_mid_host_rules_ready) {
        const char* env = coop_lite_env("ARCANUM_COOP_RULES");
        char text[160];
        char desc[512];
        int used = coop_rules_parse_env(&coop_mid_host_rules, env);

        coop_mid_host_rules_ready = true;
        coop_rules_format_env(&coop_mid_host_rules, text, sizeof(text));
        coop_rules_describe(&coop_mid_host_rules, false, desc, sizeof(desc));
        tig_debug_printf("CoopMiddle: host rules from ARCANUM_COOP_RULES='%s' (%d keys): %s | %s\n",
            env != NULL ? env : "", used, text, desc);
    }
    return &coop_mid_host_rules;
}

bool coop_middle_rule_on(uint32_t flag)
{
    return (coop_middle_rules()->flags & flag) != 0;
}

static int coop_mid_slot_of(int64_t obj)
{
    int slot;

    if (obj == OBJ_HANDLE_NULL) {
        return -1;
    }
    for (slot = 0; slot < 8; slot++) {
        if (sub_4A2B60(slot) == obj) {
            return slot;
        }
    }
    return -1;
}

static bool coop_mid_slot_living(int slot, int64_t* pc_out)
{
    int64_t pc = sub_4A2B60(slot);

    if (pc == OBJ_HANDLE_NULL || (slot > 0 && !tig_net_client_is_active(slot))) {
        return false;
    }
    if (critter_is_dead(pc) || (obj_field_int32_get(pc, OBJ_F_FLAGS) & (OF_OFF | OF_DESTROYED)) != 0) {
        return false;
    }
    if (pc_out != NULL) {
        *pc_out = pc;
    }
    return true;
}

static void coop_mid_pc_name(int slot, int64_t pc, char* out, size_t sz)
{
    char* name = NULL;
    const char* nick;

    out[0] = '\0';
    if (pc != OBJ_HANDLE_NULL) {
        obj_field_string_get(pc, OBJ_F_PC_PLAYER_NAME, &name);
        if (name != NULL) {
            strncpy(out, name, sz - 1);
            out[sz - 1] = '\0';
            FREE(name);
        }
    }
    if (out[0] == '\0') {
        nick = tig_net_client_info_get_name(slot);
        snprintf(out, sz, "%s", nick != NULL && nick[0] != '\0' ? nick : "Player");
    }
    coop_lite_chat_sanitize(out, sz);
}

static void coop_mid_ctrl_send(int op, int a, int to_slot)
{
    PacketCoopPartyCtrl pkt;
    int depth;

    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_PARTY_CTRL;
    pkt.op = op;
    pkt.a = a;
    depth = tig_net_local_suspend_pause();
    if (to_slot >= 0) {
        tig_net_send_app(&pkt, sizeof(pkt), to_slot);
    } else {
        tig_net_send_app_all(&pkt, sizeof(pkt));
    }
    tig_net_local_suspend_resume(depth);
}

/* ---- rules on the wire ---- */

void coop_middle_host_slot_synced(int slot)
{
    PacketCoopRules pkt;
    char text[128];

    if (!coop_mid_session_host() || slot <= 0 || slot >= 8) {
        return;
    }
    coop_rules_packet_fill(&pkt, coop_middle_rules());
    tig_net_send_app(&pkt, sizeof(pkt), slot);
    coop_rules_format_env(&pkt.rules, text, sizeof(text));
    tig_debug_printf("CoopMiddle: rules sent to slot %d: %s\n", slot, text);
    coop_fog_slot_pending |= 1u << slot; /* M4: town map fog snapshot on the next fog tick */
    if (coop_rest_active && coop_rest_slot == slot) {
        coop_rest_active = false; /* a rejoin starts clean */
        coop_lite_mid_prompt(NULL);
    }
    if (coop_pause_on) {
        coop_pause_send(slot); /* M3: a joiner during a pause starts paused */
        tig_debug_printf("CoopMiddle: pause state sent to slot %d (paused by %d)\n", slot, coop_pause_by);
    }
}

/* M7: the launcher lobby shows the rules (coop\host\ or coop\client\). */
static bool coop_mid_lobby_rules_written;

static void coop_mid_lobby_rules_write(const CoopRules* r)
{
    char path[1024];
    char line[512];
    FILE* f;

    if (r == NULL || !coop_paths_join(path, sizeof(path), "lobby_rules.txt")) {
        return;
    }
    coop_rules_describe(r, coop_lite_lang_ru(), line, sizeof(line));
    f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    fprintf(f, "%s\n", line);
    fclose(f);
    coop_mid_lobby_rules_written = true;
}

void coop_middle_handle_rules(PacketCoopRules* pkt)
{
    char text[160];
    char line[512];

    if (!tig_net_is_active()) {
        return;
    }
    if (tig_net_is_host()) {
        tig_debug_printf("CoopMiddle: rules packet from slot %d dropped (host -> client only)\n", sub_52A530());
        return;
    }
    if (!coop_rules_packet_valid(pkt, tig_net_last_message_size())) {
        tig_debug_printf("CoopMiddle: rules packet dropped (size %d)\n", tig_net_last_message_size());
        return;
    }
    coop_mid_rx_rules = pkt->rules;
    coop_rules_sanitize(&coop_mid_rx_rules);
    coop_mid_rx_valid = true;
    coop_rules_format_env(&coop_mid_rx_rules, text, sizeof(text));
    tig_debug_printf("CoopMiddle: rules received from the host: %s\n", text);
    coop_rules_describe(&coop_mid_rx_rules, coop_lite_lang_ru(), line, sizeof(line));
    coop_lite_mid_chat_line(line);
    coop_mid_lobby_rules_write(&coop_mid_rx_rules); /* M7 */
}

/* ---- loot feed + gold split ---- */

/* A host system line to everyone (to_a < 0) or only to slots to_a / to_b
   (the host's own slot 0 shows it locally). */
static void coop_mid_sys_line(int kind, int slot, const char* who, const char* what, int to_a, int to_b)
{
    PacketCoopChat pkt;
    char line[256];

    if (to_a < 0 || to_a == 0 || to_b == 0) {
        coop_middle_feed_format(kind, who, what, coop_lite_lang_ru(), line, sizeof(line));
        coop_lite_mid_chat_line(line);
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_CHAT;
    pkt.slot = slot >= 0 && slot < 8 ? slot : 0;
    pkt.reserved = COOP_MID_CHAT_SYS_MAGIC | kind;
    snprintf(pkt.name, sizeof(pkt.name), "%s", who != NULL ? who : "");
    snprintf(pkt.text, sizeof(pkt.text), "%s", what != NULL && what[0] != '\0' ? what : "?");
    coop_lite_chat_sanitize(pkt.name, sizeof(pkt.name));
    coop_lite_chat_sanitize(pkt.text, sizeof(pkt.text));
    if (pkt.text[0] == '\0') {
        snprintf(pkt.text, sizeof(pkt.text), "?");
    }
    {
        int depth = tig_net_local_suspend_pause();

        if (to_a < 0) {
            tig_net_send_app_all(&pkt, sizeof(pkt));
        } else {
            if (to_a > 0 && to_a < 8) {
                tig_net_send_app(&pkt, sizeof(pkt), to_a);
            }
            if (to_b > 0 && to_b < 8 && to_b != to_a) {
                tig_net_send_app(&pkt, sizeof(pkt), to_b);
            }
        }
        tig_net_local_suspend_resume(depth);
    }
}

void coop_middle_sys_line_all(int kind, int slot, const char* who, const char* what)
{
    if (coop_mid_session_host()) {
        coop_mid_sys_line(kind, slot, who, what, -1, -1);
    }
}

static void coop_mid_feed_send(int kind, int slot, const char* who, const char* what)
{
    coop_mid_sys_line(kind, slot, who, what, -1, -1);
}

static void coop_mid_feed(int kind, int slot, const char* who, const char* what)
{
    if (!coop_middle_feed_admit(&coop_feed, coop_mid_now_ms)) {
        return; /* collapsed: one "... and N more" line after the burst */
    }
    coop_mid_feed_send(kind, slot, who, what);
}

bool coop_middle_client_chat(const PacketCoopChat* pkt)
{
    char line[256];
    int kind;

    if (pkt == NULL || ((unsigned int)pkt->reserved & 0xFFFF0000u) != (unsigned int)COOP_MID_CHAT_SYS_MAGIC) {
        return false;
    }
    kind = pkt->reserved & 0xFFFF;
    if (kind < COOP_MID_CHAT_LOOT || kind > COOP_MID_CHAT_KIND_MAX) {
        return true; /* a newer Middle line we do not know: drop it */
    }
    coop_middle_feed_format(kind, pkt->name, pkt->text, coop_lite_lang_ru(), line, sizeof(line));
    coop_lite_mid_chat_line(line);
    return true;
}

void coop_middle_note_player_drop(int64_t item)
{
    if (item == OBJ_HANDLE_NULL || !coop_mid_session_host()
        || obj_field_int32_get(item, OBJ_F_TYPE) != OBJ_TYPE_GOLD) {
        return;
    }
    coop_mid_drops[coop_mid_drops_next] = obj_get_id(item);
    coop_mid_drops_next = (coop_mid_drops_next + 1) % COOP_MID_DROPS;
}

static bool coop_mid_take_player_drop(int64_t item)
{
    ObjectID oid = obj_get_id(item);
    int i;

    for (i = 0; i < COOP_MID_DROPS; i++) {
        if (coop_mid_drops[i].type != OID_TYPE_NULL && objid_is_equal(coop_mid_drops[i], oid)) {
            memset(&coop_mid_drops[i], 0, sizeof(coop_mid_drops[i]));
            coop_mid_drops[i].type = OID_TYPE_NULL;
            return true;
        }
    }
    return false;
}

static int coop_mid_stash_find(int64_t container)
{
    ObjectID oid = obj_get_id(container);
    int i;

    for (i = 0; i < COOP_MID_STASH; i++) {
        if (coop_mid_stash[i].amount > 0 && objid_is_equal(coop_mid_stash[i].container, oid)) {
            return i;
        }
    }
    return -1;
}

/* A PC's loot source: an unlocked-for-it container that is no store, or a
   dead NPC (never a PC: a fallen player's pack is that player's). */
static bool coop_mid_loot_source(int64_t src, int64_t pc)
{
    int type;

    if (src == OBJ_HANDLE_NULL) {
        return false;
    }
    type = obj_field_int32_get(src, OBJ_F_TYPE);
    if (type == OBJ_TYPE_CONTAINER) {
        return !coop_lite_container_is_store(src, pc);
    }
    return (type == OBJ_TYPE_NPC || type == OBJ_TYPE_UNIQUE_NPC || type == OBJ_TYPE_MONSTER) && critter_is_dead(src);
}

void coop_middle_take_begin(CoopMiddleTake* t, int64_t from, int64_t item, int64_t to, int qty)
{
    const CoopRules* rules;
    int to_slot;
    int type;

    if (t == NULL) {
        return;
    }
    memset(t, 0, sizeof(*t));
    if (item == OBJ_HANDLE_NULL || !coop_mid_session_host()) {
        return;
    }
    rules = coop_middle_rules();
    if ((rules->flags & (COOP_RULE_GOLD_SPLIT | COOP_RULE_LOOT_FEED)) == 0) {
        return;
    }
    type = obj_field_int32_get(item, OBJ_F_TYPE);
    to_slot = coop_mid_slot_of(to);
    if (to_slot < 0) {
        /* PC -> container gold: remember it (taking it back is no find). */
        int from_slot = coop_mid_slot_of(from);

        if (from_slot >= 0 && type == OBJ_TYPE_GOLD && to != OBJ_HANDLE_NULL
            && obj_field_int32_get(to, OBJ_F_TYPE) == OBJ_TYPE_CONTAINER) {
            t->armed = true;
            t->stash = true;
            t->other = to;
            t->stash_qty = qty > 0 ? qty : obj_field_int32_get(item, OBJ_F_GOLD_QUANTITY);
        }
        return;
    }
    if (from != OBJ_HANDLE_NULL && !coop_mid_loot_source(from, to)) {
        return; /* trade, gift, steal, own moves */
    }
    t->taker = to;
    t->other = from;
    t->slot = to_slot;
    t->type = type;
    if (type == OBJ_TYPE_GOLD) {
        int pile = obj_field_int32_get(item, OBJ_F_GOLD_QUANTITY);

        if ((rules->flags & COOP_RULE_GOLD_SPLIT) == 0) {
            return;
        }
        t->qty = qty > 0 && qty < pile ? qty : pile;
        if (from == OBJ_HANDLE_NULL && coop_mid_take_player_drop(item)) {
            tig_debug_printf("CoopMiddle: gold split: slot %d picked up %d gold a player dropped - not split\n",
                to_slot, t->qty);
            return;
        }
        if (from != OBJ_HANDLE_NULL) {
            int i = coop_mid_stash_find(from);

            if (i >= 0) {
                int64_t back = coop_mid_stash[i].amount < t->qty ? coop_mid_stash[i].amount : t->qty;

                coop_mid_stash[i].amount -= back;
                t->qty -= (int)back;
                tig_debug_printf("CoopMiddle: gold split: %d of the gold was put there by a player - not split\n",
                    (int)back);
            }
        }
        t->gold_before = item_gold_get(to);
        t->armed = t->qty > 0;
        return;
    }
    if ((rules->flags & COOP_RULE_LOOT_FEED) != 0) {
        int flags = obj_field_int32_get(item, OBJ_F_ITEM_FLAGS);
        bool quest = (flags & (int)OIF_WONT_SELL) != 0;
        char name[128];

        if (coop_middle_loot_feed_worthy(type, flags, item_worth(item), quest)) {
            name[0] = '\0';
            object_examine(item, to, name);
            snprintf(t->item_name, sizeof(t->item_name), "%s", name);
            t->item = item;
            t->feed = true;
            t->armed = true;
        }
    }
}

static void coop_mid_split(CoopMiddleTake* t)
{
    int64_t pcs[8];
    int slots[8];
    int n = 0;
    bool taker_in = false;
    int carry = 0;
    int each = 0;
    int rem;
    int given = 0;
    int slot;
    int i;
    char who[64];
    char what[32];

    for (slot = 0; slot < 8; slot++) {
        int64_t pc;

        if (!coop_mid_slot_living(slot, &pc)) {
            continue;
        }
        if (pc == t->taker) {
            taker_in = true;
        }
        pcs[n] = pc;
        slots[n++] = slot;
    }
    if (!taker_in || n <= 1) {
        tig_debug_printf("CoopMiddle: gold split: slot %d took %d gold, nobody else alive - kept\n", t->slot, t->qty);
        return;
    }
    rem = coop_middle_split_gold(t->qty, n, &carry, &each);
    if (each > 0) {
        multiplayer_lock();
        for (i = 0; i < n; i++) {
            if (pcs[i] == t->taker) {
                continue;
            }
            if (item_gold_get(t->taker) < each || !item_gold_transfer(t->taker, pcs[i], each, OBJ_HANDLE_NULL)) {
                tig_debug_printf("CoopMiddle: gold split: transfer to slot %d failed\n", slots[i]);
                continue;
            }
            given += each;
        }
        multiplayer_unlock();
        for (i = 0; i < n; i++) {
            if (slots[i] > 0) {
                coop_lite_host_sync_slot_inven(slots[i]);
            }
        }
    }
    tig_debug_printf("CoopMiddle: gold split: slot %d took %d gold (%s) -> %d players, %d each, picker +%d, given %d\n",
        t->slot, t->qty, t->other != OBJ_HANDLE_NULL ? "loot" : "ground", n, each, rem, given);
    if (t->qty >= COOP_GOLD_FEED_MIN && each > 0 && coop_middle_rule_on(COOP_RULE_LOOT_FEED)) {
        coop_mid_pc_name(t->slot, t->taker, who, sizeof(who));
        snprintf(what, sizeof(what), "%d %d", t->qty, each);
        coop_mid_feed(COOP_MID_CHAT_GOLD, t->slot, who, what);
    }
}

void coop_middle_take_done(CoopMiddleTake* t, bool ok)
{
    char who[64];

    if (t == NULL || !t->armed) {
        return;
    }
    t->armed = false;
    if (!ok || !coop_mid_session_host()) {
        return;
    }
    if (t->stash) {
        int i = coop_mid_stash_find(t->other);

        if (i < 0) {
            i = coop_mid_stash_next;
            coop_mid_stash_next = (coop_mid_stash_next + 1) % COOP_MID_STASH;
            coop_mid_stash[i].container = obj_get_id(t->other);
            coop_mid_stash[i].amount = 0;
        }
        coop_mid_stash[i].amount += t->stash_qty;
        return;
    }
    if (t->type == OBJ_TYPE_GOLD) {
        int gained = item_gold_get(t->taker) - t->gold_before;

        if (gained <= 0) {
            return; /* nothing arrived (refused / partial path) */
        }
        if (gained < t->qty) {
            t->qty = gained;
        }
        coop_mid_split(t);
        return;
    }
    if (t->feed) {
        int64_t parent = OBJ_HANDLE_NULL;

        if (!item_parent(t->item, &parent) || parent != t->taker) {
            return; /* did not land in the taker's pack */
        }
        coop_mid_pc_name(t->slot, t->taker, who, sizeof(who));
        tig_debug_printf("CoopMiddle: loot feed: slot %d '%s' (%s)\n", t->slot, t->item_name,
            t->other != OBJ_HANDLE_NULL ? "loot" : "ground");
        coop_mid_feed(COOP_MID_CHAT_LOOT, t->slot, who, t->item_name);
    }
}

/* ---- rest request ---- */

static bool coop_mid_host_in_combat(void)
{
    int64_t pc = player_get_local_pc_obj();

    return combat_turn_based_is_active() || (pc != OBJ_HANDLE_NULL && combat_critter_is_combat_mode_active(pc));
}

static void coop_rest_end(int reason, const char* why)
{
    if (!coop_rest_active) {
        return;
    }
    coop_rest_active = false;
    coop_lite_mid_prompt(NULL);
    if (reason >= 0 && tig_net_client_is_active(coop_rest_slot)) {
        coop_mid_ctrl_send(COOP_CTRL_REST_REFUSE, reason, coop_rest_slot);
    }
    tig_debug_printf("CoopMiddle: rest request of slot %d ended: %s (reason %d)\n", coop_rest_slot, why, reason);
}

static void coop_rest_host_ask(int slot, int mode)
{
    int64_t pc = OBJ_HANDLE_NULL;
    char name[64];
    char text[256];
    bool ru = coop_lite_lang_ru();
    int reason = -1;

    if (!coop_middle_rule_on(COOP_RULE_REST_VOTE)) {
        reason = COOP_REST_REFUSE_RULE_OFF;
    } else if (!coop_mid_slot_living(slot, &pc)) {
        reason = COOP_REST_REFUSE_CANCELLED;
    } else if (coop_mid_host_in_combat()) {
        reason = COOP_REST_REFUSE_COMBAT;
    } else if (coop_lite_mid_prompt_busy() || (coop_rest_active && coop_rest_slot != slot) || sleep_ui_is_active()) {
        reason = COOP_REST_REFUSE_BUSY;
    }
    if (reason >= 0) {
        coop_mid_ctrl_send(COOP_CTRL_REST_REFUSE, reason, slot);
        tig_debug_printf("CoopMiddle: rest ask from slot %d (mode %d) refused: reason %d\n", slot, mode, reason);
        return;
    }
    coop_rest_active = true;
    coop_rest_slot = slot;
    coop_rest_mode = mode;
    coop_rest_since_ms = coop_mid_now_ms;
    coop_mid_pc_name(slot, pc, name, sizeof(name));
    if (mode == COOP_REST_MODE_NIGHT) {
        snprintf(text, sizeof(text), ru ? "%s просит отдохнуть. Y - отдохнуть 8 часов, N - отказ."
                                        : "%s wants to rest. Y = rest 8 hours, N = refuse.",
            name);
    } else {
        snprintf(text, sizeof(text), ru ? "%s просит отдохнуть. Y - отдохнуть до исцеления, N - отказ."
                                        : "%s wants to rest. Y = rest until healed, N = refuse.",
            name);
    }
    coop_lite_mid_prompt(text);
    coop_lite_mid_notice(text, true);
    tig_debug_printf("CoopMiddle: rest ask from slot %d (mode %d) - Y / N prompt\n", slot, mode);
}

bool coop_middle_rest_key(bool yes)
{
    int mode;

    if (!coop_rest_active || !tig_net_is_active() || !tig_net_is_host()) {
        return false;
    }
    if (!yes) {
        coop_rest_end(COOP_REST_REFUSE_HOST, "host pressed N");
        return true;
    }
    if (coop_mid_host_in_combat()) {
        coop_rest_end(COOP_REST_REFUSE_COMBAT, "host in combat");
        return true;
    }
    mode = coop_rest_mode;
    coop_rest_active = false;
    coop_lite_mid_prompt(NULL);
    if (!sleep_ui_coop_rest(mode == COOP_REST_MODE_HEALED)) {
        coop_mid_ctrl_send(COOP_CTRL_REST_REFUSE, COOP_REST_REFUSE_CANT_REST, coop_rest_slot);
        tig_debug_printf("CoopMiddle: rest of slot %d accepted, but the sleep window refused (reason %d)\n",
            coop_rest_slot, COOP_REST_REFUSE_CANT_REST);
        return true;
    }
    tig_debug_printf("CoopMiddle: rest of slot %d accepted - party rests (%s)\n", coop_rest_slot,
        mode == COOP_REST_MODE_HEALED ? "until healed" : "8 hours");
    return true;
}

bool coop_middle_client_rest_ask(int mode)
{
    int64_t pc = player_get_local_pc_obj();
    bool ru = coop_lite_lang_ru();

    if (!tig_net_is_active() || tig_net_is_host() || !coop_middle_rule_on(COOP_RULE_REST_VOTE)) {
        return false; /* Lite: "Only the party leader can rest or wait." */
    }
    if (pc == OBJ_HANDLE_NULL || critter_is_dead(pc) || combat_critter_is_combat_mode_active(pc)) {
        return false;
    }
    if (!coop_middle_rest_ask_allowed(coop_rest_asked, coop_rest_asked_ms, coop_mid_now_ms)) {
        coop_lite_mid_notice(ru ? "Запрос на отдых уже отправлен - ждите ответа хоста."
                                : "Rest request already sent - waiting for the host.",
            true);
        return true;
    }
    if (mode != COOP_REST_MODE_NIGHT) {
        mode = COOP_REST_MODE_HEALED;
    }
    coop_rest_asked = true;
    coop_rest_asked_ms = coop_mid_now_ms;
    coop_mid_ctrl_send(COOP_CTRL_REST_ASK, mode, 0);
    coop_lite_mid_notice(ru ? "Запрос на отдых отправлен хосту (Y / N)." : "Rest request sent to the host (Y / N).", true);
    tig_debug_printf("CoopMiddle: rest ask sent (mode %d)\n", mode);
    return true;
}

bool coop_middle_handle_party_ctrl(const PacketCoopPartyCtrl* pkt)
{
    int slot;

    if (pkt != NULL && pkt->op == COOP_CTRL_SLOT_STATUS) {
        return coop_status_handle(pkt);
    }
    if (pkt != NULL && pkt->op == COOP_CTRL_PAUSE) {
        return coop_pause_handle(pkt); /* M3 */
    }
    if (pkt != NULL
        && (pkt->op == COOP_CTRL_SLEEP_VOTE || pkt->op == COOP_CTRL_SLEEP_STATE || pkt->op == COOP_CTRL_SLEEP_APPLY)) {
        return coop_sleep_handle(pkt); /* M5 */
    }
    if (pkt == NULL || (pkt->op != COOP_CTRL_REST_ASK && pkt->op != COOP_CTRL_REST_REFUSE)) {
        return false;
    }
    if (tig_net_is_host()) {
        slot = sub_52A530();
        if (pkt->op != COOP_CTRL_REST_ASK || slot <= 0 || slot >= 8) {
            tig_debug_printf("CoopMiddle: rest op %d from slot %d dropped (wrong direction)\n", pkt->op, slot);
            return true;
        }
        coop_rest_host_ask(slot, pkt->a);
        return true;
    }
    if (pkt->op != COOP_CTRL_REST_REFUSE) {
        return true; /* the host never asks a client */
    }
    coop_rest_asked = false; /* may ask again right away */
    coop_lite_mid_notice(coop_middle_rest_refuse_text(pkt->a, coop_lite_lang_ru()), false);
    tig_debug_printf("CoopMiddle: rest request refused by the host (reason %d)\n", pkt->a);
    return true;
}

/* ---- M2: status badges ---- */

static uint8_t coop_status_local; /* this machine's INVEN / DIALOG */
static uint8_t coop_status_client[8]; /* host: what each client reported */
static uint32_t coop_status_rx; /* client: the host's last broadcast */
static uint32_t coop_status_host; /* host: the merged packing */
static bool coop_status_sent_valid;
static uint32_t coop_status_sent;
static unsigned int coop_status_sent_ms;
static unsigned int coop_status_poll_ms;
static bool coop_status_c_sent_valid;
static uint8_t coop_status_c_sent;
static unsigned int coop_status_c_sent_ms;

int coop_middle_slot_of_pc(int64_t obj)
{
    return coop_mid_slot_of(obj);
}

void coop_middle_status_set_local(uint8_t bits)
{
    coop_status_local = bits & COOP_STATUS_CLIENT_MASK;
}

uint32_t coop_middle_status_packed(void)
{
    if (!tig_net_is_active() || !tig_net_is_coop_enabled()) {
        return 0;
    }
    return tig_net_is_host() ? coop_status_host : coop_status_rx;
}

uint8_t coop_middle_slot_status(int slot)
{
    return coop_middle_status_get(coop_middle_status_packed(), slot);
}

static void coop_status_host_tick(unsigned int now_ms)
{
    uint8_t bits[8];
    int slot;

    if (coop_status_sent_valid && (unsigned int)(now_ms - coop_status_poll_ms) < COOP_STATUS_POLL_MS) {
        return;
    }
    coop_status_poll_ms = now_ms;
    for (slot = 0; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);

        bits[slot] = 0;
        if (pc == OBJ_HANDLE_NULL || (slot > 0 && !tig_net_client_is_active(slot))) {
            continue;
        }
        bits[slot] = slot == 0 ? coop_status_local : coop_status_client[slot];
        if (stat_level_get(pc, STAT_POISON_LEVEL) > 0) {
            bits[slot] |= COOP_STATUS_POISON;
        }
        if (combat_critter_is_combat_mode_active(pc)) {
            bits[slot] |= COOP_STATUS_COMBAT;
        }
    }
    coop_status_host = coop_middle_status_pack(bits);
    if (coop_status_sent_valid && coop_status_host == coop_status_sent
        && (unsigned int)(now_ms - coop_status_sent_ms) < COOP_STATUS_SEND_MS) {
        return;
    }
    if (!coop_status_sent_valid || coop_status_host != coop_status_sent) {
        tig_debug_printf("CoopMiddle: status: 0x%08x\n", (unsigned int)coop_status_host);
    }
    coop_status_sent_valid = true;
    coop_status_sent = coop_status_host;
    coop_status_sent_ms = now_ms;
    coop_mid_ctrl_send(COOP_CTRL_SLOT_STATUS, (int)coop_status_host, -1);
}

static void coop_status_client_tick(unsigned int now_ms)
{
    if (coop_status_c_sent_valid && coop_status_c_sent == coop_status_local
        && (unsigned int)(now_ms - coop_status_c_sent_ms) < COOP_STATUS_SEND_MS) {
        return;
    }
    coop_status_c_sent_valid = true;
    coop_status_c_sent = coop_status_local;
    coop_status_c_sent_ms = now_ms;
    coop_mid_ctrl_send(COOP_CTRL_SLOT_STATUS, coop_status_local, 0);
}

static bool coop_status_handle(const PacketCoopPartyCtrl* pkt)
{
    int slot;

    if (tig_net_is_host()) {
        slot = sub_52A530();
        if (slot > 0 && slot < 8) {
            coop_status_client[slot] = (uint8_t)(pkt->a & COOP_STATUS_CLIENT_MASK);
        }
        return true;
    }
    coop_status_rx = (uint32_t)pkt->a;
    return true;
}

static void coop_status_reset(void)
{
    coop_status_local = 0;
    memset(coop_status_client, 0, sizeof(coop_status_client));
    coop_status_rx = 0;
    coop_status_host = 0;
    coop_status_sent_valid = false;
    coop_status_c_sent_valid = false;
}

/* ---- M2: direct trade ---- */

/* This machine's open trade window (the partner PC), host or client. */
static bool coop_trade_local;
static int64_t coop_trade_local_partner;

bool coop_middle_trade_live(int64_t pc, int64_t partner)
{
    int self_slot = coop_mid_slot_of(pc);
    int partner_slot = coop_mid_slot_of(partner);
    bool active;
    int64_t a;
    int64_t b;
    int dist = -1;

    if (pc == OBJ_HANDLE_NULL || partner == OBJ_HANDLE_NULL || !tig_net_is_coop_enabled()) {
        return false;
    }
    if ((obj_field_int32_get(partner, OBJ_F_FLAGS) & (OF_OFF | OF_DESTROYED)) != 0) {
        return false;
    }
    /* The host knows who is connected; a client trusts the PC list (the
       host checks again). */
    active = partner_slot == 0 || !tig_net_is_host() || tig_net_client_is_active(partner_slot);
    a = obj_field_int64_get(pc, OBJ_F_LOCATION);
    b = obj_field_int64_get(partner, OBJ_F_LOCATION);
    dist = (int)location_dist(a, b);
    return coop_middle_trade_partner_ok(coop_middle_rule_on(COOP_RULE_DIRECT_TRADE), self_slot, partner_slot,
        obj_field_int32_get(partner, OBJ_F_TYPE) == OBJ_TYPE_PC, active,
        critter_is_dead(pc), critter_is_dead(partner), dist);
}

bool coop_middle_host_trade_open(int slot, int64_t pc, int64_t target)
{
    int other = coop_mid_slot_of(target);
    bool ok = coop_mid_session_host() && coop_middle_trade_live(pc, target);

    tig_debug_printf("CoopMiddle: trade: slot %d asked to open a trade window with slot %d: %s\n", slot, other,
        ok ? "opened" : (coop_middle_rule_on(COOP_RULE_DIRECT_TRADE) ? "refused (not a live player in range)"
                                                                      : "refused (rule off)"));
    return ok;
}

void coop_middle_trade_given(int64_t giver, int64_t receiver, const char* item_name, int type, int qty)
{
    int from_slot = coop_mid_slot_of(giver);
    int to_slot = coop_mid_slot_of(receiver);
    char who[64];
    char to[64];
    char what[160];
    char* p;
    int kind;

    if (!coop_mid_session_host() || from_slot < 0 || to_slot < 0) {
        return;
    }
    coop_mid_pc_name(from_slot, giver, who, sizeof(who));
    coop_mid_pc_name(to_slot, receiver, to, sizeof(to));
    for (p = to; *p != '\0'; p++) {
        if (*p == '|') {
            *p = '/'; /* the separator below */
        }
    }
    if (type == OBJ_TYPE_GOLD) {
        kind = COOP_MID_CHAT_TRADE_GOLD;
        snprintf(what, sizeof(what), "%s|%d", to, qty);
    } else {
        kind = COOP_MID_CHAT_TRADE;
        if (type == OBJ_TYPE_AMMO && qty > 1) {
            snprintf(what, sizeof(what), "%s|%d %s", to, qty, item_name != NULL ? item_name : "");
        } else {
            snprintf(what, sizeof(what), "%s|%s", to, item_name != NULL && item_name[0] != '\0' ? item_name : "?");
        }
    }
    tig_debug_printf("CoopMiddle: trade: slot %d gave '%s' (type %d, qty %d) to slot %d\n", from_slot,
        item_name != NULL ? item_name : "", type, qty, to_slot);
    if (coop_middle_rule_on(COOP_RULE_LOOT_FEED)) {
        coop_mid_sys_line(kind, from_slot, who, what, -1, -1);
    } else {
        coop_mid_sys_line(kind, from_slot, who, what, from_slot, to_slot);
    }
}

void coop_middle_trade_local_begin(int64_t partner)
{
    coop_trade_local = partner != OBJ_HANDLE_NULL;
    coop_trade_local_partner = partner;
}

void coop_middle_trade_local_end(void)
{
    if (coop_trade_local) {
        tig_debug_printf("CoopMiddle: trade: window closed\n");
    }
    coop_trade_local = false;
    coop_trade_local_partner = OBJ_HANDLE_NULL;
}

int64_t coop_middle_trade_partner(void)
{
    return coop_trade_local ? coop_trade_local_partner : OBJ_HANDLE_NULL;
}

bool coop_middle_trade_local_is(int64_t partner)
{
    return coop_trade_local && partner != OBJ_HANDLE_NULL && partner == coop_trade_local_partner;
}

int coop_middle_trade_barter_kind(int64_t target, int kind)
{
    return coop_middle_trade_local_is(target) ? COOP_BARTER_OPEN_TRADE : kind;
}

bool coop_middle_trade_refuse_take(int64_t partner)
{
    if (!coop_middle_trade_local_is(partner)) {
        return false;
    }
    coop_lite_mid_notice(coop_lite_lang_ru() ? "Взять можно только то, что игрок передаст сам."
                                             : "You can only receive what your teammate gives you.",
        false);
    return true;
}

bool coop_middle_host_trade_give(int64_t pc, int64_t partner, int64_t item)
{
    char iname[128];
    int type;
    int qty = 1;
    bool ok;
    int slot;

    if (!coop_mid_session_host() || !coop_middle_trade_local_is(partner) || item == OBJ_HANDLE_NULL) {
        return false;
    }
    if (!coop_middle_trade_live(pc, partner)) {
        coop_lite_mid_notice(coop_lite_lang_ru() ? "Подойдите ближе для обмена." : "Move closer to trade.", false);
        return true;
    }
    iname[0] = '\0';
    object_examine(item, pc, iname);
    type = obj_field_int32_get(item, OBJ_F_TYPE);
    if (type == OBJ_TYPE_GOLD) {
        qty = item_gold_get(item);
        ok = item_gold_transfer(pc, partner, qty, item);
    } else if (type == OBJ_TYPE_AMMO) {
        qty = obj_field_int32_get(item, OBJ_F_AMMO_QUANTITY);
        ok = item_ammo_transfer(pc, partner, qty, obj_field_int32_get(item, OBJ_F_AMMO_TYPE), item);
    } else {
        ok = item_transfer(item, partner);
    }
    slot = coop_mid_slot_of(partner);
    if (slot > 0) {
        coop_lite_host_sync_slot_inven(slot);
    }
    if (ok) {
        coop_middle_trade_given(pc, partner, iname, type, qty);
    } else {
        tig_debug_printf("CoopMiddle: trade: host give of '%s' failed\n", iname);
    }
    return true;
}

/* ---- M3: shared tactical pause ---- */

static bool coop_pause_on;
static int coop_pause_by = -1;
static bool coop_pause_have[8]; /* host: per slot; client: [0] = own asks */
static unsigned int coop_pause_last_ms[8];
static bool coop_pause_note_have;
static unsigned int coop_pause_note_ms;

bool coop_middle_is_paused(void)
{
    /* M7: a client inside a solo instance freezes on the host's pause too. */
    return coop_pause_on && (tig_net_is_active() || coop_subinst_client_active()) && tig_net_is_coop_enabled();
}

int coop_middle_paused_by(void)
{
    return coop_middle_is_paused() ? coop_pause_by : -1;
}

/* Host: the state to one slot (> 0) or everyone (-1). */
static void coop_pause_send(int to_slot)
{
    PacketCoopPartyCtrl pkt;
    int depth;

    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_PARTY_CTRL;
    pkt.op = COOP_CTRL_PAUSE;
    pkt.a = coop_pause_on ? 1 : 0;
    pkt.b = coop_pause_on && coop_pause_by >= 0 && coop_pause_by < 8 ? coop_pause_by : 0;
    depth = tig_net_local_suspend_pause();
    if (to_slot > 0) {
        tig_net_send_app(&pkt, sizeof(pkt), to_slot);
    } else {
        tig_net_send_app_all(&pkt, sizeof(pkt));
    }
    tig_net_local_suspend_resume(depth);
}

static void coop_pause_set(bool on, int by, const char* why)
{
    if (coop_pause_on == on && (!on || coop_pause_by == by)) {
        return;
    }
    coop_pause_on = on;
    coop_pause_by = on ? by : -1;
    tig_debug_printf("CoopMiddle: pause %s (slot %d, %s)\n", on ? "ON" : "OFF", by, why);
    if (coop_mid_session_host()) {
        coop_pause_send(-1);
    }
}

static void coop_pause_host_request(int slot, bool want)
{
    if (slot > 0) {
        if (!coop_middle_rule_on(COOP_RULE_CLIENT_PAUSE) || !tig_net_client_is_active(slot)) {
            tig_debug_printf("CoopMiddle: pause request from slot %d refused (rule off or inactive)\n", slot);
            coop_pause_send(slot); /* resync the asker */
            return;
        }
        if (!coop_middle_rate_admit(&coop_pause_have[slot], &coop_pause_last_ms[slot], coop_mid_now_ms,
                COOP_PAUSE_RATE_MS)) {
            tig_debug_printf("CoopMiddle: pause request from slot %d dropped (rate limit)\n", slot);
            return;
        }
    }
    coop_pause_set(want, slot, slot == 0 ? "host key" : "client request");
}

bool coop_middle_pause_key(void)
{
    PacketCoopPartyCtrl pkt;
    int depth;
    bool want;
    bool ru = coop_lite_lang_ru();

    if (!tig_net_is_active() || !tig_net_is_coop_enabled()) {
        return false;
    }
    want = !coop_middle_is_paused();
    if (tig_net_is_host()) {
        coop_pause_host_request(0, want);
        return true;
    }
    if (!coop_middle_rule_on(COOP_RULE_CLIENT_PAUSE)) {
        coop_lite_mid_notice(ru ? "Хост не разрешил паузу клиентам." : "The host has client pause turned off.", false);
        return true;
    }
    if (!coop_middle_rate_admit(&coop_pause_have[0], &coop_pause_last_ms[0], coop_mid_now_ms, COOP_PAUSE_RATE_MS)) {
        coop_lite_mid_notice(ru ? "Не так часто: пауза раз в 2 секунды." : "Not so fast: one pause toggle per 2 seconds.", false);
        return true;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_PARTY_CTRL;
    pkt.op = COOP_CTRL_PAUSE;
    pkt.a = want ? 1 : 0;
    depth = tig_net_local_suspend_pause();
    tig_net_send_app(&pkt, sizeof(pkt), 0);
    tig_net_local_suspend_resume(depth);
    tig_debug_printf("CoopMiddle: pause %s asked\n", want ? "on" : "off");
    return true;
}

static bool coop_pause_handle(const PacketCoopPartyCtrl* pkt)
{
    if (tig_net_is_host()) {
        int slot = sub_52A530();

        if (slot <= 0 || slot >= 8) {
            return true;
        }
        coop_pause_host_request(slot, pkt->a == 1);
        return true;
    }
    if (sub_52A530() != 0) {
        return true;
    }
    if ((pkt->a == 1) != coop_pause_on || (pkt->a == 1 && pkt->b != coop_pause_by)) {
        tig_debug_printf("CoopMiddle: pause %s from the host (slot %d)\n", pkt->a == 1 ? "ON" : "OFF", pkt->b);
    }
    coop_pause_on = pkt->a == 1;
    coop_pause_by = coop_pause_on ? pkt->b : -1;
    return true;
}

bool coop_middle_orders_blocked(bool notice)
{
    if (!coop_middle_is_paused()) {
        return false;
    }
    if (notice && coop_middle_rate_admit(&coop_pause_note_have, &coop_pause_note_ms, coop_mid_now_ms, 1500u)) {
        coop_lite_mid_notice(coop_lite_lang_ru() ? "Пауза: приказы недоступны до продолжения."
                                                 : "Paused: no orders until the game resumes.",
            false);
    }
    return true;
}

static void coop_pause_clear(void)
{
    coop_pause_on = false;
    coop_pause_by = -1;
    memset(coop_pause_have, 0, sizeof(coop_pause_have));
    coop_pause_note_have = false;
}

/* ---- M3: difficulty (host) ---- */

#define COOP_DMG_REM_SLOTS 32
static struct {
    int64_t obj;
    int rem;
} coop_dmg_rem[COOP_DMG_REM_SLOTS];
static int coop_dmg_rem_next;
static bool coop_dmg_logged;
static bool coop_ff_have;
static unsigned int coop_ff_log_ms;

static int* coop_dmg_rem_for(int64_t obj)
{
    int i;

    for (i = 0; i < COOP_DMG_REM_SLOTS; i++) {
        if (coop_dmg_rem[i].obj == obj) {
            return &(coop_dmg_rem[i].rem);
        }
    }
    i = coop_dmg_rem_next;
    coop_dmg_rem_next = (coop_dmg_rem_next + 1) % COOP_DMG_REM_SLOTS;
    coop_dmg_rem[i].obj = obj;
    coop_dmg_rem[i].rem = 0;
    return &(coop_dmg_rem[i].rem);
}

int coop_middle_host_enemy_damage(int64_t target, int dam)
{
    int pct;
    int64_t host_pc;
    int scaled;

    if (dam <= 0 || target == OBJ_HANDLE_NULL || !coop_mid_session_host()) {
        return dam;
    }
    pct = coop_middle_rules()->enemy_hp_pct;
    if (pct <= 100 || obj_field_int32_get(target, OBJ_F_TYPE) != OBJ_TYPE_NPC) {
        return dam;
    }
    host_pc = sub_4A2B60(0);
    if (host_pc == OBJ_HANDLE_NULL || coop_lite_party_no_fight(host_pc, target)) {
        return dam; /* a follower / summon of the party: never scaled */
    }
    scaled = coop_middle_scale_enemy_damage(dam, pct, coop_dmg_rem_for(target));
    if (!coop_dmg_logged) {
        coop_dmg_logged = true;
        tig_debug_printf("CoopMiddle: enemy HP %d%%: damage %d -> %d (first hit on this map)\n", pct, dam, scaled);
    }
    return scaled;
}

bool coop_middle_host_ff_block(int64_t attacker, int64_t target)
{
    if (attacker == OBJ_HANDLE_NULL || target == OBJ_HANDLE_NULL || attacker == target || !coop_mid_session_host()
        || !coop_middle_rule_on(COOP_RULE_NO_FRIENDLY_FIRE_AOE) || !coop_lite_party_no_fight(attacker, target)) {
        return false;
    }
    if (coop_middle_rate_admit(&coop_ff_have, &coop_ff_log_ms, coop_mid_now_ms, 1000u)) {
        tig_debug_printf("CoopMiddle: friendly fire blocked (attacker slot %d -> target slot %d, type %d)\n",
            coop_mid_slot_of(attacker), coop_mid_slot_of(target), obj_field_int32_get(target, OBJ_F_TYPE));
    }
    return true;
}

int coop_middle_host_scale_xp(int xp)
{
    if (!coop_mid_session_host()) {
        return xp;
    }
    return coop_middle_scale_xp(xp, coop_middle_rules()->xp_mult_pct);
}

/* ---- M3: pings ---- */

typedef struct CoopPingEntry {
    bool used;
    int slot;
    int kind;
    int map;
    int64_t loc;
    unsigned int t_ms;
    char name[32];
} CoopPingEntry;

static CoopPingEntry coop_pings[COOP_PING_MAX];
static int coop_ping_next;
static unsigned int coop_ping_serial;
static bool coop_ping_have[8];
static unsigned int coop_ping_last_ms[8];

static void coop_pings_clear(void)
{
    memset(coop_pings, 0, sizeof(coop_pings));
    coop_ping_next = 0;
    coop_ping_serial++;
}

static void coop_ping_add(const PacketCoopPing* p)
{
    CoopPingEntry* e = &coop_pings[coop_ping_next];
    int64_t pc = player_get_local_pc_obj();

    coop_ping_next = (coop_ping_next + 1) % COOP_PING_MAX;
    memset(e, 0, sizeof(*e));
    e->used = true;
    e->slot = p->slot;
    e->kind = p->kind;
    e->map = p->map;
    e->loc = p->loc;
    e->t_ms = coop_mid_now_ms;
    coop_mid_pc_name(p->slot, sub_4A2B60(p->slot), e->name, sizeof(e->name));
    coop_ping_serial++;
    tig_debug_printf("CoopMiddle: ping from slot %d (%s) kind %d at %d,%d\n", p->slot, e->name, p->kind,
        (int)LOCATION_GET_X(p->loc), (int)LOCATION_GET_Y(p->loc));
    if (pc != OBJ_HANDLE_NULL
        && location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION), p->loc) <= COOP_PING_SOUND_RANGE) {
        gsound_play_sfx(0, 1); /* the soft UI click */
    }
}

/* Host: validate, stamp, rebroadcast, show. */
static void coop_ping_host_accept(PacketCoopPing* p, int sender)
{
    if (p->map != map_current_map() || !coop_lite_loc_in_map(p->loc)) {
        tig_debug_printf("CoopMiddle: ping from slot %d dropped (map %d / off map)\n", sender, p->map);
        return;
    }
    if (!coop_middle_rate_admit(&coop_ping_have[sender], &coop_ping_last_ms[sender], coop_mid_now_ms,
            COOP_PING_RATE_MS)) {
        tig_debug_printf("CoopMiddle: ping from slot %d dropped (rate limit)\n", sender);
        return;
    }
    p->slot = sender;
    {
        int depth = tig_net_local_suspend_pause();

        tig_net_send_app_all(p, sizeof(*p));
        tig_net_local_suspend_resume(depth);
    }
    coop_ping_add(p);
}

void coop_middle_handle_ping(PacketCoopPing* pkt)
{
    int size = tig_net_last_message_size();

    if (!tig_net_is_active() || !tig_net_is_coop_enabled()) {
        return;
    }
    if (!coop_middle_ping_valid(pkt, size)) {
        tig_debug_printf("CoopMiddle: ping dropped (size %d)\n", size);
        return;
    }
    if (tig_net_is_host()) {
        int sender = sub_52A530();
        PacketCoopPing p = *pkt;

        if (sender <= 0 || sender >= 8) {
            return;
        }
        coop_ping_host_accept(&p, sender);
        return;
    }
    if (sub_52A530() != 0 || pkt->map != map_current_map()) {
        return;
    }
    coop_ping_add(pkt);
}

bool coop_middle_ping_send(int64_t loc, int kind)
{
    PacketCoopPing p;
    int slot = sub_529520();

    if (!tig_net_is_active() || !tig_net_is_coop_enabled() || slot < 0 || slot >= 8) {
        return false;
    }
    memset(&p, 0, sizeof(p));
    p.type = PACKET_COOP_PING;
    p.slot = slot;
    p.map = map_current_map();
    p.kind = kind == COOP_PING_KIND_DANGER ? COOP_PING_KIND_DANGER : COOP_PING_KIND_LOOK;
    p.loc = loc;
    if (tig_net_is_host()) {
        coop_ping_host_accept(&p, 0);
        return true;
    }
    /* A little slower than the host's limit, so jitter never drops ours. */
    if (!coop_middle_rate_admit(&coop_ping_have[0], &coop_ping_last_ms[0], coop_mid_now_ms,
            COOP_PING_RATE_MS + 100u)) {
        return false;
    }
    {
        int depth = tig_net_local_suspend_pause();

        tig_net_send_app(&p, sizeof(p), 0);
        tig_net_local_suspend_resume(depth);
    }
    tig_debug_printf("CoopMiddle: ping sent kind %d at %d,%d\n", p.kind, (int)LOCATION_GET_X(loc),
        (int)LOCATION_GET_Y(loc));
    return true;
}

int coop_middle_pings_get(CoopPingView* out, int max, unsigned int now_ms)
{
    int n = 0;
    int i;
    int map = map_current_map();

    if (out == NULL || max <= 0 || !tig_net_is_active() || !tig_net_is_coop_enabled()) {
        return 0;
    }
    for (i = 0; i < COOP_PING_MAX && n < max; i++) {
        CoopPingEntry* e = &coop_pings[(coop_ping_next + i) % COOP_PING_MAX]; /* oldest first */
        unsigned int age;

        if (!e->used || e->map != map) {
            continue;
        }
        age = now_ms - e->t_ms;
        if (age >= COOP_PING_LIFE_MS) {
            continue;
        }
        out[n].slot = e->slot;
        out[n].kind = e->kind;
        out[n].loc = e->loc;
        out[n].age_ms = age;
        snprintf(out[n].name, sizeof(out[n].name), "%s", e->name);
        n++;
    }
    return n;
}

unsigned int coop_middle_pings_serial(void)
{
    return coop_ping_serial;
}

unsigned int coop_middle_now_ms(void)
{
    return coop_mid_now_ms;
}

void coop_middle_slot_name(int slot, char* out, size_t sz)
{
    if (out == NULL || sz == 0) {
        return;
    }
    if (slot < 0 || slot >= 8) {
        snprintf(out, sz, "?");
        return;
    }
    coop_mid_pc_name(slot, sub_4A2B60(slot), out, sz);
}

/* ---- M3: /tp, /stuck, /gather (host) ---- */

static bool coop_cmd_have[8];
static unsigned int coop_cmd_last_ms[8];

static void coop_cmd_refuse(int slot, int reason, int secs)
{
    char what[32];

    snprintf(what, sizeof(what), "%d %d", reason, secs);
    tig_debug_printf("CoopMiddle: command from slot %d refused (reason %d, %d s)\n", slot, reason, secs);
    coop_mid_sys_line(COOP_MID_CHAT_CMD_REFUSE, 0, "", what, slot, -1);
}

static bool coop_cmd_targeted(int64_t pc)
{
    int64_t loc = obj_field_int64_get(pc, OBJ_F_LOCATION);
    LocRect rect;
    ObjectList objects;
    ObjectNode* node;
    bool hit = false;

    rect.x1 = LOCATION_GET_X(loc) - COOP_TP_ENEMY_RANGE;
    rect.x2 = LOCATION_GET_X(loc) + COOP_TP_ENEMY_RANGE;
    rect.y1 = LOCATION_GET_Y(loc) - COOP_TP_ENEMY_RANGE;
    rect.y2 = LOCATION_GET_Y(loc) + COOP_TP_ENEMY_RANGE;
    object_list_rect(&rect, OBJ_TM_NPC, &objects);
    for (node = objects.head; node != NULL && !hit; node = node->next) {
        if (critter_is_dead(node->obj) || coop_lite_party_no_fight(pc, node->obj)) {
            continue;
        }
        hit = obj_field_handle_get(node->obj, OBJ_F_NPC_COMBAT_FOCUS) == pc;
    }
    object_list_destroy(&objects);
    return hit;
}

/* 0 = this client may be moved to the host now, else COOP_TP_REFUSE_*. */
static int coop_cmd_check(int slot, int64_t host_pc, int64_t* pc_out)
{
    int64_t pc = OBJ_HANDLE_NULL;

    if (!coop_mid_slot_living(slot, &pc) || critter_is_dead(host_pc)) {
        return COOP_TP_REFUSE_DEAD;
    }
    if (!coop_lite_loc_in_map(obj_field_int64_get(pc, OBJ_F_LOCATION))) {
        return COOP_TP_REFUSE_MAP;
    }
    if (combat_critter_is_combat_mode_active(pc) || combat_critter_is_combat_mode_active(host_pc)) {
        return COOP_TP_REFUSE_COMBAT;
    }
    if (coop_cmd_targeted(pc)) {
        return COOP_TP_REFUSE_TARGETED;
    }
    *pc_out = pc;
    return 0;
}

static void coop_cmd_move(int slot, int64_t pc, int64_t host_loc)
{
    int64_t loc = coop_lite_follow_loc_for_slot(host_loc, slot);

    coop_lite_mid_move_pc(slot, pc, loc);
    tig_debug_printf("CoopMiddle: regroup slot %d -> %d,%d\n", slot, (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc));
}

bool coop_middle_try_chat_command(int sender_slot, const char* text)
{
    int cmd = coop_middle_parse_cmd(text);
    int64_t host_pc;
    int64_t host_loc;
    int64_t pc = OBJ_HANDLE_NULL;
    char name[64];
    int left;
    int reason;

    if (cmd == COOP_CMD_NONE || !coop_mid_session_host() || sender_slot < 0 || sender_slot >= 8) {
        return false;
    }
    tig_debug_printf("CoopMiddle: command %d from slot %d\n", cmd, sender_slot);
    host_pc = sub_4A2B60(0);
    if (host_pc == OBJ_HANDLE_NULL) {
        return true;
    }
    host_loc = obj_field_int64_get(host_pc, OBJ_F_LOCATION);
    if (cmd == COOP_CMD_GATHER && sender_slot != 0) {
        coop_cmd_refuse(sender_slot, COOP_TP_REFUSE_HOST_ONLY, 0);
        return true;
    }
    if (cmd != COOP_CMD_GATHER && sender_slot == 0) {
        coop_cmd_refuse(0, COOP_TP_REFUSE_SELF, 0);
        return true;
    }
    left = coop_middle_cooldown_left_s(coop_cmd_have[sender_slot], coop_cmd_last_ms[sender_slot], coop_mid_now_ms,
        COOP_CMD_COOLDOWN_MS);
    if (left > 0) {
        coop_cmd_refuse(sender_slot, COOP_TP_REFUSE_COOLDOWN, left);
        return true;
    }
    if (cmd == COOP_CMD_GATHER) {
        int slot;
        int moved = 0;

        if (critter_is_dead(host_pc)) {
            coop_cmd_refuse(0, COOP_TP_REFUSE_DEAD, 0);
            return true;
        }
        if (combat_critter_is_combat_mode_active(host_pc)) {
            coop_cmd_refuse(0, COOP_TP_REFUSE_COMBAT, 0);
            return true;
        }
        for (slot = 1; slot < 8; slot++) {
            if (coop_cmd_check(slot, host_pc, &pc) == 0) {
                coop_cmd_move(slot, pc, host_loc);
                moved++;
            }
        }
        if (moved == 0) {
            coop_cmd_refuse(0, COOP_TP_REFUSE_NOBODY, 0);
            return true;
        }
        coop_middle_rate_admit(&coop_cmd_have[0], &coop_cmd_last_ms[0], coop_mid_now_ms, 0u);
        coop_mid_pc_name(0, host_pc, name, sizeof(name));
        tig_debug_printf("CoopMiddle: /gather moved %d player(s)\n", moved);
        coop_mid_sys_line(COOP_MID_CHAT_REGROUP, 0, name, "1", -1, -1);
        return true;
    }
    reason = coop_cmd_check(sender_slot, host_pc, &pc);
    if (reason != 0) {
        coop_cmd_refuse(sender_slot, reason, 0);
        return true;
    }
    coop_cmd_move(sender_slot, pc, host_loc);
    coop_middle_rate_admit(&coop_cmd_have[sender_slot], &coop_cmd_last_ms[sender_slot], coop_mid_now_ms, 0u);
    coop_mid_pc_name(sender_slot, pc, name, sizeof(name));
    coop_mid_sys_line(COOP_MID_CHAT_REGROUP, sender_slot, name, "0", -1, -1);
    return true;
}

/* ---- lifecycle ---- */

/* ---- M4: shared quest items (host) ---- */

static struct {
    bool armed;
    int64_t owner;
    char item[64];
} coop_qitem_last;

static bool coop_qitem_match(int64_t item, int key, int mode)
{
    if (mode == COOP_QITEM_MATCH_PROTO) {
        return sub_49B290(item) == key;
    }
    return obj_field_int32_get(item, OBJ_F_NAME) == key;
}

/* Backpack first, then worn (never the hidden / internal locations). */
static int64_t coop_qitem_in_pack(int64_t pc, int key, int mode)
{
    int64_t best = OBJ_HANDLE_NULL;
    int best_rank = 2;
    int cnt;
    int i;

    cnt = obj_field_int32_get(pc, OBJ_F_CRITTER_INVENTORY_NUM);
    for (i = 0; i < cnt; i++) {
        int64_t item = obj_arrayfield_handle_get(pc, OBJ_F_CRITTER_INVENTORY_LIST_IDX, i);
        int rank;

        if (item == OBJ_HANDLE_NULL || !coop_qitem_match(item, key, mode)) {
            continue;
        }
        rank = coop_middle_quest_item_rank(item_inventory_location_get(item));
        if (rank >= 0 && rank < best_rank) {
            best = item;
            best_rank = rank;
            if (rank == 0) {
                break;
            }
        }
    }
    return best;
}

int64_t coop_middle_find_party_item(int64_t asker_pc, int proto_or_name_key, int match_mode, int64_t* owner_out)
{
    int64_t item;
    int64_t asker_loc;
    int asker_slot;
    bool rule;
    bool host;
    int slot;

    if (owner_out != NULL) {
        *owner_out = OBJ_HANDLE_NULL;
    }
    coop_qitem_last.armed = false;
    if (asker_pc == OBJ_HANDLE_NULL) {
        return OBJ_HANDLE_NULL;
    }
    item = match_mode == COOP_QITEM_MATCH_NAME ? item_find_by_name(asker_pc, proto_or_name_key)
                                               : coop_qitem_in_pack(asker_pc, proto_or_name_key, match_mode);
    if (item != OBJ_HANDLE_NULL) {
        if (owner_out != NULL) {
            *owner_out = asker_pc;
        }
        return item;
    }
    host = coop_mid_session_host(); /* single player: vanilla lookup only */
    rule = host && coop_middle_rule_on(COOP_RULE_SHARED_QUEST_ITEMS);
    asker_slot = host ? coop_mid_slot_of(asker_pc) : -1;
    if (!rule || !host || asker_slot < 0) {
        return OBJ_HANDLE_NULL;
    }
    asker_loc = obj_field_int64_get(asker_pc, OBJ_F_LOCATION);
    for (slot = 0; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);
        int64_t loc;
        int64_t d;
        bool dead;

        if (slot == asker_slot || pc == OBJ_HANDLE_NULL) {
            continue;
        }
        loc = obj_field_int64_get(pc, OBJ_F_LOCATION);
        d = location_dist(asker_loc, loc);
        dead = critter_is_dead(pc) || (obj_field_int32_get(pc, OBJ_F_FLAGS) & (OF_OFF | OF_DESTROYED)) != 0;
        if (!coop_middle_quest_pocket_eligible(rule, host, asker_slot, slot,
                slot == 0 || tig_net_client_is_active(slot), dead, coop_lite_loc_in_map(loc),
                d >= 0 && d <= INT_MAX ? (int)d : -1)) {
            continue;
        }
        item = coop_qitem_in_pack(pc, proto_or_name_key, match_mode);
        if (item != OBJ_HANDLE_NULL) {
            char name[128];

            name[0] = '\0';
            object_examine(item, pc, name);
            coop_qitem_last.armed = true;
            coop_qitem_last.owner = pc;
            snprintf(coop_qitem_last.item, sizeof(coop_qitem_last.item), "%s", name[0] != '\0' ? name : "?");
            if (owner_out != NULL) {
                *owner_out = pc;
            }
            tig_debug_printf("CoopMiddle: quest item %d (mode %d) for slot %d found in slot %d's pack: '%s' (loc %d)\n",
                proto_or_name_key, match_mode, asker_slot, slot, coop_qitem_last.item,
                item_inventory_location_get(item));
            return item;
        }
    }
    return OBJ_HANDLE_NULL;
}

void coop_middle_quest_handed_over(int64_t owner_pc)
{
    char name[64];
    int slot;

    if (!coop_qitem_last.armed || owner_pc == OBJ_HANDLE_NULL || coop_qitem_last.owner != owner_pc
        || !coop_mid_session_host()) {
        return;
    }
    coop_qitem_last.armed = false;
    slot = coop_mid_slot_of(owner_pc);
    if (slot > 0) {
        coop_lite_host_sync_slot_inven(slot);
    }
    coop_mid_pc_name(slot >= 0 ? slot : 0, owner_pc, name, sizeof(name));
    tig_debug_printf("CoopMiddle: quest item '%s' handed over from slot %d (%s)\n", coop_qitem_last.item, slot, name);
    coop_mid_sys_line(COOP_MID_CHAT_QUEST, slot >= 0 ? slot : 0, name, coop_qitem_last.item, -1, -1);
}

/* ---- M4: shared followers ---- */

bool coop_middle_shared_follower_ok(int64_t pc, int64_t npc)
{
    int64_t leader;
    int64_t d;

    if (!tig_net_is_active() || !tig_net_is_coop_enabled() || pc == OBJ_HANDLE_NULL || npc == OBJ_HANDLE_NULL
        || obj_field_int32_get(npc, OBJ_F_TYPE) != OBJ_TYPE_NPC || coop_mid_slot_of(pc) < 0) {
        return false;
    }
    leader = critter_leader_get(npc);
    if (leader == OBJ_HANDLE_NULL) {
        return false;
    }
    d = location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION), obj_field_int64_get(npc, OBJ_F_LOCATION));
    return coop_middle_follower_access_ok(coop_middle_rule_on(COOP_RULE_SHARED_FOLLOWERS), leader == pc,
        coop_mid_slot_of(leader) >= 0, !critter_is_dead(npc) && !critter_is_dead(pc),
        d >= 0 && d <= INT_MAX ? (int)d : -1);
}

/* ---- M4: town map fog ---- */

static int coop_fog_tm; /* the local PC's town map, 0 = none */
static int coop_fog_synced_tm; /* client: the host's first packet for this town map arrived */
static uint8_t* coop_fog_shadow; /* what was last sent / received */
static int coop_fog_shadow_size;
static uint32_t coop_fog_sig;
static bool coop_fog_have_ms;
static unsigned int coop_fog_ms;
static bool coop_fog_resend_all; /* host: REPLACE snapshot to everyone */
static unsigned int coop_fog_log_ms;

static void coop_fog_forget(void)
{
    if (coop_fog_shadow != NULL) {
        FREE(coop_fog_shadow);
        coop_fog_shadow = NULL;
    }
    coop_fog_shadow_size = 0;
    coop_fog_sig = 0;
    coop_fog_tm = 0;
}

static void coop_fog_reset(void)
{
    coop_fog_forget();
    coop_fog_synced_tm = 0;
    coop_fog_have_ms = false;
    coop_fog_resend_all = false;
    coop_fog_slot_pending = 0;
}

static int coop_fog_local_townmap(void)
{
    int64_t pc = player_get_local_pc_obj();

    if (pc == OBJ_HANDLE_NULL || !map_is_valid() || map_is_clearing_objects()) {
        return 0;
    }
    return townmap_get(sector_id_from_loc(obj_field_int64_get(pc, OBJ_F_LOCATION)));
}

/* to_slot: >= 1 one client, -1 every client but `except`; a client always
   sends to the host. */
static void coop_fog_send(int to_slot, int except, int tm, const uint8_t* buf, int size, bool replace, int only_off)
{
    PacketCoopTownmapFog pkt;
    int off;
    int depth;

    depth = tig_net_local_suspend_pause();
    for (off = 0; off < size; off += COOP_TOWNMAP_FOG_CHUNK) {
        int len = size - off < COOP_TOWNMAP_FOG_CHUNK ? size - off : COOP_TOWNMAP_FOG_CHUNK;
        int n;
        int s;

        if (only_off >= 0 && off != only_off) {
            continue;
        }
        memset(&pkt, 0, COOP_TOWNMAP_FOG_HDR);
        pkt.type = PACKET_COOP_TOWNMAP_FOG;
        pkt.townmap_id = tm;
        pkt.offset = (uint32_t)off;
        pkt.len = (uint16_t)len;
        pkt.flags = replace ? COOP_TOWNMAP_FOG_F_REPLACE : 0;
        memcpy(pkt.data, buf + off, (size_t)len);
        n = COOP_TOWNMAP_FOG_HDR + len;
        if (!tig_net_is_host()) {
            tig_net_send_app_all(&pkt, n);
        } else if (to_slot >= 1 && to_slot < 8) {
            tig_net_send_app(&pkt, n, to_slot);
        } else {
            for (s = 1; s < 8; s++) {
                if (s != except && tig_net_client_is_active(s)) {
                    tig_net_send_app(&pkt, n, s);
                }
            }
        }
    }
    tig_net_local_suspend_resume(depth);
}

static void coop_fog_shadow_take(const uint8_t* buf, int size)
{
    if (coop_fog_shadow == NULL || coop_fog_shadow_size != size) {
        if (coop_fog_shadow != NULL) {
            FREE(coop_fog_shadow);
        }
        coop_fog_shadow = (uint8_t*)MALLOC((size_t)size);
        coop_fog_shadow_size = size;
    }
    memcpy(coop_fog_shadow, buf, (size_t)size);
    coop_fog_sig = coop_middle_fnv1a(buf, (size_t)size);
}

static void coop_fog_tick(unsigned int now_ms)
{
    uint8_t* buf;
    uint32_t sig;
    bool host;
    int size;
    int tm;
    int off;

    if (!tig_net_is_active() || !tig_net_is_coop_enabled()
        || !coop_middle_rate_admit(&coop_fog_have_ms, &coop_fog_ms, now_ms, COOP_TOWNMAP_FOG_TICK_MS)) {
        return;
    }
    host = tig_net_is_host();
    tm = coop_fog_local_townmap();
    if (tm != coop_fog_tm) {
        coop_fog_forget();
        coop_fog_tm = tm;
        if (host && tm > 0) {
            coop_fog_resend_all = true;
        }
    }
    if (tm <= 0) {
        return;
    }
    buf = townmap_coop_fog(tm, &size);
    if (buf == NULL || size <= 0) {
        return;
    }
    if (host) {
        if (coop_fog_resend_all) {
            coop_fog_resend_all = false;
            coop_fog_slot_pending = 0;
            coop_fog_send(-1, -1, tm, buf, size, true, -1);
            coop_fog_shadow_take(buf, size);
            tig_debug_printf("CoopMiddle: town map %d fog snapshot sent to everyone (%d bytes)\n", tm, size);
            return;
        }
        if (coop_fog_slot_pending != 0) {
            int s;

            for (s = 1; s < 8; s++) {
                if ((coop_fog_slot_pending & (1u << s)) != 0 && tig_net_client_is_active(s)) {
                    coop_fog_send(s, -1, tm, buf, size, true, -1);
                    tig_debug_printf("CoopMiddle: town map %d fog snapshot sent to slot %d (%d bytes)\n", tm, s, size);
                }
            }
            coop_fog_slot_pending = 0;
        }
    } else if (coop_fog_synced_tm != tm) {
        /* Not before the host's first packet for this town map. */
        coop_fog_shadow_take(buf, size);
        return;
    }
    if (coop_fog_shadow == NULL || coop_fog_shadow_size != size) {
        coop_fog_shadow_take(buf, size);
        return;
    }
    sig = coop_middle_fnv1a(buf, (size_t)size);
    if (sig == coop_fog_sig) {
        return;
    }
    for (off = 0; off < size; off += COOP_TOWNMAP_FOG_CHUNK) {
        int len = size - off < COOP_TOWNMAP_FOG_CHUNK ? size - off : COOP_TOWNMAP_FOG_CHUNK;

        if (memcmp(buf + off, coop_fog_shadow + off, (size_t)len) != 0) {
            coop_fog_send(-1, -1, tm, buf, size, false, off);
        }
    }
    coop_fog_shadow_take(buf, size);
}

void coop_middle_handle_townmap_fog(PacketCoopTownmapFog* pkt)
{
    uint8_t* buf;
    bool host;
    bool replace;
    int sender = -1;
    int size;
    int opened;

    if (!tig_net_is_active() || !tig_net_is_coop_enabled()) {
        return;
    }
    if (!coop_middle_townmap_fog_valid(pkt, tig_net_last_message_size())) {
        tig_debug_printf("CoopMiddle: town map fog packet dropped (size %d)\n", tig_net_last_message_size());
        return;
    }
    host = tig_net_is_host();
    replace = (pkt->flags & COOP_TOWNMAP_FOG_F_REPLACE) != 0;
    if (host) {
        sender = sub_52A530();
        if (sender < 1 || sender >= 8) {
            return;
        }
        replace = false; /* clients only ever add bits */
    }
    buf = townmap_coop_fog(pkt->townmap_id, &size);
    if (buf == NULL || (int64_t)pkt->offset + pkt->len > size) {
        return;
    }
    opened = coop_middle_fog_merge(buf + pkt->offset, pkt->data, pkt->len, replace);
    if (opened > 0 || replace) {
        townmap_coop_fog_touched();
    }
    if (pkt->townmap_id == coop_fog_tm && coop_fog_shadow != NULL && coop_fog_shadow_size == size) {
        /* Received bits are not ours to send back. */
        memcpy(coop_fog_shadow + pkt->offset, buf + pkt->offset, pkt->len);
        coop_fog_sig = coop_middle_fnv1a(coop_fog_shadow, (size_t)size);
    }
    if (!host && coop_fog_synced_tm != pkt->townmap_id) {
        coop_fog_synced_tm = pkt->townmap_id;
        tig_debug_printf("CoopMiddle: town map %d fog synced from the host\n", pkt->townmap_id);
    }
    if (host && opened > 0) {
        coop_fog_send(-1, sender, pkt->townmap_id, buf, (int)(pkt->offset + pkt->len), false, (int)pkt->offset);
    }
    if (opened > 0) {
        if (coop_fog_log_ms == 0 || (unsigned int)(coop_mid_now_ms - coop_fog_log_ms) >= 10000u) {
            coop_fog_log_ms = coop_mid_now_ms != 0 ? coop_mid_now_ms : 1;
            tig_debug_printf("CoopMiddle: town map %d fog: %d tile(s) opened by %s\n", pkt->townmap_id, opened,
                host ? "a client" : "the party");
        }
        if (wmap_ui_is_town_mode()) {
            wmap_ui_coop_town_refresh();
        }
    }
}

/* ---- M5: interest management ---- */

static int coop_im_remote_count(void)
{
    int n = 0;
    int slot;

    for (slot = 1; slot < 8; slot++) {
        if (sub_4A2B60(slot) != OBJ_HANDLE_NULL) {
            n++;
        }
    }
    return n;
}

static bool coop_im_start_map(void)
{
    int start = map_by_type(MAP_TYPE_START_MAP);

    return start > 0 && map_is_valid() && map_current_map() == start;
}

static int coop_im_margin_logged = -1;

int coop_middle_keep_sector_set(int64_t loc, int64_t* ids, int max)
{
    int capacity = 0;
    int locked = 0;
    int others;
    int remote;
    int free_proj;
    int margin;

    if (!tig_net_is_active() || !tig_net_is_coop_enabled()) {
        return coop_lite_keep_ids(loc, ids, max);
    }
    sector_cache_coop_stats(&capacity, &locked);
    remote = coop_im_remote_count();
    /* Free slots if every remote slot held its full 3x3: it does not depend
       on the window used now, so the choice does not flap. */
    others = locked - coop_lite_mid_keep_locked();
    free_proj = capacity - (others > 0 ? others : 0) - 9 * remote;
    margin = coop_middle_keep_sector_margin(remote, free_proj);
    if (margin != coop_im_margin_logged) {
        coop_im_margin_logged = margin;
        tig_debug_printf("CoopMiddle: sector keep window %s (remote %d, cache %d, locked %d, free if 3x3 %d)\n",
            margin == 0 ? "3x3" : "adaptive 2x2", remote, capacity, locked, free_proj);
    }
    return coop_middle_keep_ids(loc, margin, ids, max);
}

bool coop_middle_slot_in_range(int slot, int64_t loc, int radius)
{
    int64_t pc;

    if (slot < 0 || slot >= 8 || loc == 0) {
        return false;
    }
    pc = sub_4A2B60(slot);
    return pc != OBJ_HANDLE_NULL && coop_middle_loc_within(obj_field_int64_get(pc, OBJ_F_LOCATION), loc, radius);
}

unsigned int coop_middle_slots_in_range(int64_t loc, int radius)
{
    unsigned int mask = 0;
    int slot;

    for (slot = 1; slot < 8; slot++) {
        if (coop_middle_slot_in_range(slot, loc, radius)) {
            mask |= 1u << slot;
        }
    }
    return mask;
}

void coop_middle_send_app_near(void* data, int size, int packet_type, int64_t loc_a, int64_t loc_b)
{
    int radius;
    int slot;

    if (!coop_mid_session_host() || (loc_a == 0 && loc_b == 0)) {
        tig_net_send_app_all(data, size);
        return;
    }
    radius = coop_middle_interest_radius(packet_type, coop_im_start_map());
    if (radius <= 0) {
        tig_net_send_app_all(data, size);
        return;
    }
    for (slot = 1; slot < 8; slot++) {
        if (!tig_net_client_is_active(slot)) {
            continue;
        }
        if ((loc_a != 0 && coop_middle_slot_in_range(slot, loc_a, radius))
            || (loc_b != 0 && coop_middle_slot_in_range(slot, loc_b, radius))) {
            tig_net_send_app(data, size, slot);
        }
    }
}

/* ---- M5: free world map (map 1) ---- */

#define COOP_OWT_HIDE_MAX 9
#define COOP_OWT_HIDE_FLAGS (OF_OFF | OF_INVULNERABLE)

typedef struct CoopOwtHidden {
    int count;
    ObjectID oid[COOP_OWT_HIDE_MAX];
    unsigned int added[COOP_OWT_HIDE_MAX]; /* flags we set (restored on landing) */
} CoopOwtHidden;

typedef struct CoopOwtTransit {
    bool active;
    int x; /* sector coords */
    int y;
    int64_t sec; /* current world map sector (vanilla sector id) */
    int path_len; /* rotations of the whole route (vanilla wmap_path_make legs) */
    int path_pos;
    int8_t path[COOP_OWT_PATH_MAX];
    int64_t dest;
    unsigned int step_ms;
    unsigned int progress_ms;
    int roll_in; /* steps to the next encounter roll */
    CoopOwtHidden hid;
} CoopOwtTransit;

typedef struct CoopOwtMarker {
    bool valid;
    int x;
    int y;
    bool moving;
    unsigned int ms;
    unsigned int enc_ms; /* 0 = no encounter */
} CoopOwtMarker;

static CoopOwtTransit coop_owt[8]; /* host */
static CoopOwtRoute coop_owt_route[8]; /* host: WAYPOINT packets before a START */
/* Host: a Teleport spell waiting for the client's world map pick (SELECT
   sent): the spell, whether the caster pays fatigue (not from a scroll -
   vanilla charges only a PC caster), when. */
static struct {
    bool pending;
    bool charge;
    int spell;
    unsigned int since_ms;
} coop_owt_tp[8];
#define COOP_OWT_TP_PENDING_MS 300000u
static CoopOwtMarker coop_owt_mark[8]; /* both sides */
static unsigned int coop_owt_serial;
static unsigned int coop_owt_markers_ms; /* host: last all-slot sweep */
static unsigned int coop_owt_local_ms; /* host: own trip progress */
static int coop_owt_local_roll; /* host: own trip, steps to the next roll */
static bool coop_owt_client_active; /* client: own trip asked / running */

bool coop_middle_owt_local_active(void)
{
    return coop_owt_client_active && !tig_net_is_host();
}
static unsigned int coop_owt_client_sent_ms;
static int64_t coop_owt_client_dest;
static CoopOwtHidden coop_owt_remote_hid[8]; /* client: other players hidden while they travel */

static bool coop_owt_rule(void)
{
    return tig_net_is_active() && tig_net_is_coop_enabled() && coop_middle_rule_on(COOP_RULE_FREE_OVERWORLD);
}

bool coop_middle_overworld_free(void)
{
    return coop_owt_rule() && coop_im_start_map();
}

bool coop_middle_overworld_solo_hop(int dest_map)
{
    return coop_mid_session_host() && coop_middle_overworld_free() && dest_map == map_by_type(MAP_TYPE_START_MAP);
}

bool coop_middle_overworld_freeze_time(void)
{
    return coop_middle_overworld_free();
}

static int coop_owt_roll_steps(void)
{
    return random_between(COOP_OWT_ENC_MIN_STEPS, COOP_OWT_ENC_MAX_STEPS);
}

static int64_t coop_owt_sector_loc(int x, int y)
{
    return LOCATION_MAKE((int64_t)x * 64 + 32, (int64_t)y * 64 + 32);
}

static void coop_owt_hide_obj(CoopOwtHidden* h, int64_t obj)
{
    unsigned int cur;
    unsigned int add;

    if (obj == OBJ_HANDLE_NULL || h->count >= COOP_OWT_HIDE_MAX) {
        return;
    }
    cur = (unsigned int)obj_field_int32_get(obj, OBJ_F_FLAGS);
    add = COOP_OWT_HIDE_FLAGS & ~cur;
    h->oid[h->count] = obj_get_id(obj);
    h->added[h->count] = add;
    h->count++;
    if (add != 0) {
        multiplayer_lock();
        sub_424070(obj, 3, false, true); /* stop its walk / attack */
        object_flags_set(obj, add);
        multiplayer_unlock();
    }
}

/* PC + its followers (OIDs, so a vanished object is skipped later). */
static void coop_owt_hide(CoopOwtHidden* h, int64_t pc)
{
    ObjectList objects;
    ObjectNode* node;

    h->count = 0;
    coop_owt_hide_obj(h, pc);
    object_list_all_followers(pc, &objects);
    for (node = objects.head; node != NULL; node = node->next) {
        coop_owt_hide_obj(h, node->obj);
    }
    object_list_destroy(&objects);
}

static void coop_owt_unhide(CoopOwtHidden* h)
{
    int i;

    for (i = 0; i < h->count; i++) {
        int64_t obj = obj_pool_perm_lookup(h->oid[i]);

        if (obj != OBJ_HANDLE_NULL && h->added[i] != 0) {
            multiplayer_lock();
            object_flags_unset(obj, h->added[i]);
            multiplayer_unlock();
        }
    }
    h->count = 0;
}

static void coop_owt_mark_set(int slot, int x, int y, int flags)
{
    CoopOwtMarker* m;

    if (slot < 0 || slot >= 8) {
        return;
    }
    m = &coop_owt_mark[slot];
    if (!m->valid || m->x != x || m->y != y || m->moving != ((flags & COOP_OWT_F_MOVING) != 0)
        || (flags & COOP_OWT_F_ENCOUNTER) != 0) {
        coop_owt_serial++;
    }
    m->valid = true;
    m->x = x;
    m->y = y;
    m->moving = (flags & COOP_OWT_F_MOVING) != 0;
    m->ms = coop_mid_now_ms != 0 ? coop_mid_now_ms : 1;
    if ((flags & COOP_OWT_F_ENCOUNTER) != 0) {
        m->enc_ms = m->ms;
    }
}

static void coop_owt_send(int op, int slot, int flags, int64_t loc, int x, int y, int to_slot)
{
    PacketCoopOverworldTravel pkt;
    int depth;

    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_OVERWORLD_TRAVEL;
    pkt.op = op;
    pkt.slot = slot;
    pkt.flags = flags;
    pkt.loc = loc;
    pkt.world_x = x;
    pkt.world_y = y;
    depth = tig_net_local_suspend_pause();
    if (to_slot >= 0) {
        tig_net_send_app(&pkt, sizeof(pkt), to_slot);
    } else {
        tig_net_send_app_all(&pkt, sizeof(pkt));
    }
    tig_net_local_suspend_resume(depth);
}

/* Host: a slot's marker to everyone (and our own table). */
static void coop_owt_publish(int slot, int x, int y, int flags)
{
    coop_owt_mark_set(slot, x, y, flags);
    coop_owt_send(COOP_OWT_PROGRESS, slot, flags, coop_owt_sector_loc(x, y), x, y, -1);
}

static bool coop_owt_targeted(int64_t pc)
{
    int64_t loc = obj_field_int64_get(pc, OBJ_F_LOCATION);
    LocRect rect;
    ObjectList objects;
    ObjectNode* node;
    bool hit = false;

    rect.x1 = LOCATION_GET_X(loc) - COOP_OWT_ENEMY_RANGE;
    rect.x2 = LOCATION_GET_X(loc) + COOP_OWT_ENEMY_RANGE;
    rect.y1 = LOCATION_GET_Y(loc) - COOP_OWT_ENEMY_RANGE;
    rect.y2 = LOCATION_GET_Y(loc) + COOP_OWT_ENEMY_RANGE;
    object_list_rect(&rect, OBJ_TM_NPC, &objects);
    for (node = objects.head; node != NULL && !hit; node = node->next) {
        if (!critter_is_dead(node->obj) && !coop_lite_party_no_fight(pc, node->obj)) {
            hit = obj_field_handle_get(node->obj, OBJ_F_NPC_COMBAT_FOCUS) == pc;
        }
    }
    object_list_destroy(&objects);
    return hit;
}

static void coop_sleep_slot_left(int slot, const char* why); /* M5 sleep: drop a slot's readiness */

/* Host: end slot's trip. move: put the PC (and followers) at loc. */
static void coop_owt_finish(int slot, int64_t loc, bool move, bool encounter, const char* why)
{
    CoopOwtTransit* t = &coop_owt[slot];
    int64_t pc = sub_4A2B60(slot);
    int64_t at = loc;
    int flags = encounter ? COOP_OWT_F_ENCOUNTER : 0;

    if (!t->active) {
        return;
    }
    t->active = false;
    coop_owt_unhide(&t->hid);
    if (move && pc != OBJ_HANDLE_NULL && loc != 0 && coop_lite_loc_in_map(loc)) {
        ObjectList objects;
        ObjectNode* node;
        int k = 0;

        coop_lite_mid_keep(slot, loc); /* load the landing sectors first */
        at = coop_lite_follow_loc_for_slot(loc, slot);
        coop_lite_mid_move_pc(slot, pc, at); /* epoch bump + PC_POS FORCE to the owner */
        object_list_all_followers(pc, &objects);
        for (node = objects.head; node != NULL; node = node->next) {
            multiplayer_lock();
            sub_424070(node->obj, 3, false, true);
            sub_43E770(node->obj, coop_lite_follow_loc_for_slot(at, 1 + (k++ % 7)), 0, 0);
            multiplayer_unlock();
        }
        object_list_destroy(&objects);
    } else if (pc != OBJ_HANDLE_NULL) {
        at = obj_field_int64_get(pc, OBJ_F_LOCATION);
    }
    if (at == 0) {
        at = coop_owt_sector_loc(t->x, t->y);
    }
    coop_owt_mark_set(slot, (int)(LOCATION_GET_X(at) >> 6), (int)(LOCATION_GET_Y(at) >> 6), flags);
    coop_owt_send(COOP_OWT_ARRIVED, slot, flags, at, (int)(LOCATION_GET_X(at) >> 6), (int)(LOCATION_GET_Y(at) >> 6), -1);
    tig_debug_printf("CoopMiddle: world map trip of slot %d ended (%s) at %d,%d%s\n", slot, why,
        (int)LOCATION_GET_X(at), (int)LOCATION_GET_Y(at), encounter ? " - random encounter" : "");
}

static void coop_owt_refuse(int slot, int reason)
{
    coop_owt_send(COOP_OWT_REFUSED, slot, 0, 0, reason, 0, slot);
    tig_debug_printf("CoopMiddle: world map trip of slot %d refused (reason %d)\n", slot, reason);
}

static bool coop_sleep_executing;

/* Host: the route's sector path with the vanilla world map pathing
   (wmap_path_make: blocked sectors / terrain, bridges and passes),
   leg by leg through the client's waypoints. 0 = blocked. */
static int8_t coop_owt_scratch[COOP_OWT_PATH_MAX];

static int coop_owt_plan(int8_t* path, int64_t from_loc, const int64_t* points, int count)
{
    int64_t from = sector_id_from_loc(from_loc);
    int len = 0;
    int i;

    for (i = 0; i < count; i++) {
        WmapPathInfo info;
        int steps;

        memset(&info, 0, sizeof(info));
        info.from = from;
        info.to = sector_id_from_loc(points[i]);
        if (info.to == info.from) {
            continue;
        }
        info.max_rotations = COOP_OWT_PATH_MAX - len;
        info.rotations = &path[len];
        if (info.max_rotations <= 0) {
            return 0;
        }
        steps = wmap_path_make(&info);
        if (steps <= 0) {
            return 0;
        }
        len += steps;
        from = info.to;
    }
    return len > 0 ? len : -1; /* -1: already there */
}

static void coop_owt_host_start(int slot, int64_t dest, int before)
{
    CoopOwtTransit* t = &coop_owt[slot];
    int64_t pc = sub_4A2B60(slot);
    int64_t loc;
    int64_t points[COOP_OWT_ROUTE_MAX];
    int count;
    int len = 0;
    int reason = 0;

    count = coop_middle_owt_route_finish(&coop_owt_route[slot], before, dest, points, COOP_OWT_ROUTE_MAX);
    if (!coop_middle_rule_on(COOP_RULE_FREE_OVERWORLD)) {
        reason = COOP_OWT_REFUSE_RULE;
    } else if (!coop_im_start_map()) {
        reason = COOP_OWT_REFUSE_MAP;
    } else if (pc == OBJ_HANDLE_NULL || critter_is_dead(pc) || !tig_net_client_is_active(slot)) {
        reason = COOP_OWT_REFUSE_DEAD;
    } else if (!t->active && (combat_critter_is_combat_mode_active(pc) || coop_owt_targeted(pc))) {
        reason = COOP_OWT_REFUSE_COMBAT;
    } else if (count <= 0 || !coop_lite_loc_in_map(dest)) {
        reason = COOP_OWT_REFUSE_DEST;
    } else if (coop_lite_mid_prompt_busy() || coop_sleep_executing) {
        reason = COOP_OWT_REFUSE_BUSY;
    } else {
        int k;

        for (k = 0; k < count && reason == 0; k++) {
            if (!coop_lite_loc_in_map(points[k])) {
                reason = COOP_OWT_REFUSE_DEST;
            }
        }
        if (reason == 0) {
            loc = t->active ? coop_owt_sector_loc(t->x, t->y) : obj_field_int64_get(pc, OBJ_F_LOCATION);
            len = coop_owt_plan(coop_owt_scratch, loc, points, count);
            if (len == 0) {
                reason = COOP_OWT_REFUSE_BLOCKED;
            }
        }
    }
    if (reason != 0) {
        coop_owt_refuse(slot, reason); /* a running trip keeps its old route */
        return;
    }
    t->path_len = len > 0 ? len : 0;
    if (t->path_len > 0) {
        memcpy(t->path, coop_owt_scratch, (size_t)t->path_len);
    }
    t->path_pos = 0;
    t->dest = dest;
    if (t->active) {
        t->sec = sector_id_from_loc(coop_owt_sector_loc(t->x, t->y));
        tig_debug_printf("CoopMiddle: world map trip of slot %d re-routed to %d,%d (%d waypoint(s), %d sector(s))\n",
            slot, (int)LOCATION_GET_X(dest), (int)LOCATION_GET_Y(dest), count, t->path_len);
        return;
    }
    loc = obj_field_int64_get(pc, OBJ_F_LOCATION);
    t->active = true;
    t->sec = sector_id_from_loc(loc);
    t->x = (int)SECTOR_X(t->sec);
    t->y = (int)SECTOR_Y(t->sec);
    t->step_ms = coop_mid_now_ms;
    t->progress_ms = coop_mid_now_ms;
    t->roll_in = coop_owt_roll_steps();
    coop_owt_hide(&t->hid, pc); /* IN_TRANSIT: off + invulnerable, PC and own followers */
    coop_sleep_slot_left(slot, "world map trip");
    coop_owt_publish(slot, t->x, t->y, COOP_OWT_F_MOVING);
    tig_debug_printf("CoopMiddle: world map trip of slot %d: %d,%d -> %d,%d (%d waypoint(s), %d sector(s), %d object(s) in transit)\n",
        slot, (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc), (int)LOCATION_GET_X(dest), (int)LOCATION_GET_Y(dest),
        count, t->path_len, t->hid.count);
}

/* Host: a client's Teleport spell (the spell was cast on the host, the
   client picked the place on its world map): an instant hop on map 1. */
static void coop_owt_host_teleport(int slot, int64_t dest, int spell)
{
    CoopOwtTransit* t = &coop_owt[slot];
    int64_t pc = sub_4A2B60(slot);
    int reason = 0;
    bool charge;

    coop_middle_owt_route_reset(&coop_owt_route[slot]);
    /* Only the spell we asked this client to place (SELECT), once. */
    if (!coop_owt_tp[slot].pending
        || (unsigned int)(coop_mid_now_ms - coop_owt_tp[slot].since_ms) > COOP_OWT_TP_PENDING_MS) {
        coop_owt_tp[slot].pending = false;
        coop_owt_refuse(slot, COOP_OWT_REFUSE_DEST);
        tig_debug_printf("CoopMiddle: teleport pick of slot %d without a pending spell - refused\n", slot);
        return;
    }
    spell = coop_owt_tp[slot].spell;
    charge = coop_owt_tp[slot].charge;
    if (!coop_im_start_map()) {
        reason = COOP_OWT_REFUSE_MAP;
    } else if (pc == OBJ_HANDLE_NULL || critter_is_dead(pc) || !tig_net_client_is_active(slot)) {
        reason = COOP_OWT_REFUSE_DEAD;
    } else if (!coop_lite_loc_in_map(dest) || !antiteleport_check(pc, dest)) {
        reason = COOP_OWT_REFUSE_DEST;
    } else if (coop_lite_mid_prompt_busy() || coop_sleep_executing) {
        reason = COOP_OWT_REFUSE_BUSY;
    }
    if (reason != 0) {
        coop_owt_refuse(slot, reason);
        return;
    }
    coop_owt_tp[slot].pending = false;
    if (!t->active) {
        int64_t loc = obj_field_int64_get(pc, OBJ_F_LOCATION);

        t->active = true;
        t->hid.count = 0;
        t->sec = sector_id_from_loc(loc);
        t->x = (int)SECTOR_X(t->sec);
        t->y = (int)SECTOR_Y(t->sec);
    }
    t->path_len = 0;
    t->path_pos = 0;
    if (charge && spell >= 0 && spell < MT_SPELL_COUNT) {
        sub_4507B0(pc, spell); /* the vanilla cost, charged where the stats live (not for a scroll) */
    }
    /* coop_owt_finish moves the PC and all its followers (object_list_all_followers). */
    coop_owt_finish(slot, dest, true, false, "teleport spell");
}

/* Both sides: Teleport spell (MTComponentMovement move_location 2) in co-op.
   Vanilla multiplayer sends the caster to the map's starting location and the
   party follows; co-op uses the single-player world map pick instead. */
void coop_middle_teleport_spell(int64_t source_obj, int64_t target_obj, int spell)
{
    int slot;

    if (target_obj == OBJ_HANDLE_NULL) {
        return;
    }
    if (player_is_local_pc_obj(target_obj)) {
        if (!wmap_ui_is_created()) {
            ui_wmap_select(source_obj, spell);
        }
        return;
    }
    if (!coop_mid_session_host()) {
        return;
    }
    slot = coop_middle_slot_of_pc(target_obj);
    if (slot > 0 && slot < 8 && spell >= 0 && spell < COOP_OWT_SPELL_MAX) {
        coop_owt_tp[slot].pending = true;
        coop_owt_tp[slot].charge = source_obj == target_obj; /* a scroll casts as itself */
        coop_owt_tp[slot].spell = spell;
        coop_owt_tp[slot].since_ms = coop_mid_now_ms;
        coop_owt_send(COOP_OWT_SELECT, slot, 0, 0, spell, 0, slot);
        tig_debug_printf("CoopMiddle: teleport spell of slot %d: the client picks the place\n", slot);
    }
}

bool coop_middle_owt_client_teleport(int64_t loc, int spell)
{
    if (!tig_net_is_active() || tig_net_is_host() || !tig_net_is_coop_enabled() || !coop_im_start_map()
        || loc == 0) {
        return false;
    }
    coop_owt_client_active = true;
    coop_owt_client_dest = loc;
    coop_owt_client_sent_ms = coop_mid_now_ms;
    coop_owt_send(COOP_OWT_START, sub_529520(), COOP_OWT_F_TELEPORT, loc,
        spell >= 0 && spell < COOP_OWT_SPELL_MAX ? spell : 0, 0, 0);
    tig_debug_printf("CoopMiddle: teleport to %d,%d asked\n", (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc));
    return true;
}

static void coop_owt_host_tick(unsigned int now_ms)
{
    int slot;
    bool free_map = coop_middle_overworld_free();

    for (slot = 1; slot < 8; slot++) {
        CoopOwtTransit* t = &coop_owt[slot];
        unsigned int steps;
        bool done = false;

        if (!t->active) {
            continue;
        }
        if (!free_map || !tig_net_client_is_active(slot) || sub_4A2B60(slot) == OBJ_HANDLE_NULL) {
            coop_owt_finish(slot, 0, false, false, "rule / map / player gone");
            continue;
        }
        if (coop_middle_is_paused()) {
            t->step_ms = now_ms; /* M7: frozen - no steps, no encounter rolls */
            continue;
        }
        steps = (unsigned int)(now_ms - t->step_ms) / COOP_OWT_STEP_MS;
        if (steps > 64) {
            steps = 64; /* a long frame: do not jump across the map */
        }
        t->step_ms += steps * COOP_OWT_STEP_MS;
        while (steps-- > 0 && !done) {
            int64_t next;

            if (t->path_pos >= t->path_len) {
                break;
            }
            /* vanilla sub_565170: one sector in the planned direction */
            sector_in_dir(t->sec, t->path[t->path_pos++], &next);
            t->sec = next;
            t->x = (int)SECTOR_X(next);
            t->y = (int)SECTOR_Y(next);
            if (--t->roll_in <= 0) {
                int64_t at = coop_owt_sector_loc(t->x, t->y);

                t->roll_in = coop_owt_roll_steps();
                if (wmap_rnd_coop_check(at)) {
                    coop_owt_finish(slot, at, true, true, "random encounter");
                    done = true;
                }
            }
        }
        if (done) {
            continue;
        }
        if (t->path_pos >= t->path_len) {
            int64_t at = t->dest;
            int area = area_get_nearest_area_in_range(at, true);

            if (area > 0) {
                at = area_get_location(area); /* vanilla sub_5649F0 */
            }
            coop_owt_finish(slot, at, true, false, "arrived");
            continue;
        }
        if ((unsigned int)(now_ms - t->progress_ms) >= COOP_OWT_PROGRESS_MS) {
            t->progress_ms = now_ms;
            coop_owt_publish(slot, t->x, t->y, COOP_OWT_F_MOVING);
        }
    }
    if (!free_map) {
        return;
    }
    /* The host's own trip (vanilla loop, clock frozen). */
    {
        int x;
        int y;

        if (wmap_ui_coop_local_sector(&x, &y)) {
            if ((unsigned int)(now_ms - coop_owt_local_ms) >= COOP_OWT_PROGRESS_MS) {
                coop_owt_local_ms = now_ms;
                coop_owt_publish(0, x, y, COOP_OWT_F_MOVING);
            }
        }
    }
    /* Everyone else's spot, once a second. */
    if ((unsigned int)(now_ms - coop_owt_markers_ms) >= COOP_OWT_MARKERS_MS && coop_im_remote_count() > 0) {
        int x;
        int y;

        coop_owt_markers_ms = now_ms;
        for (slot = 0; slot < 8; slot++) {
            int64_t pc = sub_4A2B60(slot);
            int64_t loc;

            if (pc == OBJ_HANDLE_NULL || critter_is_dead(pc) || coop_owt[slot].active
                || (slot == 0 && wmap_ui_coop_local_sector(&x, &y))) {
                continue;
            }
            loc = obj_field_int64_get(pc, OBJ_F_LOCATION);
            coop_owt_publish(slot, (int)(LOCATION_GET_X(loc) >> 6), (int)(LOCATION_GET_Y(loc) >> 6), 0);
        }
    }
}

bool coop_middle_overworld_local_step(int64_t loc)
{
    if (!coop_middle_overworld_free() || loc == 0) {
        return false;
    }
    if (coop_owt_local_roll <= 0) {
        coop_owt_local_roll = coop_owt_roll_steps();
    }
    if (--coop_owt_local_roll > 0) {
        return false;
    }
    coop_owt_local_roll = coop_owt_roll_steps();
    if (!wmap_rnd_coop_check(loc)) {
        return false;
    }
    if (coop_mid_session_host()) {
        coop_owt_publish(0, (int)(LOCATION_GET_X(loc) >> 6), (int)(LOCATION_GET_Y(loc) >> 6), COOP_OWT_F_ENCOUNTER);
    }
    tig_debug_printf("CoopMiddle: random encounter on the host's own world map trip at %d,%d\n",
        (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc));
    return true;
}

bool coop_middle_owt_client_propose(int64_t dest_loc)
{
    if (!tig_net_is_active() || tig_net_is_host() || !coop_middle_overworld_free()) {
        return false;
    }
    if (dest_loc == 0) {
        return true; /* no route: nothing to do, never alone */
    }
    if (coop_owt_client_active && coop_owt_client_dest == dest_loc
        && (unsigned int)(coop_mid_now_ms - coop_owt_client_sent_ms) < 1000u) {
        return true;
    }
    coop_owt_client_active = true;
    coop_owt_client_dest = dest_loc;
    coop_owt_client_sent_ms = coop_mid_now_ms;
    {
        /* The plotted route, waypoint by waypoint (the host paths each leg
           like vanilla); the last point is the destination itself. */
        int64_t locs[COOP_OWT_ROUTE_MAX];
        int n = wmap_ui_coop_route_locs(locs, COOP_OWT_ROUTE_MAX);
        int before = 0;
        int i;
        int self = sub_529520();

        if (n > 1 && locs[n - 1] == dest_loc) {
            before = n - 1;
            for (i = 0; i < before; i++) {
                coop_owt_send(COOP_OWT_WAYPOINT, self, 0, locs[i], i, before, 0);
            }
        }
        coop_owt_send(COOP_OWT_START, self, 0, dest_loc, before, 0, 0);
        tig_debug_printf("CoopMiddle: world map trip to %d,%d asked (%d waypoint(s) before it)\n",
            (int)LOCATION_GET_X(dest_loc), (int)LOCATION_GET_Y(dest_loc), before);
    }
    return true;
}

void coop_middle_owt_client_closed(void)
{
    if (!coop_owt_client_active || !tig_net_is_active() || tig_net_is_host()) {
        return;
    }
    coop_owt_send(COOP_OWT_CANCEL, sub_529520(), 0, 0, 0, 0, 0);
    tig_debug_printf("CoopMiddle: world map closed during the trip - stop asked\n");
}

bool coop_middle_owt_in_transit(int slot)
{
    if (slot < 0 || slot >= 8) {
        return false;
    }
    if (tig_net_is_active() && tig_net_is_host()) {
        return coop_owt[slot].active;
    }
    if (slot == sub_529520()) {
        return coop_owt_client_active;
    }
    return coop_owt_mark[slot].valid && coop_owt_mark[slot].moving
        && (unsigned int)(coop_mid_now_ms - coop_owt_mark[slot].ms) < COOP_OWT_MARKER_LIFE_MS;
}

bool coop_middle_owt_local_in_transit(void)
{
    if (!tig_net_is_active()) {
        return false;
    }
    return tig_net_is_host() ? wmap_ui_is_moving() : coop_owt_client_active;
}

void coop_middle_owt_abort_all(const char* why)
{
    int slot;

    if (!coop_mid_session_host()) {
        return;
    }
    for (slot = 1; slot < 8; slot++) {
        if (coop_owt[slot].active) {
            coop_owt_finish(slot, 0, false, false, why != NULL ? why : "aborted");
        }
    }
}

static void coop_owt_client_remote(int slot, bool moving)
{
    CoopOwtHidden* h = &coop_owt_remote_hid[slot];
    int64_t pc = sub_4A2B60(slot);

    if (moving && h->count == 0 && pc != OBJ_HANDLE_NULL) {
        coop_owt_hide(h, pc);
    } else if (!moving && h->count > 0) {
        coop_owt_unhide(h);
    }
}

static const char* coop_owt_refuse_text(int reason, bool ru)
{
    switch (reason) {
    case COOP_OWT_REFUSE_COMBAT:
        return ru ? "Нельзя отправиться в путь: вы в бою." : "Can't travel: you are in combat.";
    case COOP_OWT_REFUSE_DEAD:
        return ru ? "Нельзя отправиться в путь." : "Can't travel now.";
    case COOP_OWT_REFUSE_DEST:
        return ru ? "Туда не попасть." : "That place can't be reached.";
    case COOP_OWT_REFUSE_BLOCKED:
        return ru ? "Путь прегражден. Нужен мост, перевал или транспорт." : "Your path is blocked. You must locate a bridge, pass or means of transportation.";
    case COOP_OWT_REFUSE_BUSY:
        return ru ? "Сейчас нельзя: идёт общий переход или сон." : "Not now: a party move or sleep is running.";
    default:
        return ru ? "Свободная карта мира сейчас недоступна." : "The free world map is off here.";
    }
}

void coop_middle_handle_overworld(PacketCoopOverworldTravel* pkt)
{
    bool host;
    int local;

    if (!tig_net_is_active() || !tig_net_is_coop_enabled()) {
        return;
    }
    host = tig_net_is_host();
    if (!coop_middle_overworld_pkt_valid(pkt, tig_net_last_message_size(), !host)) {
        tig_debug_printf("CoopMiddle: world map travel packet dropped (bad op / size)\n");
        return;
    }
    if (host) {
        int sender = sub_52A530();

        if (sender <= 0 || sender >= 8 || pkt->slot != sender) {
            tig_debug_printf("CoopMiddle: world map travel op %d from slot %d dropped (slot %d)\n", pkt->op, sender,
                pkt->slot);
            return;
        }
        if (pkt->op == COOP_OWT_WAYPOINT) {
            coop_middle_owt_route_add(&coop_owt_route[sender], pkt->world_x, pkt->world_y, pkt->loc);
        } else if (pkt->op == COOP_OWT_START && (pkt->flags & COOP_OWT_F_TELEPORT) != 0) {
            coop_owt_host_teleport(sender, pkt->loc, pkt->world_x);
        } else if (pkt->op == COOP_OWT_START) {
            coop_owt_host_start(sender, pkt->loc, pkt->world_x);
        } else if (pkt->op == COOP_OWT_CANCEL && coop_owt[sender].active) {
            CoopOwtTransit* t = &coop_owt[sender];
            int64_t at = coop_owt_sector_loc(t->x, t->y);
            int area = area_get_nearest_area_in_range(at, true);

            if (area > 0) {
                at = area_get_location(area); /* vanilla stop: the nearest known place */
            }
            coop_owt_finish(sender, at, true, false, "stopped");
        }
        return;
    }
    local = sub_529520();
    switch (pkt->op) {
    case COOP_OWT_SELECT:
        /* our Teleport spell, resolved on the host: pick the place (vanilla) */
        if (pkt->slot == local && player_get_local_pc_obj() != OBJ_HANDLE_NULL && !wmap_ui_is_created()) {
            ui_wmap_select(player_get_local_pc_obj(), pkt->world_x);
        }
        break;
    case COOP_OWT_PROGRESS:
        coop_owt_mark_set(pkt->slot, pkt->world_x, pkt->world_y, pkt->flags);
        if (pkt->slot == local) {
            if ((pkt->flags & COOP_OWT_F_MOVING) != 0 && coop_owt_client_active) {
                wmap_ui_coop_marker_set(pkt->world_x, pkt->world_y);
            }
        } else {
            coop_owt_client_remote(pkt->slot, (pkt->flags & COOP_OWT_F_MOVING) != 0);
        }
        break;
    case COOP_OWT_ARRIVED:
        coop_owt_mark_set(pkt->slot, pkt->world_x, pkt->world_y, pkt->flags);
        if (pkt->slot == local) {
            coop_owt_client_active = false; /* before the close: no CANCEL */
            if (wmap_ui_is_created()) {
                wmap_ui_close();
            }
            coop_lite_mid_center_local();
            if ((pkt->flags & COOP_OWT_F_ENCOUNTER) != 0) {
                coop_lite_mid_notice(coop_lite_lang_ru() ? "Засада в пути!" : "Ambushed on the road!", false);
            }
        } else {
            coop_owt_client_remote(pkt->slot, false);
            if ((pkt->flags & COOP_OWT_F_ENCOUNTER) != 0) {
                char name[64];
                char text[160];

                coop_middle_slot_name(pkt->slot, name, sizeof(name));
                snprintf(text, sizeof(text), coop_lite_lang_ru() ? "%s попал в засаду - красная метка на карте мира."
                                                                 : "%s was ambushed - red mark on the world map.",
                    name);
                coop_lite_mid_notice(text, false);
            }
        }
        tig_debug_printf("CoopMiddle: slot %d landed from the world map at %d,%d%s\n", pkt->slot,
            (int)LOCATION_GET_X(pkt->loc), (int)LOCATION_GET_Y(pkt->loc),
            (pkt->flags & COOP_OWT_F_ENCOUNTER) != 0 ? " (encounter)" : "");
        break;
    case COOP_OWT_REFUSED:
        if (pkt->slot == local) {
            coop_owt_client_active = false;
            coop_lite_mid_notice(coop_owt_refuse_text(pkt->world_x, coop_lite_lang_ru()), false);
        }
        break;
    default:
        break;
    }
}

int coop_middle_world_markers(CoopWorldMarker* out, int max)
{
    int n = 0;
    int slot;
    int local;

    if (out == NULL || max <= 0 || !tig_net_is_coop_enabled()) {
        return 0;
    }
    local = tig_net_is_host() ? 0 : sub_529520();
    /* M6: a player in a solo instance entered from the world map stays
       marked at the entrance. */
    for (slot = 1; slot < 8 && n < max; slot++) {
        int entry_map = 0;
        int64_t entry_loc = 0;

        if (slot != local && coop_subinst_slot_entry(slot, &entry_map, &entry_loc) && entry_loc != 0
            && entry_map == map_by_type(MAP_TYPE_START_MAP)) {
            out[n].slot = slot;
            out[n].world_x = (int)(LOCATION_GET_X(entry_loc) >> 6);
            out[n].world_y = (int)(LOCATION_GET_Y(entry_loc) >> 6);
            out[n].moving = false;
            out[n].encounter = false;
            n++;
        }
    }
    if (!coop_owt_rule()) {
        return n;
    }
    for (slot = 0; slot < 8 && n < max; slot++) {
        CoopOwtMarker* m = &coop_owt_mark[slot];
        bool enc;

        if (slot == local || !m->valid || coop_subinst_slot_inside(slot)) {
            continue;
        }
        enc = m->enc_ms != 0 && (unsigned int)(coop_mid_now_ms - m->enc_ms) < COOP_OWT_ENCOUNTER_MARK_MS;
        if (!enc && (unsigned int)(coop_mid_now_ms - m->ms) >= COOP_OWT_MARKER_LIFE_MS) {
            continue;
        }
        out[n].slot = slot;
        out[n].world_x = m->x;
        out[n].world_y = m->y;
        out[n].moving = m->moving;
        out[n].encounter = enc;
        n++;
    }
    return n;
}

unsigned int coop_middle_world_markers_serial(void)
{
    return coop_owt_serial + coop_subinst_serial() * 7919u; /* M6: entrance markers too */
}

static void coop_owt_reset(bool unhide)
{
    int slot;

    for (slot = 0; slot < 8; slot++) {
        if (unhide) {
            coop_owt_unhide(&coop_owt[slot].hid);
            coop_owt_unhide(&coop_owt_remote_hid[slot]);
        }
        coop_owt_remote_hid[slot].count = 0;
    }
    memset(coop_owt, 0, sizeof(coop_owt));
    memset(coop_owt_tp, 0, sizeof(coop_owt_tp));
    memset(coop_owt_mark, 0, sizeof(coop_owt_mark));
    coop_owt_serial++;
    coop_owt_client_active = false;
    coop_owt_local_roll = 0;
}

/* ---- M5: unified sleep ---- */

static CoopSleepState coop_sleep; /* host */
static int64_t coop_sleep_loc[8]; /* host: where each ready PC stood */
static unsigned int coop_sleep_regen_ms;
static unsigned int coop_sleep_state_ms;
static int coop_sleep_pending_mode; /* host: run in the tick (0 = none) */
static int coop_sleep_apply_hours; /* M7 client inside a solo instance: heal pending */
static bool coop_sleep_rx_valid; /* client: last SLEEP_STATE */
static int coop_sleep_rx_a;
static int coop_sleep_rx_b;

bool coop_middle_unified_sleep_on(void)
{
    /* M7: a solo-instance client votes too (no local vanilla sleep inside). */
    return (tig_net_is_active() || coop_subinst_client_active()) && tig_net_is_coop_enabled()
        && coop_middle_rule_on(COOP_RULE_UNIFIED_SLEEP);
}

/* Living, connected players (a PC on a world map trip counts: it blocks). */
static uint8_t coop_sleep_living(void)
{
    uint8_t mask = 0;
    int slot;

    for (slot = 0; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);

        if (pc != OBJ_HANDLE_NULL && (slot == 0 || tig_net_client_is_active(slot)) && !critter_is_dead(pc)) {
            mask |= (uint8_t)(1u << slot);
        }
    }
    return mask;
}

static void coop_sleep_broadcast(void)
{
    PacketCoopPartyCtrl pkt;
    uint8_t living = coop_sleep_living();
    int depth;

    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_PARTY_CTRL;
    pkt.op = COOP_CTRL_SLEEP_STATE;
    pkt.a = (int)coop_sleep.ready_mask | ((int)coop_sleep.mode << 8);
    pkt.b = (coop_sleep.initiator >= 0 ? coop_sleep.initiator : 0xFF) | (coop_middle_sleep_count(living) << 8);
    depth = tig_net_local_suspend_pause();
    tig_net_send_app_all(&pkt, sizeof(pkt));
    tig_net_local_suspend_resume(depth);
    coop_sleep_state_ms = coop_mid_now_ms;
}

static bool coop_sleep_pc_busy(int slot, int64_t pc)
{
    return combat_turn_based_is_active() || combat_critter_is_combat_mode_active(pc) || coop_owt[slot].active
        || (slot == 0 ? wmap_ui_is_moving() : false);
}

static void coop_sleep_apply(int res, const char* why)
{
    if (res == COOP_SLEEP_RES_EXECUTE) {
        coop_sleep_pending_mode = coop_sleep.mode;
        tig_debug_printf("CoopMiddle: unified sleep: everyone ready (%s) - sleeping, mode %d\n", why, coop_sleep.mode);
        coop_middle_sleep_reset(&coop_sleep);
        coop_sleep_broadcast();
    } else if (res == COOP_SLEEP_RES_WAIT || res == COOP_SLEEP_RES_CLEARED) {
        tig_debug_printf("CoopMiddle: unified sleep: ready 0x%02x mode %d (%s)\n", coop_sleep.ready_mask, coop_sleep.mode,
            why);
        coop_sleep_broadcast();
    }
}

static void coop_sleep_slot_left(int slot, const char* why)
{
    if (!coop_mid_session_host() || coop_sleep.initiator < 0 || (coop_sleep.ready_mask & (1u << slot)) == 0) {
        return;
    }
    coop_sleep_apply(coop_middle_sleep_step(&coop_sleep, coop_sleep_living(), COOP_SLEEP_EV_CANCEL, slot, 0), why);
}

/* Host: slot votes (true = taken over; false = vanilla sleep for a lone host). */
static bool coop_sleep_host_vote(int slot, int mode)
{
    int64_t pc = sub_4A2B60(slot);
    int res;
    bool ru = coop_lite_lang_ru();

    if (coop_sleep_executing || coop_sleep_pending_mode != 0) {
        return true;
    }
    if (mode == COOP_SLEEP_MODE_CANCEL) {
        coop_sleep_slot_left(slot, "N / cancel");
        return true;
    }
    if (pc == OBJ_HANDLE_NULL || critter_is_dead(pc)) {
        return true;
    }
    if (coop_sleep_pc_busy(slot, pc)) {
        if (slot == 0) {
            coop_lite_mid_notice(ru ? "Сейчас спать нельзя." : "You can't sleep now.", false);
        }
        tig_debug_printf("CoopMiddle: unified sleep: slot %d busy (combat / travel)\n", slot);
        return true;
    }
    res = coop_middle_sleep_step(&coop_sleep, coop_sleep_living(), COOP_SLEEP_EV_VOTE, slot, mode);
    if (res == COOP_SLEEP_RES_SOLO) {
        if (slot == 0) {
            return false; /* the host plays alone: vanilla sleep */
        }
        coop_sleep_pending_mode = mode; /* the only living player is a client */
        return true;
    }
    if (res == COOP_SLEEP_RES_REFUSED) {
        return true;
    }
    coop_sleep_loc[slot] = obj_field_int64_get(pc, OBJ_F_LOCATION);
    coop_sleep_apply(res, "vote");
    return true;
}

bool coop_middle_sleep_choose(int hours_value)
{
    int mode;
    bool ru;

    if (!coop_middle_unified_sleep_on()) {
        return false;
    }
    mode = coop_middle_sleep_mode_from_ui(hours_value);
    if (mode == 0) {
        return false;
    }
    if (tig_net_is_host()) {
        return coop_sleep_host_vote(0, mode);
    }
    ru = coop_lite_lang_ru();
    if (coop_owt_client_active) {
        coop_lite_mid_notice(ru ? "Сейчас спать нельзя." : "You can't sleep now.", false);
        return true;
    }
    coop_mid_ctrl_send(COOP_CTRL_SLEEP_VOTE, mode, 0);
    coop_lite_mid_notice(ru ? "Вы готовы ко сну - ждём остальных." : "You are ready to sleep - waiting for the others.",
        true);
    tig_debug_printf("CoopMiddle: unified sleep vote sent (mode %d)\n", mode);
    return true;
}

bool coop_middle_sleep_key(bool yes)
{
    if (!coop_middle_unified_sleep_on()) {
        return false;
    }
    if (tig_net_is_host()) {
        if (coop_sleep.initiator < 0) {
            return false;
        }
        if (!yes) {
            coop_sleep_slot_left(0, "host pressed N");
        } else if ((coop_sleep.ready_mask & 1u) == 0) {
            coop_sleep_host_vote(0, coop_sleep.mode);
        }
        return true;
    }
    if (!coop_sleep_rx_valid || (coop_sleep_rx_a & 0xFF) == 0) {
        return false;
    }
    coop_mid_ctrl_send(COOP_CTRL_SLEEP_VOTE, yes ? (coop_sleep_rx_a >> 8) & 0xFF : COOP_SLEEP_MODE_CANCEL, 0);
    return true;
}

static bool coop_sleep_handle(const PacketCoopPartyCtrl* pkt)
{
    if (tig_net_is_host()) {
        int slot = sub_52A530();

        if (pkt->op != COOP_CTRL_SLEEP_VOTE || slot <= 0 || slot >= 8) {
            tig_debug_printf("CoopMiddle: sleep op %d from slot %d dropped (wrong direction)\n", pkt->op, slot);
            return true;
        }
        if (!coop_middle_rule_on(COOP_RULE_UNIFIED_SLEEP)) {
            return true;
        }
        coop_sleep_host_vote(slot, pkt->a);
        return true;
    }
    if (pkt->op == COOP_CTRL_SLEEP_APPLY) {
        if (sub_52A530() != 0 || !coop_subinst_client_active()) {
            tig_debug_printf("CoopMiddle: SLEEP_APPLY %d h ignored (not inside a solo instance)\n", pkt->a);
            return true;
        }
        coop_sleep_apply_hours = pkt->a; /* run by coop_middle_subinst_local_tick, outside the pump */
        tig_debug_printf("CoopMiddle: party slept %d h - healing inside the solo instance\n", pkt->a);
        return true;
    }
    if (pkt->op != COOP_CTRL_SLEEP_STATE) {
        return true;
    }
    coop_sleep_rx_valid = true;
    coop_sleep_rx_a = pkt->a;
    coop_sleep_rx_b = pkt->b;
    return true;
}

bool coop_middle_sleep_banner(char* out, size_t sz)
{
    int ready;
    int living;
    int initiator;
    int mode;
    char name[64];

    if (out == NULL || sz == 0 || !coop_middle_unified_sleep_on()) {
        return false;
    }
    if (tig_net_is_host()) {
        if (coop_sleep.initiator < 0 || coop_sleep.ready_mask == 0) {
            return false;
        }
        ready = coop_middle_sleep_count(coop_sleep.ready_mask);
        living = coop_middle_sleep_count(coop_sleep_living());
        initiator = coop_sleep.initiator;
        mode = coop_sleep.mode;
    } else {
        if (!coop_sleep_rx_valid || (coop_sleep_rx_a & 0xFF) == 0) {
            return false;
        }
        ready = coop_middle_sleep_count((uint8_t)(coop_sleep_rx_a & 0xFF));
        living = (coop_sleep_rx_b >> 8) & 0xFF;
        initiator = coop_sleep_rx_b & 0xFF;
        mode = (coop_sleep_rx_a >> 8) & 0xFF;
    }
    coop_middle_slot_name(initiator < 8 ? initiator : 0, name, sizeof(name));
    coop_middle_sleep_banner_text(out, sz, ready, living, name, mode, coop_lite_lang_ru());
    return true;
}

static void coop_sleep_heal_hours(int hours)
{
    int64_t pc = player_get_local_pc_obj();

    if (pc != OBJ_HANDLE_NULL && !critter_is_dead(pc)) {
        ObjectList objects;
        ObjectNode* node;

        critter_resting_heal(pc, hours);
        object_list_all_followers(pc, &objects);
        for (node = objects.head; node != NULL; node = node->next) {
            critter_resting_heal(node->obj, hours);
        }
        object_list_destroy(&objects);
    }
    coop_lite_host_rest_heal(hours); /* every other living PC and its followers (+ vitals) */
}

static void coop_sleep_advance(int minutes)
{
    DateTime dt;

    sub_45A950(&dt, (unsigned int)minutes * 60000u);
    timeevent_inc_datetime(&dt);
}

static void coop_sleep_fade(bool out)
{
    FadeData fade;

    memset(&fade, 0, sizeof(fade));
    fade.flags = out ? 0 : FADE_IN;
    fade.duration = 2.0f;
    fade.steps = 48;
    fade.color = tig_color_make(0, 0, 0);
    gfade_run(&fade);
}

static void coop_sleep_execute(int mode)
{
    int64_t host_pc = player_get_local_pc_obj();
    int minutes;
    int hours = 0;

    coop_sleep_executing = true;
    if (sleep_ui_is_active()) {
        sleep_ui_close();
    }
    coop_lite_host_rest_begin(); /* REST_BEGIN: clients fade out */
    coop_sleep_fade(true);
    if (mode == COOP_SLEEP_MODE_HEALED) {
        do {
            coop_sleep_advance(60);
            coop_sleep_heal_hours(1);
            hours++;
        } while (hours < COOP_SLEEP_HEAL_CAP_HOURS
            && ((host_pc != OBJ_HANDLE_NULL && !critter_is_dead(host_pc) && object_hp_damage_get(host_pc) > 0)
                || coop_lite_host_rest_anyone_hurt()));
    } else {
        minutes = coop_middle_sleep_minutes(mode, datetime_current_hour(), datetime_current_minute());
        if (minutes <= 0) {
            minutes = 8 * 60;
        }
        if (minutes % 60 != 0) {
            coop_sleep_advance(minutes % 60);
        }
        for (hours = 0; hours < minutes / 60; hours++) {
            coop_sleep_advance(60);
        }
        if (hours > 0) {
            coop_sleep_heal_hours(hours);
        }
    }
    coop_lite_host_rest_end(); /* game time + vitals, then REST_END */
    {
        /* M7: players inside a solo instance heal there themselves. */
        uint8_t mask = coop_middle_sleep_apply_mask(coop_sleep_living(), coop_subinst_host_lease_mask());
        int apply = hours < 1 ? 1 : (hours > COOP_SLEEP_APPLY_MAX_HOURS ? COOP_SLEEP_APPLY_MAX_HOURS : hours);
        int slot;

        for (slot = 1; slot < 8; slot++) {
            if ((mask & (1u << slot)) != 0) {
                coop_mid_ctrl_send(COOP_CTRL_SLEEP_APPLY, apply, slot);
                tig_debug_printf("CoopMiddle: unified sleep: SLEEP_APPLY %d h -> slot %d (solo instance)\n", apply,
                    slot);
            }
        }
    }
    coop_sleep_fade(false);
    coop_sleep_executing = false;
    tig_debug_printf("CoopMiddle: unified sleep done: mode %d, %d h healed, now day %d %02d:%02d\n", mode, hours,
        datetime_current_day(), datetime_current_hour(), datetime_current_minute());
}

static void coop_sleep_host_tick(unsigned int now_ms)
{
    uint8_t living;
    int slot;

    if (coop_sleep_pending_mode != 0) {
        int mode = coop_sleep_pending_mode;

        coop_sleep_pending_mode = 0;
        coop_sleep_execute(mode);
        return;
    }
    if (coop_sleep.initiator < 0) {
        return;
    }
    if (!coop_middle_rule_on(COOP_RULE_UNIFIED_SLEEP)) {
        coop_middle_sleep_reset(&coop_sleep);
        coop_sleep_broadcast();
        return;
    }
    living = coop_sleep_living();
    for (slot = 0; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);
        const char* why = NULL;

        if ((coop_sleep.ready_mask & (1u << slot)) == 0 || pc == OBJ_HANDLE_NULL || (living & (1u << slot)) == 0) {
            continue;
        }
        if (coop_sleep_pc_busy(slot, pc)) {
            why = "combat / travel";
        } else if (obj_field_int64_get(pc, OBJ_F_LOCATION) != coop_sleep_loc[slot]) {
            why = "moved";
        } else if ((coop_middle_slot_status(slot) & COOP_STATUS_DIALOG) != 0
            || (slot == 0 && (coop_status_local & COOP_STATUS_DIALOG) != 0)) {
            why = "dialog";
        }
        if (why != NULL) {
            coop_sleep_apply(coop_middle_sleep_step(&coop_sleep, living, COOP_SLEEP_EV_CANCEL, slot, 0), why);
            if (coop_sleep.initiator < 0) {
                return;
            }
        }
    }
    coop_sleep_apply(coop_middle_sleep_step(&coop_sleep, living, COOP_SLEEP_EV_CHECK, -1, 0), "check");
    if (coop_sleep.initiator < 0) {
        return;
    }
    if ((unsigned int)(now_ms - coop_sleep_regen_ms) >= COOP_SLEEP_REGEN_MS) {
        coop_sleep_regen_ms = now_ms;
        for (slot = 0; slot < 8; slot++) {
            int64_t pc = sub_4A2B60(slot);
            int dmg;

            if ((coop_sleep.ready_mask & (1u << slot)) == 0 || pc == OBJ_HANDLE_NULL || coop_sleep_pc_busy(slot, pc)) {
                continue;
            }
            dmg = critter_fatigue_damage_get(pc);
            if (dmg > 0) {
                critter_fatigue_damage_set(pc, dmg > COOP_SLEEP_REGEN_FATIGUE ? dmg - COOP_SLEEP_REGEN_FATIGUE : 0);
            }
        }
    }
    if ((unsigned int)(now_ms - coop_sleep_state_ms) >= 2000u) {
        coop_sleep_broadcast(); /* late joiners, lost packets */
    }
}

/* M7 client inside a solo instance (from coop_subinst_ping, net suspended):
   the party's sleep heals this PC and its followers here. */
void coop_middle_subinst_local_tick(void)
{
    int hours = coop_sleep_apply_hours;
    int64_t pc;

    if (hours <= 0) {
        return;
    }
    coop_sleep_apply_hours = 0;
    if (!coop_subinst_client_active()) {
        return;
    }
    pc = player_get_local_pc_obj();
    if (sleep_ui_is_active()) {
        sleep_ui_close();
    }
    coop_sleep_fade(true); /* REST_BEGIN */
    if (pc != OBJ_HANDLE_NULL && !critter_is_dead(pc)) {
        ObjectList objects;
        ObjectNode* node;

        critter_resting_heal(pc, hours);
        object_list_all_followers(pc, &objects);
        for (node = objects.head; node != NULL; node = node->next) {
            critter_resting_heal(node->obj, hours);
        }
        object_list_destroy(&objects);
    }
    coop_sleep_fade(false); /* REST_END; the host's game time packet follows */
    tig_debug_printf("CoopMiddle: solo instance: healed %d h with the party's sleep\n", hours);
}

static void coop_sleep_reset_all(void)
{
    coop_middle_sleep_reset(&coop_sleep);
    coop_sleep_pending_mode = 0;
    coop_sleep_apply_hours = 0;
    coop_sleep_rx_valid = false;
    coop_sleep_rx_a = 0;
    coop_sleep_rx_b = 0;
}

static void coop_m7_reset(void);

void coop_middle_session_reset(void)
{
    int i;

    coop_rules_default(&coop_mid_rx_rules);
    coop_mid_rx_valid = false;
    if (coop_rest_active) {
        coop_lite_mid_prompt(NULL);
    }
    coop_rest_active = false;
    coop_rest_asked = false;
    coop_middle_feed_reset(&coop_feed);
    memset(coop_mid_drops, 0, sizeof(coop_mid_drops));
    for (i = 0; i < COOP_MID_DROPS; i++) {
        coop_mid_drops[i].type = OID_TYPE_NULL;
    }
    coop_mid_drops_next = 0;
    memset(coop_mid_stash, 0, sizeof(coop_mid_stash));
    coop_mid_stash_next = 0;
    coop_trade_local = false;
    coop_trade_local_partner = OBJ_HANDLE_NULL;
    coop_status_reset();
    coop_pause_clear(); /* M3 */
    coop_pings_clear();
    memset(coop_cmd_have, 0, sizeof(coop_cmd_have));
    memset(coop_ping_have, 0, sizeof(coop_ping_have));
    memset(coop_dmg_rem, 0, sizeof(coop_dmg_rem));
    coop_dmg_logged = false;
    coop_qitem_last.armed = false; /* M4 */
    coop_fog_reset();
    coop_owt_reset(true); /* M5 */
    coop_sleep_reset_all();
    coop_sleep_executing = false;
    coop_im_margin_logged = -1;
    coop_subinst_session_reset(); /* M6 */
    coop_m7_reset(); /* M7 */
    coop_mid_lobby_rules_written = false;
    coop_middle_ui_reset(); /* Alt labels, HUD, banner, ping sprites */
}

void coop_middle_slot_gone(int slot)
{
    coop_subinst_slot_gone(slot); /* M6: checkpoint kept, lease freed */
    if (slot > 0 && slot < 8) {
        /* M5: a traveller that left comes back on the map; no sleep wait for it. */
        if (coop_mid_session_host() && coop_owt[slot].active) {
            coop_owt_finish(slot, 0, false, false, "disconnected");
        }
        coop_owt_unhide(&coop_owt_remote_hid[slot]);
        coop_owt_mark[slot].valid = false;
        coop_owt_serial++;
        if (coop_mid_session_host() && coop_sleep.initiator >= 0) {
            coop_sleep_apply(coop_middle_sleep_step(&coop_sleep, (uint8_t)(coop_sleep_living() & ~(1u << slot)),
                                 COOP_SLEEP_EV_CHECK, -1, 0),
                "player left");
        }
    }
    if (slot > 0 && slot < 8) {
        coop_status_client[slot] = 0;
    }
    if (coop_rest_active && coop_rest_slot == slot) {
        coop_rest_end(-1, "the asker disconnected");
    }
    if (slot > 0 && slot < 8) {
        coop_cmd_have[slot] = false;
        coop_ping_have[slot] = false;
        coop_pause_have[slot] = false;
    }
    if (coop_mid_session_host() && coop_pause_on && coop_pause_by == slot && slot > 0) {
        char name[64];

        coop_mid_pc_name(slot, sub_4A2B60(slot), name, sizeof(name));
        coop_pause_set(false, slot, "the pauser disconnected");
        coop_mid_sys_line(COOP_MID_CHAT_PAUSE_DROP, slot, name, "-", -1, -1);
    }
}

void coop_middle_map_changed(void)
{
    coop_subinst_map_changed(); /* M6 */
    coop_rest_end(COOP_REST_REFUSE_CANCELLED, "map change");
    memset(coop_mid_stash, 0, sizeof(coop_mid_stash)); /* containers of the old map */
    /* M3: a new map (travel or a save load) starts unpaused, no old pings. */
    if (coop_pause_on) {
        if (coop_mid_session_host()) {
            coop_pause_set(false, coop_pause_by, "map change / load");
        } else {
            coop_pause_on = false;
            coop_pause_by = -1;
        }
    }
    coop_pings_clear();
    memset(coop_dmg_rem, 0, sizeof(coop_dmg_rem));
    coop_dmg_logged = false;
    /* M4: the host re-sends its town map fog; a client waits for it. */
    coop_fog_forget();
    coop_fog_synced_tm = 0;
    coop_fog_have_ms = false;
    if (coop_mid_session_host()) {
        coop_fog_resend_all = true;
    }
    /* M5: trips end with the map; a sleep round does not survive it. */
    coop_owt_reset(true);
    if (coop_mid_session_host() && coop_sleep.initiator >= 0) {
        coop_middle_sleep_reset(&coop_sleep);
        coop_sleep_broadcast();
    }
    coop_sleep_rx_valid = false;
}

static void (*coop_mid_ui_tick)(unsigned int now_ms);

void coop_middle_set_ui_tick(void (*fn)(unsigned int now_ms))
{
    coop_mid_ui_tick = fn;
}

/* ---- M7: collapse notices ---- */

static CoopCollapseTrack coop_collapse;
static unsigned int coop_collapse_ms;

static void coop_collapse_host_tick(unsigned int now_ms)
{
    int slot;

    if ((unsigned int)(now_ms - coop_collapse_ms) < 250u) {
        return;
    }
    coop_collapse_ms = now_ms;
    for (slot = 0; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);
        bool down = false;

        if (pc != OBJ_HANDLE_NULL && (slot == 0 || tig_net_client_is_active(slot)) && !critter_is_dead(pc)
            && !coop_subinst_slot_inside(slot)) {
            down = critter_fatigue_current(pc) <= 0;
        }
        if (coop_middle_collapse_step(&coop_collapse, slot, down, now_ms)) {
            char name[64];

            coop_mid_pc_name(slot, pc, name, sizeof(name));
            tig_debug_printf("CoopMiddle: slot %d collapsed from exhaustion\n", slot);
            coop_middle_sys_line_all(COOP_MID_CHAT_COLLAPSE, slot, name, "-");
        }
    }
}

/* ---- M7: host autosave ---- */

static bool coop_as_have_last;
static unsigned int coop_as_last_ms; /* the last save, manual or auto */
static unsigned int coop_as_prev_ms;
static unsigned int coop_as_check_ms;
static unsigned int coop_as_next_try_ms;
static bool coop_as_retry; /* due but blocked: try again at next_try */
static bool coop_as_pending; /* the main loop saves */
static int coop_as_index = -1; /* next rotation index, -1 = read the save list first */
static int coop_as_pending_index;

static void coop_autosave_reset(void)
{
    coop_as_have_last = false;
    coop_as_last_ms = 0;
    coop_as_prev_ms = 0;
    coop_as_check_ms = 0;
    coop_as_next_try_ms = 0;
    coop_as_retry = false;
    coop_as_pending = false;
    coop_as_index = -1;
    coop_as_pending_index = 0;
}

void coop_middle_note_save(void)
{
    coop_as_last_ms = coop_mid_now_ms;
    coop_as_have_last = true;
    coop_as_retry = false;
}

static void coop_autosave_gate(CoopAutosaveGate* g)
{
    int64_t pc = player_get_local_pc_obj();
    int slot;

    memset(g, 0, sizeof(*g));
    g->in_world = map_is_valid() && !mainmenu_ui_is_active() && pc != OBJ_HANDLE_NULL && !critter_is_dead(pc)
        && !gamelib_in_load() && !gamelib_in_save();
    g->paused = coop_middle_is_paused();
    g->combat = coop_mid_host_in_combat();
    g->dialog = dialog_ui_is_local_pc_in_dialog() || (coop_status_local & COOP_STATUS_DIALOG) != 0;
    g->world_map = wmap_ui_is_created() != 0;
    g->teleport = teleport_is_teleporting() || coop_sleep_executing || coop_sleep_pending_mode != 0
        || coop_lite_mid_prompt_busy();
    for (slot = 1; slot < 8; slot++) {
        if ((coop_middle_slot_status(slot) & COOP_STATUS_DIALOG) != 0) {
            g->dialog = true;
        }
        if (coop_owt[slot].active) {
            g->teleport = true; /* a hidden PC on an overworld trip */
        }
    }
    g->commit = coop_subinst_host_commit_busy();
}

static void coop_autosave_host_tick(unsigned int now_ms)
{
    int min = coop_middle_rules()->autosave_min;
    CoopAutosaveGate gate;

    if (!coop_as_have_last) {
        coop_as_have_last = true;
        coop_as_last_ms = now_ms;
        coop_as_prev_ms = now_ms;
    }
    if (coop_middle_is_paused()) {
        coop_as_last_ms += now_ms - coop_as_prev_ms; /* the pause freezes the interval */
    }
    coop_as_prev_ms = now_ms;
    if (min <= 0) {
        coop_as_retry = false;
        coop_as_pending = false;
        return;
    }
    if (coop_as_pending || (unsigned int)(now_ms - coop_as_check_ms) < COOP_AUTOSAVE_CHECK_MS) {
        return;
    }
    coop_as_check_ms = now_ms;
    if (coop_as_retry && (int)(now_ms - coop_as_next_try_ms) < 0) {
        return;
    }
    if ((unsigned int)(now_ms - coop_as_last_ms) < (unsigned int)min * 60000u) {
        return;
    }
    coop_autosave_gate(&gate);
    if (coop_middle_autosave_ready(min, now_ms, coop_as_last_ms, &gate)) {
        coop_as_pending = true;
        coop_as_retry = false;
        return;
    }
    if (!coop_as_retry) {
        tig_debug_printf("CoopMiddle: autosave due but blocked (world %d pause %d combat %d dialog %d map %d "
                         "travel %d commit %d) - retry in 15 s\n",
            gate.in_world, gate.paused, gate.combat, gate.dialog, gate.world_map, gate.teleport, gate.commit);
    }
    coop_as_retry = true;
    coop_as_next_try_ms = now_ms + COOP_AUTOSAVE_RETRY_MS;
}

static int coop_autosave_first_index(void)
{
    GameSaveList list;
    unsigned int i;
    int next = 0;

    gamelib_savelist_create(&list);
    gamelib_savelist_sort(&list, GAME_SAVE_LIST_ORDER_DATE, true); /* newest first */
    for (i = 0; i < list.count; i++) {
        const char* name = list.names[i];
        int num;

        if (strlen(name) >= 8 && SDL_strncasecmp(name, "Slot", 4) == 0 && sscanf(name + 4, "%4d", &num) == 1
            && coop_middle_autosave_is_slot(num)) {
            next = coop_middle_autosave_next_index(num);
            break;
        }
    }
    gamelib_savelist_destroy(&list);
    return next;
}

bool coop_middle_take_autosave(char* slot, size_t slot_size, char* desc, size_t desc_size)
{
    CoopAutosaveGate gate;

    if (!coop_as_pending) {
        return false;
    }
    coop_as_pending = false;
    if (!coop_mid_session_host() || slot == NULL || desc == NULL) {
        return false;
    }
    coop_autosave_gate(&gate);
    if (!coop_middle_autosave_ready(coop_middle_rules()->autosave_min, coop_mid_now_ms, coop_as_last_ms, &gate)) {
        coop_as_retry = true;
        coop_as_next_try_ms = coop_mid_now_ms + COOP_AUTOSAVE_RETRY_MS;
        return false;
    }
    if (coop_as_index < 0) {
        coop_as_index = coop_autosave_first_index();
    }
    coop_as_pending_index = coop_as_index % COOP_AUTOSAVE_SLOT_COUNT;
    snprintf(slot, slot_size, "Slot%04d", coop_middle_autosave_slot(coop_as_pending_index));
    snprintf(desc, desc_size, "[Host] AutoSave %d", coop_as_pending_index + 1);
    tig_debug_printf("CoopMiddle: autosave -> %s '%s'\n", slot, desc);
    return true;
}

void coop_middle_autosave_done(bool ok)
{
    char what[16];

    if (!ok) {
        tig_debug_printf("CoopMiddle: autosave %d FAILED - retry in 15 s\n", coop_as_pending_index + 1);
        coop_as_retry = true;
        coop_as_next_try_ms = coop_mid_now_ms + COOP_AUTOSAVE_RETRY_MS;
        return;
    }
    coop_middle_note_save();
    coop_as_index = (coop_as_pending_index + 1) % COOP_AUTOSAVE_SLOT_COUNT;
    snprintf(what, sizeof(what), "%d", coop_as_pending_index + 1);
    coop_middle_sys_line_all(COOP_MID_CHAT_AUTOSAVE, 0, "", what);
}

static void coop_m7_reset(void)
{
    memset(&coop_collapse, 0, sizeof(coop_collapse));
    coop_collapse_ms = 0;
    coop_autosave_reset();
}

void coop_middle_tick(unsigned int now_ms)
{
    int more;

    coop_mid_now_ms = now_ms;
    if (coop_mid_ui_tick != NULL) {
        coop_mid_ui_tick(now_ms);
    }
    if (tig_net_is_active() && !tig_net_is_host() && tig_net_is_coop_enabled()) {
        coop_status_client_tick(now_ms);
    }
    coop_fog_tick(now_ms); /* M4, both sides */
    coop_subinst_tick(now_ms); /* M6 */
    if (!coop_mid_session_host()) {
        return;
    }
    coop_status_host_tick(now_ms);
    if (!coop_mid_lobby_rules_written) {
        coop_mid_lobby_rules_write(coop_middle_rules()); /* M7: host session start */
    }
    if (coop_pause_on) {
        /* M3: the main menu, or a pauser that is gone, lifts the pause. */
        if (mainmenu_ui_is_active()) {
            coop_pause_set(false, coop_pause_by, "main menu");
        } else if (coop_pause_by > 0 && !tig_net_client_is_active(coop_pause_by)) {
            coop_pause_set(false, coop_pause_by, "pauser inactive");
        }
    }
    more = coop_middle_feed_take_suppressed(&coop_feed, now_ms);
    if (more > 0) {
        char what[16];

        snprintf(what, sizeof(what), "%d", more);
        tig_debug_printf("CoopMiddle: loot feed: %d line(s) collapsed\n", more);
        coop_mid_feed_send(COOP_MID_CHAT_MORE, 0, "", what);
    }
    if (coop_rest_active) {
        int64_t pc = sub_4A2B60(coop_rest_slot);

        if (coop_mid_host_in_combat()) {
            coop_rest_end(COOP_REST_REFUSE_COMBAT, "host entered combat");
        } else if (pc == OBJ_HANDLE_NULL || critter_is_dead(pc)) {
            coop_rest_end(COOP_REST_REFUSE_CANCELLED, "the asker died");
        } else if (coop_lite_mid_prompt_busy()) {
            coop_rest_end(COOP_REST_REFUSE_BUSY, "another Y / N prompt");
        } else if ((unsigned int)(now_ms - coop_rest_since_ms) >= COOP_REST_PROMPT_TIMEOUT_MS) {
            coop_rest_end(COOP_REST_REFUSE_TIMEOUT, "no answer");
        }
    }
    coop_owt_host_tick(now_ms); /* M5 */
    coop_sleep_host_tick(now_ms);
    coop_collapse_host_tick(now_ms); /* M7 */
    coop_autosave_host_tick(now_ms);
}

#else /* COOP_LITE_TEST_STUBS */

static CoopRules coop_mid_stub_rules;

const CoopRules* coop_middle_rules(void)
{
    coop_rules_default(&coop_mid_stub_rules);
    return &coop_mid_stub_rules;
}
bool coop_middle_rule_on(uint32_t flag) { return (coop_middle_rules()->flags & flag) != 0; }
void coop_middle_session_reset(void) { }
void coop_middle_note_save(void) { }
bool coop_middle_take_autosave(char* slot, size_t slot_size, char* desc, size_t desc_size)
{
    (void)slot;
    (void)slot_size;
    (void)desc;
    (void)desc_size;
    return false;
}
void coop_middle_autosave_done(bool ok) { (void)ok; }
void coop_middle_subinst_local_tick(void) { }
void coop_middle_tick(unsigned int now_ms) { (void)now_ms; }
void coop_middle_set_ui_tick(void (*fn)(unsigned int now_ms)) { (void)fn; }
void coop_middle_status_set_local(uint8_t bits) { (void)bits; }
uint8_t coop_middle_slot_status(int slot) { (void)slot; return 0; }
uint32_t coop_middle_status_packed(void) { return 0; }
int coop_middle_slot_of_pc(int64_t obj) { (void)obj; return -1; }
void coop_middle_host_slot_synced(int slot) { (void)slot; }
void coop_middle_slot_gone(int slot) { (void)slot; }
void coop_middle_map_changed(void) { }
int64_t coop_middle_find_party_item(int64_t asker_pc, int proto_or_name_key, int match_mode, int64_t* owner_out)
{
    (void)asker_pc;
    (void)proto_or_name_key;
    (void)match_mode;
    if (owner_out != NULL) {
        *owner_out = 0;
    }
    return 0;
}
void coop_middle_quest_handed_over(int64_t owner_pc) { (void)owner_pc; }
void coop_middle_handle_townmap_fog(PacketCoopTownmapFog* pkt) { (void)pkt; }
bool coop_middle_shared_follower_ok(int64_t pc, int64_t npc) { (void)pc; (void)npc; return false; }
void coop_middle_handle_rules(PacketCoopRules* pkt) { (void)pkt; }
bool coop_middle_handle_party_ctrl(const PacketCoopPartyCtrl* pkt) { (void)pkt; return false; }
bool coop_middle_client_rest_ask(int mode) { (void)mode; return false; }
bool coop_middle_rest_key(bool yes) { (void)yes; return false; }
bool coop_middle_client_chat(const PacketCoopChat* pkt) { (void)pkt; return false; }
void coop_middle_note_player_drop(int64_t item) { (void)item; }
void coop_middle_take_begin(CoopMiddleTake* t, int64_t from, int64_t item, int64_t to, int qty)
{
    (void)from;
    (void)item;
    (void)to;
    (void)qty;
    if (t != NULL) {
        memset(t, 0, sizeof(*t));
    }
}
void coop_middle_take_done(CoopMiddleTake* t, bool ok) { (void)t; (void)ok; }
bool coop_middle_trade_live(int64_t pc, int64_t partner) { (void)pc; (void)partner; return false; }
bool coop_middle_host_trade_open(int slot, int64_t pc, int64_t target) { (void)slot; (void)pc; (void)target; return false; }
void coop_middle_trade_given(int64_t giver, int64_t receiver, const char* item_name, int type, int qty)
{
    (void)giver;
    (void)receiver;
    (void)item_name;
    (void)type;
    (void)qty;
}
void coop_middle_trade_local_begin(int64_t partner) { (void)partner; }
void coop_middle_trade_local_end(void) { }
bool coop_middle_trade_local_is(int64_t partner) { (void)partner; return false; }
int64_t coop_middle_trade_partner(void) { return 0; }
bool coop_middle_owt_local_active(void) { return false; }
int coop_middle_trade_barter_kind(int64_t target, int kind) { (void)target; return kind; }
bool coop_middle_trade_refuse_take(int64_t partner) { (void)partner; return false; }
bool coop_middle_host_trade_give(int64_t pc, int64_t partner, int64_t item) { (void)pc; (void)partner; (void)item; return false; }
bool coop_middle_is_paused(void) { return false; }
int coop_middle_paused_by(void) { return -1; }
bool coop_middle_pause_key(void) { return false; }
bool coop_middle_orders_blocked(bool notice) { (void)notice; return false; }
int coop_middle_host_enemy_damage(int64_t target, int dam) { (void)target; return dam; }
bool coop_middle_host_ff_block(int64_t attacker, int64_t target) { (void)attacker; (void)target; return false; }
int coop_middle_host_scale_xp(int xp) { return xp; }
void coop_middle_handle_ping(PacketCoopPing* pkt) { (void)pkt; }
bool coop_middle_ping_send(int64_t loc, int kind) { (void)loc; (void)kind; return false; }
int coop_middle_pings_get(CoopPingView* out, int max, unsigned int now_ms) { (void)out; (void)max; (void)now_ms; return 0; }
unsigned int coop_middle_pings_serial(void) { return 0; }
unsigned int coop_middle_now_ms(void) { return 0; }
void coop_middle_slot_name(int slot, char* out, size_t sz)
{
    (void)slot;
    if (out != NULL && sz > 0) {
        out[0] = '\0';
    }
}
bool coop_middle_try_chat_command(int sender_slot, const char* text) { (void)sender_slot; (void)text; return false; }
/* M5 */
int coop_middle_keep_sector_set(int64_t loc, int64_t* ids, int max) { return coop_lite_keep_ids(loc, ids, max); }
bool coop_middle_slot_in_range(int slot, int64_t loc, int radius) { (void)slot; (void)loc; (void)radius; return true; }
unsigned int coop_middle_slots_in_range(int64_t loc, int radius) { (void)loc; (void)radius; return 0xFEu; }
void coop_middle_send_app_near(void* data, int size, int packet_type, int64_t loc_a, int64_t loc_b)
{
    (void)packet_type;
    (void)loc_a;
    (void)loc_b;
    tig_net_send_app_all(data, size);
}
bool coop_middle_overworld_free(void) { return false; }
bool coop_middle_overworld_solo_hop(int dest_map) { (void)dest_map; return false; }
bool coop_middle_overworld_freeze_time(void) { return false; }
bool coop_middle_overworld_local_step(int64_t loc) { (void)loc; return false; }
bool coop_middle_owt_client_propose(int64_t dest_loc) { (void)dest_loc; return false; }
bool coop_middle_owt_client_teleport(int64_t loc, int spell) { (void)loc; (void)spell; return false; }
void coop_middle_teleport_spell(int64_t source_obj, int64_t target_obj, int spell)
{
    (void)source_obj;
    (void)target_obj;
    (void)spell;
}
void coop_middle_owt_client_closed(void) { }
bool coop_middle_owt_in_transit(int slot) { (void)slot; return false; }
bool coop_middle_owt_local_in_transit(void) { return false; }
void coop_middle_owt_abort_all(const char* why) { (void)why; }
void coop_middle_handle_overworld(PacketCoopOverworldTravel* pkt) { (void)pkt; }
int coop_middle_world_markers(CoopWorldMarker* out, int max) { (void)out; (void)max; return 0; }
unsigned int coop_middle_world_markers_serial(void) { return 0; }
bool coop_middle_unified_sleep_on(void) { return false; }
bool coop_middle_sleep_choose(int hours_value) { (void)hours_value; return false; }
bool coop_middle_sleep_key(bool yes) { (void)yes; return false; }
bool coop_middle_sleep_banner(char* out, size_t sz) { (void)out; (void)sz; return false; }

#endif /* COOP_LITE_TEST_STUBS */
