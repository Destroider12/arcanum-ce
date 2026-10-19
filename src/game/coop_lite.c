#include "game/coop_lite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/tig_net.h"

#ifdef COOP_LITE_TEST_STUBS
#include <stdio.h>
#include <stdint.h>

#ifndef OBJ_HANDLE_NULL
#define OBJ_HANDLE_NULL 0LL
#endif

typedef unsigned int TeleportFlags;
#define TELEPORT_RENDER_LOCK 0x0020u

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

#define LOCATION_MAKE(x, y) ((int64_t)((x) & 0xFFFFFFFF) | (((int64_t)(y) & 0xFFFFFFFF) << 32))
#define LOCATION_GET_X(l) ((int64_t)((l) & 0xFFFFFFFF))
#define LOCATION_GET_Y(l) ((int64_t)(((l) >> 32) & 0xFFFFFFFF))

int64_t player_get_local_pc_obj(void);
bool teleport_do(TeleportData* teleport_data);
void tig_debug_printf(const char* fmt, ...);
int sub_529520(void);

/* Test hooks — default: not merchant, no story effects, free tiles. */
int g_coop_lite_test_merchant;
int g_coop_lite_test_story;
int g_coop_lite_test_blocked_mask; /* bit i set → candidate i blocked in spiral */

static bool coop_lite_npc_is_merchant(int64_t npc_obj)
{
    (void)npc_obj;
    return g_coop_lite_test_merchant != 0;
}

static bool coop_lite_loc_is_free_walkable(int64_t loc)
{
    /* In tests, encode candidate index in low bits via caller counting. */
    (void)loc;
    return true;
}

static bool coop_lite_export_local_character_impl(void)
{
    return true;
}

static int64_t coop_lite_host_pc_obj(void)
{
    return player_get_local_pc_obj();
}

#else
#include "game/ai.h"
#include "game/anim.h"
#include "game/critter.h"
#include "game/location.h"
#include "game/map.h"
#include "game/multiplayer.h"
#include "game/obj.h"
#include "game/obj_pool.h"
#include "game/object.h"
#include "game/player.h"
#include "game/item.h"
#include "game/descriptions.h"
#include "game/level.h"
#include "game/obj_id.h"
#include "game/proto.h"
#include "game/ui.h"
#include "game/mp_utils.h"
#include "game/script.h"
#include "game/teleport.h"
#include "game/tile.h"
#include "game/scroll.h"
#include "game/sector.h"
#include "game/invensource.h"
#include "game/combat.h"
#include "game/tf.h"
#include "game/gamelib.h"
#include "game/stat.h"
#include "tig/file.h"
#include "tig/debug.h"
#include "tig/memory.h"

static bool coop_lite_npc_is_merchant(int64_t npc_obj)
{
    Script scr;

    if (npc_obj == OBJ_HANDLE_NULL) {
        return false;
    }
    if (obj_field_int32_get(npc_obj, OBJ_F_TYPE) != OBJ_TYPE_NPC) {
        return false;
    }

    /* Retail stock / shop keepers. */
    if (obj_field_handle_get(npc_obj, OBJ_F_NPC_SUBSTITUTE_INVENTORY) != OBJ_HANDLE_NULL) {
        return true;
    }

    obj_arrayfield_script_get(npc_obj, OBJ_F_SCRIPTS_IDX, SAP_BUY_OBJECT, &scr);
    if (scr.num != 0) {
        return true;
    }

    /* Inventory source with a buy list (shop keepers such as Ristezze). */
    {
        int source_id = obj_field_int32_get(npc_obj, OBJ_F_CRITTER_INVENTORY_SOURCE);
        if (source_id != 0) {
            static InvenSourceSet set;
            invensource_get_id_list(source_id, &set);
            if (set.buy_all || set.buy_cnt > 0) {
                return true;
            }
        }
    }

    return false;
}

static bool coop_lite_loc_is_free_walkable(int64_t loc)
{
    ObjectList objects;
    ObjectNode* node;

    if (tile_is_blocking(loc, false)) {
        return false;
    }

    object_list_location(loc, OBJ_TM_CRITTER, &objects);
    node = objects.head;
    object_list_destroy(&objects);
    return node == NULL;
}

static bool coop_lite_export_local_character_impl(void)
{
    /* Existing MP char exporter (Players/<oid>.mpc). */
    return sub_4A4320();
}

static int64_t coop_lite_host_pc_obj(void)
{
    int64_t host_pc = sub_4A2B60(0);
    if (host_pc != OBJ_HANDLE_NULL) {
        return host_pc;
    }
    return player_get_local_pc_obj();
}
#endif

void coop_lite_host_on_entered_world(void);

static bool coop_lite_applying_follow;
/* Coop death (PT10), see coop_lite_host_watch_party. */
#ifndef COOP_LITE_TEST_STUBS
static CoopPartyWatch coop_party_watch;
static bool coop_reload_request_valid;
static char coop_reload_request_name[TIG_MAX_PATH];
/* Survive the gamelib reset of the reload itself. */
static bool coop_reload_resync_pending;
static bool coop_reload_raise[8];
#endif
/* Client: follow packet seen — carry other PCs on the next own teleport. */
static bool coop_follow_carry_pending;
static int64_t coop_follow_host_loc;
static bool coop_lite_follow_armed; /* set after first host PC teleport; that one is not broadcast */
static bool coop_lite_pending_follow_valid;
static PacketCoopFollowHost coop_lite_pending_follow;
static bool coop_lite_waiting_sync; /* client offered char; waiting for SESSION_SYNC */
static bool coop_lite_session_synced; /* client has applied at least one SESSION_SYNC */

typedef struct CoopPendingOffer {
    bool valid;
    int proto;
    char name[64];
    bool has_oid;
    unsigned char oid[24];
} CoopPendingOffer;

static CoopPendingOffer coop_pending_offers[8];

/* Client: SESSION_SYNC waiting for our local-PC teleport to finish. */
static bool coop_pending_sync_valid;
static PacketCoopSessionSync coop_pending_sync;
static bool coop_pending_sync_teleport_queued;
static unsigned int coop_pending_sync_retry_ms;

/* Host: last PC_POS sent per slot (rate limit + change detect). */
static PacketCoopPcPos coop_last_pc_pos[8];
static bool coop_last_pc_pos_valid[8];
static unsigned int coop_last_pc_pos_ms;
/* Host: last sample applied from the owning client, per slot. */
static PacketCoopPcPos coop_client_applied[8];
static bool coop_client_applied_valid[8];
/* Client: in a synced session (drives own PC locally). */
static bool coop_lite_client_in_session;
/* Client: last own-PC sample sent to host. */
static PacketCoopPcPos coop_client_pos;
static bool coop_client_pos_valid;
static unsigned int coop_last_census_ms;
/* Host: last PC_POS keyframe per slot. */
static unsigned int coop_pc_pos_key_ms[8];

bool coop_lite_can_use_worldmap(void)
{
    if (!tig_net_is_active()) {
        return true;
    }
    return tig_net_is_host();
}

bool coop_lite_can_advance_game_time(void)
{
    if (!tig_net_is_active()) {
        return true;
    }
    return tig_net_is_host();
}

bool coop_lite_can_save_world(void)
{
    if (!tig_net_is_active()) {
        return true;
    }
    return tig_net_is_host();
}

CoopTalkPolicy coop_lite_classify_talk(bool coop_active, bool is_host,
    bool is_merchant, bool dialog_has_story_effects)
{
    (void)dialog_has_story_effects;
    if (!coop_active || is_host) {
        return COOP_TALK_ALLOW;
    }
    return coop_lite_classify_client_talk(is_merchant, false);
}

CoopTalkPolicy coop_lite_classify_client_talk(bool is_merchant, bool follows_other_player)
{
    if (follows_other_player) {
        return COOP_TALK_BARK_FOLLOWER;
    }
    /* Merchants: reduced dialog (barter + neutral; story nodes blanked). */
    if (is_merchant) {
        return COOP_TALK_ALLOW;
    }
    return COOP_TALK_BARK;
}

const char* coop_lite_bark_text(CoopTalkPolicy policy)
{
    switch (policy) {
    case COOP_TALK_BARK_FOLLOWER:
        return "Talk to your leader.";
    case COOP_TALK_BARK:
        return "I have no business with you. Talk to your leader.";
    default:
        return NULL;
    }
}

size_t coop_lite_batch_size(int count, int max_count, size_t entry_size)
{
    if (count < 0 || count > max_count) {
        return 0;
    }
    return 8 + (size_t)count * entry_size;
}

bool coop_lite_batch_valid(int count, int max_count, size_t entry_size, int msg_size)
{
    size_t need;

    if (count < 0 || count > max_count) {
        return false;
    }
    need = coop_lite_batch_size(count, max_count, entry_size);
    /* msg_size <= 0: size unknown (local loopback) — trust the count. */
    return msg_size <= 0 || (size_t)msg_size >= need;
}

bool coop_lite_world_ready_rule(bool in_session, bool sync_pending, bool waiting_sync,
    bool map_valid, bool map_clearing, bool has_local_pc)
{
    return in_session && !sync_pending && !waiting_sync
        && map_valid && !map_clearing && has_local_pc;
}

bool coop_lite_parse_dev_loc(const char* value, int* x, int* y)
{
    int px;
    int py;
    char tail;

    if (value == NULL || value[0] == '\0') {
        return false;
    }
    if (strcmp(value, "shrouded") == 0 || strcmp(value, "merchant") == 0) {
        *x = COOP_LITE_DEV_SHROUDED_X;
        *y = COOP_LITE_DEV_SHROUDED_Y;
        return true;
    }
    if (sscanf(value, "%d,%d%c", &px, &py, &tail) != 2 || px <= 0 || py <= 0) {
        return false;
    }
    *x = px;
    *y = py;
    return true;
}

bool coop_lite_follow_needs_jump(int64_t dx, int64_t dy, int64_t half_w, int64_t half_h)
{
    return llabs(dx) > half_w || llabs(dy) > half_h;
}

unsigned int coop_lite_party_step(CoopPartyWatch* w, const bool present[8], const bool dead[8],
    int map, unsigned int now_ms, unsigned int* fell, unsigned int* rose)
{
    unsigned int acts = 0;
    unsigned int fell_mask = 0;
    unsigned int rose_mask = 0;
    bool all_dead;
    bool d;
    int s;

    for (s = 0; s < 8; s++) {
        d = present[s] && dead[s];
        if (d && !w->was_dead[s]) {
            fell_mask |= 1u << s;
        } else if (!d && w->was_dead[s]) {
            rose_mask |= 1u << s;
        }
        w->was_dead[s] = d;
    }
    if (fell != NULL) {
        *fell = fell_mask;
    }
    if (rose != NULL) {
        *rose = rose_mask;
    }
    if (w->reload_issued || !present[0]) {
        return 0; /* reload under way / host not in the world */
    }

    if ((fell_mask & 1u) != 0) {
        w->host_down = true;
        w->host_down_map = map;
        acts |= COOP_PARTY_ACT_HOST_DOWN;
    } else if ((rose_mask & 1u) != 0) {
        w->host_down = false;
        acts |= COOP_PARTY_ACT_HOST_UP;
    }

    /* The location was left while the host is down: nobody can bring the
       leader back there any more. */
    if (w->host_down && map != w->host_down_map) {
        w->reload_issued = true;
        w->wipe_pending = false;
        return acts | COOP_PARTY_ACT_RELOAD;
    }

    all_dead = dead[0];
    for (s = 1; s < 8; s++) {
        if (present[s] && !dead[s]) {
            all_dead = false;
        }
    }
    if (all_dead && !w->wipe_pending) {
        w->wipe_pending = true;
        w->wipe_since_ms = now_ms;
        acts |= COOP_PARTY_ACT_WIPE_COUNTDOWN;
    } else if (!all_dead && w->wipe_pending) {
        w->wipe_pending = false;
        acts |= COOP_PARTY_ACT_WIPE_CANCEL;
    }
    if (w->wipe_pending
        && coop_lite_interval_elapsed(w->wipe_since_ms, now_ms, COOP_LITE_WIPE_GRACE_MS)) {
        w->wipe_pending = false;
        w->reload_issued = true;
        acts |= COOP_PARTY_ACT_RELOAD;
    }
    return acts;
}

bool coop_lite_should_carry_slot(int slot, int mover_slot, bool bound)
{
    return bound && slot >= 0 && slot < 8 && slot != mover_slot;
}

bool coop_lite_is_client_role(void)
{
    /* Local suspension is only used by clients running their own PC's SP
       path (move/talk); the dialog it opens is still a client dialog. */
    if (tig_net_is_locally_suspended()) {
        return true;
    }
    return tig_net_is_active() && !tig_net_is_host();
}

bool coop_lite_dialog_action_blocked(const char* code)
{
    /*
     * Two-letter dialog action codes a client must not run: they touch
     * shared campaign state (quests, global/local/PC vars and flags, story
     * state, reputation, alignment, rumors, XP, fate points), followers and
     * party (join/wait/unwait/leave/stay close/spread out/orders), items and
     * money (host-authoritative economy), combat, kill, origin, newspapers.
     * Allowed: re (reaction), fl (float line), mm (area known), tr (skill
     * training on the client's own PC).
     */
    static const char blocked[][2] = {
        { 'q', 'u' }, { 'g', 'v' }, { 'g', 'f' }, { 'l', 'v' }, { 'l', 'f' },
        { 'l', 'c' }, { 's', 's' }, { 'r', 'p' }, { 'a', 'l' }, { 'r', 'u' },
        { 'r', 'q' }, { 'x', 'p' }, { 'f', 'p' }, { 'p', 'v' }, { 'p', 'f' },
        { 'j', 'o' }, { 'w', 'a' }, { 'u', 'w' }, { 's', 'c' }, { 's', 'o' },
        { 'c', 'e' }, { 's', 'u' }, { 'i', 'i' }, { 'r', 'i' }, { 'e', 't' },
        { '$', '$' }, { 'i', 'n' }, { 'c', 'o' }, { 'n', 'k' }, { 'o', 'r' },
        { 'n', 'p' },
    };
    char c0;
    char c1;
    size_t i;

    if (code == NULL || code[0] == '\0' || code[1] == '\0') {
        return false;
    }
    c0 = code[0];
    c1 = code[1];
    if (c0 >= 'A' && c0 <= 'Z') {
        c0 = (char)(c0 - 'A' + 'a');
    }
    if (c1 >= 'A' && c1 <= 'Z') {
        c1 = (char)(c1 - 'A' + 'a');
    }
    for (i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
        if (c0 == blocked[i][0] && c1 == blocked[i][1]) {
            return true;
        }
    }
    return false;
}

bool coop_lite_dialog_option_allowed(const char* actions, int response_code)
{
    char code[3];
    const char* pch;

    /* response_code (e.g. 3 = barter) does not change the verdict: barter
       itself is fine, only its action string matters. */
    (void)response_code;

    if (actions == NULL || actions[0] == '\0') {
        return true;
    }

    code[2] = '\0';
    pch = actions;
    while (*pch != '\0') {
        while (*pch != '\0' && !((*pch >= 'A' && *pch <= 'Z')
                || (*pch >= 'a' && *pch <= 'z')
                || *pch == '$')) {
            pch++;
        }
        if (*pch == '\0') {
            break;
        }
        code[0] = *pch++;
        if (*pch == '\0') {
            break;
        }
        code[1] = *pch++;
        if (coop_lite_dialog_action_blocked(code)) {
            return false;
        }
    }
    return true;
}

