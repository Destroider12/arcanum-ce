#ifndef ARCANUM_GAME_COOP_MIDDLE_H_
#define ARCANUM_GAME_COOP_MIDDLE_H_

/*
 * Coop Middle M1 (protocol 53): gold split, loot feed, client rest request.
 * Coop Middle M2 (protocol 54): direct trade, status badges (+ the UI-only
 * Alt loot labels and F9 net HUD in ui/coop_middle_ui.c).
 *
 * All Middle logic lives in coop_middle.c (and coop_rules.c). coop_lite.c and
 * the engine files only call the hooks below, so Coop Lite bugfixes merge in
 * without conflicts. Pure helpers are built into the headless tests; the
 * game side is compiled out under COOP_LITE_TEST_STUBS (no-op stubs).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "game/coop_lite.h"
#include "game/coop_rules.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PACKET_COOP_PARTY_CTRL ops added by Middle (Lite uses 1..4). */
#define COOP_CTRL_REST_ASK 5 /* client -> host: a = COOP_REST_MODE_* */
#define COOP_CTRL_REST_REFUSE 6 /* host -> client: a = COOP_REST_REFUSE_* */
/* M2: client -> host: a = own COOP_STATUS_INVEN | _DIALOG bits;
   host -> clients: a = all slots, 4 bits each (coop_middle_status_pack). */
#define COOP_CTRL_SLOT_STATUS 7

#define COOP_STATUS_INVEN 0x1 /* inventory / loot / trade window open */
#define COOP_STATUS_DIALOG 0x2 /* in a dialog */
#define COOP_STATUS_POISON 0x4 /* STAT_POISON_LEVEL > 0 (host) */
#define COOP_STATUS_COMBAT 0x8 /* combat mode on (host) */
#define COOP_STATUS_CLIENT_MASK (COOP_STATUS_INVEN | COOP_STATUS_DIALOG)
#define COOP_STATUS_SEND_MS 5000u /* resend even without a change */
#define COOP_STATUS_POLL_MS 250u

#define COOP_REST_MODE_NIGHT 1 /* until morning / 8 hours (bed) */
#define COOP_REST_MODE_HEALED 2 /* until healed (rest button) */

#define COOP_REST_REFUSE_HOST 0 /* host pressed N */
#define COOP_REST_REFUSE_COMBAT 1
#define COOP_REST_REFUSE_BUSY 2 /* another Y / N prompt or rest request */
#define COOP_REST_REFUSE_RULE_OFF 3
#define COOP_REST_REFUSE_CANCELLED 4 /* map change, asker died */
#define COOP_REST_REFUSE_CANT_REST 5 /* sleep window refused (town, unconscious) */
#define COOP_REST_REFUSE_TIMEOUT 6
#define COOP_REST_REFUSE_MAX 6

#define COOP_REST_ASK_INTERVAL_MS 5000u
#define COOP_REST_PROMPT_TIMEOUT_MS 60000u

/* PacketCoopChat.reserved for host system lines (clients compose the text
   in their own language). Clients' chat never carries it: the host relay
   rebuilds every client line with reserved = 0. */
#define COOP_MID_CHAT_SYS_MAGIC 0x4D490000 /* 'MI' << 16 */
#define COOP_MID_CHAT_LOOT 1 /* name = player, text = item */
#define COOP_MID_CHAT_GOLD 2 /* name = player, text = "total each" */
#define COOP_MID_CHAT_MORE 3 /* name = "", text = count of collapsed lines */
#define COOP_MID_CHAT_TRADE 4 /* M2: name = giver, text = "receiver|item" */
#define COOP_MID_CHAT_TRADE_GOLD 5 /* M2: name = giver, text = "receiver|amount" */
#define COOP_MID_CHAT_KIND_MAX 5

/* M2 direct trade: a free give between two players, in the pickpocket
   (STEAL) inventory window. COOP_BARTER_OPEN_TRADE extends Lite's
   PacketCoopBarter kinds (0..4); COOP_TARGET_TRADE its target kinds (0..2). */
#define COOP_BARTER_OPEN_TRADE 5
#define COOP_TARGET_TRADE 3
#define COOP_TRADE_RANGE 6 /* tiles */

#define COOP_LOOT_FEED_COST 250 /* equipment at or above this is worth a line */
#define COOP_LOOT_FEED_PER_SEC 5
#define COOP_GOLD_FEED_MIN 10

