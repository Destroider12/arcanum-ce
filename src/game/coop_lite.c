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
#include "game/spell.h"
#include "game/tf.h"
#include "game/gamelib.h"
#include "game/stat.h"
#include "tig/file.h"
#include "tig/debug.h"
#include "tig/memory.h"
#include "tig/art.h"
#include "game/portrait.h"
#include "game/portal.h"

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

static void coop_lite_copy_portrait(int64_t pc, const char* oidstr, int size, const char* suffix)
{
    char src_path[TIG_MAX_PATH];
    char dst_path[TIG_MAX_PATH];

    src_path[0] = '\0';
    portrait_path(portrait_get(pc), src_path, sizeof(src_path), size);
    if (src_path[0] != '\0') {
        snprintf(dst_path, sizeof(dst_path), "Players\\%s%s.bmp", oidstr, suffix);
        tig_file_copy(src_path, dst_path);
    }
}

/*
 * P0-1: non-destructive character bundle. The old path (sub_4A4320 ->
 * save_char) ran sub_463730(pc, true) — dropping every item (host: items on
 * the ground; client: orphan Packet27) — plus sub_4A6010 (full heal, flag
 * reset, which also revived a corpse) and two anim interrupts at
 * PRIORITY_HIGHEST (the anim.c:4096 assertion seen on every dev save).
 */
static bool coop_lite_build_char_bundle(int64_t pc, CoopCharBundleBuf* out)
{
    CoopCharBundleBuf buf;
    int count;
    int index;
    int scheme;
    const char* rule = NULL;
    const char* name = NULL;
    bool ok;

    count = obj_arrayfield_length_get(pc, OBJ_F_CRITTER_INVENTORY_LIST_IDX);
    if (count < 0 || count + 1 > COOP_CHAR_BUNDLE_MAX_OBJECTS) {
        return false;
    }
    if (!coop_char_bundle_begin(&buf, count + 1)) {
        return false;
    }

    ok = true;
    for (index = -1; index < count && ok; index++) {
        int64_t obj = index < 0
            ? pc
            : obj_arrayfield_handle_get(pc, OBJ_F_CRITTER_INVENTORY_LIST_IDX, index);
        uint8_t* data = NULL;
        int size = 0;

        if (obj == OBJ_HANDLE_NULL) {
            ok = false;
            break;
        }
        sub_442050(&data, &size, obj);
        ok = data != NULL && size > 0 && coop_char_bundle_add_blob(&buf, data, (size_t)size);
        if (!ok) {
            tig_debug_printf("CoopLite: character bundle: object %d not written (size %d)\n", index, size);
        }
        if (data != NULL) {
            FREE(data);
        }
    }

    scheme = auto_level_scheme_get(pc);
    if (scheme != 0) {
        rule = auto_level_scheme_rule(scheme);
        name = auto_level_scheme_name(scheme);
    }
    if (ok && !coop_char_bundle_finish(&buf, rule, name)) {
        tig_debug_printf("CoopLite: character bundle: level scheme strings rejected\n");
        ok = false;
    }
    if (!ok) {
        coop_char_bundle_free(&buf);
        return false;
    }
    *out = buf;
    return true;
}

static bool coop_lite_write_char_bundle(int64_t pc, const char* path)
{
    CoopCharBundleBuf buf;
    TigFile* stream;
    bool ok;

    ok = coop_lite_build_char_bundle(pc, &buf);
    if (ok) {
        if (!tig_file_is_directory("Players")) {
            tig_file_mkdir("Players");
        }
        stream = tig_file_fopen(path, "wb");
        ok = stream != NULL;
        if (ok) {
            ok = tig_file_fwrite(buf.data, buf.size, 1, stream) == 1;
            tig_file_fclose(stream);
        } else {
            tig_debug_printf("CoopLite: character bundle: cannot open %s\n", path);
        }
    }
    coop_char_bundle_free(&buf);
    return ok;
}

static bool coop_lite_export_local_character_impl(void)
{
    int64_t pc = player_get_local_pc_obj();
    char oidstr[40];
    char path[TIG_MAX_PATH];
    int before;
    bool ok;

    objid_id_to_str(oidstr, obj_get_id(pc));
    snprintf(path, sizeof(path), "Players\\%s.coopchar", oidstr);
    before = obj_field_int32_get(pc, OBJ_F_CRITTER_INVENTORY_NUM);
    coop_lite_copy_portrait(pc, oidstr, 32, "");
    coop_lite_copy_portrait(pc, oidstr, 128, "_b");
    ok = coop_lite_write_char_bundle(pc, path);
    tig_debug_printf("CoopLite: character bundle %s: %s (items %d -> %d, gold %d)\n",
        path, ok ? "ok" : "FAILED", before,
        obj_field_int32_get(pc, OBJ_F_CRITTER_INVENTORY_NUM), item_gold_get(pc));
    return ok;
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
/* P0-2: host map load generation (clients adopt it from FOLLOW/SYNC). */
static uint16_t coop_map_gen;
/* Host: per-slot owner pose order (epoch = the slot's correction epoch).
   Client: per-slot order of poses received from the host. */
static CoopPoseOrder coop_pose_rx[8];
static uint16_t coop_pose_tx_seq[8];
static bool coop_pose_force_active[8];
/* Client: epoch of the last correction of our own PC, and our pose seq. */
static uint16_t coop_own_pose_epoch;
static uint16_t coop_own_pose_seq;

static void coop_lite_pose_order_reset(void)
{
    memset(coop_pose_rx, 0, sizeof(coop_pose_rx));
    memset(coop_pose_tx_seq, 0, sizeof(coop_pose_tx_seq));
    memset(coop_pose_force_active, 0, sizeof(coop_pose_force_active));
    coop_own_pose_epoch = 0;
    coop_own_pose_seq = 0;
}
static void (*coop_dialog_end_hook)(int64_t pc_obj, int a2);
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
    uint64_t token_hash;
} CoopPendingOffer;

/* Host: token hash of each bound slot's player (for parking on leave). */
static uint64_t coop_slot_token_hash[8];

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
static unsigned int coop_client_pos_key_ms;
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
    pkt.map_gen = coop_map_gen;
    pkt.reserved = 0;
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
    coop_map_gen = (uint16_t)pkt->map_gen;
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
static void coop_lite_barter_reset(void);
void coop_lite_client_dev_barter_tick(unsigned int now_ms);
static void coop_lite_host_announce_roster(int new_slot);
static void coop_lite_host_roster_tick(unsigned int now_ms);
static void coop_lite_host_party_inven_tick(unsigned int now_ms);
static void coop_lite_host_dev_give_weapon_tick(unsigned int now_ms);
static void coop_lite_roster_reset(void);
static void coop_lite_host_send_roster_entry(int slot, int64_t pc, bool present, int to_slot);
static void coop_lite_remember_slot_pc(int slot, int64_t obj, int proto);
static void coop_lite_host_raise_pc(int64_t pc);
static void coop_lite_unkeep_sector(int slot);
static int64_t coop_lite_take_slot_pc(int slot);
#endif

static unsigned int coop_obj_now_ms; /* last coop_lite_ping time */
/* Client: remote PC pose rings, by slot (never the local PC). */
static CoopInterpRing coop_interp[8];
/* Not cleared by session_reset: it fires after the session is gone. */
static bool coop_host_left_menu_armed;
static unsigned int coop_host_left_menu_ms;
static void (*coop_return_to_menu_hook)(void);
#ifndef COOP_LITE_TEST_STUBS
static void coop_lite_obj_pending_reset(void);
static void coop_lite_obj_ground_fallback_tick(unsigned int now_ms);
static void coop_lite_client_interp_tick(unsigned int now_ms);
static void coop_lite_client_world_req_tick(unsigned int now_ms);
static void coop_lite_world_state_reset(void);
static void coop_lite_host_dev_world_mutate_tick(unsigned int now_ms);
#endif

/* Rejoin: our parked PC (host copy) is on its way; until it replaces the
   menu character, that one has no PC on the host to report for. */
static bool coop_rejoin_pc_wanted;