bool coop_lite_client_may_open_dialog(int64_t npc_obj, int64_t pc_obj)
{
    CoopTalkPolicy policy = coop_lite_can_talk(npc_obj, pc_obj);
    return policy == COOP_TALK_ALLOW || policy == COOP_TALK_INFO_ONLY;
}

#ifndef COOP_LITE_TEST_STUBS
/* NPC follows a PC other than ours: leader field, or listed as a follower of
   any slot PC (covers a leader field not yet synced). */
static bool coop_lite_npc_follows_other_player(int64_t npc_obj, int64_t pc_obj)
{
    int64_t leader;
    int slot;
    int idx;
    int cnt;
    int64_t slot_pc;

    leader = critter_leader_get(npc_obj);
    if (leader != OBJ_HANDLE_NULL) {
        return leader != pc_obj;
    }
    for (slot = 0; slot < 8; slot++) {
        slot_pc = sub_4A2B60(slot);
        if (slot_pc == OBJ_HANDLE_NULL || slot_pc == pc_obj) {
            continue;
        }
        cnt = obj_arrayfield_length_get(slot_pc, OBJ_F_CRITTER_FOLLOWER_IDX);
        for (idx = 0; idx < cnt; idx++) {
            if (obj_arrayfield_handle_get(slot_pc, OBJ_F_CRITTER_FOLLOWER_IDX, idx) == npc_obj) {
                return true;
            }
        }
    }
    return false;
}
#endif

CoopTalkPolicy coop_lite_can_talk(int64_t npc_obj, int64_t pc_obj)
{
    /* Suspension-aware: the client's own talk runs suspended. */
    if (!coop_lite_is_client_role()) {
        return COOP_TALK_ALLOW;
    }
#ifndef COOP_LITE_TEST_STUBS
    if (npc_obj == OBJ_HANDLE_NULL
        || obj_field_int32_get(npc_obj, OBJ_F_TYPE) != OBJ_TYPE_NPC) {
        return COOP_TALK_ALLOW;
    }
    return coop_lite_classify_client_talk(coop_lite_npc_is_merchant(npc_obj),
        coop_lite_npc_follows_other_player(npc_obj, pc_obj));
#else
    (void)pc_obj;
    return coop_lite_classify_client_talk(coop_lite_npc_is_merchant(npc_obj), false);
#endif
}

int64_t coop_lite_story_pc_obj(void)
{
    if (!tig_net_is_active()) {
        return player_get_local_pc_obj();
    }
    return coop_lite_host_pc_obj();
}

void coop_lite_slot_preferred_offset(int slot, int* dx_ptr, int* dy_ptr)
{
    /* Stable ring around host: slot 0 stays on host tile; 1..7 take unique neighbours. */
    static const int k_dx[8] = { 0, 1, -1, 0, 0, 1, -1, 1 };
    static const int k_dy[8] = { 0, 0, 0, 1, -1, 1, 1, -1 };
    int s = slot;

    if (s < 0) {
        s = 0;
    }
    s %= 8;
    if (dx_ptr != NULL) {
        *dx_ptr = k_dx[s];
    }
    if (dy_ptr != NULL) {
        *dy_ptr = k_dy[s];
    }
}

int64_t coop_lite_follow_loc_for_slot(int64_t host_loc, int slot)
{
    int dx;
    int dy;
    int64_t x;
    int64_t y;
    int64_t preferred;
    int radius;
    int dir;
    int64_t cand;

    coop_lite_slot_preferred_offset(slot, &dx, &dy);
    x = LOCATION_GET_X(host_loc) + dx;
    y = LOCATION_GET_Y(host_loc) + dy;
    preferred = LOCATION_MAKE(x, y);

#ifdef COOP_LITE_TEST_STUBS
    (void)radius;
    (void)dir;
    (void)cand;
    /* Tests: return preferred bias (no world tiles). */
    return preferred;
#else
    if (coop_lite_loc_is_free_walkable(preferred)) {
        return preferred;
    }

    /* Spiral search outward from host (include host tile as last-resort fill). */
    for (radius = 0; radius <= 8; radius++) {
        for (dir = 0; dir < 8; dir++) {
            int odx = 0;
            int ody = 0;
            /* Map dir to hex-ish offsets matching location_in_dir order preference. */
            switch (dir) {
            case 0: odx = radius; ody = 0; break;
            case 1: odx = radius; ody = radius; break;
            case 2: odx = 0; ody = radius; break;
            case 3: odx = -radius; ody = radius; break;
            case 4: odx = -radius; ody = 0; break;
            case 5: odx = -radius; ody = -radius; break;
            case 6: odx = 0; ody = -radius; break;
            case 7: odx = radius; ody = -radius; break;
            }
            /* Rotate spiral start by slot so peers fan out. */
            if (radius == 0 && dir != (slot & 7)) {
                continue;
            }
            cand = LOCATION_MAKE(LOCATION_GET_X(host_loc) + odx,
                LOCATION_GET_Y(host_loc) + ody);
            if (coop_lite_loc_is_free_walkable(cand)) {
                return cand;
            }
        }
    }

    return preferred;
#endif
}

void coop_lite_notify_host_teleported(int map, int64_t loc)
{
    PacketCoopFollowHost pkt;
    int shopping_map;

    if (!tig_net_is_active() || !tig_net_is_host()) {
        return;
    }
    if (coop_lite_applying_follow) {
        return;
    }

#ifndef COOP_LITE_TEST_STUBS
    shopping_map = map_by_type(MAP_TYPE_SHOPPING_MAP);
    /* Never broadcast menu/shop placement or invalid maps. */
    if (map <= 0 || (shopping_map != 0 && map == shopping_map)) {
        return;
    }
#else
    (void)shopping_map;
    if (map <= 0) {
        return;
    }
#endif

    /*
     * First host PC teleport after session start is the New Game / Load spawn.
     * Arm follow for subsequent real travel; do not yank clients still on the
     * main menu into the world (that crashed host via bad client anim packets).
     */
    if (!coop_lite_follow_armed) {
        coop_lite_follow_armed = true;
        tig_debug_printf("CoopLite: armed follow (skip initial spawn broadcast map=%d)\n",
            map);
        /* Host just entered the start map — bind slot 0 and sync joiners. */
        coop_lite_host_on_entered_world();
        return;
    }

    pkt.type = PACKET_COOP_FOLLOW_HOST;
    pkt.map = map;
    pkt.loc = loc;
    tig_net_send_app_all(&pkt, sizeof(pkt));
    tig_debug_printf("CoopLite: host follow broadcast map=%d loc=%lld\n",
        map, (long long)loc);
}

static bool coop_lite_client_in_world(void)
{
    int64_t pc_obj;
    int cur;
    int shopping_map;

    pc_obj = player_get_local_pc_obj();
    if (pc_obj == OBJ_HANDLE_NULL) {
        return false;
    }

#ifndef COOP_LITE_TEST_STUBS
    cur = map_current_map();
    shopping_map = map_by_type(MAP_TYPE_SHOPPING_MAP);
    if (cur <= 0) {
        return false;
    }
    if (shopping_map != 0 && cur == shopping_map) {
        return false; /* still on main-menu ShopMap backdrop */
    }
#else
    (void)cur;
    (void)shopping_map;
#endif
    return true;
}

void coop_lite_handle_follow_packet(PacketCoopFollowHost* pkt)
{
    TeleportData teleport_data;
    int64_t pc_obj;
    int64_t dest;
    int slot;

    if (pkt == NULL || pkt->type != PACKET_COOP_FOLLOW_HOST) {
        return;
    }

    if (tig_net_is_host()) {
        return;
    }
    if (!tig_net_is_active()) {
        return;
    }

    /* Clients still on the main menu must not teleport/load the host map. */
    if (!coop_lite_client_in_world()) {
        coop_lite_pending_follow = *pkt;
        coop_lite_pending_follow_valid = true;
        tig_debug_printf("CoopLite: queued follow map=%d (client not in world yet)\n",
            pkt->map);
        return;
    }

    pc_obj = player_get_local_pc_obj();
    slot = sub_529520();
    dest = coop_lite_follow_loc_for_slot(pkt->loc, slot);

    memset(&teleport_data, 0, sizeof(teleport_data));
    teleport_data.flags = TELEPORT_RENDER_LOCK;
    teleport_data.obj = pc_obj;
    teleport_data.loc = dest;
    teleport_data.map = pkt->map;

    /* The teleport runs later (teleport_ping); carry the other PCs then. */
    coop_follow_carry_pending = true;
    coop_follow_host_loc = pkt->loc;
    coop_lite_applying_follow = true;
    teleport_do(&teleport_data);
    coop_lite_applying_follow = false;

    tig_debug_printf("CoopLite: client follow apply map=%d host_loc=%lld dest=%lld slot=%d\n",
        pkt->map, (long long)pkt->loc, (long long)dest, slot);

    /* Keep companion character file current after area changes. */
    coop_lite_export_local_character();
}

#ifndef COOP_LITE_TEST_STUBS
static void coop_lite_world_reset(void);
static void coop_lite_keep_sector(int slot, int64_t loc);
static void coop_lite_remember_slot_pc(int slot, int64_t obj, int proto);
static void coop_lite_host_raise_pc(int64_t pc);
static void coop_lite_unkeep_sector(int slot);
static int64_t coop_lite_take_slot_pc(int slot);
#endif

void coop_lite_session_reset(void)
{
    int i;

    coop_lite_follow_armed = false;
    coop_lite_pending_follow_valid = false;
    memset(&coop_lite_pending_follow, 0, sizeof(coop_lite_pending_follow));
    coop_lite_waiting_sync = false;
    coop_lite_session_synced = false;
    coop_lite_client_in_session = false;
    coop_client_pos_valid = false;
    coop_last_census_ms = 0;
    coop_pending_sync_valid = false;
    coop_pending_sync_teleport_queued = false;
    coop_pending_sync_retry_ms = 0;
    coop_last_pc_pos_ms = 0;
#ifndef COOP_LITE_TEST_STUBS
    coop_lite_world_reset();
#endif
    for (i = 0; i < 8; i++) {
        coop_last_pc_pos_valid[i] = false;
        coop_client_applied_valid[i] = false;
        coop_pending_offers[i].has_oid = false;
        coop_pending_offers[i].valid = false;
        coop_pending_offers[i].proto = 0;
        coop_pending_offers[i].name[0] = '\0';
    }
}

void coop_lite_flush_pending_follow(void)
{
    PacketCoopFollowHost pkt;

    if (!coop_lite_pending_follow_valid) {
        return;
    }
    if (!tig_net_is_active() || tig_net_is_host()) {
        coop_lite_pending_follow_valid = false;
        return;
    }
    if (!coop_lite_client_in_world()) {
        return;
    }

    pkt = coop_lite_pending_follow;
    coop_lite_pending_follow_valid = false;
    coop_lite_handle_follow_packet(&pkt);
}

bool coop_lite_can_use_turn_based(void)
{
    if (!tig_net_is_active()) {
        return true;
    }
    /* Hard real-time in coop — TB desyncs DEX timing across peers. */
    return false;
}

void coop_lite_handle_attack_intent(PacketCoopAttackIntent* pkt)
{
    int64_t attacker;
    int64_t target;

    if (pkt == NULL || pkt->type != PACKET_COOP_ATTACK_INTENT) {
        return;
    }
    if (!tig_net_is_active() || !tig_net_is_host()) {
        return;
    }

#ifndef COOP_LITE_TEST_STUBS
    if (!map_is_valid() || map_is_clearing_objects()) {
        return;
    }
    {
        ObjectID attacker_oid;
        ObjectID target_oid;
        memcpy(&attacker_oid, pkt->attacker_oid, sizeof(attacker_oid));
        memcpy(&target_oid, pkt->target_oid, sizeof(target_oid));
        attacker = obj_pool_perm_lookup(attacker_oid);
        target = obj_pool_perm_lookup(target_oid);
    }
    if (attacker == OBJ_HANDLE_NULL || target == OBJ_HANDLE_NULL) {
        tig_debug_printf("CoopLite: attack intent dropped (missing oid)\n");
        return;
    }
    if (!coop_lite_sender_owns_obj(attacker)) {
        tig_debug_printf("CoopLite: attack intent dropped (slot %d does not own attacker)\n", sub_52A530());
        return;
    }
    (void)pkt->attack_kind;
    /* Host resolves AG_ATTACK — combat_dmg / ai_attack run here (event-level). */
    anim_goal_attack(attacker, target);
#else
    (void)attacker;
    (void)target;
#endif
}

void coop_lite_propagate_party_aggression(int64_t attacker_obj, int64_t target_obj)
{
#ifdef COOP_LITE_TEST_STUBS
    (void)attacker_obj;
    (void)target_obj;
#else
    int64_t host_pc;

    if (!tig_net_is_active() || !tig_net_is_host()) {
        return;
    }
    if (attacker_obj == OBJ_HANDLE_NULL || target_obj == OBJ_HANDLE_NULL) {
        return;
    }
    if (obj_field_int32_get(attacker_obj, OBJ_F_TYPE) != OBJ_TYPE_PC) {
        return;
    }
    host_pc = coop_lite_host_pc_obj();
    if (host_pc == OBJ_HANDLE_NULL || host_pc == attacker_obj) {
        return;
    }
    /* Quiet mirror: flags 0x01 = already-in-combat path reduces float spam. */
    ai_attack(host_pc, target_obj, LOUDNESS_SILENT, 0x01);
#endif
}


bool coop_lite_export_local_character(void)
{
    if (!tig_net_is_active()) {
        return false;
    }
    if (player_get_local_pc_obj() == OBJ_HANDLE_NULL) {
        return false;
    }
    return coop_lite_export_local_character_impl();
}


CoopMpPacketRole coop_lite_mp_packet_role(int type)
{
    switch (type) {
    case 4: /* move intent */
    case 29: /* open container */
    case 57: /* request cast */
    case PACKET_COOP_ATTACK_INTENT: /* client attack → host AG_ATTACK */
    case PACKET_COOP_CHAR_OFFER: /* client char → host spawn */
        return COOP_MP_PKT_HOST_RESOLVE;
    case 10: /* anim pose */
    case 11: /* combat mode */
    case 58: /* cast spell */
    case 60:
    case 61:
    case 70: /* object create — keep for summons; blood FX optional */
    case 72:
    case 73:
    case 74:
    case 75:
    case 76:
    /* 77 eye-candy: suppressed — clients spawn FX locally from Packet58 cast */
    case 92:
    case 99: /* location */
    case 2: /* player list */
    case 3: /* game time */
    case 68: /* lagging */
    case 69: /* recovered */
    case PACKET_COOP_FOLLOW_HOST:
    case PACKET_COOP_FOLLOWERS: /* host follower table → client */
    case PACKET_COOP_NPC_POS: /* host NPC poses → client */
    case PACKET_COOP_PARTY_STATE: /* host death / reload notices → client */
    case PACKET_COOP_SESSION_SYNC: /* host map+PC snapshot → client */
        return COOP_MP_PKT_CLIENT_APPLY;
    case 122: /* client notification (ignored on host) */
        return COOP_MP_PKT_HOST_RESOLVE;
    case 8: /* anim modify: host→clients applies; a client→host modify of
               its own goal is legal vanilla MP traffic (host ignores it) */
    case 98: /* multiplayer flags change */
    case PACKET_COOP_PC_POS: /* owner PC pos: client→host, host→others */
        return COOP_MP_PKT_BOTH;
    case 9: /* interrupt — host resolves client intent, peers apply */
    case 100: /* UI + vitals subtype 13 */
        return COOP_MP_PKT_BOTH;
    default:
        return COOP_MP_PKT_UNKNOWN;
    }
}