/* Item types / flags the pure filter needs, mirrored from obj.h and
   obj_flags.h (checked by _Static_assert in coop_middle.c's game part) so
   the headless tests need no engine headers. */
#define COOP_MID_T_WEAPON 5
#define COOP_MID_T_AMMO 6
#define COOP_MID_T_ARMOR 7
#define COOP_MID_T_GOLD 8
#define COOP_MID_T_FOOD 9
#define COOP_MID_T_SCROLL 10
#define COOP_MID_T_KEY 11
#define COOP_MID_T_KEY_RING 12
#define COOP_MID_T_WRITTEN 13
#define COOP_MID_T_GENERIC 14
#define COOP_MID_T_NPC 16
#define COOP_MID_OIF_WONT_SELL 0x00000002
#define COOP_MID_OIF_UBER 0x00040000

/* ---- pure ---- */

/* Splits amount + *carry_inout among living_count players: *each_out per
   player, returns the remainder that goes to the picker (nothing is ever
   lost: each * n + remainder == amount + carry). *carry_inout is consumed
   (0 after the call); living_count < 1 counts as 1. Overflow saturates. */
int coop_middle_split_gold(int amount, int living_count, int* carry_inout, int* each_out);

/* Worth a [Loot] line: keys, scrolls, written notes / schematics, quest or
   unique items, or anything (but gold and ammo) costing COOP_LOOT_FEED_COST+. */
bool coop_middle_loot_feed_worthy(int obj_type, int item_flags, int base_cost, bool is_quest_or_key);

typedef struct CoopFeedLimiter {
    unsigned int window_ms; /* start of the current 1 s window */
    int count; /* lines in this window */
    int suppressed; /* collapsed lines since the window filled */
    bool started;
} CoopFeedLimiter;

void coop_middle_feed_reset(CoopFeedLimiter* l);
/* True when one more line may go out now (<= COOP_LOOT_FEED_PER_SEC per s). */
bool coop_middle_feed_admit(CoopFeedLimiter* l, unsigned int now_ms);
/* After the window: how many lines were collapsed (then 0). */
int coop_middle_feed_take_suppressed(CoopFeedLimiter* l, unsigned int now_ms);

/* "[Loot] Name picked up: Item" / "[Лут] Name подобрал: Item", etc. */
void coop_middle_feed_format(int kind, const char* who, const char* what, bool ru, char* out, size_t sz);
const char* coop_middle_rest_refuse_text(int reason, bool ru);

/* REST_ASK / REST_REFUSE operands (direction is checked by the handler). */
bool coop_middle_party_ctrl_valid(const PacketCoopPartyCtrl* pkt);
bool coop_middle_rest_ask_allowed(bool have_last, unsigned int last_ms, unsigned int now_ms);

/* M2 trade partner: the rule is on, the partner is the PC of another
   active slot, both alive, within COOP_TRADE_RANGE tiles (dist < 0 =
   different map / unknown). */
bool coop_middle_trade_partner_ok(bool rule_on, int self_slot, int partner_slot, bool partner_is_pc,
    bool partner_active, bool self_dead, bool partner_dead, int dist);
/* M2: a Packet81 inside a TRADE session: giving from your own pack
   (flags 0x01, optionally with COOP_XFER_PLANT) is allowed, taking from the
   teammate never. Other kinds: false (Lite decides those). */
bool coop_middle_trade_transfer_ok(int kind, unsigned int flags, bool from_target);

/* M2 status badges: 4 bits per slot, slot 0 in the low nibble. */
uint32_t coop_middle_status_pack(const uint8_t bits[8]);
uint8_t coop_middle_status_get(uint32_t packed, int slot);
/* The portrait badge: false = none. Priority X dead, Z unconscious,
   P poisoned, D dialog, I inventory, C combat. tag gets 2 chars. */
bool coop_middle_status_badge(bool dead, bool unconscious, uint8_t bits, char* tag, uint8_t* r, uint8_t* g,
    uint8_t* b);

/* M2 Alt loot labels (screen rects, priority order: index 0 first). */
#define COOP_LABELS_MAX 32
#define COOP_LABELS_RANGE 22 /* tiles around the PC */
typedef struct CoopLabelRect {
    int x;
    int y;
    int w;
    int h;
    bool hidden; /* out: no room left above */
} CoopLabelRect;

/* Moves each label up past the higher-priority labels it overlaps (a
   1 px gap), keeps it inside [min_y, max_y); a label that runs out of room
   above min_y is hidden. Returns the visible count. */
