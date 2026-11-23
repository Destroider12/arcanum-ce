#ifndef ARCANUM_GAME_COOP_MIDDLE_H_
#define ARCANUM_GAME_COOP_MIDDLE_H_

/*
 * Coop Middle M1 (protocol 53): gold split, loot feed, client rest request.
 * Coop Middle M2 (protocol 54): direct trade, status badges (+ the UI-only
 * Alt loot labels and F9 net HUD in ui/coop_middle_ui.c).
 * Coop Middle M3 (protocol 55): shared tactical pause, enemy damage / XP
 * scaling, no party friendly fire, map pings (PACKET_COOP_PING), /tp
 * /stuck /gather.
 * Coop Middle M4 (protocol 56): shared quest items, town map fog sync
 * (PACKET_COOP_TOWNMAP_FOG), parallel dialogs, shared followers.
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
/* M3: client -> host: a = 1 pause / 0 resume, b = 0;
   host -> all: a = state, b = paused_by_slot (0..7). */
#define COOP_CTRL_PAUSE 8
#define COOP_PAUSE_RATE_MS 2000u /* per slot, client asks and host accepts */

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
#define COOP_MID_CHAT_REGROUP 6 /* M3: name = player, text = "0" (/tp, /stuck) or "1" (/gather) */
#define COOP_MID_CHAT_CMD_REFUSE 7 /* M3: name = "", text = "reason seconds" (COOP_TP_REFUSE_*) */
#define COOP_MID_CHAT_PAUSE_DROP 8 /* M3: name = the pauser who left, text = "-" */
#define COOP_MID_CHAT_QUEST 9 /* M4: name = owner, text = item */
#define COOP_MID_CHAT_KIND_MAX 9

/* M3 chat commands (pure parse; the host runs them, never relayed). */
#define COOP_CMD_NONE 0
#define COOP_CMD_TP 1 /* /tp: a client to the host */
#define COOP_CMD_STUCK 2 /* /stuck: the same */
#define COOP_CMD_GATHER 3 /* /gather or /tp all: the host pulls everyone */
#define COOP_CMD_COOLDOWN_MS 30000u /* per slot */
#define COOP_TP_ENEMY_RANGE 20 /* tiles: a hostile NPC focused on you blocks /tp */

#define COOP_TP_REFUSE_DEAD 1
#define COOP_TP_REFUSE_MAP 2
#define COOP_TP_REFUSE_COMBAT 3
#define COOP_TP_REFUSE_TARGETED 4
#define COOP_TP_REFUSE_COOLDOWN 5 /* + seconds left */
#define COOP_TP_REFUSE_HOST_ONLY 6 /* /gather from a client */
#define COOP_TP_REFUSE_NOBODY 7 /* /gather: nobody could be moved */
#define COOP_TP_REFUSE_SELF 8 /* /tp from the host */
#define COOP_TP_REFUSE_MAX 8

/* M3 pings: a marked tile, shown to everyone on the same map. */
#define PACKET_COOP_PING 168
typedef struct PacketCoopPing {
    /* 0000 */ int type; /* PACKET_COOP_PING */
    /* 0004 */ int slot; /* the host stamps the sender's slot */
    /* 0008 */ int map; /* map_current_map() */
    /* 000C */ int kind; /* COOP_PING_KIND_* */
    /* 0010 */ int64_t loc; /* target tile */
} PacketCoopPing;
_Static_assert(sizeof(PacketCoopPing) == 0x18, "PacketCoopPing size");
_Static_assert(offsetof(PacketCoopPing, loc) == 0x10, "PacketCoopPing.loc");

#define COOP_PING_KIND_LOOK 0 /* blue-gold: look here */
#define COOP_PING_KIND_DANGER 1 /* red: enemy / trap */
#define COOP_PING_KIND_MAX 1
#define COOP_PING_RATE_MS 1000u /* per slot */
#define COOP_PING_LIFE_MS 4500u
#define COOP_PING_MAX 16 /* ring buffer */
#define COOP_PING_SOUND_RANGE 40 /* tiles from the local PC */

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

/* M3: damage to a non-party enemy when enemies have pct% HP: (raw * 100 +
   *rem) / pct, the remainder carried in *frac_rem_inout (may be NULL);
   at least 1 when raw > 0, 0 when raw <= 0. pct < 100 counts as 100. */