bool coop_lite_client_waiting_for_sync(void)
{
    return coop_lite_waiting_sync;
}

bool coop_lite_client_session_ready(void)
{
    return coop_lite_session_synced && !coop_lite_waiting_sync;
}

void coop_lite_acknowledge_session_ready(void)
{
    coop_lite_session_synced = false;
}

bool coop_lite_should_skip_independent_new_game(void)
{
    if (!tig_net_is_active()) {
        return false;
    }
    if (tig_net_is_host()) {
        return false;
    }
    /* Coop clients never start their own campaign world. */
    return true;
}

bool coop_lite_client_offer_character(int basic_prototype, const char* name)
{
    PacketCoopCharOffer pkt;

    if (!tig_net_is_active() || tig_net_is_host()) {
        return false;
    }
    if (basic_prototype <= 0) {
        return false;
    }

    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_CHAR_OFFER;
    pkt.slot = sub_529520();
    pkt.basic_prototype = basic_prototype;
    strncpy(pkt.name, name != NULL ? name : "Companion", sizeof(pkt.name) - 1);
#ifndef COOP_LITE_TEST_STUBS
    {
        int64_t pc = player_get_local_pc_obj();
        if (pc != OBJ_HANDLE_NULL) {
            ObjectID oid = obj_get_id(pc);
            memcpy(pkt.pc_oid, &oid, sizeof(oid));
            pkt.has_oid = 1;
        }
    }
#endif
    tig_net_send_app_all(&pkt, sizeof(pkt));
    coop_lite_waiting_sync = true;
    tig_debug_printf("CoopLite: CHAR_OFFER sent slot=%d proto=%d name=%s has_oid=%d\n",
        pkt.slot, pkt.basic_prototype, pkt.name, pkt.has_oid);
    return true;
}

bool coop_lite_pc_pos_moved(const PacketCoopPcPos* prev, const PacketCoopPcPos* cur)
{
    if (prev == NULL || cur == NULL) {
        return true;
    }
    return prev->loc != cur->loc
        || prev->offset_x != cur->offset_x
        || prev->offset_y != cur->offset_y
        || memcmp(prev->oid, cur->oid, sizeof(prev->oid)) != 0;
}

bool coop_lite_pc_pos_changed(const PacketCoopPcPos* prev, const PacketCoopPcPos* cur)
{
    if (prev == NULL || cur == NULL) {
        return true;
    }
    return prev->loc != cur->loc
        || prev->offset_x != cur->offset_x
        || prev->offset_y != cur->offset_y
        || prev->art_id != cur->art_id
        || memcmp(prev->oid, cur->oid, sizeof(prev->oid)) != 0;
}

bool coop_lite_follow_delta(int64_t dx, int64_t dy, int64_t zone_x, int64_t zone_y,
    int64_t* out_x, int64_t* out_y)
{
    int64_t sx = 0;
    int64_t sy = 0;

    if (dx > zone_x) {
        sx = dx - zone_x;
    } else if (dx < -zone_x) {
        sx = dx + zone_x;
    }
    if (dy > zone_y) {
        sy = dy - zone_y;
    } else if (dy < -zone_y) {
        sy = dy + zone_y;
    }
    if (out_x != NULL) {
        *out_x = sx;
    }
    if (out_y != NULL) {
        *out_y = sy;
    }
    return sx != 0 || sy != 0;
}

bool coop_lite_interval_elapsed(unsigned int last_ms, unsigned int now_ms, unsigned int interval_ms)
{
    return (unsigned int)(now_ms - last_ms) >= interval_ms;
}

bool coop_lite_packet_allowed(int type, bool receiver_is_host, int sender_slot)
{
    CoopMpPacketRole role;

    if (type <= 0 || type >= PACKET_COOP_TYPE_LIMIT) {
        return false;
    }
    if (sender_slot < 0 || sender_slot >= 8) {
        return false;
    }

    role = coop_lite_mp_packet_role(type);
    if (receiver_is_host) {
        /* Local loopback (slot 0) is trusted. */
        if (sender_slot == 0) {
            return true;
        }
        /* Clients may not inject host→client state. */
        return role != COOP_MP_PKT_CLIENT_APPLY;
    }

    /* Clients only listen to the host. */
    if (sender_slot != 0) {
        return false;
    }
    /* Host never asks a client to resolve client→host intents. */
    return role != COOP_MP_PKT_HOST_RESOLVE;
}

size_t coop_lite_coop_packet_min_size(int type)
{
    switch (type) {
    case PACKET_COOP_ATTACK_INTENT:
        return sizeof(PacketCoopAttackIntent);
    case PACKET_COOP_FOLLOW_HOST:
        return sizeof(PacketCoopFollowHost);
    case PACKET_COOP_CHAR_OFFER:
        return sizeof(PacketCoopCharOffer);
    case PACKET_COOP_SESSION_SYNC:
        return sizeof(PacketCoopSessionSync);
    case PACKET_COOP_PC_POS:
        return sizeof(PacketCoopPcPos);
    case PACKET_COOP_PARTY_STATE:
        return sizeof(PacketCoopPartyState);
    case PACKET_COOP_FOLLOWERS:
    case PACKET_COOP_NPC_POS:
        return 8; /* header; entries validated against count */
    default:
        return 0;
    }
}

bool coop_lite_pc_proto_plausible(int proto)
{
    return proto > 0 && proto < COOP_LITE_MAX_PROTO;
}

void coop_lite_terminate_name(char* name, size_t size)
{
    if (name != NULL && size > 0) {
        name[size - 1] = '\0';
    }
}

bool coop_lite_client_drives_obj(int64_t obj)
{
    if (obj == OBJ_HANDLE_NULL) {
        return false;
    }
    if (!tig_net_is_active() || tig_net_is_host()) {
        return false;
    }
    if (!coop_lite_client_in_session || coop_pending_sync_valid) {
        return false;
    }
    return obj == player_get_local_pc_obj();
}

bool coop_lite_sender_owns_obj(int64_t obj)
{
    int slot;

    if (!tig_net_is_active() || !tig_net_is_host()) {
        return true;
    }
    slot = sub_52A530();
    if (slot == 0) {
        return true;
    }
#ifndef COOP_LITE_TEST_STUBS
    return obj != OBJ_HANDLE_NULL && slot > 0 && slot < 8 && sub_4A2B60(slot) == obj;
#else
    (void)obj;
    return false;
#endif
}

void coop_lite_host_on_client_disconnect(int slot)
{
    if (slot <= 0 || slot >= 8) {
        return;
    }
    coop_pending_offers[slot].valid = false;
    coop_pending_offers[slot].has_oid = false;
    coop_last_pc_pos_valid[slot] = false;
    coop_client_applied_valid[slot] = false;
    tig_debug_printf("CoopLite: slot %d disconnected — offer/replication state cleared\n", slot);
}

bool coop_lite_client_reconnect(void)
{
#ifndef COOP_LITE_TEST_STUBS
    if (!tig_net_is_coop_enabled() || !tig_net_join_configured() || tig_net_is_active()) {
        return false;
    }
    /* Quit-to-menu dropped the session: New Game must rejoin the host, not
       start a standalone campaign (PT8). */
    coop_lite_session_reset();
    tig_debug_printf("CoopLite: reconnecting to the host for New Game\n");
    if (!multiplayer_start() || !tig_net_is_active()) {
        tig_debug_printf("CoopLite: reconnect failed — New Game stays single-player\n");
        return false;
    }
    return true;
#else
    return false;
#endif
}

void coop_lite_on_game_reset(void)
{
#ifndef COOP_LITE_TEST_STUBS
    if (!tig_net_is_coop_enabled()) {
        return;
    }
    if (tig_net_is_active() && !tig_net_is_host() && coop_lite_client_in_session) {
        /*
         * Client: Quit Game tears the world down. Leave the session cleanly
         * (the host removes our PC) instead of handling host world packets
         * against freed objects (PT9: SIGSEGV in coop_lite_handle_followers
         * right after gamelib_reset). New Game reconnects.
         */
        tig_debug_printf("CoopLite: game reset — leaving the host's session\n");
        coop_lite_session_reset();
        tig_net_reset_connection();
        return;
    }
    if (coop_lite_client_in_session || tig_net_is_host()) {
        coop_lite_session_reset();
    }
#endif
}

void coop_lite_host_remove_left_pc(int slot, int64_t pc_obj)
{
#ifndef COOP_LITE_TEST_STUBS
    char oid_str[80];

    /* PT10: the slot is already inactive at DISCONNECT, so sub_4A2B60 gave
       NULL and the PC lingered (census slot=-1). Fall back to the OID we
       remembered for the slot. */
    if (slot > 0 && slot < 8) {
        int64_t remembered = coop_lite_take_slot_pc(slot);

        if (pc_obj == OBJ_HANDLE_NULL) {
            pc_obj = remembered;
        }
        coop_lite_unkeep_sector(slot);
    }
    if (pc_obj == OBJ_HANDLE_NULL || !tig_net_is_host()
        || pc_obj == player_get_local_pc_obj()
        || obj_field_int32_get(pc_obj, OBJ_F_TYPE) != OBJ_TYPE_PC) {
        return;
    }
    /* A player who left takes their PC along: a rejoin offers a fresh
       character (PT8: a stale dead PC lingered on the host). */
    objid_id_to_str(oid_str, obj_get_id(pc_obj));
    tig_debug_printf("CoopLite: removing PC %s of the player who left\n", oid_str);
    object_destroy(pc_obj);
#else
    (void)slot;
    (void)pc_obj;
#endif
}

#ifndef COOP_LITE_TEST_STUBS
static void coop_lite_copy_pc_name(int64_t obj, char* dest, size_t dest_sz)
{
    char* name = NULL;

    if (dest_sz == 0) {
        return;
    }
    dest[0] = '\0';
    if (obj == OBJ_HANDLE_NULL) {
        return;
    }
    obj_field_string_get(obj, OBJ_F_PC_PLAYER_NAME, &name);
    if (name != NULL) {
        strncpy(dest, name, dest_sz - 1);
        dest[dest_sz - 1] = '\0';
        FREE(name);
    }
}

/* proto must resolve to a PC prototype (player_obj_create_player exit()s otherwise). */
static bool coop_lite_proto_is_pc(int proto)
{
    int64_t proto_obj;

    if (!coop_lite_pc_proto_plausible(proto)) {
        return false;
    }
    if (!proto_is_valid(proto)) {
        return false;
    }
    proto_obj = sub_4685A0(proto);
    if (proto_obj == OBJ_HANDLE_NULL) {
        return false;
    }
    return obj_field_int32_get(proto_obj, OBJ_F_TYPE) == OBJ_TYPE_PC;
}

/*
 * Create a PC object with a fixed OID on the *current* map without touching
 * player_pc_obj (remote/proxy PC). Mirrors player_obj_create_player.
 */
void coop_lite_generate_start_inventory(int64_t pc_obj)
{
    if (pc_obj == OBJ_HANDLE_NULL) {
        return;
    }
    if (obj_field_int32_get(pc_obj, OBJ_F_CRITTER_INVENTORY_NUM) > 0) {
        /* Already equipped (shop map ran, or a reconnect adopted it). */
        return;
    }
    tig_net_local_suspend_begin();
    item_generate_inventory(pc_obj);
    item_wield_best_all(pc_obj, OBJ_HANDLE_NULL);
    tig_net_local_suspend_end();
    tig_debug_printf("CoopLite: starting kit generated: %d items, gold %d\n",
        obj_field_int32_get(pc_obj, OBJ_F_CRITTER_INVENTORY_NUM),
        item_gold_get(pc_obj));
}

static int64_t coop_lite_create_pc_with_oid(int proto, int64_t loc, ObjectID oid, const char* name)
{
    int64_t proto_obj;
    int64_t obj = OBJ_HANDLE_NULL;
    char oid_str[80];

    if (!coop_lite_proto_is_pc(proto)) {
        tig_debug_printf("CoopLite: proto %d is not a PC prototype — using generic PC\n", proto);
        proto = BP_GENERIC_PC;
    }
    proto_obj = obj_pool_perm_lookup(obj_get_id(sub_4685A0(proto)));
    if (proto_obj == OBJ_HANDLE_NULL) {
        tig_debug_printf("CoopLite: bad PC proto %d\n", proto);
        return OBJ_HANDLE_NULL;
    }

    if (oid.type == OID_TYPE_NULL) {
        if (!object_create(proto_obj, loc, &obj)) {
            return OBJ_HANDLE_NULL;
        }
    } else {
        if (!object_create_ex(proto_obj, loc, oid, &obj)) {
            return OBJ_HANDLE_NULL;
        }
    }

    obj_field_int64_set(obj, OBJ_F_CRITTER_TELEPORT_DEST, loc);
    obj_field_int32_set(obj, OBJ_F_CRITTER_TELEPORT_MAP, map_current_map());
    level_set(obj, 1);
    object_flags_unset(obj, OF_OFF);
    if (name != NULL && name[0] != '\0') {
        obj_field_string_set(obj, OBJ_F_PC_PLAYER_NAME, name);
    }
    /* Premade starting kit (clothes, gold, background items) — the same
       item_generate_inventory SP New Game runs (PT8: shirtless proxies,
       client gold 0). Local on each peer: sub_463E20 on a client only sent
       a request, so nothing was ever spawned there. */
    coop_lite_generate_start_inventory(obj);
    obj_field_int32_set(obj, OBJ_F_CRITTER_INVENTORY_SOURCE, 0);

    objid_id_to_str(oid_str, obj_get_id(obj));
    tig_debug_printf("CoopLite: created PC %s proto=%d at x=%d y=%d flags=0x%x\n",
        oid_str, proto,
        (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc),
        (unsigned int)obj_field_int32_get(obj, OBJ_F_FLAGS));
    return obj;
}

static int coop_lite_slot_of_obj(int64_t obj)
{
    int slot;

    for (slot = 0; slot < 8; slot++) {
        if (sub_4A2B60(slot) == obj) {
            return slot;
        }
    }
    return -1;
}

