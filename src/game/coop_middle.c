/*
 * Coop Middle M1 (protocol 53): gold split, loot feed, client rest request,
 * and the host -> client session rules (PACKET_COOP_RULES).
 * Coop Middle M2 (protocol 54): direct trade, status badges.
 *
 * Pure helpers first (headless tests), then the game side, compiled out
 * under COOP_LITE_TEST_STUBS (no-op stubs at the end).
 */

#include "game/coop_middle.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/tig_net.h"

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
    return false;
}

bool coop_middle_rest_ask_allowed(bool have_last, unsigned int last_ms, unsigned int now_ms)
{
    return !have_last || (unsigned int)(now_ms - last_ms) >= COOP_REST_ASK_INTERVAL_MS;
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

#ifndef COOP_LITE_TEST_STUBS

#include "game/combat.h"
#include "game/critter.h"
#include "game/item.h"
#include "game/location.h"
#include "game/obj.h"
#include "game/obj_flags.h"
#include "game/multiplayer.h"
#include "game/obj_id.h"
#include "game/object.h"
#include "game/player.h"
#include "tig/debug.h"
#include "tig/memory.h"
#include "ui/sleep_ui.h"

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

/* Host: one rest request at a time. */
static bool coop_rest_active;
static int coop_rest_slot;
static int coop_rest_mode;
static unsigned int coop_rest_since_ms;
/* Client: last ask. */
static bool coop_rest_asked;
static unsigned int coop_rest_asked_ms;

static CoopFeedLimiter coop_feed;

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
    if (tig_net_is_active() && !tig_net_is_host()) {
        if (!coop_mid_rx_valid) {
            coop_rules_default(&coop_mid_rx_rules);
        }
        return &coop_mid_rx_rules;
    }
    if (!coop_mid_host_rules_ready) {
        const char* env = coop_lite_env("ARCANUM_COOP_RULES");
        char text[128];
        char desc[256];
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
    if (coop_rest_active && coop_rest_slot == slot) {
        coop_rest_active = false; /* a rejoin starts clean */
        coop_lite_mid_prompt(NULL);
    }
}

void coop_middle_handle_rules(PacketCoopRules* pkt)
{
    char text[128];
    char line[256];

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

/* ---- lifecycle ---- */

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
}

void coop_middle_slot_gone(int slot)
{
    if (coop_rest_active && coop_rest_slot == slot) {
        coop_rest_end(-1, "the asker disconnected");
    }
}

void coop_middle_map_changed(void)
{
    coop_rest_end(COOP_REST_REFUSE_CANCELLED, "map change");
    memset(coop_mid_stash, 0, sizeof(coop_mid_stash)); /* containers of the old map */
}

static void (*coop_mid_ui_tick)(unsigned int now_ms);

void coop_middle_set_ui_tick(void (*fn)(unsigned int now_ms))
{
    coop_mid_ui_tick = fn;
}

void coop_middle_tick(unsigned int now_ms)
{
    int more;

    coop_mid_now_ms = now_ms;
    if (coop_mid_ui_tick != NULL) {
        coop_mid_ui_tick(now_ms);
    }
    if (!coop_mid_session_host()) {
        return;
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
void coop_middle_tick(unsigned int now_ms) { (void)now_ms; }
void coop_middle_set_ui_tick(void (*fn)(unsigned int now_ms)) { (void)fn; }
void coop_middle_host_slot_synced(int slot) { (void)slot; }
void coop_middle_slot_gone(int slot) { (void)slot; }
void coop_middle_map_changed(void) { }
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
int coop_middle_trade_barter_kind(int64_t target, int kind) { (void)target; return kind; }
bool coop_middle_trade_refuse_take(int64_t partner) { (void)partner; return false; }
bool coop_middle_host_trade_give(int64_t pc, int64_t partner, int64_t item) { (void)pc; (void)partner; (void)item; return false; }

#endif /* COOP_LITE_TEST_STUBS */