int coop_middle_scale_enemy_damage(int raw_dmg, int enemy_hp_pct, int* frac_rem_inout);
/* M3: raw * pct / 100 (64-bit, clamped to INT_MAX), never below raw for
   pct >= 100; raw <= 0 stays as is. */
int coop_middle_scale_xp(int raw_xp, int pct);
/* M3: "/tp", "/stuck", "/gather", "/tp all" (case-insensitive, spaces
   trimmed) -> COOP_CMD_*; anything else (plain text, "/unknown") -> NONE. */
int coop_middle_parse_cmd(const char* text);
/* M3: once per interval: true (and *last_ms = now) when nothing was
   admitted yet or interval_ms passed. */
bool coop_middle_rate_admit(bool* have_last, unsigned int* last_ms, unsigned int now_ms, unsigned int interval_ms);
/* M3: whole seconds left of a cooldown (rounded up, 0 = ready). */
int coop_middle_cooldown_left_s(bool have_last, unsigned int last_ms, unsigned int now_ms, unsigned int interval_ms);
/* M3: PACKET_COOP_PING wire check: exact size, type, kind, slot 0..7. */
bool coop_middle_ping_valid(const PacketCoopPing* pkt, int size);

/* ---- M4 ---- */

/* Shared quest items: radius around the asker, in tiles. */
#define COOP_SHARED_QUEST_RADIUS 30
#define COOP_QITEM_MATCH_NAME 0 /* OBJ_F_NAME (dialog "in"/"ni", script "has item named") */
#define COOP_QITEM_MATCH_PROTO 1 /* prototype number */
/* M4: may the asker's quest check look into this ally's pack? Host only,
   rule on, another live, connected, living slot on the same map within
   COOP_SHARED_QUEST_RADIUS (dist < 0 = unknown = no). */
bool coop_middle_quest_pocket_eligible(bool rule_on, bool is_host, int asker_slot, int ally_slot, bool ally_active,
    bool ally_dead, bool same_map, int dist);
/* M4: preference of an item at inventory location loc in an ally's pack:
   0 = backpack (loc < 1000), 1 = worn (1000..1008), -1 = not usable. */
int coop_middle_quest_item_rank(int inventory_location);

/* M4 town map fog: the town map's known-tile bit array (townmap.c kstate,
   saved as Save/Current/<name>.tmf), in chunks. */
#define PACKET_COOP_TOWNMAP_FOG 169
#define COOP_TOWNMAP_FOG_CHUNK 1024
#define COOP_TOWNMAP_FOG_F_REPLACE 0x1 /* host -> client snapshot: overwrite, not OR */
#define COOP_TOWNMAP_FOG_MAX_BYTES 65536 /* sanity cap on offset + len */
#define COOP_TOWNMAP_FOG_TICK_MS 1500u
typedef struct PacketCoopTownmapFog {
    /* 0000 */ int type;
    /* 0004 */ int townmap_id;
    /* 0008 */ uint32_t offset;
    /* 000C */ uint16_t len; /* <= COOP_TOWNMAP_FOG_CHUNK */
    /* 000E */ uint16_t flags;
    /* 0010 */ uint8_t data[COOP_TOWNMAP_FOG_CHUNK];
} PacketCoopTownmapFog;
_Static_assert(offsetof(PacketCoopTownmapFog, townmap_id) == 0x04, "PacketCoopTownmapFog.townmap_id");
_Static_assert(offsetof(PacketCoopTownmapFog, offset) == 0x08, "PacketCoopTownmapFog.offset");
_Static_assert(offsetof(PacketCoopTownmapFog, len) == 0x0C, "PacketCoopTownmapFog.len");
_Static_assert(offsetof(PacketCoopTownmapFog, flags) == 0x0E, "PacketCoopTownmapFog.flags");
_Static_assert(offsetof(PacketCoopTownmapFog, data) == 0x10, "PacketCoopTownmapFog.data");
_Static_assert(sizeof(PacketCoopTownmapFog) == 0x10 + COOP_TOWNMAP_FOG_CHUNK, "PacketCoopTownmapFog size");
#define COOP_TOWNMAP_FOG_HDR ((int)offsetof(PacketCoopTownmapFog, data))
/* Wire check: header + len bytes (size may be the full struct), type,
   townmap_id > 0, len 1..CHUNK, known flags, offset + len within the cap. */
bool coop_middle_townmap_fog_valid(const PacketCoopTownmapFog* pkt, int size);
/* dst |= src (or dst = src with replace); returns the bits newly set in
   dst (replace: bits set now that were clear before). */