static void coop_lite_fill_pc_pos(PacketCoopPcPos* pkt, int slot, int64_t obj)
{
    ObjectID oid;

    memset(pkt, 0, sizeof(*pkt));
    pkt->type = PACKET_COOP_PC_POS;
    pkt->slot = slot;
    oid = obj_get_id(obj);
    memcpy(pkt->oid, &oid, sizeof(oid));
    pkt->loc = obj_field_int64_get(obj, OBJ_F_LOCATION);
    pkt->offset_x = obj_field_int32_get(obj, OBJ_F_OFFSET_X);
    pkt->offset_y = obj_field_int32_get(obj, OBJ_F_OFFSET_Y);
    pkt->art_id = (unsigned int)obj_field_int32_get(obj, OBJ_F_CURRENT_AID);
}

static void coop_lite_apply_pc_pos(int64_t obj, const PacketCoopPcPos* pkt)
{
    if (obj_field_int64_get(obj, OBJ_F_LOCATION) != pkt->loc
        || obj_field_int32_get(obj, OBJ_F_OFFSET_X) != pkt->offset_x
        || obj_field_int32_get(obj, OBJ_F_OFFSET_Y) != pkt->offset_y) {
        sub_43E770(obj, pkt->loc, pkt->offset_x, pkt->offset_y);
    }
    if ((unsigned int)obj_field_int32_get(obj, OBJ_F_CURRENT_AID) != pkt->art_id) {
        object_set_current_aid(obj, (tig_art_id_t)pkt->art_id);
    }
}

/* Same edge-follow rule AGUpdateAnimMove uses for the local PC. */
#ifndef COOP_LITE_TEST_STUBS
/* Client: keep our own PC inside a central dead zone while it walks. */
static void coop_lite_client_follow_own_pc(int64_t obj)
{
    int64_t x;
    int64_t y;
    int64_t sx;
    int64_t sy;

    if (obj == OBJ_HANDLE_NULL || obj != player_get_local_pc_obj()) {
        return;
    }
    location_calc_dist_from_screen_center(obj_field_int64_get(obj, OBJ_F_LOCATION), &x, &y);
    /* Sprite position = tile centre + sub-tile offset. Without the offset
       the target jumps a whole tile at a time (jerky scrolling). */
    x -= obj_field_int32_get(obj, OBJ_F_OFFSET_X);
    y -= obj_field_int32_get(obj, OBJ_F_OFFSET_Y);
    if (coop_lite_follow_needs_jump(x, y, COOP_LITE_FOLLOW_JUMP_X, COOP_LITE_FOLLOW_JUMP_Y)) {
        /* Off the visible area (teleport, FORCE snap, or a refused scroll
           left it behind while running): recentre. */
        location_origin_set(obj_field_int64_get(obj, OBJ_F_LOCATION));
        return;
    }
    if (coop_lite_follow_delta(x, y, COOP_LITE_FOLLOW_DEADZONE_X, COOP_LITE_FOLLOW_DEADZONE_Y, &sx, &sy)) {
        int64_t ox0;
        int64_t oy0;
        int64_t ox1;
        int64_t oy1;

        location_origin_get(&ox0, &oy0);
        scroll_view_by(sx, sy);
        location_origin_get(&ox1, &oy1);
        if (ox0 == ox1 && oy0 == oy1) {
            /* location_origin_scroll refuses a diagonal that would expose a
               corner outside the map: try each axis on its own (PT8 lag). */
            scroll_view_by(sx, 0);
            scroll_view_by(0, sy);
        }
    }
}
#endif

static void coop_lite_camera_follow(int64_t obj)
{
    int64_t x;
    int64_t y;

    if (obj == OBJ_HANDLE_NULL || obj != player_get_local_pc_obj()) {
        return;
    }
    location_calc_dist_from_screen_center(obj_field_int64_get(obj, OBJ_F_LOCATION), &x, &y);
    if (llabs(x) > 360 || llabs(y) > 180) {
        location_origin_set(obj_field_int64_get(obj, OBJ_F_LOCATION));
    }
}

/*
 * Log every PC object near the local PC; with destroy_unbound, remove PCs
 * that are neither the local PC nor bound to a slot (stale duplicates from
 * earlier sessions / placeholder creation).
 */
static void coop_lite_pc_census(bool destroy_unbound)
{
    ObjectList objects;
    ObjectNode* node;
    int64_t local_pc;
    int64_t stale[16];
    int num_stale = 0;
    int count = 0;
    int slot;
    int i;
    int64_t loc;
    char oid_str[80];

    local_pc = player_get_local_pc_obj();
    if (local_pc == OBJ_HANDLE_NULL) {
        return;
    }

    object_list_vicinity(local_pc, OBJ_TM_PC, &objects);
    for (node = objects.head; node != NULL; node = node->next) {
        count++;
        slot = coop_lite_slot_of_obj(node->obj);
        loc = obj_field_int64_get(node->obj, OBJ_F_LOCATION);
        objid_id_to_str(oid_str, obj_get_id(node->obj));
        tig_debug_printf("CoopLite: PC census: %s @%d,%d slot=%d%s proto=%d flags=0x%x\n",
            oid_str,
            (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc),
            slot,
            node->obj == local_pc ? " (local)" : "",
            sub_49B290(node->obj),
            (unsigned int)obj_field_int32_get(node->obj, OBJ_F_FLAGS));
        if (destroy_unbound && node->obj != local_pc && slot < 0 && num_stale < 16) {
            stale[num_stale++] = node->obj;
        }
    }
    object_list_destroy(&objects);

    for (i = 0; i < num_stale; i++) {
        objid_id_to_str(oid_str, obj_get_id(stale[i]));
        tig_debug_printf("CoopLite: destroying stale unbound PC %s\n", oid_str);
        object_destroy(stale[i]);
    }
    tig_debug_printf("CoopLite: PC census total=%d destroyed=%d\n", count, num_stale);
}

static bool coop_lite_host_spawn_client(int slot)
{
    int64_t host_pc;
    int64_t spawn_loc;
    int64_t client_obj;
    int64_t existing;
    PacketCoopSessionSync sync;
    ObjectID host_oid;
    ObjectID client_oid;
    int map;
    int owner;

    if (slot <= 0 || slot >= 8 || !coop_pending_offers[slot].valid) {
        return false;
    }
    host_pc = player_get_local_pc_obj();
    if (host_pc == OBJ_HANDLE_NULL || !coop_lite_client_in_world()) {
        return false;
    }

    map = map_current_map();
    spawn_loc = coop_lite_follow_loc_for_slot(obj_field_int64_get(host_pc, OBJ_F_LOCATION), slot);

    memset(&client_oid, 0, sizeof(client_oid));
    if (coop_pending_offers[slot].has_oid) {
        memcpy(&client_oid, coop_pending_offers[slot].oid, sizeof(client_oid));
    }

    client_obj = sub_4A2B60(slot);
    if (client_obj != OBJ_HANDLE_NULL && client_obj != host_pc
        && client_oid.type != OID_TYPE_NULL
        && !objid_is_equal(obj_get_id(client_obj), client_oid)) {
        /* A loaded save bound this slot to another character (other session
           or a re-picked PC): the player's own PC wins, the stale one goes. */
        tig_debug_printf("CoopLite: slot %d was bound to another PC — replacing it\n", slot);
        object_destroy(client_obj);
        client_obj = OBJ_HANDLE_NULL;
    }
    if (client_obj == OBJ_HANDLE_NULL && client_oid.type != OID_TYPE_NULL) {
        existing = obj_pool_perm_lookup(client_oid);
        if (existing != OBJ_HANDLE_NULL) {
            /* Only adopt a leftover PC of ours (reconnect); never another
               player's PC, the host PC or a non-PC object (OID spoofing). */
            owner = coop_lite_slot_of_obj(existing);
            if (existing == host_pc
                || (owner >= 0 && owner != slot)
                || obj_field_int32_get(existing, OBJ_F_TYPE) != OBJ_TYPE_PC) {
                tig_debug_printf("CoopLite: rejecting CHAR_OFFER slot=%d — OID belongs to another object\n", slot);
                coop_pending_offers[slot].valid = false;
                return false;
            }
            client_obj = existing;
        }
    }
    if (client_obj == OBJ_HANDLE_NULL) {
        /* Adopt the client's own OID so attack/dialog OIDs match. */
        client_obj = coop_lite_create_pc_with_oid(coop_pending_offers[slot].proto,
            spawn_loc, client_oid, coop_pending_offers[slot].name);
        if (client_obj == OBJ_HANDLE_NULL) {
            tig_debug_printf("CoopLite: host failed to spawn client PC slot=%d\n", slot);
            coop_pending_offers[slot].valid = false;
            return false;
        }
    } else {
        spawn_loc = obj_field_int64_get(client_obj, OBJ_F_LOCATION);
    }

    multiplayer_bind_slot_pc(slot, client_obj);
    multiplayer_bind_slot_pc(0, host_pc);
    if (coop_reload_raise[slot]) {
        /* Party reload: everyone comes back on their feet, and the client
           gets our vitals for its PC (Packet100). */
        coop_reload_raise[slot] = false;
        if (critter_is_dead(client_obj)) {
            coop_lite_host_raise_pc(client_obj);
        } else {
            object_hp_damage_set(client_obj, object_hp_damage_get(client_obj));
        }
    }
    coop_lite_remember_slot_pc(slot, client_obj, coop_pending_offers[slot].proto);
    coop_lite_keep_sector(slot, obj_field_int64_get(client_obj, OBJ_F_LOCATION));
    coop_last_pc_pos_valid[slot] = false;
    coop_last_pc_pos_valid[0] = false;
    coop_client_applied_valid[slot] = false;

    host_oid = obj_get_id(host_pc);
    client_oid = obj_get_id(client_obj);

    memset(&sync, 0, sizeof(sync));
    sync.type = PACKET_COOP_SESSION_SYNC;
    sync.map = map;
    sync.host_loc = obj_field_int64_get(host_pc, OBJ_F_LOCATION);
    sync.client_loc = spawn_loc;
    memcpy(sync.host_pc_oid, &host_oid, sizeof(host_oid));
    memcpy(sync.client_pc_oid, &client_oid, sizeof(client_oid));
    sync.client_slot = slot;
    sync.client_proto = coop_pending_offers[slot].proto;
    sync.host_proto = sub_49B290(host_pc);
    if (!coop_lite_proto_is_pc(sync.host_proto)) {
        sync.host_proto = BP_GENERIC_PC;
    }
    coop_lite_copy_pc_name(client_obj, sync.client_name, sizeof(sync.client_name));
    coop_lite_copy_pc_name(host_pc, sync.host_name, sizeof(sync.host_name));

    /* Per-slot: only this client may apply it. */
    tig_net_send_app(&sync, sizeof(sync), slot);
    coop_pending_offers[slot].valid = false;
    tig_debug_printf("CoopLite: SESSION_SYNC sent slot=%d map=%d client_proto=%d host_proto=%d (companion spawned on host)\n",
        slot, map, sync.client_proto, sync.host_proto);
    return true;
}

/* Client, after the map is loaded: bind PCs from a pending SESSION_SYNC. */
static void coop_lite_client_finish_sync(PacketCoopSessionSync* pkt)
{
    ObjectID host_oid;
    int64_t local_pc;
    int64_t host_pc;
    ObjectID local_oid;

    memcpy(&host_oid, pkt->host_pc_oid, sizeof(host_oid));

    local_pc = player_get_local_pc_obj();
    if (local_pc == OBJ_HANDLE_NULL) {
        tig_debug_printf("CoopLite: finish sync: no local PC\n");
        return;
    }
    local_oid = obj_get_id(local_pc);
    if (memcmp(&local_oid, pkt->client_pc_oid, sizeof(local_oid)) != 0) {
        tig_debug_printf("CoopLite: WARNING local PC OID differs from host companion OID\n");
    }
    multiplayer_bind_slot_pc(pkt->client_slot, local_pc);

    host_pc = obj_pool_perm_lookup(host_oid);
    if (host_pc != OBJ_HANDLE_NULL
        && (host_pc == local_pc || obj_field_int32_get(host_pc, OBJ_F_TYPE) != OBJ_TYPE_PC)) {
        tig_debug_printf("CoopLite: host OID collides with a non-host object — ignoring\n");
        host_pc = OBJ_HANDLE_NULL;
    } else if (host_pc == OBJ_HANDLE_NULL) {
        /* Real premade proto so the proxy matches the host's look. */
        host_pc = coop_lite_create_pc_with_oid(pkt->host_proto, pkt->host_loc, host_oid, pkt->host_name);
    }
    if (host_pc != OBJ_HANDLE_NULL) {
        multiplayer_bind_slot_pc(0, host_pc);
        coop_lite_remember_slot_pc(0, host_pc, pkt->host_proto);
        coop_lite_keep_sector(0, obj_field_int64_get(host_pc, OBJ_F_LOCATION));
    }

    coop_lite_waiting_sync = false;
    coop_lite_session_synced = true;
    coop_lite_client_in_session = true;
    coop_client_pos_valid = false;

    /* Camera on our own PC (like SP New Game). */
    location_origin_set(obj_field_int64_get(local_pc, OBJ_F_LOCATION));

    /* Stale/duplicate PCs (e.g. leftovers in the client's Save\Current map
       state or earlier placeholder proxies) — remove anything unbound. */
    coop_lite_pc_census(true);

    /* Objects the host created while we were loading. */
    mp_flush_deferred_packet70();
    tig_debug_printf("CoopLite: SESSION_SYNC applied map=%d (after map load)\n", pkt->map);
}
#endif /* !COOP_LITE_TEST_STUBS */

#ifndef COOP_LITE_TEST_STUBS
/* ------------------------------------------------------------------------
 * World replication (PT8): follower table, NPC poses, fallen client PCs.
 * --------------------------------------------------------------------- */

/* Host: follower table change detection. */
static unsigned int coop_followers_hash;
static bool coop_followers_hash_valid;
static unsigned int coop_followers_ms;
static unsigned int coop_followers_full_ms;

/* Client: NPCs we made follow a slot PC (to disband when dropped). Kept as
   OIDs: handles go stale across a game reset / sector unload (PT9 crash). */
static ObjectID coop_client_followers[COOP_FOLLOWERS_MAX];
static int coop_client_followers_cnt;

/* Host: last NPC pose sent (change detection). */
#define COOP_NPC_CACHE 256
typedef struct CoopNpcCacheEntry {
    int64_t obj;
    PacketCoopNpcPosEntry last;
    unsigned int sent_ms;
} CoopNpcCacheEntry;
static CoopNpcCacheEntry coop_npc_cache[COOP_NPC_CACHE];
static unsigned int coop_npc_pos_ms;

/* Client: last hp damage seen per NPC (damage floats). */
typedef struct CoopNpcHpEntry {
    int64_t obj;
    int hp_damage;
} CoopNpcHpEntry;
static CoopNpcHpEntry coop_npc_hp[COOP_NPC_CACHE];

/* Host: per-slot time the PC fell (0 = alive). */
/* Client: last hp damage per slot PC (damage floats) and fallen state. */
static int coop_slot_hp_damage[8];
static bool coop_slot_hp_valid[8];
static bool coop_client_fallen;

/*
 * Remote PCs must survive distance (PT9: >~17 tiles apart and the other PC
 * vanished). The sector cache holds 16 sectors and evicts the least recently
 * used unlocked one; a remote PC's sector is never drawn locally, so it was
 * the first to go, taking the PC with it. Keep each remote PC's sector
 * locked (one sector per PC, to leave the cache to the view).
 */