int coop_middle_labels_deoverlap(CoopLabelRect* rects, int count, int min_y, int max_y);

/* ---- game hooks (no-ops in test stubs) ---- */

const CoopRules* coop_middle_rules(void);
bool coop_middle_rule_on(uint32_t flag);
void coop_middle_session_reset(void);
void coop_middle_tick(unsigned int now_ms);
void coop_middle_host_slot_synced(int slot); /* after SESSION_SYNC (join, rejoin) */
void coop_middle_slot_gone(int slot);
void coop_middle_map_changed(void);
void coop_middle_handle_rules(PacketCoopRules* pkt);
/* Consumes REST_ASK / REST_REFUSE; false for Lite ops. */
bool coop_middle_handle_party_ctrl(const PacketCoopPartyCtrl* pkt);
/* Client rest button / bed: true when a request went (or is pending). */
bool coop_middle_client_rest_ask(int mode);
/* Host Y / N on the rest prompt; false when no rest request waits. */
bool coop_middle_rest_key(bool yes);
/* Client: a host system chat line (loot feed); true when consumed. */
bool coop_middle_client_chat(const PacketCoopChat* pkt);
/* A PC dropped this gold / ammo pile (never split again). */
void coop_middle_note_player_drop(int64_t item);

/* Host: an item moves into a PC from the ground (from = 0) or from a
   container / corpse; or a PC puts gold into a container (stash). Call
   begin before the move (it reads the item), done after it. */
typedef struct CoopMiddleTake {
    bool armed;
    bool stash; /* PC -> container gold: remembered, not split later */
    int64_t taker; /* PC */
    int64_t other; /* container / corpse (or 0 for the ground) */
    int slot;
    int type;
    int qty; /* gold to split */
    int gold_before; /* taker's gold before the move */
    int64_t item; /* non-gold: must end up in the taker's pack */
    int64_t stash_qty;
    bool feed;
    char item_name[64];
} CoopMiddleTake;

void coop_middle_take_begin(CoopMiddleTake* t, int64_t from, int64_t item, int64_t to, int qty);
void coop_middle_take_done(CoopMiddleTake* t, bool ok);

/* M2: ui/coop_middle_ui.c's per-frame tick (game code cannot call UI). */
void coop_middle_set_ui_tick(void (*fn)(unsigned int now_ms));

/* M2 status: the UI reports this machine's INVEN / DIALOG bits each tick;
   the badge reads one slot's bits (host: merged, client: last broadcast). */
void coop_middle_status_set_local(uint8_t bits);
uint8_t coop_middle_slot_status(int slot);
uint32_t coop_middle_status_packed(void);
int coop_middle_slot_of_pc(int64_t obj);

/* M2 trade (game side). */
bool coop_middle_trade_live(int64_t pc, int64_t partner);
/* Host: a client asked for COOP_BARTER_OPEN_TRADE on `target`. */
bool coop_middle_host_trade_open(int slot, int64_t pc, int64_t target);
/* Host: a give inside a TRADE session landed (Lite's gift branch). qty is
   the gold amount for gold, the count for ammo, 1 otherwise. */
void coop_middle_trade_given(int64_t giver, int64_t receiver, const char* item_name, int type, int qty);
/* This machine's trade window: begin before inven_ui_open, end when closed. */
void coop_middle_trade_local_begin(int64_t partner);
void coop_middle_trade_local_end(void);
bool coop_middle_trade_local_is(int64_t partner);
/* Client inven_ui_open: OPEN_TRADE for the trade partner, else `kind`. */
int coop_middle_trade_barter_kind(int64_t target, int kind);
/* Inventory window drag from the partner's pack: true = refused (notice). */
bool coop_middle_trade_refuse_take(int64_t partner);
/* Host's own give in its trade window: true = done here (vanilla skipped). */
bool coop_middle_host_trade_give(int64_t pc, int64_t partner, int64_t item);

/* ---- bridge: small helpers coop_lite.c exports for Middle (game only) ---- */
void coop_lite_mid_notice(const char* str, bool good);
void coop_lite_mid_prompt(const char* text); /* NULL hides the Y / N panel */
bool coop_lite_mid_prompt_busy(void); /* a Lite Y / N (travel, load) is up */
void coop_lite_mid_chat_line(const char* line);
void coop_lite_host_sync_slot_inven(int slot);
bool coop_lite_container_is_store(int64_t container, int64_t pc);

#ifdef __cplusplus
}
#endif

#endif /* ARCANUM_GAME_COOP_MIDDLE_H_ */
