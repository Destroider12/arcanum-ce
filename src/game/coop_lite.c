#include "game/coop_lite.h"

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
#include "game/script.h"
#include "game/teleport.h"
#include "game/tile.h"
#include "tig/debug.h"

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

static bool coop_lite_applying_follow;
static bool coop_lite_follow_armed; /* set after first host PC teleport; that one is not broadcast */
static bool coop_lite_pending_follow_valid;
static PacketCoopFollowHost coop_lite_pending_follow;

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
    char code[3];
    const char* pch;

    /* Barter / trade response code from dialog engine (b: → field_17F0=3). */
    if (response_code == 3) {
        return true;
    }
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

        if ((code[0] == 'q' || code[0] == 'Q') && (code[1] == 'u' || code[1] == 'U')) {
            return false;
        }
        if ((code[0] == 'g' || code[0] == 'G') && (code[1] == 'v' || code[1] == 'V')) {
            return false;
        }
        if ((code[0] == 'g' || code[0] == 'G') && (code[1] == 'f' || code[1] == 'F')) {
            return false;
        }
        if ((code[0] == 's' || code[0] == 'S') && (code[1] == 's' || code[1] == 'S')) {
            return false;
        }
        if ((code[0] == 'r' || code[0] == 'R') && (code[1] == 'p' || code[1] == 'P')) {
            return false;
        }
        if ((code[0] == 'a' || code[0] == 'A') && (code[1] == 'l' || code[1] == 'L')) {
            return false;
        }
        if ((code[0] == 'r' || code[0] == 'R') && (code[1] == 'u' || code[1] == 'U')) {
            return false;
        }
        if ((code[0] == 'r' || code[0] == 'R') && (code[1] == 'q' || code[1] == 'Q')) {
            return false;
        }
        if ((code[0] == 'x' || code[0] == 'X') && (code[1] == 'p' || code[1] == 'P')) {
            return false;
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
    coop_lite_follow_armed = false;
    coop_lite_pending_follow_valid = false;
    memset(&coop_lite_pending_follow, 0, sizeof(coop_lite_pending_follow));
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
        return COOP_MP_PKT_HOST_RESOLVE;
    case 8: /* anim modify */
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
        return COOP_MP_PKT_CLIENT_APPLY;
    case 9: /* interrupt — host resolves client intent, peers apply */
    case 100: /* UI + vitals subtype 13 */
        return COOP_MP_PKT_BOTH;
    default:
        return COOP_MP_PKT_UNKNOWN;
    }
}