typedef struct CoopSectorKeep {
    bool valid;
    int map;
    int64_t id;
} CoopSectorKeep;
static CoopSectorKeep coop_sector_keep[8];

static void coop_lite_keep_sector(int slot, int64_t loc)
{
    CoopSectorKeep* k;
    int64_t id;
    int map;
    Sector* sector;

    if (slot < 0 || slot >= 8 || loc == 0 || !map_is_valid()) {
        return;
    }
    k = &coop_sector_keep[slot];
    id = sector_id_from_loc(loc);
    map = map_current_map();
    if (k->valid && k->map == map && k->id == id) {
        return;
    }
    if (!sector_lock(id, &sector)) {
        return;
    }
    if (k->valid && k->map == map) {
        sector_unlock(k->id);
    }
    k->valid = true;
    k->map = map;
    k->id = id;
}

static void coop_lite_unkeep_sector(int slot)
{
    if (coop_sector_keep[slot].valid
        && map_is_valid()
        && coop_sector_keep[slot].map == map_current_map()) {
        sector_unlock(coop_sector_keep[slot].id);
    }
    coop_sector_keep[slot].valid = false;
}

static void coop_lite_release_sectors(void)
{
    int slot;

    for (slot = 0; slot < 8; slot++) {
        if (coop_sector_keep[slot].valid
            && map_is_valid()
            && coop_sector_keep[slot].map == map_current_map()) {
            sector_unlock(coop_sector_keep[slot].id);
        }
        /* Map changed: the cache was flushed with it, nothing to unlock. */
        coop_sector_keep[slot].valid = false;
    }
}

/*
 * map_flush skips (does not save) locked sectors and map_close evicts them
 * without dropping the lock count, so hand our keep-alive locks back first;
 * the next ping re-takes them.
 */
void coop_lite_before_map_flush(void)
{
    coop_lite_release_sectors();
}

/* Remote PCs known per slot, to re-create one that still went missing. */
typedef struct CoopSlotPc {
    bool valid;
    ObjectID oid;
    int proto;
    char name[64];
} CoopSlotPc;
static CoopSlotPc coop_slot_pc[8];

/* Forget a slot's PC (player left); returns it if still loaded. */
static int64_t coop_lite_take_slot_pc(int slot)
{
    int64_t obj = OBJ_HANDLE_NULL;

    if (coop_slot_pc[slot].valid) {
        obj = obj_pool_perm_lookup(coop_slot_pc[slot].oid);
    }
    coop_slot_pc[slot].valid = false;
    return obj;
}

static void coop_lite_remember_slot_pc(int slot, int64_t obj, int proto)
{
    if (slot < 0 || slot >= 8 || obj == OBJ_HANDLE_NULL) {
        return;
    }
    coop_slot_pc[slot].valid = true;
    coop_slot_pc[slot].oid = obj_get_id(obj);
    coop_slot_pc[slot].proto = proto;
    coop_lite_copy_pc_name(obj, coop_slot_pc[slot].name, sizeof(coop_slot_pc[slot].name));
}

/* Remote PC for this slot/OID: resolve by OID; if its sector is gone, load
   the sector at the reported location; as a last resort re-create it. */
static int64_t coop_lite_resolve_remote_pc(int slot, const ObjectID* oid, int64_t loc)
{
    int64_t obj;
    char oid_str[80];

    obj = obj_pool_perm_lookup(*oid);
    if (obj != OBJ_HANDLE_NULL) {
        return obj;
    }
    if (slot < 0 || slot >= 8 || !coop_slot_pc[slot].valid
        || memcmp(&coop_slot_pc[slot].oid, oid, sizeof(*oid)) != 0) {
        return OBJ_HANDLE_NULL;
    }
    coop_lite_keep_sector(slot, loc);
    obj = obj_pool_perm_lookup(*oid);
    if (obj != OBJ_HANDLE_NULL) {
        return obj;
    }
    multiplayer_lock();
    tig_net_local_suspend_begin();
    obj = coop_lite_create_pc_with_oid(coop_slot_pc[slot].proto, loc, *oid, coop_slot_pc[slot].name);
    tig_net_local_suspend_end();
    multiplayer_unlock();
    if (obj != OBJ_HANDLE_NULL) {
        multiplayer_bind_slot_pc(slot, obj);
        objid_id_to_str(oid_str, *oid);
        tig_debug_printf("CoopLite: re-created missing remote PC %s (slot %d)\n", oid_str, slot);
    }
    return obj;
}

/*
 * Frame-time probe (PT9: host looked frozen). Every 10 s: the longest gap
 * between two pings and the most time coop replication took in one ping.
 */
static unsigned int coop_stats_last_ping_ms;
static unsigned int coop_stats_window_ms;
static unsigned int coop_stats_max_gap_ms;
static Uint64 coop_stats_max_cost_ns;

static void coop_lite_frame_stats(unsigned int now_ms, Uint64 cost_ns)
{
    unsigned int gap;

    if (coop_stats_last_ping_ms != 0) {
        gap = now_ms - coop_stats_last_ping_ms;
        if (gap > coop_stats_max_gap_ms) {
            coop_stats_max_gap_ms = gap;
        }
    }
    coop_stats_last_ping_ms = now_ms;
    if (cost_ns > coop_stats_max_cost_ns) {
        coop_stats_max_cost_ns = cost_ns;
    }
    if (coop_stats_window_ms == 0) {
        coop_stats_window_ms = now_ms;
    } else if (coop_lite_interval_elapsed(coop_stats_window_ms, now_ms, 10000u)) {
        tig_debug_printf("CoopLite: 10s frame: max gap %u ms, max coop cost %.2f ms\n",
            coop_stats_max_gap_ms, (double)coop_stats_max_cost_ns / 1000000.0);
        coop_stats_window_ms = now_ms;
        coop_stats_max_gap_ms = 0;
        coop_stats_max_cost_ns = 0;
    }
}

/* ---- Dev hooks (env-gated, coop host only) ---- */
static bool coop_dev_start_done;
static unsigned int coop_dev_start_ms;
static ObjectID coop_dev_wolf[8];
static bool coop_dev_wolf_valid[8];
static unsigned int coop_dev_wolf_ms[8];

/*
 * ARCANUM_DEV_START_LOC=x,y (or "shrouded" = Shrouded Hills, gamearea.mes
 * 90226,84176): 3 s after the host is in the world, teleport the host PC
 * there on the current map; clients follow via the usual follow packet.
 * ARCANUM_DEV_SPAWN_HOSTILE_NEAR_CLIENT=1: a wolf is spawned next to every
 * client PC (10 s after it is bound, again 45 s after the last one died).
 */
static bool coop_dev_save_request;
static bool coop_dev_save_done;
static unsigned int coop_dev_save_ms;

bool coop_lite_take_dev_save_request(void)
{
    bool r = coop_dev_save_request;

    coop_dev_save_request = false;
    return r;
}

static bool coop_dev_wolf_spawned;
static bool coop_dev_merchant_pending;
static unsigned int coop_dev_merchant_ms;

/* ARCANUM_DEV_START_LOC=merchant: host (and so the party) next to the
   nearest living merchant within 80 tiles. */
static void coop_lite_dev_go_to_merchant(void)
{
    int64_t host_pc = player_get_local_pc_obj();
    int64_t loc;
    int64_t best = OBJ_HANDLE_NULL;
    int64_t best_dist = 0;
    int64_t d;
    LocRect rect;
    ObjectList objects;
    ObjectNode* node;
    TeleportData teleport_data;
    char name[2000];

    if (host_pc == OBJ_HANDLE_NULL) {
        return;
    }
    loc = obj_field_int64_get(host_pc, OBJ_F_LOCATION);
    rect.x1 = LOCATION_GET_X(loc) - 80;
    rect.y1 = LOCATION_GET_Y(loc) - 80;
    rect.x2 = LOCATION_GET_X(loc) + 80;
    rect.y2 = LOCATION_GET_Y(loc) + 80;
    object_list_rect(&rect, OBJ_TM_NPC, &objects);
    for (node = objects.head; node != NULL; node = node->next) {
        if (critter_is_dead(node->obj) || !coop_lite_npc_is_merchant(node->obj)) {
            continue;
        }
        d = location_dist(loc, obj_field_int64_get(node->obj, OBJ_F_LOCATION));
        if (best == OBJ_HANDLE_NULL || d < best_dist) {
            best = node->obj;
            best_dist = d;
        }
    }
    object_list_destroy(&objects);
    if (best == OBJ_HANDLE_NULL) {
        tig_debug_printf("CoopLite: dev start: no merchant within 80 tiles\n");
        return;
    }
    loc = obj_field_int64_get(best, OBJ_F_LOCATION);
    object_examine(best, host_pc, name);
    memset(&teleport_data, 0, sizeof(teleport_data));
    teleport_data.obj = host_pc;
    teleport_data.loc = coop_lite_follow_loc_for_slot(loc, 1);
    teleport_data.map = map_current_map();
    tig_debug_printf("CoopLite: dev start: merchant '%s' at %d,%d (%d tiles) — moving the party there\n",
        name, (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc), (int)best_dist);
    teleport_do(&teleport_data);
}

static void coop_lite_dev_tick(unsigned int now_ms)
{
    const char* env;
    int slot;
    int64_t pc;
    int64_t wolf;
    int64_t loc;

    if (!coop_dev_start_done) {
        env = getenv("ARCANUM_DEV_START_LOC");
        if (env == NULL || env[0] == '\0') {
            coop_dev_start_done = true;
        } else if (coop_dev_start_ms == 0) {
            coop_dev_start_ms = now_ms != 0 ? now_ms : 1;
        } else if (coop_lite_interval_elapsed(coop_dev_start_ms, now_ms, 3000u)) {
            int x;
            int y;
            TeleportData teleport_data;

            coop_dev_start_done = true;
            if (!coop_lite_parse_dev_loc(env, &x, &y)) {
                tig_debug_printf("CoopLite: dev start: bad ARCANUM_DEV_START_LOC '%s'\n", env);
            } else {
                memset(&teleport_data, 0, sizeof(teleport_data));
                teleport_data.obj = player_get_local_pc_obj();
                teleport_data.loc = LOCATION_MAKE((int64_t)x, (int64_t)y);
                teleport_data.map = map_current_map();
                tig_debug_printf("CoopLite: dev start: teleport host to %d,%d\n", x, y);
                teleport_do(&teleport_data);
                if (strcmp(env, "merchant") == 0) {
                    coop_dev_merchant_pending = true;
                    coop_dev_merchant_ms = now_ms;
                }
            }
        }
    }
    if (coop_dev_merchant_pending && coop_lite_interval_elapsed(coop_dev_merchant_ms, now_ms, 2000u)) {
        coop_dev_merchant_pending = false;
        coop_lite_dev_go_to_merchant();
    }

    if (!coop_dev_save_done && sub_4A2B60(1) != OBJ_HANDLE_NULL) {
        env = getenv("ARCANUM_DEV_COOP_SAVE");
        if (env == NULL || env[0] != '1') {
            coop_dev_save_done = true;
        } else if (coop_dev_save_ms == 0) {
            coop_dev_save_ms = now_ms != 0 ? now_ms : 1;
        } else if (coop_lite_interval_elapsed(coop_dev_save_ms, now_ms, 4000u)) {
            coop_dev_save_done = true;
            coop_dev_save_request = true;
        }
    }

    env = getenv("ARCANUM_DEV_SPAWN_HOSTILE_NEAR_CLIENT");
    if (env == NULL || (env[0] != '1' && env[0] != '2')) {
        return;
    }
    for (slot = 1; slot < 8; slot++) {
        pc = sub_4A2B60(slot);
        if (pc == OBJ_HANDLE_NULL || critter_is_dead(pc)) {
            coop_dev_wolf_ms[slot] = 0;
            continue;
        }
        if (coop_dev_wolf_spawned) {
            break; /* PT10: once per session, not on a loop */
        }
        if (coop_dev_wolf_ms[slot] == 0) {
            /* First sighting of a client PC: spawn 10 s later. */
            coop_dev_wolf_ms[slot] = now_ms != 0 ? now_ms : 1;
            continue;
        }
        if (!coop_lite_interval_elapsed(coop_dev_wolf_ms[slot], now_ms, 10000u)) {
            continue;
        }
        coop_dev_wolf_spawned = true;
        loc = obj_field_int64_get(pc, OBJ_F_LOCATION);
        loc = coop_lite_follow_loc_for_slot(LOCATION_MAKE(LOCATION_GET_X(loc) + 3, LOCATION_GET_Y(loc)), 0);
        if (!mp_object_create(BP_WOLF, loc, &wolf) || wolf == OBJ_HANDLE_NULL) {
            tig_debug_printf("CoopLite: dev: wolf spawn failed\n");
            continue;
        }
        coop_dev_wolf[slot] = obj_get_id(wolf);
        coop_dev_wolf_valid[slot] = true;
        ai_attack(pc, wolf, LOUDNESS_NORMAL, 0);
        tig_debug_printf("CoopLite: dev: wolf spawned to attack slot %d PC at %d,%d\n",
            slot, (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc));
        if (env[0] == '2') {
            /* =2: a second wolf for the host too (party-wipe testing). */
            int64_t host_pc = player_get_local_pc_obj();
            int64_t wolf2;

            loc = obj_field_int64_get(host_pc, OBJ_F_LOCATION);
            loc = coop_lite_follow_loc_for_slot(LOCATION_MAKE(LOCATION_GET_X(loc) - 3, LOCATION_GET_Y(loc)), 0);
            if (mp_object_create(BP_WOLF, loc, &wolf2) && wolf2 != OBJ_HANDLE_NULL) {
                ai_attack(host_pc, wolf2, LOUDNESS_NORMAL, 0);
                tig_debug_printf("CoopLite: dev: second wolf spawned to attack the host\n");
            }
        }
    }
}

/* Client: synced into the host's world and not between maps / in the menu.
   Coop world packets (followers, NPC poses, PC poses) are dropped otherwise. */
static bool coop_lite_client_world_ready(void)
{
    return coop_lite_world_ready_rule(coop_lite_client_in_session,
        coop_pending_sync_valid,
        coop_lite_waiting_sync,
        map_is_valid(),
        map_is_clearing_objects(),
        player_get_local_pc_obj() != OBJ_HANDLE_NULL);
}

static void coop_lite_world_reset(void)
{
    int i;

    coop_followers_hash_valid = false;
    coop_followers_ms = 0;
    coop_followers_full_ms = 0;
    coop_client_followers_cnt = 0;
    memset(coop_npc_cache, 0, sizeof(coop_npc_cache));
    memset(coop_npc_hp, 0, sizeof(coop_npc_hp));
    coop_npc_pos_ms = 0;
    for (i = 0; i < 8; i++) {
        coop_slot_hp_valid[i] = false;
        coop_slot_pc[i].valid = false;
    }
    coop_client_fallen = false;
    coop_lite_release_sectors();
    memset(&coop_party_watch, 0, sizeof(coop_party_watch));
    coop_follow_carry_pending = false;
    memset(coop_dev_wolf_valid, 0, sizeof(coop_dev_wolf_valid));
    memset(coop_dev_wolf_ms, 0, sizeof(coop_dev_wolf_ms));
    coop_stats_last_ping_ms = 0;
    coop_stats_window_ms = 0;
}