void coop_lite_session_reset(void)
{
    int i;

    for (i = 0; i < 8; i++) {
        coop_interp_reset(&coop_interp[i]);
    }
    coop_rejoin_pc_wanted = false;

    coop_lite_follow_armed = false;
    coop_lite_pose_order_reset();
#ifndef COOP_LITE_TEST_STUBS
    coop_lite_obj_pending_reset();
    coop_lite_roster_reset();
    coop_lite_world_state_reset();
#endif
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
    coop_lite_barter_reset();
    mp_clear_deferred_packet70();
    memset(coop_reload_raise, 0, sizeof(coop_reload_raise));
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
    case 81: /* inventory/barter transfer → host validates + performs */
    case 26: /* item drop (P1-3) */
    case 27: /* item drop at location */
    case 28: /* item transfer / pickup / equip */
    case 125: /* identify service */
    case PACKET_COOP_BARTER: /* barter open/close/inventory resync */
    case PACKET_COOP_OBJ_REQ: /* item blob request (P1-2) */
    case PACKET_COOP_CHAR_BUNDLE: /* v2 character offer */
    case PACKET_COOP_WORLD_REQ: /* §7 world state request */
        return COOP_MP_PKT_HOST_RESOLVE;
    case 10: /* anim pose */
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
    case PACKET_COOP_INVEN_SYNC: /* host inventory snapshot → client */
    case PACKET_COOP_ROSTER: /* other clients' PCs → client */
    case PACKET_COOP_OBJ_BLOB: /* item blob → client (P1-2) */
    case PACKET_COOP_WORLD_STATE: /* §7 world state → client */
    /* Review H2: host→client effects a client must never send the host. */
    case 54: /* magictech end */
    case 71: /* party */
    case 94: /* inventory UI refresh */
    case 106: /* sound */
    case 118: /* object move */
    case 121: /* inventory drop/destroy */
        return COOP_MP_PKT_CLIENT_APPLY;
    case 122: /* client notification (ignored on host) */
        return COOP_MP_PKT_HOST_RESOLVE;
    case 8: /* anim modify: host→clients applies; a client→host modify of
               its own goal is legal vanilla MP traffic (host ignores it) */
    case 98: /* multiplayer flags change */
    case PACKET_COOP_PC_POS: /* owner PC pos: client→host, host→others */
        return COOP_MP_PKT_BOTH;
    case 11: /* combat mode: client toggles own PC, host mirrors (PT12) */
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

#ifndef COOP_LITE_TEST_STUBS
/* Players\coop_token.txt: 32 hex digits, random, created once. Never
   logged. Returns false (token all zero) if it cannot be read or made. */
static bool coop_lite_load_or_create_token(unsigned char token[16])
{
    static const char* path = "Players\\coop_token.txt";
    char hex[40];
    TigFile* stream;
    int i;

    memset(token, 0, 16);
    stream = tig_file_fopen(path, "rb");
    if (stream != NULL) {
        memset(hex, 0, sizeof(hex));
        if (tig_file_fread(hex, 32, 1, stream) == 1) {
            bool ok = true;

            for (i = 0; i < 16 && ok; i++) {
                unsigned int b;

                ok = sscanf(hex + i * 2, "%2x", &b) == 1;
                token[i] = (unsigned char)b;
            }
            if (ok && coop_lite_token_hash(token) != 0) {
                tig_file_fclose(stream);
                return true;
            }
        }
        tig_file_fclose(stream);
        memset(token, 0, 16);
    }

    {
        FILE* rnd = fopen("/dev/urandom", "rb");
        bool got = false;

        if (rnd != NULL) {
            got = fread(token, 16, 1, rnd) == 1;
            fclose(rnd);
        }
        if (!got) {
            /* No /dev/urandom (Windows): mix clocks and addresses. */
            uint64_t s = (uint64_t)SDL_GetPerformanceCounter() ^ ((uint64_t)time(NULL) << 32)
                ^ (uint64_t)(uintptr_t)&s ^ (uint64_t)(uintptr_t)token;

            for (i = 0; i < 16; i++) {
                s ^= s << 13;
                s ^= s >> 7;
                s ^= s << 17;
                token[i] = (unsigned char)(s >> 24);
            }
        }
    }
    if (!tig_file_is_directory("Players")) {
        tig_file_mkdir("Players");
    }
    stream = tig_file_fopen(path, "wb");
    if (stream == NULL) {
        tig_debug_printf("CoopLite: rejoin token could not be saved\n");
        return coop_lite_token_hash(token) != 0;
    }
    for (i = 0; i < 16; i++) {
        snprintf(hex + i * 2, 3, "%02x", token[i]);
    }
    tig_file_fwrite(hex, 32, 1, stream);
    tig_file_fclose(stream);
    tig_debug_printf("CoopLite: new rejoin token created\n");
    return true;
}
#endif

#ifndef COOP_LITE_TEST_STUBS
/* v2 offer: our character bundle, in order, ahead of CHAR_OFFER. */
static void coop_lite_client_send_char_bundle(int64_t pc)
{
    CoopCharBundleBuf buf;
    PacketCoopCharChunk* chunk;
    int off;

    {
        /* Dev: a custom stat ("STAT:VALUE", e.g. "0:15") to see it carry
           over, or an absurd one to see the host refuse the bundle. */
        const char* env = getenv("ARCANUM_DEV_OFFER_STAT");
        int stat;
        int value;

        if (env != NULL && sscanf(env, "%d:%d", &stat, &value) == 2
            && stat >= STAT_STRENGTH && stat <= STAT_CHARISMA) {
            tig_net_local_suspend_begin();
            stat_base_set(pc, stat, value);
            tig_net_local_suspend_end();
            tig_debug_printf("CoopLite: dev: offer stat %d = %d (now %d)\n", stat, value,
                stat_base_get(pc, stat));
        }
    }
    if (getenv("ARCANUM_COOP_NO_CHAR_BUNDLE") != NULL && getenv("ARCANUM_COOP_NO_CHAR_BUNDLE")[0] == '1') {
        tig_debug_printf("CoopLite: v2 offer: disabled (ARCANUM_COOP_NO_CHAR_BUNDLE)\n");
        return;
    }
    if (!coop_lite_build_char_bundle(pc, &buf)) {
        tig_debug_printf("CoopLite: v2 offer: no bundle (proto offer only)\n");
        return;
    }
    if (buf.size == 0 || buf.size > COOP_CHAR_WIRE_MAX) {
        tig_debug_printf("CoopLite: v2 offer: bundle too large (%d bytes)\n", (int)buf.size);
        coop_char_bundle_free(&buf);
        return;
    }
    chunk = (PacketCoopCharChunk*)MALLOC(sizeof(*chunk));
    for (off = 0; off < (int)buf.size; off += COOP_CHAR_CHUNK) {
        int len = (int)buf.size - off < COOP_CHAR_CHUNK ? (int)buf.size - off : COOP_CHAR_CHUNK;

        chunk->type = PACKET_COOP_CHAR_BUNDLE;
        chunk->total = (int)buf.size;
        chunk->offset = off;
        chunk->len = len;
        memcpy(chunk->data, buf.data + off, (size_t)len);
        tig_net_send_app_all(chunk, (int)offsetof(PacketCoopCharChunk, data) + len);
    }
    FREE(chunk);
    tig_debug_printf("CoopLite: v2 offer: sent character bundle (%d bytes)\n", (int)buf.size);
    coop_char_bundle_free(&buf);
}
#endif

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
        coop_lite_load_or_create_token(pkt.token);
        if (pc != OBJ_HANDLE_NULL) {
            coop_lite_client_send_char_bundle(pc);
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

bool coop_lite_pc_pos_plausible(int pkt_map, int cur_map, int64_t step_tiles,
    int pkt_art_type, int cur_art_type)
{
    if (pkt_map != cur_map) {
        return false;
    }
    if (step_tiles < 0 || step_tiles > COOP_LITE_PC_POS_MAX_STEP) {
        return false;
    }
    return pkt_art_type == cur_art_type;
}

void coop_interp_reset(CoopInterpRing* ring)
{
    memset(ring, 0, sizeof(*ring));
}

bool coop_interp_push(CoopInterpRing* ring, const CoopInterpSample* sample, int64_t step_tiles)
{
    if (ring->n > 0) {
        const CoopInterpSample* last = &ring->s[ring->n - 1];

        if (last->loc == sample->loc && last->offset_x == sample->offset_x
            && last->offset_y == sample->offset_y && last->art_id == sample->art_id) {
            /* Keyframe resend of the same pose: keep the original time. */
            return true;
        }
    }
    if (ring->n == 0 || step_tiles > 1 || step_tiles < 0) {
        ring->s[0] = *sample;
        ring->n = 1;
        return false;
    }
    if (ring->n == COOP_INTERP_RING) {
        memmove(&ring->s[0], &ring->s[1], sizeof(ring->s[0]) * (COOP_INTERP_RING - 1));
        ring->n--;
    }
    ring->s[ring->n++] = *sample;
    return true;
}

int coop_interp_pick(const CoopInterpRing* ring, unsigned int rt, int* ia, int* ib,
    unsigned int* num, unsigned int* den)
{
    int i;

    if (ring->n <= 0) {
        return 0;
    }
    /* Before the oldest sample: hold it. */
    if ((int)(rt - ring->s[0].t_ms) <= 0 || ring->n == 1) {
        *ia = 0;
        return 1;
    }
    for (i = 0; i + 1 < ring->n; i++) {
        unsigned int ta = ring->s[i].t_ms;
        unsigned int tb = ring->s[i + 1].t_ms;

        if ((int)(rt - ta) >= 0 && (int)(rt - tb) < 0) {
            *ia = i;
            *ib = i + 1;
            *num = rt - ta;
            *den = tb - ta;
            return *den == 0 ? (*ia = i + 1, 1) : 2;
        }
    }
    /* Past the newest: show it. */
    *ia = ring->n - 1;
    return 1;
}

int coop_interp_offset(int oa, int ob, int64_t d, unsigned int num, unsigned int den, bool rel_b)
{
    int64_t span = d + (int64_t)ob - (int64_t)oa;
    int64_t v;

    if (den == 0) {
        num = 1;
        den = 1;
    }
    if (num > den) {
        num = den;
    }
    v = (int64_t)oa + span * (int64_t)num / (int64_t)den;
    if (rel_b) {
        v -= d;
    }
    return (int)v;
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
        /* Review H2: a whitelist. Clients may only send intents the host
           resolves (or two-way types); unknown types are dropped. */
        return role == COOP_MP_PKT_HOST_RESOLVE || role == COOP_MP_PKT_BOTH;
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
    case PACKET_COOP_BARTER:
        return sizeof(PacketCoopBarter);
    case PACKET_COOP_ROSTER:
        return sizeof(PacketCoopRoster);
    case PACKET_COOP_INVEN_SYNC:
        return offsetof(PacketCoopInvenSync, entries);
    case PACKET_COOP_OBJ_REQ:
        return offsetof(PacketCoopObjReq, oid);
    case PACKET_COOP_OBJ_BLOB:
        return offsetof(PacketCoopObjBlob, data);
    case PACKET_COOP_CHAR_BUNDLE:
        return offsetof(PacketCoopCharChunk, data);
    case PACKET_COOP_WORLD_REQ:
        return sizeof(PacketCoopWorldReq);
    case PACKET_COOP_WORLD_STATE:
        return offsetof(PacketCoopWorldState, entries);
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

static CoopCharRx coop_char_rx[8];

void coop_lite_host_on_client_disconnect(int slot)
{
    if (slot <= 0 || slot >= 8) {
        return;
    }
    coop_char_rx_free(&coop_char_rx[slot]);
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
    /* Parked PCs belong to the world that was just left. */
    coop_lite_parked_reset();
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
    objid_id_to_str(oid_str, obj_get_id(pc_obj));
    coop_lite_host_send_roster_entry(slot, pc_obj, false, -1);
    if (slot > 0 && slot < 8 && coop_slot_token_hash[slot] != 0) {
        ObjectID oid = obj_get_id(pc_obj);

        if (coop_lite_parked_put(coop_slot_token_hash[slot], (const unsigned char*)&oid)) {
            /* Parked: out of the world until the same player comes back. */
            object_flags_set(pc_obj, OF_OFF);
            coop_slot_token_hash[slot] = 0;
            tig_debug_printf("CoopLite: parking PC %s of the player who left (parked=%d)\n",
                oid_str, coop_lite_parked_count());
            return;
        }
    }
    /* No token (or table full): a rejoin offers a fresh character (PT8: a
       stale dead PC lingered on the host). */
    tig_debug_printf("CoopLite: removing PC %s of the player who left\n", oid_str);
    if (slot > 0 && slot < 8) {
        coop_slot_token_hash[slot] = 0;
    }
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
    if (coop_lite_is_client_role()) {
        /* P1-2: the kit is host-only. A client-rolled kit had different
           OIDs (and rolls) than the host's; the host's kit reaches us via
           INVEN_SYNC (own PC) and the party sync (remote PCs). */
        tig_debug_printf("CoopLite: starting kit left to the host\n");
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
    pkt->map = coop_lite_pose_map_key(map_current_map(), coop_map_gen);
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
/* Remote PCs known per slot, to re-create one that still went missing. */
typedef struct CoopSlotPc {
    bool valid;
    ObjectID oid;
    int proto;
    char name[64];
} CoopSlotPc;
static CoopSlotPc coop_slot_pc[8];
/* Client: last roster location per slot (deferred creation). */
static int64_t coop_roster_loc[8];
static unsigned int coop_roster_reconcile_ms;

/* Client, once world-ready: create/bind every roster PC that is still
   missing (entries that arrived while we were loading). */
static void coop_lite_client_roster_reconcile(void);

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

    /* PT11: the census used to list only PCs in the local vicinity, so a
       far-away (but loaded and tracked) remote PC read as "gone". Report
       every player slot by OID, wherever it is on the map. */
    {
        int64_t local_loc = obj_field_int64_get(local_pc, OBJ_F_LOCATION);
        int64_t obj;
        int missing = 0;

        for (slot = 0; slot < 8; slot++) {
            obj = sub_4A2B60(slot);
            if (obj == OBJ_HANDLE_NULL) {
                if (slot < 8 && coop_slot_pc[slot].valid) {
                    objid_id_to_str(oid_str, coop_slot_pc[slot].oid);
                    tig_debug_printf("CoopLite: PC census: slot=%d %s MISSING\n", slot, oid_str);
                    missing++;
                }
                continue;
            }
            count++;
            loc = obj_field_int64_get(obj, OBJ_F_LOCATION);
            objid_id_to_str(oid_str, obj_get_id(obj));
            tig_debug_printf("CoopLite: PC census: %s @%d,%d slot=%d%s dist=%d proto=%d flags=0x%x%s\n",
                oid_str,
                (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc),
                slot,
                obj == local_pc ? " (local)" : "",
                (int)location_dist(local_loc, loc),
                sub_49B290(obj),
                (unsigned int)obj_field_int32_get(obj, OBJ_F_FLAGS),
                critter_is_dead(obj) ? " dead" : "");
        }

        /* Unbound PCs (stale duplicates) are only looked for nearby. */
        object_list_vicinity(local_pc, OBJ_TM_PC, &objects);
        for (node = objects.head; node != NULL; node = node->next) {
            if (node->obj == local_pc || coop_lite_slot_of_obj(node->obj) >= 0) {
                continue;
            }
            objid_id_to_str(oid_str, obj_get_id(node->obj));
            tig_debug_printf("CoopLite: PC census: unbound PC %s nearby\n", oid_str);
            if (destroy_unbound && num_stale < 16) {
                stale[num_stale++] = node->obj;
            }
        }
        object_list_destroy(&objects);

        for (i = 0; i < num_stale; i++) {
            objid_id_to_str(oid_str, obj_get_id(stale[i]));
            tig_debug_printf("CoopLite: destroying stale unbound PC %s\n", oid_str);
            object_destroy(stale[i]);
        }
        tig_debug_printf("CoopLite: PC census total=%d missing=%d destroyed=%d\n", count, missing, num_stale);
    }
}

void coop_lite_handle_char_chunk(PacketCoopCharChunk* pkt)
{
    int slot;

    if (!tig_net_is_host()) {
        return;
    }
    slot = sub_52A530();
    if (slot <= 0 || slot >= 8) {
        return;
    }
    if (!coop_char_rx_accept(&coop_char_rx[slot], pkt, tig_net_last_message_size())) {
        tig_debug_printf("CoopLite: v2 offer: slot %d bundle chunk refused\n", slot);
        return;
    }
    if (coop_char_rx[slot].complete) {
        tig_debug_printf("CoopLite: v2 offer: slot %d bundle received (%d bytes)\n",
            slot, coop_char_rx[slot].total);
    }
}

#ifndef COOP_LITE_TEST_STUBS
#define COOP_BLOB_PROTO_AT 4
#define COOP_BLOB_OID_AT (4 + 24)
#define COOP_BLOB_TYPE_AT (4 + 24 + 24)

typedef struct CoopBundleBlobs {
    const uint8_t* blob[COOP_CHAR_MAX_ITEMS + 1];
    size_t size[COOP_CHAR_MAX_ITEMS + 1];
    int n;
} CoopBundleBlobs;

static bool coop_lite_bundle_collect(void* ctx, int index, const uint8_t* blob, size_t size)
{
    CoopBundleBlobs* b = (CoopBundleBlobs*)ctx;

    (void)index;
    if (b->n > COOP_CHAR_MAX_ITEMS || size < COOP_BLOB_TYPE_AT + 4) {
        return false;
    }
    b->blob[b->n] = blob;
    b->size[b->n] = size;
    b->n++;
    return true;
}

/* Header checks on one blob: its type, a prototype we have, an OID that is
   not taken yet (no overwriting or aliasing a host object). */
static bool coop_lite_bundle_blob_ok(const uint8_t* blob, bool want_pc, const ObjectID* want_oid)
{
    ObjectID proto_oid;
    ObjectID oid;
    int type;

    memcpy(&proto_oid, blob + COOP_BLOB_PROTO_AT, sizeof(proto_oid));
    memcpy(&oid, blob + COOP_BLOB_OID_AT, sizeof(oid));
    memcpy(&type, blob + COOP_BLOB_TYPE_AT, sizeof(type));
    if (want_pc ? type != OBJ_TYPE_PC : (type < OBJ_TYPE_WEAPON || type > OBJ_TYPE_GENERIC)) {
        return false;
    }
    if (oid.type == OID_TYPE_NULL || obj_pool_perm_lookup(oid) != OBJ_HANDLE_NULL) {
        return false;
    }
    if (want_oid != NULL && want_oid->type != OID_TYPE_NULL && !objid_is_equal(oid, *want_oid)) {
        return false;
    }
    return obj_pool_perm_lookup(proto_oid) != OBJ_HANDLE_NULL;
}

/* Host: the client's own character from its v2 bundle, at loc. NULL when
   there is none or it does not pass the checks (proto offer then). */
static int64_t coop_lite_host_pc_from_bundle(int slot, int64_t loc, const ObjectID* want_oid)
{
    CoopCharRx* rx = &coop_char_rx[slot];
    CoopBundleBlobs blobs;
    int64_t objs[COOP_CHAR_MAX_ITEMS + 1];
    int64_t pc = OBJ_HANDLE_NULL;
    int stats[8];
    int count = 0;
    int i;
    bool ok;

    if (!rx->complete) {
        return OBJ_HANDLE_NULL;
    }
    memset(&blobs, 0, sizeof(blobs));
    memset(objs, 0, sizeof(objs));
    ok = coop_char_bundle_parse(rx->data, (size_t)rx->total, coop_lite_bundle_collect, &blobs,
             &count, NULL, NULL)
        && blobs.n >= 1 && blobs.n == count;
    for (i = 0; ok && i < blobs.n; i++) {
        ok = coop_lite_bundle_blob_ok(blobs.blob[i], i == 0, i == 0 ? want_oid : NULL);
    }
    if (!ok) {
        tig_debug_printf("CoopLite: v2 offer: slot %d bundle refused (layout/objects)\n", slot);
        coop_char_rx_free(rx);
        return OBJ_HANDLE_NULL;
    }

    /* Items first (registered, handles still ids), then the PC with its
       postprocess (its inventory list now resolves), then the items'
       postprocess (their parent resolves to the PC). */
    tig_net_local_suspend_begin();
    for (i = 1; ok && i < blobs.n; i++) {
        ok = obj_read_mem((uint8_t*)blobs.blob[i], &objs[i]) && objs[i] != OBJ_HANDLE_NULL;
    }
    if (ok) {
        ok = sub_4420D0((uint8_t*)blobs.blob[0], &pc, loc) && pc != OBJ_HANDLE_NULL;
        objs[0] = pc;
    }
    for (i = 1; ok && i < blobs.n; i++) {
        obj_load_postprocess(objs[i]);
    }
    if (ok) {
        for (i = 0; i < 8; i++) {
            stats[i] = stat_base_get(pc, STAT_STRENGTH + i);
        }
        ok = coop_lite_char_plausible(stat_level_get(pc, STAT_LEVEL),
            stat_level_get(pc, STAT_EXPERIENCE_POINTS), stats, 8, item_gold_get(pc),
            obj_field_int32_get(pc, OBJ_F_CRITTER_INVENTORY_NUM));
        if (!ok) {
            tig_debug_printf("CoopLite: v2 offer: slot %d character implausible (level %d)\n",
                slot, stat_level_get(pc, STAT_LEVEL));
        }
    }
    if (!ok) {
        for (i = 0; i <= COOP_CHAR_MAX_ITEMS; i++) {
            if (objs[i] != OBJ_HANDLE_NULL) {
                object_destroy(objs[i]);
            }
        }
        tig_net_local_suspend_end();
        coop_char_rx_free(rx);
        return OBJ_HANDLE_NULL;
    }
    object_flags_unset(pc, OF_OFF);
    obj_field_int64_set(pc, OBJ_F_CRITTER_TELEPORT_DEST, loc);
    obj_field_int32_set(pc, OBJ_F_CRITTER_TELEPORT_MAP, map_current_map());
    tig_net_local_suspend_end();
    /* A fresh custom character has no kit yet: the host's, as for a
       proto offer. */
    coop_lite_generate_start_inventory(pc);
    tig_debug_printf("CoopLite: v2 offer: slot %d character from bundle (level %d, ST %d, %d items, gold %d)\n",
        slot, stat_level_get(pc, STAT_LEVEL), stat_base_get(pc, STAT_STRENGTH),
        obj_field_int32_get(pc, OBJ_F_CRITTER_INVENTORY_NUM), item_gold_get(pc));
    coop_char_rx_free(rx);
    return pc;
}
#endif

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
    bool rejoined;

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
    rejoined = false;
    {
        ObjectID parked_oid;
        int64_t parked;

        if (coop_lite_parked_take(coop_pending_offers[slot].token_hash, (unsigned char*)&parked_oid)) {
            parked = obj_pool_perm_lookup(parked_oid);
            if (parked != OBJ_HANDLE_NULL && parked != host_pc
                && obj_field_int32_get(parked, OBJ_F_TYPE) == OBJ_TYPE_PC
                && (coop_lite_slot_of_obj(parked) < 0 || coop_lite_slot_of_obj(parked) == slot)
                && (obj_field_int32_get(parked, OBJ_F_FLAGS) & OF_DESTROYED) == 0) {
                /* The same player is back: their parked PC, next to us. */
                client_oid = parked_oid;
                object_flags_unset(parked, OF_OFF);
                sub_43E770(parked, spawn_loc, 0, 0);
                rejoined = true;
                tig_debug_printf("CoopLite: slot %d rejoined — parked PC restored\n", slot);
            } else {
                tig_debug_printf("CoopLite: slot %d parked PC is gone (other map?) — fresh PC\n", slot);
            }
        }
    }
    coop_slot_token_hash[slot] = coop_pending_offers[slot].token_hash;

    client_obj = sub_4A2B60(slot);
    if (client_obj != OBJ_HANDLE_NULL && client_obj != host_pc
        && client_oid.type != OID_TYPE_NULL
        && !objid_is_equal(obj_get_id(client_obj), client_oid)) {
        /* A loaded save bound this slot to another character (other session
           or a re-picked PC): the player's own PC wins, the stale one goes. */
        ObjectID stale = obj_get_id(client_obj);

        if (coop_lite_parked_has_oid((const unsigned char*)&stale)) {
            /* Another player's PC from the save, parked: it waits for them. */
            tig_debug_printf("CoopLite: slot %d was bound to a parked PC — leaving it parked\n", slot);
            object_flags_set(client_obj, OF_OFF);
        } else {
            tig_debug_printf("CoopLite: slot %d was bound to another PC — replacing it\n", slot);
            object_destroy(client_obj);
        }
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
        /* v2: the client's own character (stats, skills, spells, gear). */
        client_obj = coop_lite_host_pc_from_bundle(slot, spawn_loc, &client_oid);
    } else {
        coop_char_rx_free(&coop_char_rx[slot]);
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

    /* A PC parked by a load (or a leave) is back in the world. */
    object_flags_unset(client_obj, OF_OFF);
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
    /* P0-2: a (re)joining slot starts a fresh epoch, above anything the
       previous occupant or an older session of this slot used. */
    coop_lite_pose_order_bump(&coop_pose_rx[slot]);
    coop_pose_force_active[slot] = false;

    host_oid = obj_get_id(host_pc);
    client_oid = obj_get_id(client_obj);

    memset(&sync, 0, sizeof(sync));
    sync.type = PACKET_COOP_SESSION_SYNC;
    sync.map_gen = coop_map_gen;
    sync.pose_epoch = coop_pose_rx[slot].epoch;
    sync.flags = rejoined ? COOP_SYNC_F_REJOINED : 0;
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
    coop_lite_host_announce_roster(slot);
    return true;
}

static void coop_lite_client_request_own_pc(const unsigned char oid[24]);

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
        if ((pkt->flags & COOP_SYNC_F_REJOINED) != 0) {
            /* Expected: the menu character stands in until the parked PC
               blob replaces it (coop_lite_client_request_own_pc). */
            tig_debug_printf("CoopLite: rejoin: menu character stands in until our parked PC arrives\n");
        } else {
            tig_debug_printf("CoopLite: WARNING local PC OID differs from host companion OID\n");
        }
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

    if ((pkt->flags & COOP_SYNC_F_REJOINED) != 0
        && memcmp(&local_oid, pkt->client_pc_oid, sizeof(local_oid)) != 0) {
        /* Rejoin: the host kept our PC. Take its copy (stats, level, XP)
           instead of the character picked in the menu; the inventory
           follows once it is in place. */
        coop_lite_client_request_own_pc(pkt->client_pc_oid);
        return;
    }

    /* Our inventory as the host has it (review M4: the starting kit was
       generated on both sides with different item OIDs). */
    coop_lite_client_request_barter(local_pc, OBJ_HANDLE_NULL, COOP_BARTER_SYNC);
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
static void coop_lite_dev_items_log(int64_t pc, const char* step);
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

/*
 * ARCANUM_DEV_GRANT_RESURRECT=1 (set it for host and client): every PC learns
 * Resurrect (the whole Necromantic White college up to it) and gets enough
 * Willpower/Constitution for the fatigue it costs, once per PC. The host
 * grants every bound PC (its copies are the ones that cast); the client
 * grants its own PC so its spell bar shows it.
 */
static ObjectID coop_dev_granted[8];
static bool coop_dev_granted_valid[8];

static void coop_lite_dev_grant_resurrect(int slot, int64_t pc)
{
    const char* env = getenv("ARCANUM_DEV_GRANT_RESURRECT");
    ObjectID oid;

    if (env == NULL || env[0] != '1' || pc == OBJ_HANDLE_NULL || slot < 0 || slot >= 8) {
        return;
    }
    oid = obj_get_id(pc);
    if (coop_dev_granted_valid[slot] && objid_is_equal(coop_dev_granted[slot], oid)) {
        return;
    }
    coop_dev_granted[slot] = oid;
    coop_dev_granted_valid[slot] = true;
    tig_net_local_suspend_begin();
    if (stat_base_get(pc, STAT_WILLPOWER) < 18) {
        stat_base_set(pc, STAT_WILLPOWER, 18);
    }
    if (stat_base_get(pc, STAT_CONSTITUTION) < 18) {
        stat_base_set(pc, STAT_CONSTITUTION, 18);
    }
    spell_add(pc, SPELL_RESURRECT, true);
    critter_fatigue_damage_set(pc, 0);
    tig_net_local_suspend_end();
    tig_debug_printf("CoopLite: dev: slot %d PC knows Resurrect (fatigue max %d, cost %d)\n",
        slot, critter_fatigue_max(pc), spell_cast_cost(SPELL_RESURRECT, pc));
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
                /* PT12: Virgil's intro dialog was still open (invisible after
                   the jump), leaving the UI in dialog mode, so Esc only showed
                   the locked menu (no Save/Load). Close it before teleporting. */
                if (coop_dialog_end_hook != NULL) {
                    tig_debug_printf("CoopLite: dev start: closing any open dialog first\n");
                    coop_dialog_end_hook(player_get_local_pc_obj(), 0);
                }
                tig_debug_printf("CoopLite: dev start: teleport host to %d,%d\n", x, y);
                teleport_do(&teleport_data);
                if (strcmp(env, "merchant") == 0) {
                    coop_dev_merchant_pending = true;
                    coop_dev_merchant_ms = now_ms;
                }
            }
        }
    }
    for (slot = 0; slot < 8; slot++) {
        coop_lite_dev_grant_resurrect(slot, sub_4A2B60(slot));
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
    {
        /* ARCANUM_DEV_COOP_LOAD=N (host, with COOP_SAVE=1): N s after the
           dev save, load it back (the party-reload path, whoever is
           connected then). */
        static bool coop_dev_load_done;
        const char* load_env = getenv("ARCANUM_DEV_COOP_LOAD");

        if (!coop_dev_load_done && coop_dev_save_done && coop_dev_save_ms != 0
            && load_env != NULL && atoi(load_env) > 0
            && coop_lite_interval_elapsed(coop_dev_save_ms, now_ms, (unsigned int)atoi(load_env) * 1000u)) {
            coop_dev_load_done = true;
            snprintf(coop_reload_request_name, sizeof(coop_reload_request_name), "%s", "Slot9990Coop dev save");
            coop_reload_request_valid = true;
            coop_reload_resync_pending = true;
            for (slot = 1; slot < 8; slot++) {
                coop_reload_raise[slot] = tig_net_client_is_active(slot);
            }
            tig_debug_printf("CoopLite: dev: loading Slot9990\n");
            coop_lite_dev_items_log(player_get_local_pc_obj(), "host PC before dev load");
        }
    }

    env = getenv("ARCANUM_DEV_SPAWN_HOSTILE_NEAR_CLIENT");
    if (env == NULL || (env[0] != '1' && env[0] != '2' && env[0] != '3')) {
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
        /* PT11: the wolf killed both 24 HP PCs in seconds. 1/2 now spawn a
           sewer rat (fightable, still hurts); 3 keeps the wolf. */
        if (!mp_object_create(env[0] == '3' ? BP_WOLF : BP_SEWER_RAT, loc, &wolf) || wolf == OBJ_HANDLE_NULL) {
            tig_debug_printf("CoopLite: dev: wolf spawn failed\n");
            continue;
        }
        coop_dev_wolf[slot] = obj_get_id(wolf);
        coop_dev_wolf_valid[slot] = true;
        ai_attack(pc, wolf, LOUDNESS_NORMAL, 0);
        tig_debug_printf("CoopLite: dev: %s spawned to attack slot %d PC at %d,%d\n",
            env[0] == '3' ? "wolf" : "sewer rat", slot, (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc));
        if (env[0] == '2') {
            /* =2: a second wolf for the host too (party-wipe testing). */
            int64_t host_pc = player_get_local_pc_obj();
            int64_t wolf2;

            loc = obj_field_int64_get(host_pc, OBJ_F_LOCATION);
            loc = coop_lite_follow_loc_for_slot(LOCATION_MAKE(LOCATION_GET_X(loc) - 3, LOCATION_GET_Y(loc)), 0);
            if (mp_object_create(BP_SEWER_RAT, loc, &wolf2) && wolf2 != OBJ_HANDLE_NULL) {
                ai_attack(host_pc, wolf2, LOUDNESS_NORMAL, 0);
                tig_debug_printf("CoopLite: dev: second creature spawned to attack the host\n");
            }
        }
    }
}

/*
 * ARCANUM_DEV_TELEPORT_CLIENT_FAR=N (host; "1" = 60 tiles): 15 s after the
 * slot-1 client PC is bound, the host moves it N tiles east (a host move, so
 * the owner gets a FORCE pose like any knockback), and back 45 s later.
 * Both sides' 30 s census must keep listing both PCs (PT11: "the other PC
 * vanishes beyond ~20-34 tiles").
 */
static int coop_dev_far_stage;
static unsigned int coop_dev_far_ms;
static int64_t coop_dev_far_home;

/* P0-1 dev check (ARCANUM_DEV_EXPORT_CHAR=1, either side): 8 s after
   joining, export the local character twice and compare the live PC. */
static unsigned int coop_dev_export_ms;
static bool coop_dev_export_done;

static bool coop_lite_dev_export_count_visit(void* ctx, int index, const uint8_t* blob, size_t size)
{
    (void)index;
    (void)blob;
    (void)size;
    (*(int*)ctx)++;
    return true;
}

/* PT12 dev check (ARCANUM_DEV_CLIENT_COMBAT=1, client): combat mode on at
   10 s, off at 14 s; the host logs each mirrored change. */
static unsigned int coop_dev_combat_ms;
static int coop_dev_combat_stage;

static void coop_lite_client_dev_combat_tick(unsigned int now_ms)
{
    const char* env;
    int64_t pc = player_get_local_pc_obj();

    if (coop_dev_combat_stage >= 2) {
        return;
    }
    env = getenv("ARCANUM_DEV_CLIENT_COMBAT");
    if (env == NULL || env[0] != '1' || pc == OBJ_HANDLE_NULL) {
        return;
    }
    if (coop_dev_combat_ms == 0) {
        coop_dev_combat_ms = now_ms != 0 ? now_ms : 1;
        return;
    }
    if (!coop_lite_interval_elapsed(coop_dev_combat_ms, now_ms, coop_dev_combat_stage == 0 ? 10000u : 4000u)) {
        return;
    }
    coop_dev_combat_ms = now_ms != 0 ? now_ms : 1;
    if (coop_dev_combat_stage == 0) {
        combat_critter_activate_combat_mode(pc);
    } else {
        combat_critter_deactivate_combat_mode(pc);
    }
    tig_debug_printf("CoopLite: dev combat: local combat mode %d\n",
        combat_critter_is_combat_mode_active(pc) ? 1 : 0);
    coop_dev_combat_stage++;
}

/* P1-3 dev check (ARCANUM_DEV_CLIENT_ITEMS=1, client): 12 s in the world,
   then every 4 s: drop a backpack item (26), pick it up again (28), unwield
   the weapon (28 to backpack), wield it again (28 to the weapon slot). */
static unsigned int coop_dev_items_ms;
static int coop_dev_items_stage;
static ObjectID coop_dev_items_oid;
static ObjectID coop_dev_items_weapon;

static void coop_lite_dev_items_log(int64_t pc, const char* step)
{
    int64_t w = item_wield_get(pc, ITEM_INV_LOC_WEAPON);
    char name[256];

    name[0] = '\0';
    if (w != OBJ_HANDLE_NULL) {
        object_examine(w, pc, name);
    }
    {
        int64_t* items;
        int cnt = item_list_get(pc, &items);
        int i;

        tig_debug_printf("CoopLite: dev items: %s: items %d (list %d), weapon '%s'\n", step,
            obj_field_int32_get(pc, OBJ_F_CRITTER_INVENTORY_NUM), cnt, name);
        for (i = 0; i < cnt; i++) {
            char n2[256];
            ObjectID id = obj_get_id(items[i]);
            char ids[80];

            n2[0] = '\0';
            object_examine(items[i], pc, n2);
            objid_id_to_str(ids, id);
            tig_debug_printf("CoopLite: dev items:   %s @%d %s h=%llx flags=0x%x iflags=0x%x\n", n2, item_inventory_location_get(items[i]), ids,
                (unsigned long long)items[i],
                (unsigned int)obj_field_int32_get(items[i], OBJ_F_FLAGS), (unsigned int)obj_field_int32_get(items[i], OBJ_F_ITEM_FLAGS));
        }
        item_list_free(items);
    }
}

static void coop_lite_client_dev_items_tick(unsigned int now_ms)
{
    const char* env;
    int64_t pc = player_get_local_pc_obj();
    int64_t item;
    int depth;

    if (coop_dev_items_stage >= 5) {
        return;
    }
    env = getenv("ARCANUM_DEV_CLIENT_ITEMS");
    if (env == NULL || env[0] != '1' || pc == OBJ_HANDLE_NULL) {
        coop_dev_items_stage = env == NULL || env[0] != '1' ? 5 : coop_dev_items_stage;
        return;
    }
    if (coop_dev_items_ms == 0) {
        coop_dev_items_ms = now_ms != 0 ? now_ms : 1;
        return;
    }
    if (!coop_lite_interval_elapsed(coop_dev_items_ms, now_ms, coop_dev_items_stage == 0 ? 12000u : 4000u)) {
        return;
    }
    coop_dev_items_ms = now_ms != 0 ? now_ms : 1;
    depth = tig_net_local_suspend_pause();
    switch (coop_dev_items_stage) {
    case 0: {
        int64_t* items;
        int cnt = item_list_get(pc, &items);
        int i;

        coop_lite_dev_items_log(pc, "start");
        item = OBJ_HANDLE_NULL;
        for (i = 0; i < cnt; i++) {
            int t = obj_field_int32_get(items[i], OBJ_F_TYPE);

            if (t != OBJ_TYPE_GOLD && t != OBJ_TYPE_AMMO
                && !IS_WEAR_INV_LOC(item_inventory_location_get(items[i]))) {
                item = items[i];
                break;
            }
        }
        item_list_free(items);
        if (item != OBJ_HANDLE_NULL) {
            coop_dev_items_oid = obj_get_id(item);
            item_drop(item); /* client: Packet26 */
        }
        break;
    }
    case 1:
        coop_lite_dev_items_log(pc, "after drop");
        item = obj_pool_perm_lookup(coop_dev_items_oid);
        tig_debug_printf("CoopLite: dev items: dropped item %s on the ground\n",
            item != OBJ_HANDLE_NULL && !item_parent(item, NULL) ? "is" : "is NOT");
        if (item != OBJ_HANDLE_NULL) {
            item_transfer(item, pc); /* client: Packet28 */
        }
        break;
    case 2:
    case 3:
        coop_lite_dev_items_log(pc, coop_dev_items_stage == 2 ? "after pickup" : "after first equip change");
        item = item_wield_get(pc, ITEM_INV_LOC_WEAPON);
        if (item == OBJ_HANDLE_NULL) {
            int64_t* items;
            int cnt = item_list_get(pc, &items);
            int i;

            for (i = 0; i < cnt; i++) {
                if (obj_field_int32_get(items[i], OBJ_F_TYPE) == OBJ_TYPE_WEAPON) {
                    item = items[i];
                    break;
                }
            }
            item_list_free(items);
            if (item != OBJ_HANDLE_NULL) {
                item_transfer_ex(item, pc, ITEM_INV_LOC_WEAPON); /* wield */
            }
        } else {
            item_transfer_ex(item, pc, -1); /* unwield to the backpack */
        }
        break;
    default:
        coop_lite_dev_items_log(pc, "after second equip change");
        break;
    }
    tig_net_local_suspend_resume(depth);
    coop_dev_items_stage++;
}

static void coop_lite_dev_export_tick(unsigned int now_ms)
{
    const char* env;
    int64_t pc = player_get_local_pc_obj();
    int items_before;
    int gold_before;
    int hp_before;
    char oidstr[40];
    char path[TIG_MAX_PATH];
    TigFile* stream;
    int visited = 0;
    int count = 0;
    bool parsed = false;

    if (coop_dev_export_done) {
        return;
    }
    env = getenv("ARCANUM_DEV_EXPORT_CHAR");
    if (env == NULL || env[0] != '1') {
        coop_dev_export_done = true;
        return;
    }
    if (pc == OBJ_HANDLE_NULL) {
        return;
    }
    if (coop_dev_export_ms == 0) {
        coop_dev_export_ms = now_ms != 0 ? now_ms : 1;
        return;
    }
    if (!coop_lite_interval_elapsed(coop_dev_export_ms, now_ms, 8000u)) {
        return;
    }
    coop_dev_export_done = true;

    items_before = obj_field_int32_get(pc, OBJ_F_CRITTER_INVENTORY_NUM);
    gold_before = item_gold_get(pc);
    hp_before = object_hp_current(pc);
    coop_lite_export_local_character();
    coop_lite_export_local_character();

    objid_id_to_str(oidstr, obj_get_id(pc));
    snprintf(path, sizeof(path), "Players\\%s.coopchar", oidstr);
    stream = tig_file_fopen(path, "rb");
    if (stream != NULL) {
        int len = tig_file_filelength(stream);
        uint8_t* data = len > 0 ? (uint8_t*)MALLOC((size_t)len) : NULL;

        if (data != NULL) {
            if (tig_file_fread(data, (size_t)len, 1, stream) == 1) {
                parsed = coop_char_bundle_parse(data, (size_t)len,
                    coop_lite_dev_export_count_visit, &visited, &count, NULL, NULL);
            }
            FREE(data);
        }
        tig_file_fclose(stream);
    }
    tig_debug_printf("CoopLite: dev export check: items %d -> %d, gold %d -> %d, hp %d -> %d, "
                     "bundle %s objects=%d -> %s\n",
        items_before, obj_field_int32_get(pc, OBJ_F_CRITTER_INVENTORY_NUM),
        gold_before, item_gold_get(pc), hp_before, object_hp_current(pc),
        parsed ? "parsed" : "UNREADABLE", count,
        (parsed && count == items_before + 1
            && items_before == obj_field_int32_get(pc, OBJ_F_CRITTER_INVENTORY_NUM)
            && gold_before == item_gold_get(pc) && hp_before == object_hp_current(pc))
            ? "PASS"
            : "FAIL");
}

static void coop_lite_host_dev_far_tick(unsigned int now_ms)
{
    const char* env;
    int tiles;
    int64_t pc;
    int64_t loc;
    TeleportData teleport_data;

    if (coop_dev_far_stage >= 3) {
        return;
    }
    env = getenv("ARCANUM_DEV_TELEPORT_CLIENT_FAR");
    if (env == NULL || env[0] == '\0' || env[0] == '0') {
        coop_dev_far_stage = 3;
        return;
    }
    pc = sub_4A2B60(1);
    if (pc == OBJ_HANDLE_NULL) {
        return;
    }
    if (coop_dev_far_ms == 0) {
        coop_dev_far_ms = now_ms != 0 ? now_ms : 1;
        return;
    }
    if (!coop_lite_interval_elapsed(coop_dev_far_ms, now_ms, coop_dev_far_stage == 0 ? 15000u : 45000u)) {
        return;
    }
    tiles = atoi(env);
    if (tiles <= 1) {
        tiles = 60;
    }
    loc = obj_field_int64_get(pc, OBJ_F_LOCATION);
    memset(&teleport_data, 0, sizeof(teleport_data));
    teleport_data.obj = pc;
    teleport_data.map = map_current_map();
    if (coop_dev_far_stage == 0) {
        coop_dev_far_home = loc;
        teleport_data.loc = coop_lite_follow_loc_for_slot(
            LOCATION_MAKE(LOCATION_GET_X(loc) + tiles, LOCATION_GET_Y(loc)), 0);
    } else {
        teleport_data.loc = coop_dev_far_home;
    }
    tig_debug_printf("CoopLite: dev far: slot 1 PC %s %d,%d -> %d,%d\n",
        coop_dev_far_stage == 0 ? "away" : "back",
        (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc),
        (int)LOCATION_GET_X(teleport_data.loc), (int)LOCATION_GET_Y(teleport_data.loc));
    teleport_do(&teleport_data);
    coop_dev_far_stage = coop_dev_far_stage == 0 ? 1 : 3;
    coop_dev_far_ms = now_ms != 0 ? now_ms : 1;
    /* Census soon after on the host too. */
    coop_last_census_ms = now_ms - 25000u;
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
    memset(coop_dev_granted_valid, 0, sizeof(coop_dev_granted_valid));
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
                tig_net_send_pose_all(&pkt, (int)coop_lite_batch_size(pkt.count,
                    COOP_NPC_POS_MAX, sizeof(PacketCoopNpcPosEntry)));
                pkt.count = 0;
            }
        }
        object_list_destroy(&objects);
    }
    if (pkt.count > 0) {
        tig_net_send_pose_all(&pkt, (int)coop_lite_batch_size(pkt.count,
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
    memset(coop_reload_raise, 0, sizeof(coop_reload_raise)); /* review L6 */
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
    coop_pending_offers[slot].token_hash = coop_lite_token_hash(pkt->token);
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
    coop_map_gen = (uint16_t)pkt->map_gen;
    /* P0-2: fresh order state; our poses carry the host's epoch for us. */
    coop_lite_pose_order_reset();
    coop_own_pose_epoch = (uint16_t)pkt->pose_epoch;
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
    coop_obj_now_ms = now_ms;
#ifndef COOP_LITE_TEST_STUBS
    int slot;
    int64_t obj;
    PacketCoopPcPos pkt;

    if (coop_lite_host_left_menu_due(coop_host_left_menu_armed, coop_host_left_menu_ms, now_ms)) {
        coop_host_left_menu_armed = false;
        if (coop_return_to_menu_hook != NULL) {
            tig_debug_printf("CoopLite: host gone — back to the main menu\n");
            coop_return_to_menu_hook();
        }
        return;
    }
    if (!tig_net_is_active()) {
        return;
    }

    if (!tig_net_is_host()) {
        coop_lite_obj_ground_fallback_tick(now_ms);
        coop_lite_client_interp_tick(now_ms);
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
        if (coop_lite_client_in_session
            && coop_lite_interval_elapsed(coop_roster_reconcile_ms, now_ms, 1000u)) {
            coop_roster_reconcile_ms = now_ms;
            coop_lite_client_roster_reconcile();
        }
        if (coop_lite_client_in_session) {
            coop_lite_dev_grant_resurrect(sub_529520(), player_get_local_pc_obj());
            coop_lite_client_dev_barter_tick(now_ms);
            coop_lite_dev_export_tick(now_ms);
            coop_lite_client_dev_combat_tick(now_ms);
            coop_lite_client_dev_items_tick(now_ms);
            coop_lite_client_world_req_tick(now_ms);
        }
        coop_lite_frame_stats(now_ms, 0);

        /* Client: we drive our own PC — report its pose to the host. */
        if (coop_lite_interval_elapsed(coop_last_pc_pos_ms, now_ms, COOP_LITE_PC_POS_INTERVAL_MS)) {
            coop_last_pc_pos_ms = now_ms;
            obj = player_get_local_pc_obj();
            /* Not while a party teleport is in flight (review M2). */
            if (obj != OBJ_HANDLE_NULL && !critter_is_dead(obj) && !coop_follow_carry_pending
                && !coop_rejoin_pc_wanted) {
                coop_lite_fill_pc_pos(&pkt, sub_529520(), obj);
                /* Unreliable channel: resend once a second even unchanged,
                   so a lost final pose cannot stick. */
                if (!coop_client_pos_valid || coop_lite_pc_pos_changed(&coop_client_pos, &pkt)
                    || coop_lite_interval_elapsed(coop_client_pos_key_ms, now_ms, 1000u)) {
                    coop_client_pos = pkt;
                    coop_client_pos_valid = true;
                    coop_client_pos_key_ms = now_ms;
                    pkt.epoch = coop_own_pose_epoch;
                    pkt.seq = ++coop_own_pose_seq;
                    tig_net_send_pose_all(&pkt, sizeof(pkt));
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
    coop_lite_host_dev_far_tick(now_ms);
    coop_lite_dev_export_tick(now_ms);
    coop_lite_host_roster_tick(now_ms);
    coop_lite_host_party_inven_tick(now_ms);
    coop_lite_host_dev_give_weapon_tick(now_ms);
    coop_lite_host_dev_world_mutate_tick(now_ms);

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
        pkt.seq = ++coop_pose_tx_seq[slot];

        if (slot > 0
            && coop_client_applied_valid[slot]
            && !coop_lite_pc_pos_moved(&coop_client_applied[slot], &pkt)) {
            /* Same place as its owner reported (art-only drift such as a
               host-side stand frame is not a correction): relay, no echo,
               never FORCE. */
            /* Pose came from its owner: relay to everyone else, no echo. */
            pkt.epoch = coop_pose_rx[slot].epoch;
            tig_net_send_pose_except(&pkt, sizeof(pkt), slot);
        } else if (slot > 0) {
            /* Host moved an owner-driven PC (knockback, follow teleport,
               refused jump …) — the owner must apply this one: reliable. */
            pkt.flags |= COOP_PC_POS_FORCE;
            if (!coop_pose_force_active[slot]) {
                /* One epoch per correction, not per resend: repeated FORCEs
                   while the owner catches up share it (no livelock). */
                coop_lite_pose_order_bump(&coop_pose_rx[slot]);
                coop_pose_force_active[slot] = true;
            }
            pkt.epoch = coop_pose_rx[slot].epoch;
            tig_net_send_app_all(&pkt, sizeof(pkt));
        } else {
            pkt.epoch = 0;
            tig_net_send_pose_all(&pkt, sizeof(pkt));
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
    /* Review M2: a pose from another map (in flight across a party
       teleport) is meaningless here. */
    if (map_is_valid() && pkt->map != coop_lite_pose_map_key(map_current_map(), coop_map_gen)) {
        return;
    }
    memcpy(&oid, pkt->oid, sizeof(oid));
    if (tig_net_is_host()) {
        sender = sub_52A530();
        /* Review M3: ownership before anything (sector loads included). */
        if (sender <= 0 || sender >= 8 || !coop_slot_pc[sender].valid
            || memcmp(&coop_slot_pc[sender].oid, &oid, sizeof(oid)) != 0) {
            obj = obj_pool_perm_lookup(oid);
            if (obj == OBJ_HANDLE_NULL || sender <= 0 || sender >= 8 || sub_4A2B60(sender) != obj) {
                tig_debug_printf("CoopLite: PC_POS from slot %d for a PC it does not own — dropped\n", sender);
                return;
            }
        }
        coop_lite_keep_sector(sender, pkt->loc);
        obj = coop_lite_resolve_remote_pc(sender, &oid, pkt->loc);
    } else {
        obj = obj_pool_perm_lookup(oid);
        if (pkt->slot >= 0 && pkt->slot < 8 && pkt->slot != sub_529520()) {
            coop_lite_keep_sector(pkt->slot, pkt->loc);
            obj = coop_lite_resolve_remote_pc(pkt->slot, &oid, pkt->loc);
            if (obj != OBJ_HANDLE_NULL && pkt->slot > 0 && sub_4A2B60(pkt->slot) != obj) {
                /* Another client's PC (roster, review H3). */
                multiplayer_bind_slot_pc(pkt->slot, obj);
            }
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
        if (!coop_lite_pose_order_accept(&coop_pose_rx[sender], pkt->epoch, pkt->seq, false, true)) {
            /* In flight from before the last correction (or out of order). */
            return;
        }
        if (!coop_lite_pc_pos_plausible(pkt->map, coop_lite_pose_map_key(map_current_map(), coop_map_gen),
                location_dist(obj_field_int64_get(obj, OBJ_F_LOCATION), pkt->loc),
                tig_art_type((tig_art_id_t)pkt->art_id),
                tig_art_type(obj_field_int32_get(obj, OBJ_F_CURRENT_AID)))) {
            /* Refused: the next host pose for this PC goes out FORCE so the
               owner snaps back to where the host has it. */
            tig_debug_printf("CoopLite: PC_POS from slot %d implausible (step %d) — corrected\n",
                sender, (int)location_dist(obj_field_int64_get(obj, OBJ_F_LOCATION), pkt->loc));
            coop_client_applied_valid[sender] = false;
            coop_last_pc_pos_valid[sender] = false;
            return;
        }
        coop_lite_apply_pc_pos(obj, pkt);
        coop_client_applied[sender] = *pkt;
        coop_client_applied_valid[sender] = true;
        coop_pose_force_active[sender] = false;
        return;
    }

    if (coop_lite_waiting_sync || coop_pending_sync_valid) {
        return;
    }
    if (pkt->slot >= 0 && pkt->slot < 8
        && !coop_lite_pose_order_accept(&coop_pose_rx[pkt->slot], pkt->epoch, pkt->seq,
            (pkt->flags & COOP_PC_POS_FORCE) != 0, false)) {
        return;
    }
    if (obj == player_get_local_pc_obj() && (pkt->flags & COOP_PC_POS_FORCE) != 0) {
        /* Adopt the correction's epoch: our older in-flight poses are now
           dropped by the host instead of rubber-banding us back. */
        coop_own_pose_epoch = pkt->epoch;
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
    } else if (pkt->slot >= 0 && pkt->slot < 8) {
        CoopInterpSample smp;

        smp.loc = pkt->loc;
        smp.offset_x = pkt->offset_x;
        smp.offset_y = pkt->offset_y;
        smp.art_id = pkt->art_id;
        smp.t_ms = coop_obj_now_ms;
        CoopInterpRing* ring = &coop_interp[pkt->slot];

        /* Step from the newest sample, not from where the (delayed) copy
           is drawn right now. */
        if (!coop_interp_push(ring, &smp,
                ring->n > 0 ? location_dist(ring->s[ring->n - 1].loc, pkt->loc) : -1)) {
            /* First pose or a jump (teleport, door): snap. */
            coop_lite_apply_pc_pos(obj, pkt);
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

/* ------------------------------------------------------------------------
 * Barter / inventory (PT11, review H4). Vanilla MP's barter handshake
 * (Packet100/12) never opened the window on clients, and the item packets it
 * relies on (81, 28, 26, 27, 125) had no handler. Coop design:
 *   - the client opens its inventory / the barter window locally;
 *   - every transfer it makes is a Packet81 the host validates and performs;
 *   - the host answers with PACKET_COOP_INVEN_SYNC batches (client PC,
 *     merchant, merchant's store) that the client mirrors.
 * ---------------------------------------------------------------------- */

size_t coop_lite_inven_sync_size(int count)
{
    if (count < 0) {
        count = 0;
    }
    if (count > COOP_INVEN_CHUNK_ENTRIES) {
        count = COOP_INVEN_CHUNK_ENTRIES;
    }
    return offsetof(PacketCoopInvenSync, entries) + (size_t)count * sizeof(PacketCoopInvenEntry);
}

int coop_lite_world_state_size(int count)
{
    return (int)offsetof(PacketCoopWorldState, entries) + count * (int)sizeof(CoopWorldEntry);
}

bool coop_lite_world_state_valid(const PacketCoopWorldState* pkt, int size)
{
    int i;

    if (pkt == NULL || pkt->type != PACKET_COOP_WORLD_STATE
        || size < (int)offsetof(PacketCoopWorldState, entries)
        || pkt->count < 0 || pkt->count > COOP_WORLD_CHUNK
        || size != coop_lite_world_state_size(pkt->count)) {
        return false;
    }
    for (i = 0; i < pkt->count; i++) {
        if (pkt->entries[i].kind < COOP_WORLD_PORTAL || pkt->entries[i].kind > COOP_WORLD_GROUND_ITEM) {
            return false;
        }
    }
    return true;
}

bool coop_lite_world_req_due(int last_key, int key, unsigned int ready_ms,
    unsigned int last_req_ms, unsigned int now_ms)
{
    if (key != last_key) {
        /* Let the new map's sectors settle first. */
        return coop_lite_interval_elapsed(ready_ms, now_ms, 1500u);
    }
    return coop_lite_interval_elapsed(last_req_ms, now_ms, 30000u);
}

void coop_oid_set_reset(CoopOidSet* set)
{
    set->count = 0;
    set->overflow = false;
}

void coop_oid_set_add(CoopOidSet* set, const uint8_t oid[24])
{
    if (coop_oid_set_has(set, oid)) {
        return;
    }
    if (set->count >= COOP_OID_SET_MAX) {
        set->overflow = true;
        return;
    }
    memcpy(set->oid[set->count++], oid, 24);
}

bool coop_oid_set_has(const CoopOidSet* set, const uint8_t oid[24])
{
    int i;

    for (i = 0; i < set->count; i++) {
        if (memcmp(set->oid[i], oid, 24) == 0) {
            return true;
        }
    }
    return false;
}

void coop_char_rx_free(CoopCharRx* rx)
{
    free(rx->data);
    memset(rx, 0, sizeof(*rx));
}

bool coop_char_rx_accept(CoopCharRx* rx, const PacketCoopCharChunk* pkt, int size)
{
    if (pkt == NULL || pkt->type != PACKET_COOP_CHAR_BUNDLE
        || size < (int)offsetof(PacketCoopCharChunk, data)
        || pkt->len <= 0 || pkt->len > COOP_CHAR_CHUNK
        || size < (int)offsetof(PacketCoopCharChunk, data) + pkt->len
        || pkt->total <= 0 || pkt->total > COOP_CHAR_WIRE_MAX
        || pkt->offset < 0 || pkt->offset > pkt->total - pkt->len) {
        coop_char_rx_free(rx);
        return false;
    }
    if (pkt->offset == 0) {
        coop_char_rx_free(rx);
        rx->data = (uint8_t*)malloc((size_t)pkt->total);
        if (rx->data == NULL) {
            return false;
        }
        rx->total = pkt->total;
    }
    if (rx->data == NULL || pkt->total != rx->total || pkt->offset != rx->got || rx->complete) {
        coop_char_rx_free(rx);
        return false;
    }
    memcpy(rx->data + rx->got, pkt->data, (size_t)pkt->len);
    rx->got += pkt->len;
    rx->complete = rx->got == rx->total;
    return true;
}

bool coop_lite_char_plausible(int level, int xp, const int* base_stats, int num_stats,
    int gold, int num_items)
{
    int i;

    if (level < 1 || level > 127 || xp < 0 || gold < 0 || gold > 1000000
        || num_items < 0 || num_items > COOP_CHAR_MAX_ITEMS) {
        return false;
    }
    for (i = 0; i < num_stats; i++) {
        if (base_stats[i] < 1 || base_stats[i] > 25) {
            return false;
        }
    }
    return true;
}

bool coop_lite_obj_req_valid(const PacketCoopObjReq* pkt, int size)
{
    if (pkt == NULL || pkt->type != PACKET_COOP_OBJ_REQ
        || size < (int)offsetof(PacketCoopObjReq, oid)) {
        return false;
    }
    if (pkt->count <= 0 || pkt->count > COOP_OBJ_REQ_MAX) {
        return false;
    }
    return (size_t)size >= offsetof(PacketCoopObjReq, oid) + (size_t)pkt->count * 24u;
}

bool coop_lite_obj_blob_valid(const PacketCoopObjBlob* pkt, int size)
{
    if (pkt == NULL || pkt->type != PACKET_COOP_OBJ_BLOB
        || size < (int)offsetof(PacketCoopObjBlob, data)) {
        return false;
    }
    if ((pkt->flags & COOP_OBJ_BLOB_F_GONE) != 0) {
        return pkt->size == 0;
    }
    /* version + proto oid + oid + type at least. */
    if (pkt->size < 4 + 24 + 24 + 4 || pkt->size > COOP_OBJ_BLOB_MAX) {
        return false;
    }
    if ((size_t)size < offsetof(PacketCoopObjBlob, data) + (size_t)pkt->size) {
        return false;
    }
    return memcmp(pkt->data + 4 + 24, pkt->oid, 24) == 0;
}

bool coop_lite_inven_sync_valid(const PacketCoopInvenSync* pkt, int size)
{
    if (pkt == NULL || pkt->type != PACKET_COOP_INVEN_SYNC) {
        return false;
    }
    if (size < (int)offsetof(PacketCoopInvenSync, entries)) {
        return false;
    }
    if (pkt->count < 0 || pkt->count > COOP_INVEN_CHUNK_ENTRIES) {
        return false;
    }
    if ((size_t)size < coop_lite_inven_sync_size(pkt->count)) {
        return false;
    }
    if (pkt->gold < 0) {
        return false;
    }
    return (pkt->flags & ~(COOP_INVEN_BATCH_FIRST | COOP_INVEN_BATCH_LAST)) == 0;
}

bool coop_lite_client_transfer_flags_allowed(unsigned int flags)
{
    /* 0x01 = move into the target, 0x20 = whole stack (no quantity box),
       0x02 = drop on the ground (P1-3; host checks the item is the PC's). */
    if ((flags & 0x02u) != 0) {
        return (flags & ~0x22u) == 0;
    }
    return (flags & 0x01u) != 0 && (flags & ~0x21u) == 0;
}

#ifndef COOP_LITE_TEST_STUBS
#include "game/obj_flags.h"

typedef struct CoopBarterSession {
    bool valid;
    ObjectID npc;
} CoopBarterSession;
static CoopBarterSession coop_barter[8];

static bool coop_inv_active;

static void coop_lite_barter_reset(void)
{
    memset(coop_barter, 0, sizeof(coop_barter));
    coop_inv_active = false;
}

static const int coop_ammo_fld[COOP_INVEN_AMMO_TYPES] = {
    OBJ_F_CRITTER_ARROWS,
    OBJ_F_CRITTER_BULLETS,
    OBJ_F_CRITTER_POWER_CELLS,
    OBJ_F_CRITTER_FUEL,
};

static void coop_lite_put_oid(uint8_t* dst, int64_t obj)
{
    ObjectID oid;

    memset(&oid, 0, sizeof(oid));
    if (obj != OBJ_HANDLE_NULL) {
        oid = obj_get_id(obj);
    }
    memcpy(dst, &oid, sizeof(oid));
}

static int64_t coop_lite_get_oid_obj(const uint8_t* src)
{
    ObjectID oid;

    memcpy(&oid, src, sizeof(oid));
    if (oid.type == OID_TYPE_NULL) {
        return OBJ_HANDLE_NULL;
    }
    return obj_pool_perm_lookup(oid);
}

static bool coop_lite_is_stackable(int64_t item)
{
    int type = obj_field_int32_get(item, OBJ_F_TYPE);

    return type == OBJ_TYPE_GOLD || type == OBJ_TYPE_AMMO;
}

static int64_t coop_lite_store_of(int64_t npc)
{
    if (npc == OBJ_HANDLE_NULL || obj_field_int32_get(npc, OBJ_F_TYPE) != OBJ_TYPE_NPC) {
        return OBJ_HANDLE_NULL;
    }
    return obj_field_handle_get(npc, OBJ_F_NPC_SUBSTITUTE_INVENTORY);
}

/* Host → one client: the owners' inventories as one batch. */
static bool coop_party_inven_dirty;

static void coop_lite_host_send_inven_batch(int slot, const int64_t* owners, int num_owners)
{
    PacketCoopInvenSync* pkt;
    int64_t* items;
    int cnt;
    int idx;
    int o;
    int t;
    bool first = true;
    bool critter;

    if (slot <= 0 || slot >= 8 || num_owners <= 0) {
        return;
    }
    /* Anything that resyncs a slot may have changed what its PC wears. */
    coop_party_inven_dirty = true;
    pkt = (PacketCoopInvenSync*)MALLOC(sizeof(*pkt));
    for (o = 0; o < num_owners; o++) {
        int64_t owner = owners[o];

        if (owner == OBJ_HANDLE_NULL) {
            continue;
        }
        memset(pkt, 0, offsetof(PacketCoopInvenSync, entries));
        pkt->type = PACKET_COOP_INVEN_SYNC;
        coop_lite_put_oid(pkt->owner_oid, owner);
        critter = obj_type_is_critter(obj_field_int32_get(owner, OBJ_F_TYPE));
        pkt->gold = item_gold_get(owner);
        if (critter) {
            coop_lite_put_oid(pkt->gold_oid, obj_field_handle_get(owner, OBJ_F_CRITTER_GOLD));
        }
        for (t = 0; t < COOP_INVEN_AMMO_TYPES; t++) {
            pkt->ammo[t] = item_ammo_quantity_get(owner, t);
            if (critter) {
                coop_lite_put_oid(pkt->ammo_oid[t], obj_field_handle_get(owner, coop_ammo_fld[t]));
            }
        }

        cnt = item_list_get(owner, &items);
        for (idx = 0; idx <= cnt; idx++) {
            if (idx < cnt) {
                PacketCoopInvenEntry* e;

                if (coop_lite_is_stackable(items[idx])) {
                    continue;
                }
                if (pkt->count == COOP_INVEN_CHUNK_ENTRIES) {
                    pkt->flags = first ? COOP_INVEN_BATCH_FIRST : 0;
                    first = false;
                    tig_net_send_app(pkt, (int)coop_lite_inven_sync_size(pkt->count), slot);
                    pkt->count = 0;
                }
                e = &pkt->entries[pkt->count++];
                coop_lite_put_oid(e->oid, items[idx]);
                e->proto = sub_49B290(items[idx]);
                e->inventory_location = item_inventory_location_get(items[idx]);
                e->reserved = 0;
                e->item_flags = (unsigned int)obj_field_int32_get(items[idx], OBJ_F_ITEM_FLAGS);
                if (getenv("ARCANUM_DEV_INVEN_TRACE") != NULL) {
                    char oid_str[40];

                    objid_id_to_str(oid_str, obj_get_id(items[idx]));
                    tig_debug_printf("CoopLite: dev: inven sync slot %d: %s loc %d proto %d\n",
                        slot, oid_str, e->inventory_location, e->proto);
                }
                continue;
            }
            /* Owner's last chunk (possibly empty: it still carries gold). */
            pkt->flags = first ? COOP_INVEN_BATCH_FIRST : 0;
            first = false;
            if (o == num_owners - 1) {
                pkt->flags |= COOP_INVEN_BATCH_LAST;
            }
            tig_net_send_app(pkt, (int)coop_lite_inven_sync_size(pkt->count), slot);
        }
        item_list_free(items);
    }
    if (!first && num_owners > 0 && owners[num_owners - 1] == OBJ_HANDLE_NULL) {
        /* Last owner vanished: close the batch with an empty owner-less chunk. */
        memset(pkt, 0, offsetof(PacketCoopInvenSync, entries));
        pkt->type = PACKET_COOP_INVEN_SYNC;
        pkt->flags = COOP_INVEN_BATCH_LAST;
        tig_net_send_app(pkt, (int)coop_lite_inven_sync_size(0), slot);
    }
    FREE(pkt);
}

/* Host: re-send what a slot's barter window shows. */
static void coop_lite_host_sync_slot_inven(int slot)
{
    int64_t owners[3];
    int n = 0;
    int64_t pc = sub_4A2B60(slot);
    int64_t npc = OBJ_HANDLE_NULL;

    if (pc == OBJ_HANDLE_NULL) {
        return;
    }
    if (coop_barter[slot].valid) {
        npc = obj_pool_perm_lookup(coop_barter[slot].npc);
    }
    owners[n++] = pc;
    if (npc != OBJ_HANDLE_NULL) {
        owners[n++] = npc;
        if (coop_lite_store_of(npc) != OBJ_HANDLE_NULL) {
            owners[n++] = coop_lite_store_of(npc);
        }
    }
    coop_lite_host_send_inven_batch(slot, owners, n);
}

static bool coop_lite_barter_partner_ok(int64_t pc, int64_t npc)
{
    if (pc == OBJ_HANDLE_NULL || npc == OBJ_HANDLE_NULL) {
        return false;
    }
    if (critter_is_dead(pc) || critter_is_dead(npc)) {
        return false;
    }
    if (!coop_lite_npc_is_merchant(npc)) {
        return false;
    }
    return location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION),
               obj_field_int64_get(npc, OBJ_F_LOCATION))
        <= 12;
}

/* ---- P1-3: host handlers for the client item ops 26/27/28/125 ---- */
#define COOP_ITEM_REACH 3

static bool coop_lite_item_near(int64_t pc, int64_t loc)
{
    return location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION), loc) <= COOP_ITEM_REACH;
}

/* Everyone sees an item that just landed on the ground (the owner's own
   inventory copy is removed by the INVEN_SYNC sent first). */
static void coop_lite_host_announce_ground_item(int64_t item)
{
    Packet70 pkt;

    if (item == OBJ_HANDLE_NULL || (obj_field_int32_get(item, OBJ_F_FLAGS) & OF_INVENTORY) != 0) {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = 70;
    pkt.subtype = 0;
    pkt.s0.name = sub_49B290(item);
    pkt.s0.oid = obj_get_id(item);
    pkt.s0.loc = obj_field_int64_get(item, OBJ_F_LOCATION);
    tig_net_send_app_all(&pkt, sizeof(pkt));
}

static void coop_lite_host_sync_slot_and(int slot, int64_t extra)
{
    int64_t owners[2];
    int n = 0;

    owners[n++] = sub_4A2B60(slot);
    if (extra != OBJ_HANDLE_NULL && extra != owners[0]) {
        owners[n++] = extra;
    }
    if (owners[0] != OBJ_HANDLE_NULL) {
        coop_lite_host_send_inven_batch(slot, owners, n);
    }
}

/* Dev (ARCANUM_DEV_CLIENT_ITEMS=1 on the host): 5 s after a client PC is
   bound, give it a dagger so the client-side equip test has a weapon. */
static unsigned int coop_dev_give_ms[8];
static bool coop_dev_give_done[8];

static void coop_lite_host_dev_give_weapon_tick(unsigned int now_ms)
{
    const char* env = getenv("ARCANUM_DEV_CLIENT_ITEMS");
    int slot;

    if (env == NULL || env[0] != '1') {
        return;
    }
    for (slot = 1; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);
        int64_t dagger;

        if (pc == OBJ_HANDLE_NULL || coop_dev_give_done[slot]) {
            continue;
        }
        if (coop_dev_give_ms[slot] == 0) {
            coop_dev_give_ms[slot] = now_ms != 0 ? now_ms : 1;
            continue;
        }
        if (!coop_lite_interval_elapsed(coop_dev_give_ms[slot], now_ms, 5000u)) {
            continue;
        }
        coop_dev_give_done[slot] = true;
        multiplayer_lock();
        if (object_create(sub_4685A0(BP_DAGGER), obj_field_int64_get(pc, OBJ_F_LOCATION), &dagger)) {
            item_transfer(dagger, pc);
        }
        multiplayer_unlock();
        tig_debug_printf("CoopLite: dev items: gave slot %d a dagger\n", slot);
        coop_lite_host_sync_slot_inven(slot);
    }
}

void coop_lite_host_handle_item_op(int type, void* msg)
{
    int slot;
    int64_t pc;
    int64_t item = OBJ_HANDLE_NULL;
    int64_t parent = OBJ_HANDLE_NULL;
    int64_t extra = OBJ_HANDLE_NULL;
    bool ok = false;
    bool was_on_ground = false;
    const char* what = "?";

    if (msg == NULL || !tig_net_is_host()) {
        return;
    }
    slot = sub_52A530();
    if (slot <= 0 || slot >= 8) {
        return;
    }
    pc = sub_4A2B60(slot);
    if (pc == OBJ_HANDLE_NULL || critter_is_dead(pc)) {
        return;
    }

    multiplayer_lock();
    switch (type) {
    case 26: {
        Packet26* pkt = (Packet26*)msg;
        int distance = pkt->field_20 < 0 ? 0 : (pkt->field_20 > 4 ? 4 : pkt->field_20);

        what = "drop";
        item = obj_pool_perm_lookup(pkt->oid);
        if (item != OBJ_HANDLE_NULL && item_parent(item, &parent) && parent == pc) {
            ok = item_drop_ex(item, distance);
        }
        break;
    }
    case 27: {
        Packet27* pkt = (Packet27*)msg;

        what = "drop at";
        item = obj_pool_perm_lookup(pkt->oid);
        if (item != OBJ_HANDLE_NULL && item_parent(item, &parent) && parent == pc
            && coop_lite_item_near(pc, pkt->loc) && item_check_remove(item) == ITEM_CANNOT_OK) {
            item_remove(item);
            sub_466E50(item, pkt->loc);
            ok = true;
        }
        break;
    }
    case 28: {
        Packet28* pkt = (Packet28*)msg;

        what = "transfer";
        item = obj_pool_perm_lookup(pkt->item_oid);
        if (item == OBJ_HANDLE_NULL || obj_pool_perm_lookup(pkt->critter_oid) != pc) {
            break; /* only into the sender's own PC */
        }
        if (!item_parent(item, &parent)) {
            /* From the ground: in reach, really on the ground. */
            if ((obj_field_int32_get(item, OBJ_F_FLAGS) & (OF_INVENTORY | OF_DESTROYED)) != 0
                || !coop_lite_item_near(pc, obj_field_int64_get(item, OBJ_F_LOCATION))) {
                break;
            }
            was_on_ground = true;
        } else if (parent != pc) {
            /* Loot from an unlocked container in reach; never from critters. */
            if (obj_field_int32_get(parent, OBJ_F_TYPE) != OBJ_TYPE_CONTAINER
                || object_locked_get(parent)
                || !coop_lite_item_near(pc, obj_field_int64_get(parent, OBJ_F_LOCATION))) {
                break;
            }
            extra = parent;
        }
        /* parent == pc: rearrange / equip (inventory_location). */
        ok = item_transfer_ex(item, pc, pkt->inventory_location);
        break;
    }
    case 125: {
        PacketPerformIdentifyService* pkt = (PacketPerformIdentifyService*)msg;
        int64_t npc = obj_pool_perm_lookup(pkt->npc_oid);

        what = "identify";
        item = obj_pool_perm_lookup(pkt->item_oid);
        if (item != OBJ_HANDLE_NULL && obj_pool_perm_lookup(pkt->pc_oid) == pc
            && item_parent(item, &parent) && parent == pc
            && coop_lite_barter_partner_ok(pc, npc)
            && pkt->cost >= 0 && pkt->cost <= item_gold_get(pc)) {
            item_perform_identify_service(item, npc, pc, pkt->cost);
            ok = true;
        }
        break;
    }
    default:
        break;
    }
    multiplayer_unlock();

    {
        int64_t w = item_wield_get(pc, ITEM_INV_LOC_WEAPON);
        char wname[256];

        wname[0] = '\0';
        if (w != OBJ_HANDLE_NULL) {
            object_examine(w, pc, wname);
        }
        tig_debug_printf("CoopLite: item op: slot %d %s %s (weapon now '%s')\n", slot, what,
            ok ? "done" : "refused", wname);
    }
    /* Ground first: the owner moves its own copy out of the inventory
       (keeping its state), then the resync finds nothing to remove. */
    if (ok && (type == 26 || type == 27)) {
        coop_lite_host_announce_ground_item(item);
    }
    /* Always resync, so a refused op is undone on the client. */
    coop_lite_host_sync_slot_and(slot, extra);
    if (ok && was_on_ground) {
        PacketObjectDestroy gone;

        memset(&gone, 0, sizeof(gone));
        gone.type = 72;
        gone.oid = obj_get_id(item);
        tig_net_send_app_except(&gone, sizeof(gone), slot);
    }
}
#endif

void coop_lite_handle_barter(PacketCoopBarter* pkt)
{
#ifndef COOP_LITE_TEST_STUBS
    int slot;
    int64_t pc;
    int64_t npc;
    int64_t store;
    ObjectID oid;

    if (pkt == NULL || pkt->type != PACKET_COOP_BARTER || !tig_net_is_host()) {
        return;
    }
    slot = sub_52A530();
    if (slot <= 0 || slot >= 8) {
        return;
    }
    pc = sub_4A2B60(slot);
    if (pc == OBJ_HANDLE_NULL || coop_lite_get_oid_obj(pkt->pc_oid) != pc) {
        tig_debug_printf("CoopLite: barter: slot %d request for a PC it does not own\n", slot);
        return;
    }
    if (pkt->kind == COOP_BARTER_CLOSE) {
        coop_barter[slot].valid = false;
        return;
    }
    if (pkt->kind == COOP_BARTER_SYNC) {
        coop_barter[slot].valid = false;
        coop_lite_host_sync_slot_inven(slot);
        return;
    }
    if (pkt->kind != COOP_BARTER_OPEN) {
        return;
    }
    npc = coop_lite_get_oid_obj(pkt->npc_oid);
    if (!coop_lite_barter_partner_ok(pc, npc)) {
        tig_debug_printf("CoopLite: barter: slot %d refused (not a merchant / too far / dead)\n", slot);
        coop_barter[slot].valid = false;
        coop_lite_host_sync_slot_inven(slot);
        return;
    }
    oid = obj_get_id(npc);
    coop_barter[slot].valid = true;
    coop_barter[slot].npc = oid;

    /* What inven_ui does on open (sub_572640): the store's gold goes to the
       merchant and the stock is identified. */
    store = coop_lite_store_of(npc);
    if (store != OBJ_HANDLE_NULL) {
        int amt = item_gold_get(store);

        if (amt > 0) {
            item_gold_transfer(store, npc, amt, OBJ_HANDLE_NULL);
        }
    }
    if (critter_pc_leader_get(npc) == OBJ_HANDLE_NULL) {
        item_identify_all(npc);
        if (store != OBJ_HANDLE_NULL) {
            item_identify_all(store);
        }
    }
    tig_debug_printf("CoopLite: barter: slot %d opened barter with a merchant (gold %d)\n",
        slot, item_gold_get(npc));
    coop_lite_host_sync_slot_inven(slot);
#else
    (void)pkt;
#endif
}

void coop_lite_host_handle_inven_transfer(struct Packet81* pkt)
{
#ifndef COOP_LITE_TEST_STUBS
    int slot;
    int64_t pc;
    int64_t npc;
    int64_t store;
    int64_t from;
    int64_t to;
    int64_t item;
    int64_t parent = OBJ_HANDLE_NULL;
    int qty;
    int cost;
    Packet81* copy;

    if (pkt == NULL || !tig_net_is_host()) {
        return;
    }
    slot = sub_52A530();
    if (slot <= 0 || slot >= 8) {
        return;
    }
    pc = sub_4A2B60(slot);
    if (pc == OBJ_HANDLE_NULL || obj_pool_perm_lookup(pkt->field_A0) != pc) {
        tig_debug_printf("CoopLite: inven: slot %d transfer for a PC it does not own\n", slot);
        return;
    }
    npc = coop_barter[slot].valid ? obj_pool_perm_lookup(coop_barter[slot].npc) : OBJ_HANDLE_NULL;
    store = coop_lite_store_of(npc);
    item = obj_pool_perm_lookup(pkt->field_10);
    from = obj_pool_perm_lookup(pkt->field_28);
    to = obj_pool_perm_lookup(pkt->field_40);
    qty = pkt->field_60;

#define COOP_SIDE_OK(o) ((o) == pc || ((o) != OBJ_HANDLE_NULL && ((o) == npc || (o) == store)))
    if (((unsigned int)pkt->field_8 & 0x02u) != 0) {
        /* Drop from the PC's own inventory onto the ground. */
        bool ok = false;

        if (coop_lite_client_transfer_flags_allowed((unsigned int)pkt->field_8)
            && item != OBJ_HANDLE_NULL && from == pc
            && item_parent(item, &parent) && parent == pc
            && obj_field_int32_get(item, OBJ_F_TYPE) != OBJ_TYPE_GOLD
            && obj_field_int32_get(item, OBJ_F_TYPE) != OBJ_TYPE_AMMO) {
            multiplayer_lock();
            ok = item_drop(item);
            multiplayer_unlock();
        }
        tig_debug_printf("CoopLite: inven: slot %d drop %s\n", slot, ok ? "done" : "refused");
        if (ok) {
            coop_lite_host_announce_ground_item(item);
        }
        coop_lite_host_sync_slot_inven(slot);
        return;
    }
    if (!coop_lite_client_transfer_flags_allowed((unsigned int)pkt->field_8)
        || item == OBJ_HANDLE_NULL
        || !COOP_SIDE_OK(from) || !COOP_SIDE_OK(to)
        || (from != pc && to != pc)
        || !item_parent(item, &parent) || parent != from
        || qty <= 0
        || (npc != OBJ_HANDLE_NULL && !coop_lite_barter_partner_ok(pc, npc))
        || (npc == OBJ_HANDLE_NULL && from != to)
        || obj_pool_perm_lookup(pkt->field_70) != npc
        || (pkt->field_88.type != OID_TYPE_NULL
            && obj_pool_perm_lookup(pkt->field_88) != (store != OBJ_HANDLE_NULL ? store : npc))) {
        tig_debug_printf("CoopLite: inven: slot %d transfer refused (flags 0x%x)\n", slot, pkt->field_8);
        coop_lite_host_sync_slot_inven(slot);
        return;
    }
#undef COOP_SIDE_OK

    /* Price: never the client's number. Seller first (item_cost). */
    cost = 0;
    if (npc != OBJ_HANDLE_NULL && from != to) {
        cost = from == pc
            ? item_cost(item, pc, npc, false)
            : item_cost(item, npc, pc, false);
        if (cost <= 0 && from != pc) {
            tig_debug_printf("CoopLite: inven: merchant will not sell that\n");
            coop_lite_host_sync_slot_inven(slot);
            return;
        }
        if (cost <= 0) {
            tig_debug_printf("CoopLite: inven: merchant will not buy that\n");
            coop_lite_host_sync_slot_inven(slot);
            return;
        }
        if (item_gold_get(to == pc ? pc : npc) < cost * qty) {
            tig_debug_printf("CoopLite: inven: not enough gold (%d < %d)\n",
                item_gold_get(to == pc ? pc : npc), cost * qty);
            coop_lite_host_sync_slot_inven(slot);
            return;
        }
    }

    copy = (Packet81*)MALLOC(sizeof(*copy));
    *copy = *pkt;
    copy->field_5C = cost;
    tig_debug_printf("CoopLite: inven: slot %d transfer qty %d cost %d\n", slot, qty, cost);
    sub_460960(copy); /* inven_ui's transfer (sub_578EA0); frees `copy` */

    /* Bought items are identified (inven_ui does it only for its own mode). */
    if (to == pc && npc != OBJ_HANDLE_NULL && item_parent(item, &parent) && parent == pc) {
        obj_field_int32_set(item, OBJ_F_ITEM_FLAGS,
            obj_field_int32_get(item, OBJ_F_ITEM_FLAGS) | OIF_IDENTIFIED);
    }
    coop_lite_host_sync_slot_inven(slot);
#else
    (void)pkt;
#endif
}

void coop_lite_client_request_barter(int64_t pc_obj, int64_t npc_obj, int kind)
{
#ifndef COOP_LITE_TEST_STUBS
    PacketCoopBarter pkt;
    int depth;

    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_BARTER;
    pkt.kind = kind;
    coop_lite_put_oid(pkt.pc_oid, pc_obj);
    coop_lite_put_oid(pkt.npc_oid, npc_obj);
    /* The click may run inside the PC's suspended SP path. */
    depth = tig_net_local_suspend_pause();
    if (tig_net_is_active() && !tig_net_is_host()) {
        tig_net_send_app_all(&pkt, sizeof(pkt));
    }
    tig_net_local_suspend_resume(depth);
#else
    (void)pc_obj;
    (void)npc_obj;
    (void)kind;
#endif
}

bool coop_lite_client_opens_inven_locally(int mode, int64_t pc_obj)
{
#ifndef COOP_LITE_TEST_STUBS
    /* INVEN_UI_MODE_INVENTORY = 0, INVEN_UI_MODE_BARTER = 1 */
    if (!coop_lite_is_client_role()) {
        return false;
    }
    if (mode != 0 && mode != 1) {
        return false;
    }
    return pc_obj != OBJ_HANDLE_NULL && pc_obj == player_get_local_pc_obj();
#else
    (void)mode;
    (void)pc_obj;
    return false;
#endif
}

#ifndef COOP_LITE_TEST_STUBS
/* Client: one batch, gathered over several chunks. */
#define COOP_INV_BUF_OWNERS 4
#define COOP_INV_BUF_ENTRIES 1024
typedef struct CoopInvBufOwner {
    PacketCoopInvenSync head; /* header fields only */
    int first;
    int count;
} CoopInvBufOwner;
static CoopInvBufOwner coop_inv_owner[COOP_INV_BUF_OWNERS];
static int coop_inv_owner_cnt;
static PacketCoopInvenEntry coop_inv_entry[COOP_INV_BUF_ENTRIES];
static int coop_inv_entry_cnt;
static bool coop_inv_overflow;

static bool coop_lite_inv_listed(const CoopInvBufOwner* o, ObjectID oid)
{
    int i;

    for (i = o->first; i < o->first + o->count; i++) {
        if (memcmp(coop_inv_entry[i].oid, &oid, sizeof(oid)) == 0) {
            return true;
        }
    }
    return false;
}

static void coop_lite_inv_destroy_stray(const uint8_t* oid_bytes)
{
    int64_t obj = coop_lite_get_oid_obj(oid_bytes);

    /* A host-created stack reached us as a ground Packet70 object. */
    if (obj != OBJ_HANDLE_NULL
        && (obj_field_int32_get(obj, OBJ_F_FLAGS) & OF_INVENTORY) == 0) {
        object_destroy(obj);
    }
}

/* ---- P1-2: item object blobs ---- */
#define COOP_OBJ_PENDING_MAX 64
#define COOP_OBJ_BLOB_TIMEOUT_MS 3000u

typedef struct CoopObjPending {
    uint8_t oid[24];
    unsigned int asked_ms;
    bool used;
    bool queued;
    bool ground; /* a Packet70 ground item: create at ground_loc */
    int64_t ground_loc;
    int ground_proto;
} CoopObjPending;

static CoopObjPending coop_obj_pending[COOP_OBJ_PENDING_MAX];

static void coop_lite_obj_pending_reset(void)
{
    memset(coop_obj_pending, 0, sizeof(coop_obj_pending));
}

static bool coop_lite_obj_blobs_enabled(void)
{
    const char* env = getenv("ARCANUM_COOP_NO_OBJ_BLOB");

    return env == NULL || env[0] != '1';
}

static CoopObjPending* coop_lite_obj_pending_find(const uint8_t* oid)
{
    int i;

    for (i = 0; i < COOP_OBJ_PENDING_MAX; i++) {
        if (coop_obj_pending[i].used && memcmp(coop_obj_pending[i].oid, oid, 24) == 0) {
            return &coop_obj_pending[i];
        }
    }
    return NULL;
}

/* true: skip this entry now (a blob is on its way). false: create it from
   the proto (blobs off, table full, or the blob never came). */
static bool coop_lite_obj_blob_defer(const uint8_t* oid)
{
    unsigned int now = coop_obj_now_ms;
    CoopObjPending* p;
    int i;

    if (!coop_lite_obj_blobs_enabled()) {
        return false;
    }
    p = coop_lite_obj_pending_find(oid);
    if (p != NULL) {
        if (coop_lite_interval_elapsed(p->asked_ms, now, COOP_OBJ_BLOB_TIMEOUT_MS)) {
            tig_debug_printf("CoopLite: obj blob: timed out, using the proto\n");
            p->used = false;
            return false;
        }
        return true;
    }
    for (i = 0; i < COOP_OBJ_PENDING_MAX; i++) {
        if (!coop_obj_pending[i].used) {
            memcpy(coop_obj_pending[i].oid, oid, 24);
            coop_obj_pending[i].asked_ms = now != 0 ? now : 1;
            coop_obj_pending[i].used = true;
            coop_obj_pending[i].queued = true;
            coop_obj_pending[i].ground = false;
            return true;
        }
    }
    return false;
}

static void coop_lite_obj_req_flush(void)
{
    PacketCoopObjReq pkt;
    int i;

    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_OBJ_REQ;
    for (i = 0; i <= COOP_OBJ_PENDING_MAX; i++) {
        if (pkt.count == COOP_OBJ_REQ_MAX || (i == COOP_OBJ_PENDING_MAX && pkt.count > 0)) {
            tig_net_send_app(&pkt, (int)(offsetof(PacketCoopObjReq, oid) + (size_t)pkt.count * 24u), 0);
            pkt.count = 0;
        }
        if (i < COOP_OBJ_PENDING_MAX && coop_obj_pending[i].used && coop_obj_pending[i].queued) {
            coop_obj_pending[i].queued = false;
            memcpy(pkt.oid[pkt.count++], coop_obj_pending[i].oid, 24);
        }
    }
}

bool coop_lite_client_want_ground_blob(const unsigned char oid[24], int64_t loc, int proto)
{
    CoopObjPending* p;

    if (tig_net_is_host() || !coop_lite_obj_blobs_enabled()) {
        return false;
    }
    if (coop_lite_obj_pending_find(oid) != NULL) {
        return true;
    }
    if (!coop_lite_obj_blob_defer(oid)) {
        return false;
    }
    p = coop_lite_obj_pending_find(oid);
    p->ground = true;
    p->ground_loc = loc;
    p->ground_proto = proto;
    coop_lite_obj_req_flush();
    return true;
}

/* Client: show each remote PC where it was COOP_INTERP_DELAY_MS ago. */
static void coop_lite_client_interp_tick(unsigned int now_ms)
{
    int64_t local_pc = player_get_local_pc_obj();
    int slot;

    if (!coop_lite_client_world_ready() || coop_lite_waiting_sync || coop_pending_sync_valid) {
        return;
    }
    for (slot = 0; slot < 8; slot++) {
        CoopInterpRing* ring = &coop_interp[slot];
        int64_t obj;
        int64_t loc;
        int ox;
        int oy;
        unsigned int art;
        int ia = 0;
        int ib = 0;
        unsigned int num = 0;
        unsigned int den = 1;
        int kind;

        if (ring->n == 0) {
            continue;
        }
        obj = sub_4A2B60(slot);
        if (obj == OBJ_HANDLE_NULL || obj == local_pc || critter_is_dead(obj)) {
            continue;
        }
        kind = coop_interp_pick(ring, now_ms - COOP_INTERP_DELAY_MS, &ia, &ib, &num, &den);
        if (kind == 0) {
            continue;
        }
        if (kind == 1) {
            loc = ring->s[ia].loc;
            ox = ring->s[ia].offset_x;
            oy = ring->s[ia].offset_y;
            art = ring->s[ia].art_id;
        } else {
            const CoopInterpSample* a = &ring->s[ia];
            const CoopInterpSample* b = &ring->s[ib];
            int64_t ax;
            int64_t ay;
            int64_t bx;
            int64_t by;
            bool rel_b = num * 2 >= den;

            location_xy(a->loc, &ax, &ay);
            location_xy(b->loc, &bx, &by);
            loc = rel_b ? b->loc : a->loc;
            ox = coop_interp_offset(a->offset_x, b->offset_x, bx - ax, num, den, rel_b);
            oy = coop_interp_offset(a->offset_y, b->offset_y, by - ay, num, den, rel_b);
            art = b->art_id; /* the newer sample's frame */
        }
        if (obj_field_int64_get(obj, OBJ_F_LOCATION) != loc
            || obj_field_int32_get(obj, OBJ_F_OFFSET_X) != ox
            || obj_field_int32_get(obj, OBJ_F_OFFSET_Y) != oy
            || (unsigned int)obj_field_int32_get(obj, OBJ_F_CURRENT_AID) != art) {
            multiplayer_lock();
            if (obj_field_int64_get(obj, OBJ_F_LOCATION) != loc
                || obj_field_int32_get(obj, OBJ_F_OFFSET_X) != ox
                || obj_field_int32_get(obj, OBJ_F_OFFSET_Y) != oy) {
                sub_43E770(obj, loc, ox, oy);
            }
            if ((unsigned int)obj_field_int32_get(obj, OBJ_F_CURRENT_AID) != art) {
                object_set_current_aid(obj, (tig_art_id_t)art);
            }
            multiplayer_unlock();
        }
    }
}

/* Ground items whose blob never came: the old proto creation. */
static void coop_lite_obj_ground_fallback_tick(unsigned int now_ms)
{
    int i;

    for (i = 0; i < COOP_OBJ_PENDING_MAX; i++) {
        CoopObjPending* p = &coop_obj_pending[i];
        int64_t proto_obj;
        int64_t obj = OBJ_HANDLE_NULL;
        ObjectID oid;

        if (!p->used || !p->ground
            || !coop_lite_interval_elapsed(p->asked_ms, now_ms, COOP_OBJ_BLOB_TIMEOUT_MS)) {
            continue;
        }
        p->used = false;
        memcpy(&oid, p->oid, sizeof(oid));
        proto_obj = sub_4685A0(p->ground_proto);
        if (proto_obj == OBJ_HANDLE_NULL || obj_pool_perm_lookup(oid) != OBJ_HANDLE_NULL) {
            continue;
        }
        tig_debug_printf("CoopLite: obj blob: ground item timed out, using the proto\n");
        multiplayer_lock();
        tig_net_local_suspend_begin();
        object_create_ex(proto_obj, p->ground_loc, oid, &obj);
        tig_net_local_suspend_end();
        multiplayer_unlock();
    }
}

void coop_lite_handle_obj_req(PacketCoopObjReq* pkt)
{
#ifndef COOP_LITE_TEST_STUBS
    PacketCoopObjBlob* out;
    int slot;
    int i;
    int sent = 0;

    if (!tig_net_is_host() || !coop_lite_obj_req_valid(pkt, tig_net_last_message_size())) {
        return;
    }
    slot = sub_52A530();
    if (slot <= 0 || slot >= 8 || sub_4A2B60(slot) == OBJ_HANDLE_NULL) {
        return;
    }
    out = (PacketCoopObjBlob*)MALLOC(sizeof(*out));
    for (i = 0; i < pkt->count; i++) {
        int64_t item = coop_lite_get_oid_obj(pkt->oid[i]);
        int64_t parent = OBJ_HANDLE_NULL;
        uint8_t* data = NULL;
        int size = 0;

        if (item != OBJ_HANDLE_NULL && item == sub_4A2B60(slot)) {
            /* Rejoin: the sender's own (restored) PC. */
            parent = OBJ_HANDLE_NULL;
        } else if (item != OBJ_HANDLE_NULL && item_is_item(item) && !coop_lite_is_stackable(item)
            && (obj_field_int32_get(item, OBJ_F_FLAGS) & (OF_INVENTORY | OF_DESTROYED)) == 0) {
            /* A ground item (Packet70 announcement). */
            parent = OBJ_HANDLE_NULL;
        } else if (item == OBJ_HANDLE_NULL || !item_is_item(item) || coop_lite_is_stackable(item)
            || (obj_field_int32_get(item, OBJ_F_FLAGS) & OF_DESTROYED) != 0
            || !item_parent(item, &parent) || parent == OBJ_HANDLE_NULL) {
            memset(out, 0, offsetof(PacketCoopObjBlob, data));
            out->type = PACKET_COOP_OBJ_BLOB;
            out->flags = COOP_OBJ_BLOB_F_GONE;
            memcpy(out->oid, pkt->oid[i], 24);
            tig_net_send_app(out, (int)offsetof(PacketCoopObjBlob, data), slot);
            continue;
        }
        sub_442050(&data, &size, item);
        if (data != NULL && size > 0 && size <= COOP_OBJ_BLOB_MAX) {
            memset(out, 0, offsetof(PacketCoopObjBlob, data));
            out->type = PACKET_COOP_OBJ_BLOB;
            out->size = size;
            coop_lite_put_oid(out->oid, item);
            if (parent != OBJ_HANDLE_NULL) {
                coop_lite_put_oid(out->owner_oid, parent);
                out->inventory_location = item_inventory_location_get(item);
            }
            memcpy(out->data, data, (size_t)size);
            tig_net_send_app(out, (int)(offsetof(PacketCoopObjBlob, data) + (size_t)size), slot);
            sent++;
        }
        if (data != NULL) {
            FREE(data);
        }
    }
    FREE(out);
    tig_debug_printf("CoopLite: obj blob: slot %d asked %d, sent %d\n", slot, pkt->count, sent);
#else
    (void)pkt;
#endif
}

/* Client rejoin: the host's copy of our PC replaces the local one. */
static unsigned char coop_rejoin_pc_oid[24];

static void coop_lite_client_request_own_pc(const unsigned char oid[24])
{
    memcpy(coop_rejoin_pc_oid, oid, 24);
    coop_rejoin_pc_wanted = true;
    if (coop_lite_obj_pending_find(oid) == NULL) {
        coop_lite_obj_blob_defer(oid);
    }
    coop_lite_obj_req_flush();
    tig_debug_printf("CoopLite: rejoin: asking the host for our parked PC\n");
}

static void coop_lite_client_adopt_own_pc_blob(PacketCoopObjBlob* pkt)
{
    int64_t old_pc = player_get_local_pc_obj();
    int64_t pc = OBJ_HANDLE_NULL;
    int64_t loc;
    int slot = sub_529520();
    int t;

    if (old_pc == OBJ_HANDLE_NULL || slot <= 0 || slot >= 8) {
        return;
    }
    loc = obj_field_int64_get(old_pc, OBJ_F_LOCATION);
    multiplayer_lock();
    tig_net_local_suspend_begin();
    if (!sub_4420D0(pkt->data, &pc, loc) || pc == OBJ_HANDLE_NULL
        || obj_field_int32_get(pc, OBJ_F_TYPE) != OBJ_TYPE_PC) {
        if (pc != OBJ_HANDLE_NULL) {
            object_destroy(pc);
        }
        tig_net_local_suspend_end();
        multiplayer_unlock();
        tig_debug_printf("CoopLite: rejoin: PC blob rejected — keeping the menu character\n");
        coop_lite_client_request_barter(old_pc, OBJ_HANDLE_NULL, COOP_BARTER_SYNC);
        return;
    }
    /* Its item handles did not resolve here (the items come with the next
       INVEN_SYNC as blobs): start from an empty inventory. */
    obj_field_int32_set(pc, OBJ_F_CRITTER_INVENTORY_NUM, 0);
    obj_arrayfield_length_set(pc, OBJ_F_CRITTER_INVENTORY_LIST_IDX, 0);
    obj_field_handle_set(pc, OBJ_F_CRITTER_GOLD, OBJ_HANDLE_NULL);
    for (t = 0; t < COOP_INVEN_AMMO_TYPES; t++) {
        obj_field_handle_set(pc, coop_ammo_fld[t], OBJ_HANDLE_NULL);
    }
    object_flags_unset(pc, OF_OFF);
    sub_40DAF0(pc);
    multiplayer_bind_slot_pc(slot, pc);
    object_destroy(old_pc);
    tig_net_local_suspend_end();
    multiplayer_unlock();

    coop_client_pos_valid = false;
    location_origin_set(obj_field_int64_get(pc, OBJ_F_LOCATION));
    tig_debug_printf("CoopLite: rejoin: parked PC restored (level %d, %d bytes)\n",
        stat_level_get(pc, STAT_LEVEL), pkt->size);
    coop_lite_client_request_barter(pc, OBJ_HANDLE_NULL, COOP_BARTER_SYNC);
}

void coop_lite_handle_obj_blob(PacketCoopObjBlob* pkt)
{
#ifndef COOP_LITE_TEST_STUBS
    CoopObjPending* p;
    bool ground;
    int64_t ground_loc;
    int64_t owner;
    int64_t item = OBJ_HANDLE_NULL;
    int64_t local_pc = player_get_local_pc_obj();
    int loc;
    int free_loc;
    unsigned int flags;
    char name[64] = "?";

    if (!tig_net_is_active() || tig_net_is_host()
        || !coop_lite_obj_blob_valid(pkt, tig_net_last_message_size())) {
        return;
    }
    p = coop_lite_obj_pending_find(pkt->oid);
    if (p == NULL) {
        /* Unasked (or already timed out and created from the proto). */
        return;
    }
    ground = p->ground;
    ground_loc = p->ground_loc;
    p->used = false;
    if ((pkt->flags & COOP_OBJ_BLOB_F_GONE) != 0) {
        /* An inventory item is asked for again by the next sync that
           lists it; a ground item just isn't there any more. */
        return;
    }
    if (!coop_lite_client_world_ready() || coop_lite_get_oid_obj(pkt->oid) != OBJ_HANDLE_NULL) {
        return;
    }
    if (ground) {
        multiplayer_lock();
        tig_net_local_suspend_begin();
        if (!sub_4420D0(pkt->data, &item, ground_loc) || item == OBJ_HANDLE_NULL || !item_is_item(item)) {
            if (item != OBJ_HANDLE_NULL) {
                object_destroy(item);
            }
            item = OBJ_HANDLE_NULL;
        }
        tig_net_local_suspend_end();
        multiplayer_unlock();
        tig_debug_printf("CoopLite: obj blob: ground item %s (%d bytes)\n",
            item != OBJ_HANDLE_NULL ? "loaded" : "rejected", pkt->size);
        return;
    }
    if (coop_rejoin_pc_wanted && memcmp(pkt->oid, coop_rejoin_pc_oid, 24) == 0) {
        coop_rejoin_pc_wanted = false;
        coop_lite_client_adopt_own_pc_blob(pkt);
        return;
    }
    owner = coop_lite_get_oid_obj(pkt->owner_oid);
    if (owner == OBJ_HANDLE_NULL
        || (!obj_type_is_critter(obj_field_int32_get(owner, OBJ_F_TYPE))
            && obj_field_int32_get(owner, OBJ_F_TYPE) != OBJ_TYPE_CONTAINER)) {
        return; /* §7: container contents come as blobs too */
    }

    multiplayer_lock();
    tig_net_local_suspend_begin();
    if (obj_read_mem(pkt->data, &item) && item != OBJ_HANDLE_NULL) {
        obj_load_postprocess(item);
        if (!item_is_item(item) || coop_lite_is_stackable(item)) {
            object_destroy(item);
            item = OBJ_HANDLE_NULL;
        }
    } else {
        item = OBJ_HANDLE_NULL;
    }
    if (item != OBJ_HANDLE_NULL) {
        loc = pkt->inventory_location;
        flags = (unsigned int)obj_field_int32_get(item, OBJ_F_FLAGS);
        obj_field_int32_set(item, OBJ_F_FLAGS, (int)(flags | OF_INVENTORY));
        obj_field_handle_set(item, OBJ_F_ITEM_PARENT, owner);
        if (IS_WEAR_INV_LOC(loc)) {
            if (item_wield_get(owner, loc) != OBJ_HANDLE_NULL
                && item_check_insert(item, owner, &free_loc) == ITEM_CANNOT_OK) {
                loc = free_loc;
            }
        } else if (owner == local_pc) {
            /* The host's grid cell when it is free here (PT14: positions
               were reshuffled), else any free one. */
            static int slots[960];

            memset(slots, 0, sizeof(slots));
            item_inventory_slots_get(owner, slots);
            if ((loc < 0 || loc >= 120 || !item_inventory_slots_has_room_for(item, owner, loc, slots))
                && item_check_insert(item, owner, &free_loc) == ITEM_CANNOT_OK) {
                loc = free_loc;
            }
        }
        item_insert(item, owner, loc);
        {
            char buf[256];

            buf[0] = '\0';
            object_examine(item, local_pc, buf);
            snprintf(name, sizeof(name), "%s", buf);
        }
    }
    tig_net_local_suspend_end();
    multiplayer_unlock();
    {
        char oid_str[40];
        ObjectID oid;

        memcpy(&oid, pkt->oid, sizeof(oid));
        objid_id_to_str(oid_str, oid);
        tig_debug_printf("CoopLite: obj blob: %s '%s' %s loc %d->%d (%d bytes)\n",
            item != OBJ_HANDLE_NULL ? "loaded" : "rejected", name, oid_str, pkt->inventory_location,
            item != OBJ_HANDLE_NULL ? item_inventory_location_get(item) : -1, pkt->size);
    }
    if (item != OBJ_HANDLE_NULL) {
        /* PT13: an open inventory window showed an empty grid for items
           that arrived after it was drawn. */
        ui_update_inven(owner);
    }
#else
    (void)pkt;
#endif
}

static void coop_lite_client_apply_inven_batch(void)
{
    int64_t owner_obj[COOP_INV_BUF_OWNERS];
    int64_t doomed[256];
    int64_t doomed_parent[256];
    int num_doomed = 0;
    int64_t local_pc = player_get_local_pc_obj();
    int moved = 0;
    int created = 0;
    int o;
    int i;
    int t;

    multiplayer_lock();
    tig_net_local_suspend_begin();

    for (o = 0; o < coop_inv_owner_cnt; o++) {
        owner_obj[o] = coop_lite_get_oid_obj(coop_inv_owner[o].head.owner_oid);
    }

    /* Unlisted (non-stack) items: gone, unless another owner lists them. */
    for (o = 0; o < coop_inv_owner_cnt; o++) {
        int64_t* items;
        int cnt;

        if (owner_obj[o] == OBJ_HANDLE_NULL) {
            continue;
        }
        cnt = item_list_get(owner_obj[o], &items);
        for (i = 0; i < cnt; i++) {
            if (coop_lite_is_stackable(items[i])) {
                continue;
            }
            if (!coop_lite_inv_listed(&coop_inv_owner[o], obj_get_id(items[i])) && num_doomed < 256) {
                doomed_parent[num_doomed] = owner_obj[o];
                doomed[num_doomed++] = items[i];
            }
        }
        item_list_free(items);
    }

    /* Listed items: into their owner (created with the host's OID if new). */
    for (o = 0; o < coop_inv_owner_cnt; o++) {
        int64_t owner = owner_obj[o];
        int64_t owner_loc;

        if (owner == OBJ_HANDLE_NULL) {
            continue;
        }
        owner_loc = obj_field_int64_get(owner, OBJ_F_LOCATION);
        for (i = coop_inv_owner[o].first; i < coop_inv_owner[o].first + coop_inv_owner[o].count; i++) {
            PacketCoopInvenEntry* e = &coop_inv_entry[i];
            int64_t item = coop_lite_get_oid_obj(e->oid);
            int64_t parent = OBJ_HANDLE_NULL;
            int loc = e->inventory_location;
            int free_loc;

            if (item != OBJ_HANDLE_NULL && (obj_field_int32_get(item, OBJ_F_FLAGS) & OF_DESTROYED) != 0) {
                /* Destroyed but not yet freed: never re-insert it (that left
                   a parentless duplicate in the inventory list). */
                continue;
            }
            if (item == OBJ_HANDLE_NULL && coop_lite_obj_blob_defer(e->oid)) {
                /* Asked the host for the full object; it is inserted when
                   the blob arrives (proto fallback after a timeout). */
                continue;
            }
            if (item == OBJ_HANDLE_NULL) {
                ObjectID oid;
                int64_t proto_obj;

                if (!coop_lite_pc_proto_plausible(e->proto)) {
                    continue;
                }
                proto_obj = sub_4685A0(e->proto);
                if (proto_obj == OBJ_HANDLE_NULL
                    || obj_field_int32_get(proto_obj, OBJ_F_TYPE) < OBJ_TYPE_WEAPON
                    || obj_field_int32_get(proto_obj, OBJ_F_TYPE) > OBJ_TYPE_GENERIC) {
                    continue;
                }
                memcpy(&oid, e->oid, sizeof(oid));
                if (!object_create_ex(proto_obj, owner_loc, oid, &item) || item == OBJ_HANDLE_NULL) {
                    continue;
                }
                created++;
            } else if (item_parent(item, &parent) && parent == owner) {
                /* Already there; our own PC keeps its local backpack
                   arrangement, but what is worn/wielded follows the host
                   (P1-3: host-side combat uses the host's equipment). */
                int cur = item_inventory_location_get(item);

                if (owner != local_pc && cur != loc) {
                    item_remove(item);
                    item_insert(item, owner, loc);
                } else if (owner == local_pc && cur != loc
                    && (IS_WEAR_INV_LOC(loc) || IS_WEAR_INV_LOC(cur))) {
                    item_remove(item);
                    if (IS_WEAR_INV_LOC(loc) && item_wield_get(owner, loc) == OBJ_HANDLE_NULL) {
                        item_insert(item, owner, loc);
                    } else if (item_check_insert(item, owner, &free_loc) == ITEM_CANNOT_OK) {
                        item_insert(item, owner, free_loc);
                    } else {
                        item_insert(item, owner, loc);
                    }
                    moved++;
                }
                goto flags;
            }

            if (parent != OBJ_HANDLE_NULL) {
                item_remove(item);
            } else if ((obj_field_int32_get(item, OBJ_F_FLAGS) & OF_INVENTORY) == 0) {
                object_pickup(item, owner);
            }
            if (owner == local_pc) {
                /* Host's slot may be taken locally: find room here. */
                if (!(IS_WEAR_INV_LOC(loc) && item_wield_get(owner, loc) == OBJ_HANDLE_NULL)
                    && item_check_insert(item, owner, &free_loc) == ITEM_CANNOT_OK) {
                    loc = free_loc;
                }
            }
            item_insert(item, owner, loc);
            moved++;

        flags:
            if ((e->item_flags & OIF_IDENTIFIED) != 0) {
                obj_field_int32_set(item, OBJ_F_ITEM_FLAGS,
                    obj_field_int32_get(item, OBJ_F_ITEM_FLAGS) | OIF_IDENTIFIED);
            }
        }
    }

    /* Doomed items nobody re-homed. */
    {
        int destroyed = 0;

        for (i = 0; i < num_doomed; i++) {
            int64_t parent = OBJ_HANDLE_NULL;

            if (item_parent(doomed[i], &parent) && parent == doomed_parent[i]) {
                object_destroy(doomed[i]);
                destroyed++;
            }
        }
        num_doomed = destroyed;
    }

    /* Stacks by total. */
    for (o = 0; o < coop_inv_owner_cnt; o++) {
        int64_t owner = owner_obj[o];
        int d;

        if (owner == OBJ_HANDLE_NULL) {
            continue;
        }
        d = coop_inv_owner[o].head.gold - item_gold_get(owner);
        if (d > 0) {
            item_gold_transfer(OBJ_HANDLE_NULL, owner, d, OBJ_HANDLE_NULL);
        } else if (d < 0) {
            item_gold_transfer(owner, OBJ_HANDLE_NULL, -d, OBJ_HANDLE_NULL);
        }
        coop_lite_inv_destroy_stray(coop_inv_owner[o].head.gold_oid);
        for (t = 0; t < COOP_INVEN_AMMO_TYPES; t++) {
            d = coop_inv_owner[o].head.ammo[t] - item_ammo_quantity_get(owner, t);
            if (d > 0) {
                item_ammo_transfer(OBJ_HANDLE_NULL, owner, d, t, OBJ_HANDLE_NULL);
            } else if (d < 0) {
                item_ammo_transfer(owner, OBJ_HANDLE_NULL, -d, t, OBJ_HANDLE_NULL);
            }
            coop_lite_inv_destroy_stray(coop_inv_owner[o].head.ammo_oid[t]);
        }
    }

    tig_net_local_suspend_end();
    multiplayer_unlock();
    coop_lite_obj_req_flush();

    /* §7 sends one batch per container: only log containers that changed. */
    if (moved != 0 || created != 0 || num_doomed != 0 || coop_inv_owner_cnt != 1 || owner_obj[0] == OBJ_HANDLE_NULL
        || obj_field_int32_get(owner_obj[0], OBJ_F_TYPE) != OBJ_TYPE_CONTAINER) {
        tig_debug_printf("CoopLite: inven sync: %d owners, %d entries, %d moved, %d created, %d removed, gold %d\n",
            coop_inv_owner_cnt, coop_inv_entry_cnt, moved, created, num_doomed,
            local_pc != OBJ_HANDLE_NULL ? item_gold_get(local_pc) : -1);
    }
    ui_update_inven(OBJ_HANDLE_NULL);
}
#endif

void coop_lite_handle_inven_sync(PacketCoopInvenSync* pkt)
{
#ifndef COOP_LITE_TEST_STUBS
    CoopInvBufOwner* o;

    if (!tig_net_is_active() || tig_net_is_host()) {
        return;
    }
    if (!coop_lite_inven_sync_valid(pkt, tig_net_last_message_size())) {
        tig_debug_printf("CoopLite: inven sync: malformed chunk dropped\n");
        return;
    }
    if (!coop_lite_client_world_ready()) {
        coop_inv_active = false;
        return;
    }
    if ((pkt->flags & COOP_INVEN_BATCH_FIRST) != 0) {
        coop_inv_active = true;
        coop_inv_overflow = false;
        coop_inv_owner_cnt = 0;
        coop_inv_entry_cnt = 0;
    }
    if (!coop_inv_active) {
        return;
    }

    {
        ObjectID owner_oid;

        memcpy(&owner_oid, pkt->owner_oid, sizeof(owner_oid));
        if (owner_oid.type == OID_TYPE_NULL) {
            goto last; /* batch terminator */
        }
    }
    {
        o = coop_inv_owner_cnt > 0 ? &coop_inv_owner[coop_inv_owner_cnt - 1] : NULL;
        if (o == NULL || memcmp(o->head.owner_oid, pkt->owner_oid, sizeof(pkt->owner_oid)) != 0) {
            if (coop_inv_owner_cnt == COOP_INV_BUF_OWNERS) {
                coop_inv_overflow = true;
                o = NULL;
            } else {
                o = &coop_inv_owner[coop_inv_owner_cnt++];
                memcpy(&o->head, pkt, offsetof(PacketCoopInvenSync, entries));
                o->first = coop_inv_entry_cnt;
                o->count = 0;
            }
        }
        if (o != NULL) {
            if (coop_inv_entry_cnt + pkt->count > COOP_INV_BUF_ENTRIES) {
                coop_inv_overflow = true;
            } else {
                memcpy(&coop_inv_entry[coop_inv_entry_cnt], pkt->entries,
                    (size_t)pkt->count * sizeof(PacketCoopInvenEntry));
                coop_inv_entry_cnt += pkt->count;
                o->count += pkt->count;
            }
        }
    }

last:
    if ((pkt->flags & COOP_INVEN_BATCH_LAST) != 0) {
        coop_inv_active = false;
        if (coop_inv_overflow) {
            tig_debug_printf("CoopLite: inven sync: batch too large, ignored\n");
            return;
        }
        coop_lite_client_apply_inven_batch();
    }
#else
    (void)pkt;
#endif
}

/* ---- Roster (review H3: 3+ players) ---- */
#ifndef COOP_LITE_TEST_STUBS
static void coop_lite_client_roster_reconcile(void)
{
    int slot;
    int64_t obj;

    if (tig_net_is_host() || !coop_lite_client_world_ready()) {
        return;
    }
    for (slot = 1; slot < 8; slot++) {
        if (slot == sub_529520() || !coop_slot_pc[slot].valid || coop_roster_loc[slot] == 0) {
            continue;
        }
        obj = obj_pool_perm_lookup(coop_slot_pc[slot].oid);
        if (obj != OBJ_HANDLE_NULL && sub_4A2B60(slot) == obj) {
            continue;
        }
        obj = coop_lite_resolve_remote_pc(slot, &coop_slot_pc[slot].oid, coop_roster_loc[slot]);
        if (obj != OBJ_HANDLE_NULL) {
            multiplayer_bind_slot_pc(slot, obj);
            tig_debug_printf("CoopLite: roster: slot %d PC bound after world-ready\n", slot);
        }
    }
}
#endif
bool coop_lite_roster_slot_ok(int slot, int local_slot)
{
    return slot > 0 && slot < 8 && slot != local_slot;
}

#ifndef COOP_LITE_TEST_STUBS
/* to_slot < 0: everyone except `slot` itself. */
static void coop_lite_host_send_roster_entry(int slot, int64_t pc, bool present, int to_slot)
{
    PacketCoopRoster pkt;

    if (!tig_net_is_host() || slot <= 0 || slot >= 8 || pc == OBJ_HANDLE_NULL) {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_ROSTER;
    pkt.slot = slot;
    coop_lite_put_oid(pkt.oid, pc);
    pkt.proto = sub_49B290(pc);
    pkt.present = present ? 1 : 0;
    pkt.loc = obj_field_int64_get(pc, OBJ_F_LOCATION);
    coop_lite_copy_pc_name(pc, pkt.name, sizeof(pkt.name));
    if (to_slot < 0) {
        tig_net_send_app_except(&pkt, sizeof(pkt), slot);
    } else {
        tig_net_send_app(&pkt, sizeof(pkt), to_slot);
    }
}

/*
 * P1-1: idempotent full roster. Every 5 s, and whenever the host map load
 * generation changes, every client gets every slot: present ones with
 * their PC, slots that left since with present=0. A missed one-shot entry
 * (sent before the receiver was world-ready, or lost with a reload) heals
 * on the next round.
 */
static unsigned char coop_roster_seen; /* bit per slot ever announced */
static unsigned int coop_roster_ms;
static uint16_t coop_roster_gen;

static void coop_lite_roster_reset(void)
{
    coop_roster_seen = 0;
    coop_roster_ms = 0;
    coop_roster_gen = 0;
    memset(coop_roster_loc, 0, sizeof(coop_roster_loc));
}

/* P1-2: every client gets the host's inventories of the *other* PCs
   (worn clothes/armour/weapon are what makes a remote PC look right), at
   most once a second after a change and every 10 s as a keyframe. One
   batch per PC so each stays atomic within the 4-owner buffer. */
static unsigned int coop_party_inven_ms;
static unsigned int coop_party_inven_key_ms;

static void coop_lite_host_party_inven_tick(unsigned int now_ms)
{
    int dst;
    int src;
    bool key;

    if (!coop_lite_interval_elapsed(coop_party_inven_ms, now_ms, 1000u)) {
        return;
    }
    key = coop_lite_interval_elapsed(coop_party_inven_key_ms, now_ms, 10000u);
    if (!coop_party_inven_dirty && !key) {
        return;
    }
    coop_party_inven_ms = now_ms;
    if (key) {
        coop_party_inven_key_ms = now_ms;
    }
    coop_party_inven_dirty = false;
    for (dst = 1; dst < 8; dst++) {
        if (sub_4A2B60(dst) == OBJ_HANDLE_NULL) {
            continue;
        }
        for (src = 0; src < 8; src++) {
            int64_t pc = src == 0 ? player_get_local_pc_obj() : sub_4A2B60(src);

            if (src == dst || pc == OBJ_HANDLE_NULL) {
                continue;
            }
            coop_lite_host_send_inven_batch(dst, &pc, 1);
        }
    }
    /* Sending marked it dirty again; that was us. */
    coop_party_inven_dirty = false;
}

static void coop_lite_host_roster_tick(unsigned int now_ms)
{
    int slot;
    int64_t pc;
    PacketCoopRoster pkt;

    if (!tig_net_is_host()) {
        return;
    }
    if (coop_roster_gen == coop_map_gen
        && !coop_lite_interval_elapsed(coop_roster_ms, now_ms, 5000u)) {
        return;
    }
    coop_roster_gen = coop_map_gen;
    coop_roster_ms = now_ms != 0 ? now_ms : 1;
    for (slot = 1; slot < 8; slot++) {
        pc = sub_4A2B60(slot);
        if (pc != OBJ_HANDLE_NULL) {
            coop_roster_seen |= (unsigned char)(1u << slot);
            coop_lite_host_send_roster_entry(slot, pc, true, -1);
        } else if ((coop_roster_seen & (1u << slot)) != 0 && coop_slot_pc[slot].valid) {
            memset(&pkt, 0, sizeof(pkt));
            pkt.type = PACKET_COOP_ROSTER;
            pkt.slot = slot;
            memcpy(pkt.oid, &coop_slot_pc[slot].oid, sizeof(pkt.oid));
            pkt.proto = coop_slot_pc[slot].proto;
            pkt.present = 0;
            tig_net_send_app_all(&pkt, sizeof(pkt));
        }
    }
}

static void coop_lite_host_announce_roster(int new_slot)
{
    int slot;
    int64_t pc;

    /* The newcomer learns everyone else; everyone else learns it. */
    for (slot = 1; slot < 8; slot++) {
        if (slot == new_slot) {
            continue;
        }
        pc = sub_4A2B60(slot);
        if (pc != OBJ_HANDLE_NULL) {
            coop_lite_host_send_roster_entry(slot, pc, true, new_slot);
        }
    }
    pc = sub_4A2B60(new_slot);
    if (pc != OBJ_HANDLE_NULL) {
        coop_lite_host_send_roster_entry(new_slot, pc, true, -1);
    }
}
#endif

void coop_lite_handle_roster(PacketCoopRoster* pkt)
{
#ifndef COOP_LITE_TEST_STUBS
    ObjectID oid;
    int64_t obj;
    char oid_str[80];

    if (pkt == NULL || pkt->type != PACKET_COOP_ROSTER) {
        return;
    }
    if (!tig_net_is_active() || tig_net_is_host()) {
        return;
    }
    if (!coop_lite_roster_slot_ok(pkt->slot, sub_529520())) {
        return;
    }
    memcpy(&oid, pkt->oid, sizeof(oid));
    objid_id_to_str(oid_str, oid);
    if (!pkt->present) {
        obj = coop_lite_take_slot_pc(pkt->slot);
        coop_lite_unkeep_sector(pkt->slot);
        tig_net_set_remote_slot_present(pkt->slot, false);
        if (obj != OBJ_HANDLE_NULL && obj != player_get_local_pc_obj()) {
            multiplayer_lock();
            tig_net_local_suspend_begin();
            object_destroy(obj);
            tig_net_local_suspend_end();
            multiplayer_unlock();
        }
        tig_debug_printf("CoopLite: roster: slot %d left (%s)\n", pkt->slot, oid_str);
        return;
    }
    if (!coop_lite_pc_proto_plausible(pkt->proto)) {
        return;
    }
    coop_lite_terminate_name(pkt->name, sizeof(pkt->name));
    {
        /* A different PC bound to this slot is a ghost of an earlier
           occupant (rejoin with a new character): drop it. */
        int64_t old = sub_4A2B60(pkt->slot);

        if (old != OBJ_HANDLE_NULL && old != player_get_local_pc_obj()) {
            ObjectID old_oid = obj_get_id(old);

            if (memcmp(&old_oid, &oid, sizeof(oid)) != 0) {
                multiplayer_lock();
                tig_net_local_suspend_begin();
                object_destroy(old);
                tig_net_local_suspend_end();
                multiplayer_unlock();
                tig_debug_printf("CoopLite: roster: slot %d ghost PC removed\n", pkt->slot);
            }
        }
    }
    coop_roster_loc[pkt->slot] = pkt->loc;
    coop_slot_pc[pkt->slot].valid = true;
    coop_slot_pc[pkt->slot].oid = oid;
    coop_slot_pc[pkt->slot].proto = pkt->proto;
    snprintf(coop_slot_pc[pkt->slot].name, sizeof(coop_slot_pc[pkt->slot].name), "%s", pkt->name);
    tig_net_set_remote_slot_present(pkt->slot, true);
    if (!coop_lite_client_world_ready()) {
        /* Created on the first PC_POS once we are in the world. */
        tig_debug_printf("CoopLite: roster: slot %d noted (%s), not in world yet\n", pkt->slot, oid_str);
        return;
    }
    if (sub_4A2B60(pkt->slot) != OBJ_HANDLE_NULL
        && obj_pool_perm_lookup(oid) == sub_4A2B60(pkt->slot)) {
        /* Periodic round: nothing to do (no log spam). */
        return;
    }
    obj = coop_lite_resolve_remote_pc(pkt->slot, &oid, pkt->loc);
    if (obj != OBJ_HANDLE_NULL) {
        multiplayer_bind_slot_pc(pkt->slot, obj);
    }
    tig_debug_printf("CoopLite: roster: slot %d PC %s %s\n", pkt->slot, oid_str,
        obj != OBJ_HANDLE_NULL ? "bound" : "unresolved");
#else
    (void)pkt;
#endif
}

static bool (*coop_inven_open_hook)(int64_t pc_obj, int64_t target_obj, int mode);

void coop_lite_set_dialog_end_hook(void (*hook)(int64_t pc_obj, int a2))
{
    coop_dialog_end_hook = hook;
}

void coop_lite_set_return_to_menu_hook(void (*hook)(void))
{
    coop_return_to_menu_hook = hook;
}

void coop_lite_set_inven_open_hook(bool (*hook)(int64_t pc_obj, int64_t target_obj, int mode))
{
    coop_inven_open_hook = hook;
}

#ifndef COOP_LITE_TEST_STUBS
/*
 * ARCANUM_DEV_CLIENT_BARTER=1 (client; pair with host
 * ARCANUM_DEV_START_LOC=merchant): headless barter round trip without input
 * automation. 12 s in the world: open barter with the nearest merchant
 * through inven_ui_open; +4 s buy the first affordable stock item; +4 s sell
 * the first carried item the merchant wants; +4 s log the result. Transfers
 * use the same Packet81 the barter window sends.
 */
static int coop_dev_barter_stage;
static unsigned int coop_dev_barter_ms;
static ObjectID coop_dev_barter_npc;

static void coop_lite_dev_send_transfer(int64_t pc, int64_t npc, int64_t item, int64_t from, int64_t to)
{
    Packet81 pkt;
    int64_t store = coop_lite_store_of(npc);

    memset(&pkt, 0, sizeof(pkt));
    pkt.type = 81;
    pkt.field_8 = 0x01;
    pkt.field_10 = obj_get_id(item);
    pkt.field_28 = obj_get_id(from);
    pkt.field_40 = obj_get_id(to);
    pkt.field_58 = -1;
    pkt.field_5C = to == pc ? item_cost(item, npc, pc, false) : item_cost(item, pc, npc, false);
    pkt.field_60 = 1;
    pkt.field_68 = sub_529520();
    pkt.field_70 = obj_get_id(npc);
    pkt.field_88 = obj_get_id(store != OBJ_HANDLE_NULL ? store : npc);
    pkt.field_A0 = obj_get_id(pc);
    {
        int depth = tig_net_local_suspend_pause();
        tig_net_send_app_all(&pkt, sizeof(pkt));
        tig_net_local_suspend_resume(depth);
    }
}

static int64_t coop_lite_dev_pick_item(int64_t owner, int64_t seller, int64_t buyer, int max_cost)
{
    int64_t* items;
    int cnt;
    int i;
    int64_t pick = OBJ_HANDLE_NULL;
    int cost;

    if (owner == OBJ_HANDLE_NULL) {
        return OBJ_HANDLE_NULL;
    }
    cnt = item_list_get(owner, &items);
    for (i = 0; i < cnt && pick == OBJ_HANDLE_NULL; i++) {
        if (coop_lite_is_stackable(items[i])
            || IS_WEAR_INV_LOC(item_inventory_location_get(items[i]))) {
            continue;
        }
        cost = item_cost(items[i], seller, buyer, false);
        if (cost > 0 && cost <= max_cost) {
            pick = items[i];
        }
    }
    item_list_free(items);
    return pick;
}

/* Dev (ARCANUM_DEV_OPEN_INVEN=1, client): open our own inventory window
   20 s into the session (for a screenshot of the synced grid). =2: also,
   4 s later, drop the first backpack item the way the window's drop box
   does (Packet81 flag 0x02). */
static unsigned int coop_dev_open_inven_ms;
static bool coop_dev_open_inven_done;
static bool coop_dev_dropbox_done;

static void coop_lite_client_dev_open_inven_tick(unsigned int now_ms)
{
    const char* env = getenv("ARCANUM_DEV_OPEN_INVEN");
    int64_t pc = player_get_local_pc_obj();

    if (env == NULL || (env[0] != '1' && env[0] != '2') || pc == OBJ_HANDLE_NULL) {
        return;
    }
    if (coop_dev_open_inven_done && env[0] == '2' && !coop_dev_dropbox_done
        && coop_lite_interval_elapsed(coop_dev_open_inven_ms, now_ms, 24000u)) {
        int64_t* items;
        int64_t pick = OBJ_HANDLE_NULL;
        int cnt;
        int i;

        coop_dev_dropbox_done = true;
        cnt = item_list_get(pc, &items);
        for (i = 0; i < cnt && pick == OBJ_HANDLE_NULL; i++) {
            if (!coop_lite_is_stackable(items[i]) && !IS_WEAR_INV_LOC(item_inventory_location_get(items[i]))) {
                pick = items[i];
            }
        }
        item_list_free(items);
        if (pick != OBJ_HANDLE_NULL) {
            Packet81 pkt;
            int depth;

            memset(&pkt, 0, sizeof(pkt));
            pkt.type = 81;
            pkt.field_8 = 0x02;
            pkt.field_10 = obj_get_id(pick);
            pkt.field_28 = obj_get_id(pc);
            pkt.field_40.type = OID_TYPE_NULL;
            pkt.field_58 = -1;
            pkt.field_5C = -1;
            pkt.field_60 = 1;
            pkt.field_68 = sub_529520();
            pkt.field_70.type = OID_TYPE_NULL;
            pkt.field_88.type = OID_TYPE_NULL;
            pkt.field_A0 = obj_get_id(pc);
            depth = tig_net_local_suspend_pause();
            tig_net_send_app_all(&pkt, sizeof(pkt));
            tig_net_local_suspend_resume(depth);
            tig_debug_printf("CoopLite: dev: drop box: dropping one item (%d carried)\n",
                obj_field_int32_get(pc, OBJ_F_CRITTER_INVENTORY_NUM));
        }
        return;
    }
    if (coop_dev_open_inven_done) {
        return;
    }
    if (coop_dev_open_inven_ms == 0) {
        coop_dev_open_inven_ms = now_ms != 0 ? now_ms : 1;
        return;
    }
    if (!coop_lite_interval_elapsed(coop_dev_open_inven_ms, now_ms, 20000u) || coop_inven_open_hook == NULL) {
        return;
    }
    coop_dev_open_inven_done = true;
    tig_debug_printf("CoopLite: dev: opening own inventory (%d items)\n",
        obj_field_int32_get(pc, OBJ_F_CRITTER_INVENTORY_NUM));
    coop_inven_open_hook(pc, OBJ_HANDLE_NULL, 0 /* INVEN_UI_MODE_INVENTORY */);
}

/* Dev (ARCANUM_DEV_CLIENT_STEP=1, client): 20 s into the session move our
   own PC 3 tiles east locally, as walking would; the host's census must
   follow (PC_POS ownership, e.g. after a rejoin). */
static unsigned int coop_dev_step_ms;
static bool coop_dev_step_done;

static void coop_lite_client_dev_step_tick(unsigned int now_ms)
{
    const char* env = getenv("ARCANUM_DEV_CLIENT_STEP");
    int64_t pc = player_get_local_pc_obj();
    int64_t loc;

    if (coop_dev_step_done || env == NULL || env[0] != '1' || pc == OBJ_HANDLE_NULL) {
        return;
    }
    if (coop_dev_step_ms == 0) {
        coop_dev_step_ms = now_ms != 0 ? now_ms : 1;
        return;
    }
    if (!coop_lite_interval_elapsed(coop_dev_step_ms, now_ms, 20000u)) {
        return;
    }
    coop_dev_step_done = true;
    loc = obj_field_int64_get(pc, OBJ_F_LOCATION);
    loc = location_make(LOCATION_GET_X(loc) + 3, LOCATION_GET_Y(loc));
    tig_net_local_suspend_begin();
    sub_43E770(pc, loc, 0, 0);
    tig_net_local_suspend_end();
    tig_debug_printf("CoopLite: dev: stepped own PC to %d,%d\n",
        (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc));
}

void coop_lite_client_dev_barter_tick(unsigned int now_ms)
{
    coop_lite_client_dev_open_inven_tick(now_ms);
    coop_lite_client_dev_step_tick(now_ms);

    const char* env;
    int64_t pc = player_get_local_pc_obj();
    int64_t npc;
    int64_t item;
    char name[2000];

    if (coop_dev_barter_stage >= 4) {
        return;
    }
    env = getenv("ARCANUM_DEV_CLIENT_BARTER");
    if (env == NULL || env[0] != '1') {
        coop_dev_barter_stage = 4;
        return;
    }
    if (pc == OBJ_HANDLE_NULL || !coop_lite_client_in_session) {
        return;
    }
    if (coop_dev_barter_ms == 0) {
        coop_dev_barter_ms = now_ms != 0 ? now_ms : 1;
        return;
    }
    if (!coop_lite_interval_elapsed(coop_dev_barter_ms, now_ms, coop_dev_barter_stage == 0 ? 12000u : 4000u)) {
        return;
    }
    coop_dev_barter_ms = now_ms != 0 ? now_ms : 1;

    if (coop_dev_barter_stage == 0) {
        ObjectList objects;
        ObjectNode* node;
        int64_t best = OBJ_HANDLE_NULL;

        object_list_vicinity(pc, OBJ_TM_NPC, &objects);
        for (node = objects.head; node != NULL; node = node->next) {
            if (!critter_is_dead(node->obj) && coop_lite_npc_is_merchant(node->obj)
                && location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION),
                       obj_field_int64_get(node->obj, OBJ_F_LOCATION)) <= 10) {
                best = node->obj;
                break;
            }
        }
        object_list_destroy(&objects);
        coop_dev_barter_stage = 1;
        if (best == OBJ_HANDLE_NULL || coop_inven_open_hook == NULL) {
            tig_debug_printf("CoopLite: dev barter: no merchant within 10 tiles\n");
            coop_dev_barter_stage = 4;
            return;
        }
        coop_dev_barter_npc = obj_get_id(best);
        object_examine(best, pc, name);
        tig_debug_printf("CoopLite: dev barter: opening barter with '%s' (gold %d)\n", name, item_gold_get(pc));
        coop_inven_open_hook(pc, best, 1 /* INVEN_UI_MODE_BARTER */);
        return;
    }

    npc = obj_pool_perm_lookup(coop_dev_barter_npc);
    if (npc == OBJ_HANDLE_NULL) {
        coop_dev_barter_stage = 4;
        return;
    }
    if (coop_dev_barter_stage == 1) {
        int64_t store = coop_lite_store_of(npc);
        int64_t from = store != OBJ_HANDLE_NULL ? store : npc;

        item = coop_lite_dev_pick_item(from, npc, pc, item_gold_get(pc));
        if (item == OBJ_HANDLE_NULL && store != OBJ_HANDLE_NULL) {
            from = npc;
            item = coop_lite_dev_pick_item(npc, npc, pc, item_gold_get(pc));
        }
        if (item != OBJ_HANDLE_NULL) {
            object_examine(item, pc, name);
            tig_debug_printf("CoopLite: dev barter: buying '%s' for %d (gold %d)\n",
                name, item_cost(item, npc, pc, false), item_gold_get(pc));
            coop_lite_dev_send_transfer(pc, npc, item, from, pc);
        } else {
            tig_debug_printf("CoopLite: dev barter: nothing affordable to buy\n");
        }
        coop_dev_barter_stage = 2;
        return;
    }
    if (coop_dev_barter_stage == 2) {
        int64_t store = coop_lite_store_of(npc);

        item = coop_lite_dev_pick_item(pc, pc, npc, 100000);
        if (item != OBJ_HANDLE_NULL) {
            object_examine(item, pc, name);
            tig_debug_printf("CoopLite: dev barter: selling '%s' for %d (gold %d)\n",
                name, item_cost(item, pc, npc, false), item_gold_get(pc));
            coop_lite_dev_send_transfer(pc, npc, item, pc, store != OBJ_HANDLE_NULL ? store : npc);
        } else {
            tig_debug_printf("CoopLite: dev barter: nothing the merchant buys\n");
        }
        coop_dev_barter_stage = 3;
        return;
    }
    tig_debug_printf("CoopLite: dev barter: done, gold %d, items %d\n",
        item_gold_get(pc), obj_field_int32_get(pc, OBJ_F_CRITTER_INVENTORY_NUM));
    coop_dev_barter_stage = 4;
}
#endif

void coop_lite_client_host_left(void)
{
#ifndef COOP_LITE_TEST_STUBS
    if (!coop_lite_client_in_session) {
        return;
    }
    tig_debug_printf("CoopLite: the host left the session\n");
    /* Keep the player's progress: the session's state of this PC goes to
       its character bundle before anything is reset. */
    if (player_get_local_pc_obj() != OBJ_HANDLE_NULL && coop_lite_client_world_ready()) {
        coop_lite_export_local_character_impl();
    }
    coop_lite_show_notice("The host left the game. Your character was saved; returning to the main menu.", TF_TYPE_RED);
    coop_host_left_menu_armed = true;
    coop_host_left_menu_ms = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1;
#endif
}

bool coop_lite_host_left_menu_due(bool armed, unsigned int armed_ms, unsigned int now_ms)
{
    /* A few seconds so the notice can be read. */
    return armed && coop_lite_interval_elapsed(armed_ms, now_ms, 4000u);
}

/* ---- Parked PCs (pure table; unit-tested) ---- */
typedef struct CoopParked {
    bool used;
    uint64_t hash;
    unsigned char oid[24];
} CoopParked;

static CoopParked coop_parked[COOP_PARKED_MAX];

uint64_t coop_lite_token_hash(const unsigned char token[16])
{
    uint64_t h = 1469598103934665603ull; /* FNV-1a 64 */
    bool any = false;
    int i;

    for (i = 0; i < 16; i++) {
        any = any || token[i] != 0;
        h ^= token[i];
        h *= 1099511628211ull;
    }
    if (!any) {
        return 0;
    }
    return h != 0 ? h : 1;
}

void coop_lite_parked_reset(void)
{
    memset(coop_parked, 0, sizeof(coop_parked));
}

bool coop_lite_parked_put(uint64_t hash, const unsigned char oid[24])
{
    int i;
    int free_idx = -1;

    if (hash == 0) {
        return false;
    }
    for (i = 0; i < COOP_PARKED_MAX; i++) {
        if (coop_parked[i].used && coop_parked[i].hash == hash) {
            /* One parked PC per token: the newest wins. */
            memcpy(coop_parked[i].oid, oid, 24);
            return true;
        }
        if (!coop_parked[i].used && free_idx < 0) {
            free_idx = i;
        }
    }
    if (free_idx < 0) {
        return false;
    }
    coop_parked[free_idx].used = true;
    coop_parked[free_idx].hash = hash;
    memcpy(coop_parked[free_idx].oid, oid, 24);
    return true;
}

bool coop_lite_parked_take(uint64_t hash, unsigned char oid_out[24])
{
    int i;

    if (hash == 0) {
        return false;
    }
    for (i = 0; i < COOP_PARKED_MAX; i++) {
        if (coop_parked[i].used && coop_parked[i].hash == hash) {
            memcpy(oid_out, coop_parked[i].oid, 24);
            coop_parked[i].used = false;
            return true;
        }
    }
    return false;
}

bool coop_lite_parked_has_oid(const unsigned char oid[24])
{
    int i;

    for (i = 0; i < COOP_PARKED_MAX; i++) {
        if (coop_parked[i].used && memcmp(coop_parked[i].oid, oid, 24) == 0) {
            return true;
        }
    }
    return false;
}

int coop_lite_parked_serialize(uint8_t* out, int cap, const uint64_t* extra_hash,
    const unsigned char (*extra_oid)[24], int num_extra)
{
    CoopParked saved[COOP_PARKED_MAX];
    uint32_t magic = COOP_PARKED_SAVE_MAGIC;
    uint32_t count = 0;
    int off = 8;
    int i;

    if (out == NULL || cap < COOP_PARKED_SAVE_MAX) {
        return -1;
    }
    memcpy(saved, coop_parked, sizeof(saved));
    for (i = 0; i < num_extra; i++) {
        coop_lite_parked_put(extra_hash[i], extra_oid[i]);
    }
    for (i = 0; i < COOP_PARKED_MAX; i++) {
        if (!coop_parked[i].used) {
            continue;
        }
        memcpy(out + off, &coop_parked[i].hash, 8);
        memcpy(out + off + 8, coop_parked[i].oid, 24);
        off += COOP_PARKED_SAVE_ENTRY;
        count++;
    }
    memcpy(coop_parked, saved, sizeof(saved));
    memcpy(out, &magic, 4);
    memcpy(out + 4, &count, 4);
    return off;
}

bool coop_lite_parked_deserialize(const uint8_t* in, int len)
{
    uint32_t magic;
    uint32_t count;
    uint32_t i;

    coop_lite_parked_reset();
    if (in == NULL || len < 8) {
        return false;
    }
    memcpy(&magic, in, 4);
    memcpy(&count, in + 4, 4);
    if (magic != COOP_PARKED_SAVE_MAGIC || count > COOP_PARKED_MAX
        || len != 8 + (int)count * COOP_PARKED_SAVE_ENTRY) {
        return false;
    }
    for (i = 0; i < count; i++) {
        uint64_t hash;

        memcpy(&hash, in + 8 + i * COOP_PARKED_SAVE_ENTRY, 8);
        if (!coop_lite_parked_put(hash, in + 8 + i * COOP_PARKED_SAVE_ENTRY + 8)) {
            coop_lite_parked_reset();
            return false;
        }
    }
    return true;
}

#ifndef COOP_LITE_TEST_STUBS
bool coop_lite_save_appendix(TigFile* stream)
{
    uint8_t buf[COOP_PARKED_SAVE_MAX];
    uint64_t hash[8];
    unsigned char oid[8][24];
    int n = 0;
    int size;
    int slot;

    if (!tig_net_is_coop_enabled() || !tig_net_is_host()) {
        return true;
    }
    for (slot = 1; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);
        ObjectID id;

        if (coop_slot_token_hash[slot] == 0 || pc == OBJ_HANDLE_NULL
            || obj_field_int32_get(pc, OBJ_F_TYPE) != OBJ_TYPE_PC) {
            continue;
        }
        id = obj_get_id(pc);
        hash[n] = coop_slot_token_hash[slot];
        memcpy(oid[n], &id, 24);
        n++;
    }
    size = coop_lite_parked_serialize(buf, (int)sizeof(buf), hash, (const unsigned char(*)[24])oid, n);
    if (size < 0 || tig_file_fwrite(buf, (size_t)size, 1, stream) != 1) {
        return false;
    }
    tig_debug_printf("CoopLite: save: %d player PC(s) kept for rejoin\n", (size - 8) / COOP_PARKED_SAVE_ENTRY);
    return true;
}

bool coop_lite_load_appendix(TigFile* stream)
{
    uint8_t buf[COOP_PARKED_SAVE_MAX];
    uint32_t count;
    int pos;
    int i;

    coop_lite_parked_reset();
    pos = tig_file_ftell(stream);
    if (tig_file_fread(buf, 8, 1, stream) != 1) {
        tig_file_fseek(stream, pos, SEEK_SET);
        return true;
    }
    memcpy(&count, buf + 4, 4);
    if (*(uint32_t*)buf != COOP_PARKED_SAVE_MAGIC) {
        /* Older save: the next module's data. */
        tig_file_fseek(stream, pos, SEEK_SET);
        return true;
    }
    if (count > COOP_PARKED_MAX
        || (count > 0 && tig_file_fread(buf + 8, (size_t)count * COOP_PARKED_SAVE_ENTRY, 1, stream) != 1)
        || !coop_lite_parked_deserialize(buf, 8 + (int)count * COOP_PARKED_SAVE_ENTRY)) {
        return false;
    }
    /* Out of the world until their players come back. */
    for (i = 0; i < COOP_PARKED_MAX; i++) {
        int64_t pc;
        ObjectID id;

        if (!coop_parked[i].used) {
            continue;
        }
        memcpy(&id, coop_parked[i].oid, sizeof(id));
        pc = obj_pool_perm_lookup(id);
        if (pc != OBJ_HANDLE_NULL && obj_field_int32_get(pc, OBJ_F_TYPE) == OBJ_TYPE_PC
            && pc != player_get_local_pc_obj()) {
            object_flags_set(pc, OF_OFF);
        }
    }
    tig_debug_printf("CoopLite: load: %d player PC(s) parked for rejoin\n", (int)count);
    return true;
}
#endif

int coop_lite_parked_count(void)
{
    int i;
    int n = 0;

    for (i = 0; i < COOP_PARKED_MAX; i++) {
        n += coop_parked[i].used ? 1 : 0;
    }
    return n;
}

/* ---- Character bundle wire format (pure; unit-tested) ---- */

static bool coop_char_bundle_reserve(CoopCharBundleBuf* buf, size_t extra)
{
    size_t need;
    size_t cap;
    uint8_t* data;

    if (extra > SIZE_MAX - buf->size) {
        return false;
    }
    need = buf->size + extra;
    if (need <= buf->cap) {
        return true;
    }
    cap = buf->cap != 0 ? buf->cap : 4096;
    while (cap < need) {
        cap *= 2;
    }
    data = (uint8_t*)realloc(buf->data, cap);
    if (data == NULL) {
        return false;
    }
    buf->data = data;
    buf->cap = cap;
    return true;
}

static bool coop_char_bundle_put_u32(CoopCharBundleBuf* buf, uint32_t v)
{
    if (!coop_char_bundle_reserve(buf, 4)) {
        return false;
    }
    buf->data[buf->size++] = (uint8_t)(v & 0xFF);
    buf->data[buf->size++] = (uint8_t)((v >> 8) & 0xFF);
    buf->data[buf->size++] = (uint8_t)((v >> 16) & 0xFF);
    buf->data[buf->size++] = (uint8_t)((v >> 24) & 0xFF);
    return true;
}

static uint32_t coop_char_bundle_get_u32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool coop_char_bundle_begin(CoopCharBundleBuf* buf, int count)
{
    memset(buf, 0, sizeof(*buf));
    if (count <= 0 || count > COOP_CHAR_BUNDLE_MAX_OBJECTS) {
        return false;
    }
    return coop_char_bundle_put_u32(buf, COOP_CHAR_BUNDLE_MAGIC)
        && coop_char_bundle_put_u32(buf, COOP_CHAR_BUNDLE_VERSION)
        && coop_char_bundle_put_u32(buf, (uint32_t)count);
}

bool coop_char_bundle_add_blob(CoopCharBundleBuf* buf, const void* blob, size_t size)
{
    if (blob == NULL || size == 0 || size > COOP_CHAR_BUNDLE_MAX_BLOB) {
        return false;
    }
    if (!coop_char_bundle_put_u32(buf, (uint32_t)size) || !coop_char_bundle_reserve(buf, size)) {
        return false;
    }
    memcpy(buf->data + buf->size, blob, size);
    buf->size += size;
    return true;
}

static bool coop_char_bundle_put_str(CoopCharBundleBuf* buf, const char* str)
{
    size_t len;

    if (str == NULL) {
        str = "";
    }
    len = strlen(str) + 1;
    if (len > COOP_CHAR_BUNDLE_MAX_STR) {
        return false;
    }
    if (!coop_char_bundle_put_u32(buf, (uint32_t)len) || !coop_char_bundle_reserve(buf, len)) {
        return false;
    }
    memcpy(buf->data + buf->size, str, len);
    buf->size += len;
    return true;
}

bool coop_char_bundle_finish(CoopCharBundleBuf* buf, const char* rule, const char* name)
{
    return coop_char_bundle_put_str(buf, rule) && coop_char_bundle_put_str(buf, name);
}

void coop_char_bundle_free(CoopCharBundleBuf* buf)
{
    free(buf->data);
    memset(buf, 0, sizeof(*buf));
}

static bool coop_char_bundle_walk(const uint8_t* data, size_t size, CoopCharBundleVisitFn visit,
    void* ctx, int* count_out, const char** rule_out, const char** name_out)
{
    size_t off = 12;
    uint32_t count;
    uint32_t index;

    if (data == NULL || size < 12
        || coop_char_bundle_get_u32(data) != COOP_CHAR_BUNDLE_MAGIC
        || coop_char_bundle_get_u32(data + 4) != COOP_CHAR_BUNDLE_VERSION) {
        return false;
    }
    count = coop_char_bundle_get_u32(data + 8);
    if (count == 0 || count > COOP_CHAR_BUNDLE_MAX_OBJECTS) {
        return false;
    }
    for (index = 0; index < count; index++) {
        uint32_t len;

        if (size - off < 4) {
            return false;
        }
        len = coop_char_bundle_get_u32(data + off);
        off += 4;
        if (len == 0 || len > COOP_CHAR_BUNDLE_MAX_BLOB || len > size - off) {
            return false;
        }
        if (visit != NULL && !visit(ctx, (int)index, data + off, len)) {
            return false;
        }
        off += len;
    }
    for (index = 0; index < 2; index++) {
        uint32_t len;
        const char* str;

        if (size - off < 4) {
            return false;
        }
        len = coop_char_bundle_get_u32(data + off);
        off += 4;
        if (len == 0 || len > COOP_CHAR_BUNDLE_MAX_STR || len > size - off) {
            return false;
        }
        str = (const char*)(data + off);
        if (str[len - 1] != '\0') {
            return false;
        }
        if (index == 0 && rule_out != NULL) {
            *rule_out = str;
        }
        if (index == 1 && name_out != NULL) {
            *name_out = str;
        }
        off += len;
    }
    if (off != size) {
        return false;
    }
    if (count_out != NULL) {
        *count_out = (int)count;
    }
    return true;
}

bool coop_char_bundle_parse(const uint8_t* data, size_t size, CoopCharBundleVisitFn visit,
    void* ctx, int* count_out, const char** rule_out, const char** name_out)
{
    /* Structure first, so a truncated file never reaches the visitor. */
    if (!coop_char_bundle_walk(data, size, NULL, NULL, NULL, NULL, NULL)) {
        return false;
    }
    return coop_char_bundle_walk(data, size, visit, ctx, count_out, rule_out, name_out);
}

/* ---- P0-2 pose ordering / map identity (pure; unit-tested) ---- */

bool coop_lite_pose_order_accept(CoopPoseOrder* st, uint16_t epoch, uint16_t seq,
    bool force, bool owner_link)
{
    int16_t d;

    if (st == NULL) {
        return true;
    }
    if (!st->valid) {
        st->valid = true;
        st->epoch = epoch;
        st->seq = seq;
        st->seq_valid = true;
        return true;
    }
    d = (int16_t)(uint16_t)(epoch - st->epoch);
    if (d < 0) {
        return false;
    }
    if (d > 0) {
        if (owner_link) {
            /* Only the host mints epochs. */
            return false;
        }
        st->epoch = epoch;
        st->seq = seq;
        st->seq_valid = true;
        return true;
    }
    if (st->seq_valid && !force && (int16_t)(uint16_t)(seq - st->seq) <= 0) {
        return false;
    }
    st->seq = seq;
    st->seq_valid = true;
    return true;
}

uint16_t coop_lite_pose_order_bump(CoopPoseOrder* st)
{
    st->epoch = (uint16_t)(st->epoch + 1);
    st->valid = true;
    st->seq_valid = false;
    return st->epoch;
}

int coop_lite_pose_map_key(int map, uint16_t gen)
{
    return (int)(((uint32_t)gen << 16) | ((uint32_t)map & 0xFFFFu));
}

void coop_lite_on_map_opened(void)
{
    if (tig_net_is_host()) {
        coop_map_gen = (uint16_t)(coop_map_gen + 1);
        if (coop_map_gen == 0) {
            coop_map_gen = 1;
        }
    }
}

uint16_t coop_lite_map_gen(void)
{
    return coop_map_gen;
}

/* ---- §7 subset: world state on join / map change ------------------------ */

#ifndef COOP_LITE_TEST_STUBS
COOP_STATIC_ASSERT(OF_SEE_THROUGH_BIT == OF_SEE_THROUGH, "OF_SEE_THROUGH bit");
COOP_STATIC_ASSERT(OF_SHOOT_THROUGH_BIT == OF_SHOOT_THROUGH, "OF_SHOOT_THROUGH bit");
COOP_STATIC_ASSERT(OF_NO_BLOCK_BIT == OF_NO_BLOCK, "OF_NO_BLOCK bit");
COOP_STATIC_ASSERT(OF_OFF_BIT == OF_OFF, "OF_OFF bit");
COOP_STATIC_ASSERT(OF_FLAT_BIT == OF_FLAT, "OF_FLAT bit");

#define COOP_WORLD_MAX_CONTAINERS 64
#define COOP_WORLD_NPC_DEAD 0x1

static unsigned int coop_world_req_host_ms[8];
static bool coop_world_req_host_seen[8];
static ObjectID coop_dev_mutate_dagger;
static bool coop_dev_mutate_dagger_valid;

static void coop_lite_world_flush(PacketCoopWorldState* out, int slot, bool last, bool* first)
{
    out->flags = (*first ? COOP_WORLD_F_FIRST : 0u) | (last ? COOP_WORLD_F_LAST : 0u);
    tig_net_send_app(out, coop_lite_world_state_size(out->count), slot);
    *first = false;
    out->count = 0;
}
#endif

void coop_lite_handle_world_req(PacketCoopWorldReq* pkt)
{
#ifndef COOP_LITE_TEST_STUBS
    PacketCoopWorldState* out;
    int64_t containers[COOP_WORLD_MAX_CONTAINERS];
    int num_containers = 0;
    int64_t pc;
    int64_t center;
    int64_t obj;
    int iter;
    int slot;
    int key;
    bool first = true;
    int n_portal = 0;
    int n_container = 0;
    int n_npc = 0;
    int n_item = 0;

    if (!tig_net_is_host() || tig_net_last_message_size() != (int)sizeof(*pkt)) {
        return;
    }
    slot = sub_52A530();
    if (slot <= 0 || slot >= 8) {
        return;
    }
    pc = sub_4A2B60(slot);
    if (pc == OBJ_HANDLE_NULL) {
        return;
    }
    key = coop_lite_pose_map_key(map_current_map(), coop_map_gen);
    if (pkt->map != key) {
        tig_debug_printf("CoopLite: world req from slot %d for map %d (host %d) — ignored\n", slot, pkt->map, key);
        return;
    }
    if (coop_world_req_host_seen[slot]
        && !coop_lite_interval_elapsed(coop_world_req_host_ms[slot], coop_obj_now_ms, 3000u)) {
        return;
    }
    coop_world_req_host_seen[slot] = true;
    coop_world_req_host_ms[slot] = coop_obj_now_ms;

    center = obj_field_int64_get(pc, OBJ_F_LOCATION);
    out = (PacketCoopWorldState*)MALLOC(sizeof(*out));
    memset(out, 0, offsetof(PacketCoopWorldState, entries));
    out->type = PACKET_COOP_WORLD_STATE;
    out->map = key;

    if (obj_inst_first(&obj, &iter)) {
        do {
            unsigned int flags = obj_field_int32_get(obj, OBJ_F_FLAGS);
            int type;
            int64_t loc;
            CoopWorldEntry* e;
            ObjectID oid;

            if ((flags & (OF_DESTROYED | OF_INVENTORY)) != 0) {
                continue;
            }
            type = obj_field_int32_get(obj, OBJ_F_TYPE);
            if (type != OBJ_TYPE_PORTAL && type != OBJ_TYPE_CONTAINER && type != OBJ_TYPE_NPC
                && (type < OBJ_TYPE_WEAPON || type > OBJ_TYPE_GENERIC)) {
                continue;
            }
            if (type == OBJ_TYPE_NPC && !critter_is_dead(obj) && (flags & OF_OFF) == 0) {
                continue; /* the living follow NPC_POS / their own sync */
            }
            loc = obj_field_int64_get(obj, OBJ_F_LOCATION);
            if (location_dist(loc, center) > COOP_WORLD_RADIUS) {
                continue;
            }
            e = &out->entries[out->count];
            memset(e, 0, sizeof(*e));
            oid = obj_get_id(obj);
            memcpy(e->oid, &oid, sizeof(oid));
            e->aid = (unsigned int)obj_field_int32_get(obj, OBJ_F_CURRENT_AID);
            e->loc = loc;
            switch (type) {
            case OBJ_TYPE_PORTAL:
                e->kind = COOP_WORLD_PORTAL;
                e->of_flags = flags & COOP_WORLD_PORTAL_OF_MASK;
                e->kind_flags = (unsigned int)obj_field_int32_get(obj, OBJ_F_PORTAL_FLAGS);
                n_portal++;
                break;
            case OBJ_TYPE_CONTAINER:
                e->kind = COOP_WORLD_CONTAINER;
                e->of_flags = flags & COOP_WORLD_PORTAL_OF_MASK;
                e->kind_flags = (unsigned int)obj_field_int32_get(obj, OBJ_F_CONTAINER_FLAGS);
                if (num_containers < COOP_WORLD_MAX_CONTAINERS) {
                    containers[num_containers++] = obj;
                }
                n_container++;
                break;
            case OBJ_TYPE_NPC:
                e->kind = COOP_WORLD_NPC;
                e->of_flags = flags & COOP_WORLD_NPC_OF_MASK;
                e->kind_flags = critter_is_dead(obj) ? COOP_WORLD_NPC_DEAD : 0u;
                e->hp_damage = obj_field_int32_get(obj, OBJ_F_HP_DAMAGE);
                n_npc++;
                break;
            default:
                e->kind = COOP_WORLD_GROUND_ITEM;
                e->proto = sub_49B290(obj);
                e->kind_flags = coop_lite_is_stackable(obj) ? 1u : 0u;
                n_item++;
                if (getenv("ARCANUM_COOP_WORLD_TRACE") != NULL) {
                    char oid_str[80];

                    objid_id_to_str(oid_str, oid);
                    tig_debug_printf("CoopLite: world trace: host ground item %s at %d,%d (client PC %d,%d)\n",
                        oid_str, (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc),
                        (int)LOCATION_GET_X(center), (int)LOCATION_GET_Y(center));
                }
                break;
            }
            if (++out->count == COOP_WORLD_CHUNK) {
                coop_lite_world_flush(out, slot, false, &first);
            }
        } while (obj_inst_next(&obj, &iter));
    }
    coop_lite_world_flush(out, slot, true, &first);
    FREE(out);
    if (coop_dev_mutate_dagger_valid && getenv("ARCANUM_COOP_WORLD_TRACE") != NULL) {
        int64_t d = obj_pool_perm_lookup(coop_dev_mutate_dagger);
        int64_t parent = OBJ_HANDLE_NULL;

        if (d == OBJ_HANDLE_NULL) {
            tig_debug_printf("CoopLite: world trace: dev dagger: no handle\n");
        } else {
            item_parent(d, &parent);
            tig_debug_printf("CoopLite: world trace: dev dagger: flags 0x%x parent %s\n",
                obj_field_int32_get(d, OBJ_F_FLAGS), parent == OBJ_HANDLE_NULL ? "none" : "yes");
        }
    }

    /* Contents after the list: the client knows the containers by then. */
    {
        int i;

        for (i = 0; i < num_containers; i++) {
            coop_lite_host_send_inven_batch(slot, &containers[i], 1);
        }
    }
    tig_debug_printf("CoopLite: world state → slot %d: %d portals, %d containers, %d npcs, %d ground items\n",
        slot, n_portal, n_container, n_npc, n_item);
#else
    (void)pkt;
#endif
}

#ifndef COOP_LITE_TEST_STUBS
static CoopOidSet* coop_world_seen;
static bool coop_world_rx_active;
static int coop_world_rx_map;
static int coop_world_stats[5]; /* per kind: applied changes */
static int coop_world_req_key = -1;
static unsigned int coop_world_req_ms;
static unsigned int coop_world_ready_ms;
static bool coop_world_was_ready;
static int coop_world_ready_key = -1;

static void coop_lite_world_apply_flags(int64_t obj, unsigned int want, unsigned int mask, int kind)
{
    unsigned int have = (unsigned int)obj_field_int32_get(obj, OBJ_F_FLAGS) & mask;

    if (have == (want & mask)) {
        return;
    }
    if ((want & mask & ~have) != 0) {
        object_flags_set(obj, want & mask & ~have);
    }
    if ((have & ~want) != 0) {
        object_flags_unset(obj, have & ~want);
    }
    coop_world_stats[kind]++;
}

static const CoopWorldEntry* coop_world_want[COOP_WORLD_CHUNK];
static int coop_world_want_cnt;

static void coop_lite_world_apply(const CoopWorldEntry* e)
{
    int64_t obj = coop_lite_get_oid_obj(e->oid);
    int type;

    if (e->kind == COOP_WORLD_GROUND_ITEM) {
        coop_oid_set_add(coop_world_seen, e->oid);
    }
    if (obj == OBJ_HANDLE_NULL) {
        /* Unknown ground item: asked for after the batch (a request sent
           while packets are suspended would be lost). */
        if (e->kind == COOP_WORLD_GROUND_ITEM && e->kind_flags == 0
            && coop_world_want_cnt < COOP_WORLD_CHUNK) {
            coop_world_want[coop_world_want_cnt++] = e;
        }
        return;
    }
    if ((obj_field_int32_get(obj, OBJ_F_FLAGS) & OF_DESTROYED) != 0) {
        return;
    }
    type = obj_field_int32_get(obj, OBJ_F_TYPE);
    switch (e->kind) {
    case COOP_WORLD_PORTAL:
    case COOP_WORLD_CONTAINER:
        if (type != (e->kind == COOP_WORLD_PORTAL ? OBJ_TYPE_PORTAL : OBJ_TYPE_CONTAINER)) {
            return;
        }
        if ((unsigned int)obj_field_int32_get(obj, OBJ_F_CURRENT_AID) != e->aid) {
            object_set_current_aid(obj, (tig_art_id_t)e->aid);
            coop_world_stats[e->kind]++;
        }
        coop_lite_world_apply_flags(obj, e->of_flags, COOP_WORLD_PORTAL_OF_MASK, e->kind);
        if (type == OBJ_TYPE_PORTAL
            && (unsigned int)obj_field_int32_get(obj, OBJ_F_PORTAL_FLAGS) != e->kind_flags) {
            obj_field_int32_set(obj, OBJ_F_PORTAL_FLAGS, (int)e->kind_flags);
            coop_world_stats[e->kind]++;
        } else if (type == OBJ_TYPE_CONTAINER
            && (unsigned int)obj_field_int32_get(obj, OBJ_F_CONTAINER_FLAGS) != e->kind_flags) {
            obj_field_int32_set(obj, OBJ_F_CONTAINER_FLAGS, (int)e->kind_flags);
            coop_world_stats[e->kind]++;
        }
        break;
    case COOP_WORLD_NPC:
        if (type != OBJ_TYPE_NPC) {
            return;
        }
        if ((e->kind_flags & COOP_WORLD_NPC_DEAD) != 0 && !critter_is_dead(obj)) {
            obj_field_int32_set(obj, OBJ_F_HP_DAMAGE, e->hp_damage > 0 ? e->hp_damage : 32000);
            object_set_current_aid(obj, (tig_art_id_t)e->aid);
            sub_432D90(obj);
            coop_world_stats[e->kind]++;
        }
        coop_lite_world_apply_flags(obj, e->of_flags, COOP_WORLD_NPC_OF_MASK, e->kind);
        break;
    case COOP_WORLD_GROUND_ITEM:
        if (!item_is_item(obj) || (obj_field_int32_get(obj, OBJ_F_FLAGS) & OF_INVENTORY) != 0) {
            /* Locally carried, on the ground at the host: the next
               INVEN_SYNC of the carrier settles it. */
            return;
        }
        if (obj_field_int64_get(obj, OBJ_F_LOCATION) != e->loc) {
            sub_43E770(obj, e->loc, 0, 0);
            coop_world_stats[e->kind]++;
        }
        break;
    }
}

/* LAST chunk: ground items near our PC the host did not list are gone. */
static int coop_lite_world_prune(void)
{
    int64_t pc = player_get_local_pc_obj();
    int64_t center;
    int64_t doomed[256];
    int num = 0;
    int64_t obj;
    int iter;
    int i;

    if (pc == OBJ_HANDLE_NULL || coop_world_seen->overflow) {
        return 0;
    }
    center = obj_field_int64_get(pc, OBJ_F_LOCATION);
    if (obj_inst_first(&obj, &iter)) {
        do {
            unsigned int flags = obj_field_int32_get(obj, OBJ_F_FLAGS);
            ObjectID oid;

            if ((flags & (OF_DESTROYED | OF_INVENTORY)) != 0 || !item_is_item(obj)) {
                continue;
            }
            if (location_dist(obj_field_int64_get(obj, OBJ_F_LOCATION), center) > COOP_WORLD_PRUNE_RADIUS) {
                continue;
            }
            oid = obj_get_id(obj);
            if (!coop_oid_set_has(coop_world_seen, (const uint8_t*)&oid) && num < 256) {
                doomed[num++] = obj;
            }
        } while (obj_inst_next(&obj, &iter));
    }
    for (i = 0; i < num; i++) {
        if (getenv("ARCANUM_COOP_WORLD_TRACE") != NULL) {
            char oid_str[80];
            int64_t l = obj_field_int64_get(doomed[i], OBJ_F_LOCATION);

            objid_id_to_str(oid_str, obj_get_id(doomed[i]));
            tig_debug_printf("CoopLite: world trace: prune %s at %d,%d\n", oid_str,
                (int)LOCATION_GET_X(l), (int)LOCATION_GET_Y(l));
        }
        object_destroy(doomed[i]);
    }
    return num;
}
#endif

void coop_lite_handle_world_state(PacketCoopWorldState* pkt)
{
#ifndef COOP_LITE_TEST_STUBS
    int i;

    if (!tig_net_is_active() || tig_net_is_host()) {
        return;
    }
    if (!coop_lite_world_state_valid(pkt, tig_net_last_message_size())) {
        tig_debug_printf("CoopLite: world state: malformed chunk dropped\n");
        return;
    }
    if (!coop_lite_client_world_ready()
        || pkt->map != coop_lite_pose_map_key(map_current_map(), coop_map_gen)) {
        coop_world_rx_active = false;
        return;
    }
    if (coop_world_seen == NULL) {
        coop_world_seen = (CoopOidSet*)MALLOC(sizeof(*coop_world_seen));
        coop_oid_set_reset(coop_world_seen);
    }
    if ((pkt->flags & COOP_WORLD_F_FIRST) != 0) {
        coop_oid_set_reset(coop_world_seen);
        memset(coop_world_stats, 0, sizeof(coop_world_stats));
        coop_world_rx_active = true;
        coop_world_rx_map = pkt->map;
    }
    if (!coop_world_rx_active || coop_world_rx_map != pkt->map) {
        return;
    }

    multiplayer_lock();
    tig_net_local_suspend_begin();
    coop_world_want_cnt = 0;
    for (i = 0; i < pkt->count; i++) {
        coop_lite_world_apply(&pkt->entries[i]);
    }
    if ((pkt->flags & COOP_WORLD_F_LAST) != 0) {
        int pruned = coop_lite_world_prune();

        if (getenv("ARCANUM_COOP_WORLD_TRACE") != NULL && player_get_local_pc_obj() != OBJ_HANDLE_NULL) {
            int64_t l = obj_field_int64_get(player_get_local_pc_obj(), OBJ_F_LOCATION);

            tig_debug_printf("CoopLite: world trace: local PC at %d,%d\n",
                (int)LOCATION_GET_X(l), (int)LOCATION_GET_Y(l));
        }

        if (getenv("ARCANUM_COOP_WORLD_TRACE") != NULL) {
            int64_t pc = player_get_local_pc_obj();
            int64_t obj;
            int iter;

            /* Dump what the dev mutation touched, as the client sees it. */
            if (pc != OBJ_HANDLE_NULL && obj_inst_first(&obj, &iter)) {
                do {
                    int type = obj_field_int32_get(obj, OBJ_F_TYPE);
                    char oid_str[80];

                    if ((obj_field_int32_get(obj, OBJ_F_FLAGS) & (OF_DESTROYED | OF_INVENTORY)) != 0
                        || location_dist(obj_field_int64_get(obj, OBJ_F_LOCATION),
                               obj_field_int64_get(pc, OBJ_F_LOCATION)) > 15) {
                        continue;
                    }
                    objid_id_to_str(oid_str, obj_get_id(obj));
                    if (type == OBJ_TYPE_PORTAL) {
                        tig_debug_printf("CoopLite: world trace: portal %s open=%d flags=0x%x\n", oid_str,
                            portal_is_open(obj), obj_field_int32_get(obj, OBJ_F_PORTAL_FLAGS));
                    } else if (type == OBJ_TYPE_CONTAINER) {
                        tig_debug_printf("CoopLite: world trace: container %s flags=0x%x items=%d\n", oid_str,
                            obj_field_int32_get(obj, OBJ_F_CONTAINER_FLAGS),
                            obj_field_int32_get(obj, OBJ_F_CONTAINER_INVENTORY_NUM));
                    } else if (type == OBJ_TYPE_NPC) {
                        tig_debug_printf("CoopLite: world trace: npc %s dead=%d\n", oid_str, critter_is_dead(obj));
                    } else if (item_is_item(obj)) {
                        int64_t l = obj_field_int64_get(obj, OBJ_F_LOCATION);

                        tig_debug_printf("CoopLite: world trace: ground item %s at %d,%d\n", oid_str,
                            (int)LOCATION_GET_X(l), (int)LOCATION_GET_Y(l));
                    }
                } while (obj_inst_next(&obj, &iter));
            }
        }

        coop_world_rx_active = false;
        tig_debug_printf("CoopLite: world state applied: portals %d, containers %d, npcs %d, ground items %d (%d listed), %d pruned\n",
            coop_world_stats[COOP_WORLD_PORTAL], coop_world_stats[COOP_WORLD_CONTAINER],
            coop_world_stats[COOP_WORLD_NPC], coop_world_stats[COOP_WORLD_GROUND_ITEM],
            coop_world_seen->count, pruned);
    }
    tig_net_local_suspend_end();
    multiplayer_unlock();
    for (i = 0; i < coop_world_want_cnt; i++) {
        if (coop_lite_client_want_ground_blob(coop_world_want[i]->oid, coop_world_want[i]->loc,
                coop_world_want[i]->proto)) {
            coop_world_stats[COOP_WORLD_GROUND_ITEM]++;
        }
    }
    coop_world_want_cnt = 0;
    coop_lite_obj_req_flush();
#else
    (void)pkt;
#endif
}

#ifndef COOP_LITE_TEST_STUBS
static void coop_lite_world_state_reset(void)
{
    memset(coop_world_req_host_seen, 0, sizeof(coop_world_req_host_seen));
    coop_world_rx_active = false;
    coop_world_req_key = -1;
    coop_world_was_ready = false;
    coop_world_ready_key = -1;
}

/* Client tick (world ready): ask after a map load and every 30 s. */
static void coop_lite_client_world_req_tick(unsigned int now_ms)
{
    PacketCoopWorldReq req;
    int key;

    if (getenv("ARCANUM_COOP_NO_WORLD_STATE") != NULL) {
        return;
    }
    key = coop_lite_pose_map_key(map_current_map(), coop_map_gen);
    if (!coop_world_was_ready || key != coop_world_ready_key) {
        coop_world_was_ready = true;
        coop_world_ready_key = key;
        coop_world_ready_ms = now_ms;
    }
    if (!coop_lite_world_req_due(coop_world_req_key, key, coop_world_ready_ms, coop_world_req_ms, now_ms)) {
        return;
    }
    coop_world_req_key = key;
    coop_world_req_ms = now_ms;
    req.type = PACKET_COOP_WORLD_REQ;
    req.map = key;
    tig_net_send_app(&req, sizeof(req), 0);
}
#endif

#ifndef COOP_LITE_TEST_STUBS
/*
 * Dev (ARCANUM_DEV_WORLD_MUTATE=1 on the host): 10 s into the host session,
 * change the world near the host PC without telling anyone (packets
 * suspended): open a closed door, lock another, lock a container and put a
 * dagger in it, kill a living NPC, remove one ground item and drop a new
 * dagger. A client joining (or refreshing) then only learns these through
 * the §7 world state.
 */
static bool coop_dev_mutate_done;
static unsigned int coop_dev_mutate_ms;

static void coop_lite_dev_log_obj(const char* what, int64_t obj)
{
    char oid_str[80];

    objid_id_to_str(oid_str, obj_get_id(obj));
    tig_debug_printf("CoopLite: dev: world mutate: %s %s\n", what, oid_str);
}

static void coop_lite_host_dev_world_mutate_tick(unsigned int now_ms)
{
    const char* env = getenv("ARCANUM_DEV_WORLD_MUTATE");
    int64_t pc = player_get_local_pc_obj();
    int64_t center;
    int64_t obj;
    int64_t opened = OBJ_HANDLE_NULL;
    int64_t locked = OBJ_HANDLE_NULL;
    int64_t container = OBJ_HANDLE_NULL;
    int64_t npc = OBJ_HANDLE_NULL;
    int64_t ground = OBJ_HANDLE_NULL;
    int64_t dagger;
    int64_t portals[8];
    int num_portals = 0;
    int iter;
    int i;

    if (env == NULL || env[0] != '1' || coop_dev_mutate_done || pc == OBJ_HANDLE_NULL
        || map_is_clearing_objects()) {
        return;
    }
    if (coop_dev_mutate_ms == 0) {
        coop_dev_mutate_ms = now_ms != 0 ? now_ms : 1;
        return;
    }
    if (!coop_lite_interval_elapsed(coop_dev_mutate_ms, now_ms, 10000u)) {
        return;
    }
    coop_dev_mutate_done = true;
    center = obj_field_int64_get(pc, OBJ_F_LOCATION);
    if (obj_inst_first(&obj, &iter)) {
        do {
            unsigned int flags = obj_field_int32_get(obj, OBJ_F_FLAGS);
            int type = obj_field_int32_get(obj, OBJ_F_TYPE);

            if ((flags & (OF_DESTROYED | OF_INVENTORY | OF_OFF)) != 0
                || location_dist(obj_field_int64_get(obj, OBJ_F_LOCATION), center) > 20) {
                continue;
            }
            if (type == OBJ_TYPE_PORTAL && !portal_is_open(obj)
                && (obj_field_int32_get(obj, OBJ_F_PORTAL_FLAGS) & OPF_LOCKED) == 0) {
                if (num_portals < 8) {
                    portals[num_portals++] = obj;
                }
            } else if (type == OBJ_TYPE_CONTAINER && container == OBJ_HANDLE_NULL) {
                container = obj;
            } else if (type == OBJ_TYPE_NPC && npc == OBJ_HANDLE_NULL && !critter_is_dead(obj)
                && critter_pc_leader_get(obj) == OBJ_HANDLE_NULL) {
                npc = obj;
            } else if (item_is_item(obj) && !coop_lite_is_stackable(obj) && ground == OBJ_HANDLE_NULL) {
                ground = obj;
            }
        } while (obj_inst_next(&obj, &iter));
    }

    multiplayer_lock();
    tig_net_local_suspend_begin();
    for (i = 0; i < num_portals; i++) {
        if (opened == OBJ_HANDLE_NULL && portal_open(portals[i])) {
            opened = portals[i];
            coop_lite_dev_log_obj("opened door", opened);
        } else if (locked == OBJ_HANDLE_NULL && portals[i] != opened) {
            locked = portals[i];
        }
    }
    if (locked != OBJ_HANDLE_NULL) {
        obj_field_int32_set(locked, OBJ_F_PORTAL_FLAGS, obj_field_int32_get(locked, OBJ_F_PORTAL_FLAGS) | OPF_LOCKED);
        coop_lite_dev_log_obj("locked door", locked);
    }
    if (container != OBJ_HANDLE_NULL) {
        obj_field_int32_set(container, OBJ_F_CONTAINER_FLAGS,
            obj_field_int32_get(container, OBJ_F_CONTAINER_FLAGS) ^ OCOF_LOCKED);
        coop_lite_dev_log_obj("toggled container lock", container);
        if (object_create(sub_4685A0(BP_DAGGER), obj_field_int64_get(container, OBJ_F_LOCATION), &dagger)) {
            item_transfer(dagger, container);
            coop_lite_dev_log_obj("dagger into container", dagger);
        }
    }
    if (npc != OBJ_HANDLE_NULL) {
        critter_kill(npc);
        coop_lite_dev_log_obj("killed npc", npc);
    }
    if (ground != OBJ_HANDLE_NULL) {
        coop_lite_dev_log_obj("removed ground item", ground);
        object_destroy(ground);
    }
    if (object_create(sub_4685A0(BP_DAGGER),
            LOCATION_MAKE(LOCATION_GET_X(center) + 2, LOCATION_GET_Y(center)), &dagger)) {
        coop_lite_dev_log_obj("dropped dagger", dagger);
        coop_dev_mutate_dagger = obj_get_id(dagger);
        coop_dev_mutate_dagger_valid = true;
    }
    tig_net_local_suspend_end();
    multiplayer_unlock();
}
#endif