int coop_middle_fog_merge(uint8_t* dst, const uint8_t* src, size_t len, bool replace);
/* FNV-1a 32 over len bytes (fog change signature). */
uint32_t coop_middle_fnv1a(const uint8_t* data, size_t len);

/* M4 parallel dialogs: which session (= owner slot) a viewer gets.
   active[s]: slot s's session is open (0 = the host's own dialog).
   Own session first, then the attached one (watch), then the host's, then
   the most recent client session; -1 = nothing to show. */
int coop_middle_dialog_route(int viewer_slot, const bool active[8], int watch_slot, int recent_slot);

/* M4 shared followers: may `slot` manage / barter with a follower?
   The leader always may; with the rule, any party player within range. */
#define COOP_SHARED_FOLLOWER_RANGE 6
bool coop_middle_follower_access_ok(bool rule_on, bool is_leader, bool leader_in_party, bool alive, int dist);

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

/* M3 pause: one shared state (host owns it). While paused timeevent_ping
   advances only real time: no game / anim time, AI, combat, spell ticks.
   Net, render, camera, chat and the UI windows keep running. */
bool coop_middle_is_paused(void);
int coop_middle_paused_by(void); /* slot, -1 when not paused */
/* The Pause key: true when consumed (an active co-op session). */
bool coop_middle_pause_key(void);
/* New world orders (move / attack / cast / throw / hotkey) are refused
   while paused; notice = show the "paused" line (rate limited). */
bool coop_middle_orders_blocked(bool notice);

/* M3 difficulty (host only, no-ops elsewhere). */
int coop_middle_host_enemy_damage(int64_t target, int dam);
/* True: this hit is party on party with the rule on (zero it, no aggro). */
bool coop_middle_host_ff_block(int64_t attacker, int64_t target);
int coop_middle_host_scale_xp(int xp);

/* M4 shared quest items (host). Looks for the item in the asker's pack,
   then in eligible allies' packs (backpack before worn). *owner_out gets
   the pack it was found in. Remembers an ally hit for _handed_over. */
int64_t coop_middle_find_party_item(int64_t asker_pc, int proto_or_name_key, int match_mode, int64_t* owner_out);
/* After a remove / transfer of the item found in owner_pc (an ally):
   resync that player's pack and say "[Quest] <item> handed over from <owner>". */
void coop_middle_quest_handed_over(int64_t owner_pc);
/* M4 town map fog. */
void coop_middle_handle_townmap_fog(PacketCoopTownmapFog* pkt);
/* M4 shared followers (both sides; rules as the host sent them). */
bool coop_middle_shared_follower_ok(int64_t pc, int64_t npc);

/* M3 pings. */
void coop_middle_handle_ping(PacketCoopPing* pkt);
/* Local trigger (MMB / Alt+RMB on the map): true when sent. */
bool coop_middle_ping_send(int64_t loc, int kind);
typedef struct CoopPingView {
    int slot;
    int kind;
    int64_t loc;
    unsigned int age_ms;
    char name[32];
} CoopPingView;
/* Live pings on the current map, oldest first. */
int coop_middle_pings_get(CoopPingView* out, int max, unsigned int now_ms);
unsigned int coop_middle_pings_serial(void); /* bumps on add / clear */
unsigned int coop_middle_now_ms(void); /* the Middle tick clock */
/* A slot's player name (PC name, else the net nick). */
void coop_middle_slot_name(int slot, char* out, size_t sz);

/* M3 chat commands: the host runs them before the relay (or on its own
   send); true = consumed (not chat). */
bool coop_middle_try_chat_command(int sender_slot, const char* text);

/* ---- bridge: small helpers coop_lite.c exports for Middle (game only) ---- */
void coop_lite_mid_notice(const char* str, bool good);
void coop_lite_mid_prompt(const char* text); /* NULL hides the Y / N panel */
bool coop_lite_mid_prompt_busy(void); /* a Lite Y / N (travel, load) is up */
void coop_lite_mid_chat_line(const char* line);
void coop_lite_host_sync_slot_inven(int slot);
bool coop_lite_container_is_store(int64_t container, int64_t pc);
/* M3: host moves a slot's PC to loc (lock, stop anims, keep the sector);
   the pose tick then sends the owner a FORCE with a fresh epoch. */
void coop_lite_mid_move_pc(int slot, int64_t pc, int64_t loc);

#ifdef __cplusplus
}
#endif

#endif /* ARCANUM_GAME_COOP_MIDDLE_H_ */