/* Every bound remote PC keeps its sector (both sides). */
static void coop_lite_keep_remote_sectors(void)
{
    int slot;
    int64_t obj;
    int64_t local_pc = player_get_local_pc_obj();

    for (slot = 0; slot < 8; slot++) {
        obj = sub_4A2B60(slot);
        if (obj == OBJ_HANDLE_NULL || obj == local_pc) {
            continue;
        }
        coop_lite_keep_sector(slot, obj_field_int64_get(obj, OBJ_F_LOCATION));
    }
}

static void coop_lite_host_send_followers(unsigned int now_ms)
{
    PacketCoopFollowers pkt;
    int slot;
    int idx;
    int cnt;
    int64_t pc;
    int64_t npc;
    ObjectID oid;
    unsigned int hash = 2166136261u;
    size_t i;
    const unsigned char* bytes;

    if (!coop_lite_interval_elapsed(coop_followers_ms, now_ms, 500u)) {
        return;
    }
    coop_followers_ms = now_ms;

    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_FOLLOWERS;
    for (slot = 0; slot < 8; slot++) {
        pc = sub_4A2B60(slot);
        if (pc == OBJ_HANDLE_NULL) {
            continue;
        }
        cnt = obj_arrayfield_length_get(pc, OBJ_F_CRITTER_FOLLOWER_IDX);
        for (idx = 0; idx < cnt && pkt.count < COOP_FOLLOWERS_MAX; idx++) {
            npc = obj_arrayfield_handle_get(pc, OBJ_F_CRITTER_FOLLOWER_IDX, idx);
            if (npc == OBJ_HANDLE_NULL) {
                continue;
            }
            oid = obj_get_id(npc);
            memcpy(pkt.entries[pkt.count].npc_oid, &oid, sizeof(oid));
            pkt.entries[pkt.count].leader_slot = slot;
            pkt.count++;
        }
    }

    bytes = (const unsigned char*)&pkt;
    for (i = 0; i < coop_lite_batch_size(pkt.count, COOP_FOLLOWERS_MAX, sizeof(PacketCoopFollowerEntry)); i++) {
        hash = (hash ^ bytes[i]) * 16777619u;
    }
    /* On change, plus a full refresh every 5 s (late joiners, lost packets). */
    if (coop_followers_hash_valid && hash == coop_followers_hash
        && !coop_lite_interval_elapsed(coop_followers_full_ms, now_ms, 5000u)) {
        return;
    }
    coop_followers_hash = hash;
    coop_followers_hash_valid = true;
    coop_followers_full_ms = now_ms;
    tig_net_send_app_all(&pkt,
        (int)coop_lite_batch_size(pkt.count, COOP_FOLLOWERS_MAX, sizeof(PacketCoopFollowerEntry)));
}

void coop_lite_handle_followers(PacketCoopFollowers* pkt)
{
    ObjectID now_list[COOP_FOLLOWERS_MAX];
    int now_cnt = 0;
    int i;
    int j;
    ObjectID oid;
    int64_t npc;
    int64_t leader;

    if (pkt == NULL || pkt->type != PACKET_COOP_FOLLOWERS) {
        return;
    }
    if (!tig_net_is_active() || tig_net_is_host()) {
        return;
    }
    if (!coop_lite_batch_valid(pkt->count, COOP_FOLLOWERS_MAX,
            sizeof(PacketCoopFollowerEntry), tig_net_last_message_size())) {
        tig_debug_printf("CoopLite: FOLLOWERS bad count %d\n", pkt->count);
        return;
    }
    if (!coop_lite_client_world_ready()) {
        return;
    }

    multiplayer_lock();
    tig_net_local_suspend_begin();
    for (i = 0; i < pkt->count; i++) {
        if (pkt->entries[i].leader_slot < 0 || pkt->entries[i].leader_slot >= 8) {
            continue;
        }
        memcpy(&oid, pkt->entries[i].npc_oid, sizeof(oid));
        npc = obj_pool_perm_lookup(oid);
        leader = sub_4A2B60(pkt->entries[i].leader_slot);
        if (npc == OBJ_HANDLE_NULL || leader == OBJ_HANDLE_NULL
            || obj_field_int32_get(npc, OBJ_F_TYPE) != OBJ_TYPE_NPC) {
            continue;
        }
        now_list[now_cnt++] = oid;
        if (critter_leader_get(npc) != leader) {
            if (critter_follow(npc, leader, true)) {
                tig_debug_printf("CoopLite: follower synced (leader slot %d)\n",
                    pkt->entries[i].leader_slot);
            }
        }
    }
    /* Dropped by the host since last table. */
    for (i = 0; i < coop_client_followers_cnt; i++) {
        for (j = 0; j < now_cnt; j++) {
            if (memcmp(&now_list[j], &coop_client_followers[i], sizeof(ObjectID)) == 0) {
                break;
            }
        }
        if (j < now_cnt) {
            continue;
        }
        /* Resolve again: never touch a remembered handle. */
        npc = obj_pool_perm_lookup(coop_client_followers[i]);
        if (npc != OBJ_HANDLE_NULL
            && obj_field_int32_get(npc, OBJ_F_TYPE) == OBJ_TYPE_NPC
            && critter_leader_get(npc) != OBJ_HANDLE_NULL) {
            critter_disband(npc, true);
        }
    }
    tig_net_local_suspend_end();
    multiplayer_unlock();

    memcpy(coop_client_followers, now_list, sizeof(ObjectID) * (size_t)now_cnt);
    coop_client_followers_cnt = now_cnt;
}

static bool coop_lite_obj_is_slot_pc(int64_t obj)
{
    return coop_lite_slot_of_obj(obj) >= 0;
}

static void coop_lite_fill_npc_entry(PacketCoopNpcPosEntry* e, int64_t obj)
{
    ObjectID oid;
    unsigned int flags;

    memset(e, 0, sizeof(*e));
    oid = obj_get_id(obj);
    memcpy(e->oid, &oid, sizeof(oid));
    e->loc = obj_field_int64_get(obj, OBJ_F_LOCATION);
    e->offset_x = obj_field_int32_get(obj, OBJ_F_OFFSET_X);
    e->offset_y = obj_field_int32_get(obj, OBJ_F_OFFSET_Y);
    e->art_id = (unsigned int)obj_field_int32_get(obj, OBJ_F_CURRENT_AID);
    flags = obj_field_int32_get(obj, OBJ_F_FLAGS);
    if (critter_is_dead(obj) || (flags & OF_FLAT) != 0) {
        e->flags |= COOP_NPC_POS_DEAD;
    }
    e->hp_damage = object_hp_damage_get(obj);
}

static CoopNpcCacheEntry* coop_lite_npc_cache_slot(int64_t obj)
{
    unsigned int h = (unsigned int)((uint64_t)obj * 2654435761u) % COOP_NPC_CACHE;
    unsigned int i;

    for (i = 0; i < COOP_NPC_CACHE; i++) {
        CoopNpcCacheEntry* c = &coop_npc_cache[(h + i) % COOP_NPC_CACHE];
        if (c->obj == obj || c->obj == OBJ_HANDLE_NULL) {
            return c;
        }
    }
    /* Full: reuse the home slot. */
    return &coop_npc_cache[h];
}

/* Host: NPCs around every PC — position, art (animation frame) and death. */
static void coop_lite_host_send_npc_pos(unsigned int now_ms)
{
    PacketCoopNpcPos pkt;
    ObjectList objects;
    ObjectNode* node;
    int slot;
    int64_t pc;
    PacketCoopNpcPosEntry e;
    CoopNpcCacheEntry* c;
    int64_t done[COOP_NPC_CACHE];
    int done_cnt = 0;
    int k;
    bool any_client = false;

    if (!coop_lite_interval_elapsed(coop_npc_pos_ms, now_ms, 100u)) {
        return;
    }
    coop_npc_pos_ms = now_ms;

    for (slot = 1; slot < 8; slot++) {
        if (sub_4A2B60(slot) != OBJ_HANDLE_NULL) {
            any_client = true;
        }
    }
    if (!any_client) {
        return;
    }

    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_NPC_POS;
    for (slot = 0; slot < 8; slot++) {
        pc = sub_4A2B60(slot);
        if (pc == OBJ_HANDLE_NULL) {
            continue;
        }
        object_list_vicinity(pc, OBJ_TM_NPC, &objects);
        for (node = objects.head; node != NULL; node = node->next) {
            for (k = 0; k < done_cnt; k++) {
                if (done[k] == node->obj) {
                    break;
                }
            }
            if (k < done_cnt) {
                continue;
            }
            if (done_cnt < COOP_NPC_CACHE) {
                done[done_cnt++] = node->obj;
            }
            coop_lite_fill_npc_entry(&e, node->obj);
            c = coop_lite_npc_cache_slot(node->obj);
            if (c->obj == node->obj
                && memcmp(&c->last, &e, sizeof(e)) == 0
                && !coop_lite_interval_elapsed(c->sent_ms, now_ms, 2000u)) {
                continue;
            }
            c->obj = node->obj;
            c->last = e;
            c->sent_ms = now_ms;
            pkt.entries[pkt.count++] = e;
            if (pkt.count == COOP_NPC_POS_MAX) {
                tig_net_send_app_all(&pkt, (int)coop_lite_batch_size(pkt.count,
                    COOP_NPC_POS_MAX, sizeof(PacketCoopNpcPosEntry)));
                pkt.count = 0;
            }
        }
        object_list_destroy(&objects);
    }
    if (pkt.count > 0) {
        tig_net_send_app_all(&pkt, (int)coop_lite_batch_size(pkt.count,
            COOP_NPC_POS_MAX, sizeof(PacketCoopNpcPosEntry)));
    }
}

static void coop_lite_float_damage(int64_t obj, int delta)
{
    char str[32];

    if (delta <= 0) {
        return;
    }
    snprintf(str, sizeof(str), "-%d", delta);
    tf_add(obj, TF_TYPE_RED, str);
}

static CoopNpcHpEntry* coop_lite_npc_hp_slot(int64_t obj)
{
    unsigned int h = (unsigned int)((uint64_t)obj * 2654435761u) % COOP_NPC_CACHE;
    unsigned int i;

    for (i = 0; i < COOP_NPC_CACHE; i++) {
        CoopNpcHpEntry* c = &coop_npc_hp[(h + i) % COOP_NPC_CACHE];
        if (c->obj == obj || c->obj == OBJ_HANDLE_NULL) {
            return c;
        }
    }
    return &coop_npc_hp[h];
}

void coop_lite_handle_npc_pos(PacketCoopNpcPos* pkt)
{
    int i;
    ObjectID oid;
    int64_t obj;
    PacketCoopNpcPosEntry* e;
    unsigned int flags;
    CoopNpcHpEntry* hp;

    if (pkt == NULL || pkt->type != PACKET_COOP_NPC_POS) {
        return;
    }
    if (!tig_net_is_active() || tig_net_is_host()) {
        return;
    }
    if (!coop_lite_batch_valid(pkt->count, COOP_NPC_POS_MAX,
            sizeof(PacketCoopNpcPosEntry), tig_net_last_message_size())) {
        tig_debug_printf("CoopLite: NPC_POS bad count %d\n", pkt->count);
        return;
    }
    if (!coop_lite_client_world_ready()) {
        return;
    }

    multiplayer_lock();
    for (i = 0; i < pkt->count; i++) {
        e = &pkt->entries[i];
        memcpy(&oid, e->oid, sizeof(oid));
        obj = obj_pool_perm_lookup(oid);
        if (obj == OBJ_HANDLE_NULL
            || obj_field_int32_get(obj, OBJ_F_TYPE) != OBJ_TYPE_NPC
            || coop_lite_obj_is_slot_pc(obj)) {
            continue;
        }
        if (obj_field_int64_get(obj, OBJ_F_LOCATION) != e->loc
            || obj_field_int32_get(obj, OBJ_F_OFFSET_X) != e->offset_x
            || obj_field_int32_get(obj, OBJ_F_OFFSET_Y) != e->offset_y) {
            sub_43E770(obj, e->loc, e->offset_x, e->offset_y);
        }
        if ((unsigned int)obj_field_int32_get(obj, OBJ_F_CURRENT_AID) != e->art_id) {
            object_set_current_aid(obj, (tig_art_id_t)e->art_id);
        }
        flags = obj_field_int32_get(obj, OBJ_F_FLAGS);
        if ((e->flags & COOP_NPC_POS_DEAD) != 0) {
            if ((flags & (OF_FLAT | OF_NO_BLOCK)) != (OF_FLAT | OF_NO_BLOCK)) {
                object_flags_set(obj, OF_FLAT | OF_NO_BLOCK);
            }
        } else if ((flags & OF_FLAT) != 0) {
            object_flags_unset(obj, OF_FLAT | OF_NO_BLOCK);
        }
        hp = coop_lite_npc_hp_slot(obj);
        if (hp->obj == obj && e->hp_damage > hp->hp_damage) {
            coop_lite_float_damage(obj, e->hp_damage - hp->hp_damage);
        }
        hp->obj = obj;
        hp->hp_damage = e->hp_damage;
    }
    multiplayer_unlock();
}

/* ---- Coop death (PT10): corpses, host down, party wipe reload ---- */

static void coop_lite_show_notice(const char* str, int tf_type)
{
    int64_t pc = player_get_local_pc_obj();
    UiMessage ui_message;

    if (str == NULL) {
        return;
    }
    ui_message.type = UI_MSG_TYPE_FEEDBACK;
    ui_message.str = (char*)str;
    ui_display_msg(&ui_message);
    if (pc != OBJ_HANDLE_NULL) {
        tf_add(pc, tf_type, str);
    }
    tig_debug_printf("CoopLite: notice: %s\n", str);
}

static void coop_lite_party_notice_text(int kind, unsigned int countdown_ms, bool local_is_host,
    char* buf, size_t size)
{
    switch (kind) {
    case COOP_PARTY_HOST_DOWN:
        snprintf(buf, size, "%s", local_is_host
                ? "You have fallen. Your companions can resurrect you."
                : "The party leader has fallen! Resurrect them to continue.");
        break;
    case COOP_PARTY_HOST_UP:
        snprintf(buf, size, "%s", local_is_host ? "You have been resurrected." : "The party leader is back on their feet.");
        break;
    case COOP_PARTY_WIPE:
        snprintf(buf, size, "The whole party has fallen. Reloading the last save in %u seconds...",
            (countdown_ms + 999u) / 1000u);
        break;
    case COOP_PARTY_WIPE_CANCEL:
        snprintf(buf, size, "%s", "Someone got back up - reload cancelled.");
        break;
    case COOP_PARTY_RELOADING:
        snprintf(buf, size, "%s", "Reloading the party leader's last save...");
        break;
    case COOP_PARTY_RESYNC:
        snprintf(buf, size, "%s", "The party leader reloaded. Rejoining...");
        break;
    case COOP_PARTY_NO_SAVE:
        snprintf(buf, size, "%s", "No save to reload - the party gets back up.");
        break;
    default:
        buf[0] = '\0';
        break;
    }
}

