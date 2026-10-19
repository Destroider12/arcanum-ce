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

static bool coop_lite_dialog_has_story_effects(int64_t npc_obj, int script_num)
{
    (void)npc_obj;
    (void)script_num;
    return g_coop_lite_test_story != 0;
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

    return false;
}

static bool coop_lite_dialog_has_story_effects(int64_t npc_obj, int script_num)
{
    Script scr;

    if (script_num != 0 && script_has_global_side_effects(script_num)) {
        return true;
    }

    if (npc_obj == OBJ_HANDLE_NULL) {
        return false;
    }

    /* Quest-giver / story dialog attachment even if start used another num. */
    obj_arrayfield_script_get(npc_obj, OBJ_F_SCRIPTS_IDX, SAP_DIALOG, &scr);
    if (scr.num != 0 && scr.num != script_num
        && script_has_global_side_effects(scr.num)) {
        return true;
    }

    obj_arrayfield_script_get(npc_obj, OBJ_F_SCRIPTS_IDX, SAP_DIALOG_OVERRIDE, &scr);
    if (scr.num != 0 && script_has_global_side_effects(scr.num)) {
        return true;
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
    (void)is_merchant;
    (void)dialog_has_story_effects;
    /* Per-node filtering replaces whole-NPC INFO_ONLY. */
    (void)coop_active;
    (void)is_host;
    return COOP_TALK_ALLOW;
}

bool coop_lite_dialog_option_allowed(const char* actions, int response_code)
{
    /*
     * Two-letter dialog action codes that touch shared campaign state:
     * quests, global vars/flags, local (per-NPC) vars/flags, story state,
     * reputation, alignment, rumors, XP.
     */
    static const char blocked[][2] = {
        { 'q', 'u' }, { 'g', 'v' }, { 'g', 'f' }, { 'l', 'v' }, { 'l', 'f' },
        { 's', 's' }, { 'r', 'p' }, { 'a', 'l' }, { 'r', 'u' }, { 'r', 'q' },
        { 'x', 'p' },
    };
    char c0;
    char c1;
    const char* pch;
    size_t i;

    (void)response_code;

    /* Barter (response code 3) is allowed only if its actions are harmless
       too — a node can carry b: plus story codes. */
    if (actions == NULL || actions[0] == '\0') {
        return true;
    }

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
        c0 = *pch++;
        if (*pch == '\0') {
            break;
        }
        c1 = *pch++;
        if (c0 >= 'A' && c0 <= 'Z') {
            c0 = (char)(c0 - 'A' + 'a');
        }
        if (c1 >= 'A' && c1 <= 'Z') {
            c1 = (char)(c1 - 'A' + 'a');
        }
        for (i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
            if (c0 == blocked[i][0] && c1 == blocked[i][1]) {
                return false;
            }
        }
    }
    return true;
}

CoopTalkPolicy coop_lite_can_talk(int64_t npc_obj, int64_t pc_obj)
{
    int script_num = 0;

    (void)pc_obj;

    if (!tig_net_is_active() || tig_net_is_host()) {
        return COOP_TALK_ALLOW;
    }

#ifndef COOP_LITE_TEST_STUBS
    {
        Script scr;
        if (npc_obj != OBJ_HANDLE_NULL) {
            obj_arrayfield_script_get(npc_obj, OBJ_F_SCRIPTS_IDX, SAP_DIALOG, &scr);
            script_num = scr.num;
        }
    }
#endif

    return coop_lite_classify_talk(true,
        false,
        coop_lite_npc_is_merchant(npc_obj),
        coop_lite_dialog_has_story_effects(npc_obj, script_num));
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

    coop_lite_applying_follow = true;
    teleport_do(&teleport_data);
    coop_lite_applying_follow = false;

    tig_debug_printf("CoopLite: client follow apply map=%d host_loc=%lld dest=%lld slot=%d\n",
        pkt->map, (long long)pkt->loc, (long long)dest, slot);

    /* Keep companion character file current after area changes. */
    coop_lite_export_local_character();
}

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
    sub_463E20(obj);
    obj_field_int32_set(obj, OBJ_F_CRITTER_INVENTORY_SOURCE, 0);
    object_flags_unset(obj, OF_OFF);
    if (name != NULL && name[0] != '\0') {
        obj_field_string_set(obj, OBJ_F_PC_PLAYER_NAME, name);
    }
    item_wield_best_all(obj, OBJ_HANDLE_NULL);

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

        /* Client: we drive our own PC — report its pose to the host. */
        if (coop_lite_client_in_session
            && coop_lite_interval_elapsed(coop_last_pc_pos_ms, now_ms, COOP_LITE_PC_POS_INTERVAL_MS)) {
            coop_last_pc_pos_ms = now_ms;
            obj = player_get_local_pc_obj();
            if (obj != OBJ_HANDLE_NULL) {
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

        if (coop_last_pc_pos_valid[slot]
            && !coop_lite_pc_pos_changed(&coop_last_pc_pos[slot], &pkt)) {
            continue;
        }
        coop_last_pc_pos[slot] = pkt;
        coop_last_pc_pos_valid[slot] = true;

        if (slot > 0
            && coop_client_applied_valid[slot]
            && !coop_lite_pc_pos_changed(&coop_client_applied[slot], &pkt)) {
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
    memcpy(&oid, pkt->oid, sizeof(oid));
    obj = obj_pool_perm_lookup(oid);
    if (obj == OBJ_HANDLE_NULL) {
        return;
    }

    if (tig_net_is_host()) {
        sender = sub_52A530();
        if (sender <= 0 || sender >= 8 || sub_4A2B60(sender) != obj) {
            tig_debug_printf("CoopLite: PC_POS from slot %d for a PC it does not own — dropped\n", sender);
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
    coop_lite_apply_pc_pos(obj, pkt);
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