/* Host: tell every client, and show it here too. */
static void coop_lite_host_party_notice(int kind, int slot, unsigned int countdown_ms)
{
    PacketCoopPartyState pkt;
    char str[160];

    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_PARTY_STATE;
    pkt.kind = kind;
    pkt.slot = slot;
    pkt.countdown_ms = countdown_ms;
    tig_net_send_app_all(&pkt, sizeof(pkt));

    coop_lite_party_notice_text(kind, countdown_ms, true, str, sizeof(str));
    if (str[0] != '\0') {
        coop_lite_show_notice(str, kind == COOP_PARTY_HOST_UP ? TF_TYPE_GREEN : TF_TYPE_RED);
    }
}

/* Corpse pose (local only; peers get the pose from PC_POS / vitals). */
static void coop_lite_set_corpse_pose(int64_t pc, bool dead)
{
    tig_art_id_t art_id;
    TigArtAnimData art_anim_data;

    art_id = obj_field_int32_get(pc, OBJ_F_CURRENT_AID);
    if (dead) {
        art_id = tig_art_id_anim_set(art_id, TIG_ART_ANIM_FALL_DOWN);
        if (tig_art_anim_data(art_id, &art_anim_data) == TIG_OK) {
            art_id = tig_art_id_frame_set(art_id, art_anim_data.num_frames - 1);
        }
        object_flags_set(pc, OF_FLAT | OF_NO_BLOCK);
    } else {
        art_id = tig_art_id_anim_set(art_id, TIG_ART_ANIM_STAND);
        art_id = tig_art_id_frame_set(art_id, 0);
        object_flags_unset(pc, OF_FLAT | OF_NO_BLOCK);
    }
    object_set_current_aid(pc, art_id);
}

/* Host: bring a PC back with full health (no-save fallback, post-reload). */
static void coop_lite_host_raise_pc(int64_t pc)
{
    sub_424070(pc, PRIORITY_HIGHEST, false, true);
    if (combat_critter_is_combat_mode_active(pc)) {
        combat_critter_deactivate_combat_mode(pc);
    }
    critter_decay_timeevent_cancel(pc);
    coop_lite_set_corpse_pose(pc, false);
    critter_fatigue_damage_set(pc, 0);
    object_hp_damage_set(pc, 0); /* broadcasts vitals (Packet100) */
}

bool coop_lite_take_reload_request(char* name, size_t size)
{
    if (!coop_reload_request_valid) {
        return false;
    }
    coop_reload_request_valid = false;
    snprintf(name, size, "%s", coop_reload_request_name);
    return true;
}

void coop_lite_reload_failed(void)
{
    int slot;
    int64_t pc;

    tig_debug_printf("CoopLite: reload of '%s' failed — raising the party instead\n",
        coop_reload_request_name);
    coop_reload_resync_pending = false;
    memset(&coop_party_watch, 0, sizeof(coop_party_watch));
    for (slot = 0; slot < 8; slot++) {
        pc = sub_4A2B60(slot);
        if (pc != OBJ_HANDLE_NULL && critter_is_dead(pc)) {
            coop_lite_host_raise_pc(pc);
        }
    }
    coop_lite_host_party_notice(COOP_PARTY_NO_SAVE, -1, 0);
}

static void coop_lite_host_start_reload(void)
{
    const char* name = gamelib_last_save_name();
    int slot;

    if (name == NULL || name[0] == '\0') {
        snprintf(coop_reload_request_name, sizeof(coop_reload_request_name), "%s", "(none)");
        coop_lite_reload_failed();
        return;
    }
    snprintf(coop_reload_request_name, sizeof(coop_reload_request_name), "%s", name);
    coop_reload_request_valid = true;
    coop_reload_resync_pending = true;
    for (slot = 1; slot < 8; slot++) {
        coop_reload_raise[slot] = tig_net_client_is_active(slot);
    }
    coop_lite_host_party_notice(COOP_PARTY_RELOADING, -1, 0);
    tig_debug_printf("CoopLite: party reload of last save '%s' requested\n", coop_reload_request_name);
}

/* Host, every ping in the world: deaths, host down, wipe, reload. */
static void coop_lite_host_watch_party(unsigned int now_ms)
{
    bool present[8];
    bool dead[8];
    unsigned int fell;
    unsigned int rose;
    unsigned int acts;
    int slot;
    int64_t pc;
    char name[64];
    char str[160];

    for (slot = 0; slot < 8; slot++) {
        pc = sub_4A2B60(slot);
        if (slot == 0 && pc == OBJ_HANDLE_NULL) {
            pc = player_get_local_pc_obj();
        }
        present[slot] = pc != OBJ_HANDLE_NULL;
        dead[slot] = present[slot] && critter_is_dead(pc);
    }

    acts = coop_lite_party_step(&coop_party_watch, present, dead, map_current_map(), now_ms, &fell, &rose);

    for (slot = 1; slot < 8; slot++) {
        pc = sub_4A2B60(slot);
        if (pc == OBJ_HANDLE_NULL) {
            continue;
        }
        if ((fell & (1u << slot)) != 0) {
            coop_lite_copy_pc_name(pc, name, sizeof(name));
            snprintf(str, sizeof(str), "%s has fallen.", name);
            tf_add(pc, TF_TYPE_RED, str);
            tig_debug_printf("CoopLite: slot %d PC fell — corpse until resurrected\n", slot);
        } else if ((rose & (1u << slot)) != 0) {
            tig_debug_printf("CoopLite: slot %d PC resurrected\n", slot);
        }
    }

    if ((acts & COOP_PARTY_ACT_HOST_DOWN) != 0) {
        tig_debug_printf("CoopLite: host PC fell\n");
        coop_lite_host_party_notice(COOP_PARTY_HOST_DOWN, 0, 0);
    }
    if ((acts & COOP_PARTY_ACT_HOST_UP) != 0) {
        tig_debug_printf("CoopLite: host PC resurrected\n");
        coop_lite_host_party_notice(COOP_PARTY_HOST_UP, 0, 0);
    }
    if ((acts & COOP_PARTY_ACT_WIPE_COUNTDOWN) != 0) {
        tig_debug_printf("CoopLite: party wiped — reload in %u ms\n", COOP_LITE_WIPE_GRACE_MS);
        coop_lite_host_party_notice(COOP_PARTY_WIPE, -1, COOP_LITE_WIPE_GRACE_MS);
    }
    if ((acts & COOP_PARTY_ACT_WIPE_CANCEL) != 0) {
        coop_lite_host_party_notice(COOP_PARTY_WIPE_CANCEL, -1, 0);
    }
    if ((acts & COOP_PARTY_ACT_RELOAD) != 0) {
        coop_lite_host_start_reload();
    }
}

/* Host, once in the world after a party reload: clients re-offer their PC. */
static void coop_lite_host_resync_after_reload(void)
{
    PacketCoopPartyState pkt;

    if (!coop_reload_resync_pending) {
        return;
    }
    coop_reload_resync_pending = false;
    coop_lite_follow_armed = true; /* the load had no spawn teleport to arm it */
    multiplayer_bind_slot_pc(0, player_get_local_pc_obj());
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_PARTY_STATE;
    pkt.kind = COOP_PARTY_RESYNC;
    pkt.slot = -1;
    tig_net_send_app_all(&pkt, sizeof(pkt));
    tig_debug_printf("CoopLite: reload done — asking clients to resync\n");
}

/* Camera: our PC, or (dead) the first living party member — spectate. */
static int64_t coop_lite_camera_target(void)
{
    int64_t local_pc = player_get_local_pc_obj();
    int64_t pc;
    int slot;

    if (local_pc == OBJ_HANDLE_NULL || !critter_is_dead(local_pc)) {
        return local_pc;
    }
    for (slot = 0; slot < 8; slot++) {
        pc = sub_4A2B60(slot);
        if (pc != OBJ_HANDLE_NULL && pc != local_pc && !critter_is_dead(pc)) {
            return pc;
        }
    }
    return local_pc;
}

int coop_lite_party_carry_list(int64_t mover, int64_t* objs, int max)
{
    int n = 0;
    int slot;
    int mover_slot;
    int64_t pc;

    if (!tig_net_is_coop_enabled() || !tig_net_is_active() || objs == NULL) {
        return 0;
    }
    if (mover == OBJ_HANDLE_NULL || mover != player_get_local_pc_obj()) {
        return 0;
    }
    if (tig_net_is_host()) {
        if (!coop_lite_follow_armed) {
            return 0; /* spawn / load placement, nobody to carry yet */
        }
        mover_slot = 0;
    } else {
        if (!coop_follow_carry_pending) {
            return 0; /* only the host's travel moves the party */
        }
        mover_slot = sub_529520();
    }
    for (slot = 0; slot < 8 && n < max; slot++) {
        pc = sub_4A2B60(slot);
        if (pc == mover) {
            continue;
        }
        if (coop_lite_should_carry_slot(slot, mover_slot, pc != OBJ_HANDLE_NULL)) {
            objs[n++] = pc;
        }
    }
    return n;
}

void coop_lite_after_party_carry(int64_t mover, int64_t dest_loc)
{
    int slot;
    int64_t pc;
    int64_t host_loc;
    int64_t loc;

    if (!tig_net_is_coop_enabled() || !tig_net_is_active()) {
        return;
    }
    if (mover == OBJ_HANDLE_NULL || mover != player_get_local_pc_obj()) {
        return;
    }
    if (tig_net_is_host()) {
        host_loc = dest_loc;
    } else {
        if (!coop_follow_carry_pending) {
            return;
        }
        coop_follow_carry_pending = false;
        host_loc = coop_follow_host_loc;
    }
    for (slot = 0; slot < 8; slot++) {
        pc = sub_4A2B60(slot);
        if (pc == OBJ_HANDLE_NULL || pc == mover) {
            continue;
        }
        loc = slot == 0 ? host_loc : coop_lite_follow_loc_for_slot(host_loc, slot);
        multiplayer_lock();
        sub_43E770(pc, loc, 0, 0);
        if (critter_is_dead(pc)) {
            coop_lite_set_corpse_pose(pc, true);
        }
        multiplayer_unlock();
        coop_lite_keep_sector(slot, loc);
        tig_debug_printf("CoopLite: carried slot %d PC%s to %d,%d\n", slot,
            critter_is_dead(pc) ? " (corpse)" : "",
            (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc));
    }
}

/* Client, on COOP_PARTY_RESYNC: the host reloaded — re-offer our PC. */
static void coop_lite_client_resync(void)
{
    int64_t pc = player_get_local_pc_obj();
    char name[64];
    int proto;

    if (pc == OBJ_HANDLE_NULL) {
        return;
    }
    coop_lite_session_reset();
    if (critter_is_dead(pc)) {
        /* The host brings every PC back after a reload; match it locally. */
        multiplayer_lock();
        tig_net_local_suspend_begin();
        critter_decay_timeevent_cancel(pc);
        critter_fatigue_damage_set(pc, 0);
        object_hp_damage_set(pc, 0);
        coop_lite_set_corpse_pose(pc, false);
        tig_net_local_suspend_end();
        multiplayer_unlock();
    }
    proto = sub_49B290(pc);
    if (!coop_lite_proto_is_pc(proto)) {
        proto = BP_GENERIC_PC;
    }
    coop_lite_copy_pc_name(pc, name, sizeof(name));
    coop_lite_show_notice("The party leader reloaded. Rejoining...", TF_TYPE_WHITE);
    coop_lite_client_offer_character(proto, name);
}

void coop_lite_handle_party_state(PacketCoopPartyState* pkt)
{
    char str[160];

    if (pkt == NULL || pkt->type != PACKET_COOP_PARTY_STATE) {
        return;
    }
    if (!tig_net_is_active() || tig_net_is_host()) {
        return;
    }
    if (pkt->kind == COOP_PARTY_RESYNC) {
        tig_debug_printf("CoopLite: host reloaded its save — resyncing\n");
        coop_lite_client_resync();
        return;
    }
    if (!coop_lite_client_world_ready()) {
        return;
    }
    coop_lite_party_notice_text(pkt->kind, pkt->countdown_ms, false, str, sizeof(str));
    if (str[0] != '\0') {
        coop_lite_show_notice(str, pkt->kind == COOP_PARTY_HOST_UP ? TF_TYPE_GREEN : TF_TYPE_RED);
    }
}

/* Client: damage floats for slot PCs, local fallen/revived handling. */
static void coop_lite_client_watch_vitals(void)
{
    int slot;
    int64_t pc;
    int dmg;
    int64_t local_pc = player_get_local_pc_obj();
    tig_art_id_t art_id;
    TigArtAnimData art_anim_data;

    for (slot = 0; slot < 8; slot++) {
        pc = sub_4A2B60(slot);
        if (pc == OBJ_HANDLE_NULL) {
            coop_slot_hp_valid[slot] = false;
            continue;
        }
        dmg = object_hp_damage_get(pc);
        if (coop_slot_hp_valid[slot] && dmg > coop_slot_hp_damage[slot]) {
            coop_lite_float_damage(pc, dmg - coop_slot_hp_damage[slot]);
        }
        coop_slot_hp_damage[slot] = dmg;
        coop_slot_hp_valid[slot] = true;
    }

    if (local_pc == OBJ_HANDLE_NULL) {
        return;
    }
    if (critter_is_dead(local_pc) && !coop_client_fallen) {
        coop_client_fallen = true;
        multiplayer_lock();
        tig_net_local_suspend_begin();
        sub_424070(local_pc, PRIORITY_HIGHEST, false, true);
        art_id = obj_field_int32_get(local_pc, OBJ_F_CURRENT_AID);
        art_id = tig_art_id_anim_set(art_id, TIG_ART_ANIM_FALL_DOWN);
        if (tig_art_anim_data(art_id, &art_anim_data) == TIG_OK) {
            art_id = tig_art_id_frame_set(art_id, art_anim_data.num_frames - 1);
        }
        object_set_current_aid(local_pc, art_id);
        object_flags_set(local_pc, OF_FLAT | OF_NO_BLOCK);
        tig_net_local_suspend_end();
        multiplayer_unlock();
        coop_lite_show_notice("You have fallen. A companion can resurrect you.", TF_TYPE_RED);
        tig_debug_printf("CoopLite: local PC fell\n");
    } else if (!critter_is_dead(local_pc) && coop_client_fallen) {
        coop_client_fallen = false;
        multiplayer_lock();
        tig_net_local_suspend_begin();
        object_flags_unset(local_pc, OF_FLAT | OF_NO_BLOCK);
        art_id = obj_field_int32_get(local_pc, OBJ_F_CURRENT_AID);
        art_id = tig_art_id_anim_set(art_id, TIG_ART_ANIM_STAND);
        art_id = tig_art_id_frame_set(art_id, 0);
        object_set_current_aid(local_pc, art_id);
        tig_net_local_suspend_end();
        multiplayer_unlock();
        coop_lite_show_notice("You have been resurrected.", TF_TYPE_GREEN);
        tig_debug_printf("CoopLite: local PC revived\n");
    }
}
#endif /* !COOP_LITE_TEST_STUBS */

void coop_lite_host_on_entered_world(void)
{
    int slot;

    if (!tig_net_is_active() || !tig_net_is_host()) {
        return;
    }
#ifndef COOP_LITE_TEST_STUBS
    {
        int64_t host_pc = player_get_local_pc_obj();
        if (host_pc != OBJ_HANDLE_NULL && coop_lite_client_in_world()) {
            multiplayer_bind_slot_pc(0, host_pc);
        }
    }
    for (slot = 1; slot < 8; slot++) {
        if (coop_pending_offers[slot].valid && tig_net_client_is_active(slot)) {
            coop_lite_host_spawn_client(slot);
        }
    }
#else
    (void)slot;
#endif
}

void coop_lite_handle_char_offer(PacketCoopCharOffer* pkt)
{
    int slot;

    if (pkt == NULL || pkt->type != PACKET_COOP_CHAR_OFFER) {
        return;
    }
    if (!tig_net_is_active() || !tig_net_is_host()) {
        return;
    }
    /* Authenticated sender slot, never the payload. */
    slot = sub_52A530();
    if (slot <= 0 || slot >= 8) {
        tig_debug_printf("CoopLite: CHAR_OFFER from bad sender slot=%d\n", slot);
        return;
    }
    if (pkt->slot != slot) {
        tig_debug_printf("CoopLite: CHAR_OFFER payload slot=%d != sender %d (using sender)\n", pkt->slot, slot);
    }
    if (!coop_lite_pc_proto_plausible(pkt->basic_prototype)) {
        tig_debug_printf("CoopLite: CHAR_OFFER rejected — bad proto %d\n", pkt->basic_prototype);
        return;
    }
#ifndef COOP_LITE_TEST_STUBS
    if (!coop_lite_proto_is_pc(pkt->basic_prototype)) {
        tig_debug_printf("CoopLite: CHAR_OFFER rejected — proto %d is not a PC\n", pkt->basic_prototype);
        return;
    }
#endif
    coop_pending_offers[slot].valid = true;
    coop_pending_offers[slot].proto = pkt->basic_prototype;
    memcpy(coop_pending_offers[slot].name, pkt->name, sizeof(coop_pending_offers[slot].name));
    coop_lite_terminate_name(coop_pending_offers[slot].name, sizeof(coop_pending_offers[slot].name));
    coop_pending_offers[slot].has_oid = pkt->has_oid != 0;
    memcpy(coop_pending_offers[slot].oid, pkt->pc_oid, sizeof(coop_pending_offers[slot].oid));
    tig_debug_printf("CoopLite: CHAR_OFFER from slot=%d proto=%d name=%s has_oid=%d\n",
        slot, pkt->basic_prototype, coop_pending_offers[slot].name, pkt->has_oid);

#ifndef COOP_LITE_TEST_STUBS
    if (coop_lite_client_in_world()) {
        coop_lite_host_spawn_client(slot);
    } else {
        tig_debug_printf("CoopLite: offer queued until host enters world\n");
    }
#endif
}

bool coop_lite_test_pending_offer(int slot)
{
    return slot > 0 && slot < 8 && coop_pending_offers[slot].valid;
}

static bool coop_lite_client_request_sync_teleport(void)
{
    TeleportData teleport_data;
    bool ok;

    memset(&teleport_data, 0, sizeof(teleport_data));
    teleport_data.flags = TELEPORT_RENDER_LOCK;
    teleport_data.obj = player_get_local_pc_obj();
    teleport_data.loc = coop_pending_sync.client_loc;
    teleport_data.map = coop_pending_sync.map;
    coop_lite_applying_follow = true;
    ok = teleport_do(&teleport_data);
    coop_lite_applying_follow = false;
    coop_pending_sync_teleport_queued = ok;
    if (!ok) {
        tig_debug_printf("CoopLite: sync teleport busy — will retry\n");
    }
    return ok;
}

void coop_lite_handle_session_sync(PacketCoopSessionSync* pkt)
{
    if (pkt == NULL || pkt->type != PACKET_COOP_SESSION_SYNC) {
        return;
    }
    if (!tig_net_is_active() || tig_net_is_host()) {
        return;
    }
    /* Host sends per slot; anything for another slot is not ours. */
    if (pkt->client_slot != sub_529520()) {
        tig_debug_printf("CoopLite: SESSION_SYNC for slot %d dropped (we are %d)\n",
            pkt->client_slot, sub_529520());
        return;
    }
    if (pkt->map <= 0) {
        tig_debug_printf("CoopLite: SESSION_SYNC dropped — bad map %d\n", pkt->map);
        return;
    }
    coop_lite_terminate_name(pkt->client_name, sizeof(pkt->client_name));
    coop_lite_terminate_name(pkt->host_name, sizeof(pkt->host_name));
    if (!coop_lite_pc_proto_plausible(pkt->host_proto)) {
        pkt->host_proto = 0; /* create helper falls back to generic PC */
    }

    tig_debug_printf("CoopLite: SESSION_SYNC recv map=%d slot=%d — loading host map first\n",
        pkt->map, pkt->client_slot);

    /*
     * Order matters: teleport our existing local PC (like SP New Game) so the
     * host map loads first; PCs are bound/created in
     * coop_lite_client_on_teleport_done once the map is current.
     */
    coop_pending_sync = *pkt;
    coop_pending_sync_valid = true;
    /* A queued follow from before sync is superseded. */
    coop_lite_pending_follow_valid = false;

#ifndef COOP_LITE_TEST_STUBS
    /*
     * The client never runs its own New Game reset before loading the host
     * map, and a killed client leaves Save\Current behind. Its stale copy of
     * the host map (old proxies / old local PCs = the "bare-torso" extras)
     * would be loaded instead of the pristine map. Drop it first.
     */
    if (map_current_map() != pkt->map) {
        char* map_name = NULL;
        char path[TIG_MAX_PATH];

        if (map_get_name(pkt->map, &map_name) && map_name != NULL) {
            snprintf(path, sizeof(path), "Save\\Current\\maps\\%s", map_name);
            if (tig_file_is_directory(path)) {
                tig_debug_printf("CoopLite: clearing stale client map state %s\n", path);
                tig_file_empty_directory(path);
            }
        }
    }
#endif

    coop_lite_client_request_sync_teleport();
    /* Binding happens in coop_lite_client_on_teleport_done (teleport.c). */
}

void coop_lite_client_on_teleport_done(void)
{
    PacketCoopSessionSync pkt;

    if (!coop_pending_sync_valid) {
        return;
    }
    if (!tig_net_is_active() || tig_net_is_host()) {
        coop_pending_sync_valid = false;
        return;
    }
#ifndef COOP_LITE_TEST_STUBS
    if (map_current_map() != coop_pending_sync.map) {
        /* An earlier teleport finished (e.g. ShopMap start); re-queue ours
           from coop_lite_ping. */
        coop_pending_sync_teleport_queued = false;
        return;
    }
#endif
    pkt = coop_pending_sync;
    coop_pending_sync_valid = false;
#ifndef COOP_LITE_TEST_STUBS
    coop_lite_client_finish_sync(&pkt);
#else
    (void)pkt;
    coop_lite_waiting_sync = false;
    coop_lite_session_synced = true;
    coop_lite_client_in_session = true;
#endif
}

void coop_lite_ping(unsigned int now_ms)
{
#ifndef COOP_LITE_TEST_STUBS
    int slot;
    int64_t obj;
    PacketCoopPcPos pkt;

    if (!tig_net_is_active()) {
        return;
    }

    if (!tig_net_is_host()) {
        /* Client: retry a SESSION_SYNC teleport that could not be queued. */
        if (coop_pending_sync_valid && !coop_pending_sync_teleport_queued
            && coop_lite_interval_elapsed(coop_pending_sync_retry_ms, now_ms, 250u)) {
            coop_pending_sync_retry_ms = now_ms;
            coop_lite_client_request_sync_teleport();
        }

        if (!coop_lite_client_world_ready()) {
            return;
        }

        /* Client camera: every frame (not the 50 ms pose tick) so the
           scroll follows the PC smoothly. */
        coop_lite_client_follow_own_pc(coop_lite_camera_target());
        coop_lite_client_watch_vitals();
        coop_lite_keep_remote_sectors();
        coop_lite_frame_stats(now_ms, 0);

        /* Client: we drive our own PC — report its pose to the host. */
        if (coop_lite_interval_elapsed(coop_last_pc_pos_ms, now_ms, COOP_LITE_PC_POS_INTERVAL_MS)) {
            coop_last_pc_pos_ms = now_ms;
            obj = player_get_local_pc_obj();
            if (obj != OBJ_HANDLE_NULL && !critter_is_dead(obj)) {
                coop_lite_fill_pc_pos(&pkt, sub_529520(), obj);
                if (!coop_client_pos_valid || coop_lite_pc_pos_changed(&coop_client_pos, &pkt)) {
                    coop_client_pos = pkt;
                    coop_client_pos_valid = true;
                    tig_net_send_app_all(&pkt, sizeof(pkt));
                }
            }
        }

        if (coop_lite_client_in_session
            && coop_lite_interval_elapsed(coop_last_census_ms, now_ms, 30000u)) {
            coop_last_census_ms = now_ms;
            coop_lite_pc_census(false);
        }
        return;
    }

    /* Host: nothing to replicate from the menu / while a map loads. */
    if (!coop_lite_client_in_world() || !map_is_valid() || map_is_clearing_objects()) {
        return;
    }

    /* Host camera follows the host PC exactly like a client's (PT9: the
       host had to press Home). Same dead zone, every frame. */
    coop_lite_host_resync_after_reload();
    coop_lite_client_follow_own_pc(coop_lite_camera_target());
    coop_lite_keep_remote_sectors();
    coop_lite_dev_tick(now_ms);

    {
        Uint64 t0 = SDL_GetTicksNS();
        coop_lite_host_send_followers(now_ms);
        coop_lite_host_send_npc_pos(now_ms);
        coop_lite_host_watch_party(now_ms);
        coop_lite_frame_stats(now_ms, SDL_GetTicksNS() - t0);
    }

    if (!coop_lite_interval_elapsed(coop_last_pc_pos_ms, now_ms, COOP_LITE_PC_POS_INTERVAL_MS)) {
        return;
    }
    coop_last_pc_pos_ms = now_ms;

    for (slot = 0; slot < 8; slot++) {
        obj = sub_4A2B60(slot);
        if (obj == OBJ_HANDLE_NULL) {
            continue;
        }
        coop_lite_fill_pc_pos(&pkt, slot, obj);

        /* Unchanged: still resend once a second (keyframe) so a client that
           joined or loaded late gets the host PC's current art/pose. */
        if (coop_last_pc_pos_valid[slot]
            && !coop_lite_pc_pos_changed(&coop_last_pc_pos[slot], &pkt)
            && !coop_lite_interval_elapsed(coop_pc_pos_key_ms[slot], now_ms, 1000u)) {
            continue;
        }
        coop_pc_pos_key_ms[slot] = now_ms;
        coop_last_pc_pos[slot] = pkt;
        coop_last_pc_pos_valid[slot] = true;

        if (slot > 0
            && coop_client_applied_valid[slot]
            && !coop_lite_pc_pos_moved(&coop_client_applied[slot], &pkt)) {
            /* Same place as its owner reported (art-only drift such as a
               host-side stand frame is not a correction): relay, no echo,
               never FORCE. */
            /* Pose came from its owner: relay to everyone else, no echo. */
            tig_net_send_app_except(&pkt, sizeof(pkt), slot);
        } else {
            if (slot > 0) {
                /* Host moved an owner-driven PC (knockback, follow teleport
                   …) — the owner must apply this one. */
                pkt.flags |= COOP_PC_POS_FORCE;
            }
            tig_net_send_app_all(&pkt, sizeof(pkt));
        }
    }

    if (coop_lite_interval_elapsed(coop_last_census_ms, now_ms, 30000u)) {
        coop_last_census_ms = now_ms;
        coop_lite_pc_census(false);
    }
#else
    (void)now_ms;
#endif
}

void coop_lite_handle_pc_pos(PacketCoopPcPos* pkt)
{
#ifndef COOP_LITE_TEST_STUBS
    ObjectID oid;
    int64_t obj;
    int sender;

    if (pkt == NULL || pkt->type != PACKET_COOP_PC_POS) {
        return;
    }
    if (!tig_net_is_active()) {
        return;
    }
    if (!tig_net_is_host() && !coop_lite_client_world_ready()) {
        return;
    }
    if (tig_net_is_host() && !map_is_valid()) {
        return;
    }
    memcpy(&oid, pkt->oid, sizeof(oid));
    if (tig_net_is_host()) {
        sender = sub_52A530();
        coop_lite_keep_sector(sender, pkt->loc);
        obj = coop_lite_resolve_remote_pc(sender, &oid, pkt->loc);
    } else {
        obj = obj_pool_perm_lookup(oid);
        if (pkt->slot >= 0 && pkt->slot < 8 && pkt->slot != sub_529520()) {
            coop_lite_keep_sector(pkt->slot, pkt->loc);
            obj = coop_lite_resolve_remote_pc(pkt->slot, &oid, pkt->loc);
        }
    }
    if (obj == OBJ_HANDLE_NULL) {
        return;
    }

    if (tig_net_is_host()) {
        if (sender <= 0 || sender >= 8 || sub_4A2B60(sender) != obj) {
            tig_debug_printf("CoopLite: PC_POS from slot %d for a PC it does not own — dropped\n", sender);
            return;
        }
        if (critter_is_dead(obj)) {
            /* Fallen: the host owns it until the revive. */
            return;
        }
        coop_lite_apply_pc_pos(obj, pkt);
        coop_client_applied[sender] = *pkt;
        coop_client_applied_valid[sender] = true;
        return;
    }

    if (coop_lite_waiting_sync || coop_pending_sync_valid) {
        return;
    }
    if (obj == player_get_local_pc_obj() && (pkt->flags & COOP_PC_POS_FORCE) == 0) {
        /* Our own movement echoed back — we are authoritative for it. */
        return;
    }

    multiplayer_lock();
    if (obj == player_get_local_pc_obj()) {
        /* Host correction of our PC: position only. Its art would be a
           stand frame, and a stand art ends our running move goal. */
        if (obj_field_int64_get(obj, OBJ_F_LOCATION) != pkt->loc
            || obj_field_int32_get(obj, OBJ_F_OFFSET_X) != pkt->offset_x
            || obj_field_int32_get(obj, OBJ_F_OFFSET_Y) != pkt->offset_y) {
            tig_debug_printf("CoopLite: host correction of own PC to %d,%d\n",
                (int)LOCATION_GET_X(pkt->loc), (int)LOCATION_GET_Y(pkt->loc));
            sub_43E770(obj, pkt->loc, pkt->offset_x, pkt->offset_y);
        }
    } else {
        coop_lite_apply_pc_pos(obj, pkt);
    }
    multiplayer_unlock();

    if (obj == player_get_local_pc_obj()) {
        coop_lite_camera_follow(obj);
        coop_client_pos = *pkt;
        coop_client_pos.flags = 0;
        coop_client_pos_valid = true;
    }
#else
    (void)pkt;
#endif
}
