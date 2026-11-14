#include "game/coop_lite.h"

#ifndef COOP_LITE_TEST_STUBS
#include "game/area.h" /* PT78 */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "net/tig_net.h"

/*
 * Review PT24 #6: every coop env knob goes through here. Values are read
 * once and cached (nothing in the game calls setenv), and ARCANUM_DEV_*
 * switches - some of them change host rolls - are ignored unless the
 * player explicitly opted in with ARCANUM_DEV=1.
 */
const char* coop_lite_env(const char* name)
{
    static struct {
        const char* name;
        const char* value;
    } cache[64];
    static int cached;
    static int dev = -1;
    static bool dev_warned;
    const char* value;
    int i;

    if (name == NULL) {
        return NULL;
    }
    if (dev < 0) {
        const char* d = getenv("ARCANUM_DEV");

        dev = d != NULL && d[0] == '1';
    }
    for (i = 0; i < cached; i++) {
        if (strcmp(cache[i].name, name) == 0) {
            return cache[i].value;
        }
    }
    value = getenv(name);
    if (value != NULL && !dev && strncmp(name, "ARCANUM_DEV_", 12) == 0) {
        if (!dev_warned) {
            dev_warned = true;
            fprintf(stderr, "CoopLite: %s ignored - dev switches need ARCANUM_DEV=1\n", name);
        }
        value = NULL;
    }
    if (cached < (int)(sizeof(cache) / sizeof(cache[0]))) {
        cache[cached].name = name;
        cache[cached].value = value;
        cached++;
    }
    return value;
}


/* PT52f: client UI close before a host-forced follow teleport (dialog_ui) */
static void (*coop_travel_close_hook)(void);
void coop_lite_set_travel_close_hook(void (*close_ui)(void))
{
    coop_travel_close_hook = close_ui;
}

#ifdef COOP_LITE_TEST_STUBS
#include <stdio.h>
#include <stdint.h>

#ifndef OBJ_HANDLE_NULL
#define OBJ_HANDLE_NULL 0LL
#endif

typedef unsigned int TeleportFlags;
#define TELEPORT_RENDER_LOCK 0x0020u
#define TELEPORT_FORCE_RELOAD 0x0200u

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
#define SECTOR_MAKE(a, b) ((a) | ((b) << 26))

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
#include "game/reaction.h"
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
#include "game/tech.h"
#include "game/mp_utils.h"
#include "game/script.h"
#include "game/teleport.h"
#include "game/tile.h"
#include "game/scroll.h"
#include "game/sector.h"
#include "game/invensource.h"
#include "game/combat.h"
#include "game/spell.h"
#include "game/magictech.h"
#include "game/tf.h"
#include "game/gamelib.h"
#include "game/wallcheck.h"
#include "game/stat.h"
#include "game/timeevent.h"
#include "tig/file.h"
#include "tig/debug.h"
#include "tig/memory.h"
#include "tig/art.h"
#include "game/portrait.h"
#include "game/portal.h"
#include "game/gsound.h"
#include "game/sfx.h"
#include "game/skill.h"
#include "game/trap.h"
#include "game/quest.h"
#include "game/rumor.h"
#include "game/reputation.h"
#include "ui/logbook_ui.h"
#include "game/random.h"
#include "game/gfade.h"
#include "ui/wmap_ui.h"
#include "ui/inven_ui.h"
#include "ui/sleep_ui.h"
#include "ui/dialog_ui.h"
#include "coop_paths.h"

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
    if (count < 0 || count > COOP_CHAR_MAX_ITEMS || count + 1 > COOP_CHAR_BUNDLE_MAX_OBJECTS) {
        tig_debug_printf("CoopLite: character bundle not built: %d items (max %d)\n", count, COOP_CHAR_MAX_ITEMS);
        return false;
    }
    if (!coop_char_bundle_begin(&buf, count + 1)) {
        coop_char_bundle_free(&buf); /* PT53c: begin may have allocated */
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
    CoopCharBundleBuf buf = { 0 }; /* build may fail before writing it */
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

/* PT57: co-op's own teleports (follow, session sync, rejoin, host travel)
   pass the client travel intercept in teleport_do. */
static int coop_client_tp_auth;
static bool coop_lite_auth_teleport(TeleportData* td)
{
    bool ok;

    coop_client_tp_auth++;
    ok = teleport_do(td);
    coop_client_tp_auth--;
    return ok;
}

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
static void coop_lite_inv_list_clear(int64_t owner); /* PT54d */
static bool coop_lite_inv_selfcheck(int64_t owner, const char* what);
static int coop_lite_inv_list_repair(int64_t owner);
#endif

#ifndef COOP_LITE_TEST_STUBS
static CoopPartyWatch coop_party_watch;
static bool coop_reload_request_valid;
static char coop_reload_request_name[TIG_MAX_PATH];
/* Survive the gamelib reset of the reload itself. */
static bool coop_reload_resync_pending;
static bool coop_reload_raise[8];
#endif

/* PT53c: ObjectID equality by value on the 24-byte wire/struct layout
   (type int16 at 0, union at 8). Padding bytes 2..7 and the unused union
   tail are not zeroed everywhere, so memcmp could call equal IDs different.
   Layout-only so the stub test builds (no obj_id.h) can use it. */
bool coop_lite_oid_raw_equal(const void* a, const void* b)
{
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    int16_t tx;
    int16_t ty;
    size_t n;

    memcpy(&tx, x, sizeof(tx));
    memcpy(&ty, y, sizeof(ty));
    if (tx != ty) {
        return false;
    }
    switch (tx) {
    case 0: /* OID_TYPE_NULL */
        return true;
    case 1: /* OID_TYPE_A: int */
        n = 4;
        break;
    case 2: /* OID_TYPE_GUID */
    case 3: /* OID_TYPE_P: int64 location, int temp_id, int map */
        n = 16;
        break;
    default: /* handle / blocked: int64 */
        n = 8;
        break;
    }
    return memcmp(x + 8, y + 8, n) == 0;
}

#ifndef COOP_LITE_TEST_STUBS
_Static_assert(sizeof(ObjectID) == 24 && offsetof(ObjectID, d) == 8, "PT53c: ObjectID layout");
_Static_assert(OID_TYPE_NULL == 0 && OID_TYPE_A == 1 && OID_TYPE_GUID == 2 && OID_TYPE_P == 3, "PT53c: OID types");
_Static_assert(sizeof(TigGuid) == 16 && sizeof(ObjectID_P) == 16, "PT53c: OID payloads");
#endif

/* PT53c: small handle-list helpers (unit-tested). */
bool coop_lite_handle_list_contains(const int64_t* list, int n, int64_t h)
{
    int i;

    for (i = 0; i < n; i++) {
        if (list[i] == h) {
            return true;
        }
    }
    return false;
}

int coop_lite_handle_list_clear(int64_t* list, int n, int64_t h)
{
    int i;
    int cleared = 0;

    for (i = 0; i < n; i++) {
        if (list[i] == h) {
            list[i] = OBJ_HANDLE_NULL;
            cleared++;
        }
    }
    return cleared;
}

/* Client: follow packet seen — carry other PCs on the next own teleport. */
static bool coop_follow_carry_pending;
static int64_t coop_follow_host_loc;
static bool coop_lite_follow_armed; /* set after first host PC teleport; that one is not broadcast */
static bool coop_lite_pending_follow_valid;
static PacketCoopFollowHost coop_lite_pending_follow;
static bool coop_lite_waiting_sync; /* client offered char; waiting for SESSION_SYNC */
static bool coop_lite_session_synced; /* client has applied at least one SESSION_SYNC */
static bool coop_lite_rejoin_no_pc; /* PT69: host had no PC for our rejoin */

typedef struct CoopPendingOffer {
    bool valid;
    int proto;
    char name[64];
    bool has_oid;
    bool rejoin_only; /* PT69: main-menu rejoin, placeholder PC */
    unsigned char oid[24];
    uint64_t token_hash;
} CoopPendingOffer;

/* Host: token hash of each bound slot's player (for parking on leave). */
static char coop_map_wipe_path[260]; /* PT63 */
static char coop_carry_mdy_path[260]; /* PT77aa: carried mobile.mdy, outside the wipe dir */
static bool coop_carry_mdy_started; /* PT77aa */
#define COOP_LITE_CARRY_MDY "Save\\Current\\coop_lite_carry.mdy"
static uint64_t coop_slot_token_hash[8];

static CoopPendingOffer coop_pending_offers[8];

/* Client: SESSION_SYNC waiting for our local-PC teleport to finish. */
static bool coop_pending_sync_valid;
static PacketCoopSessionSync coop_pending_sync;
static int coop_sync_nopc_tries; /* PT72 */
static bool coop_sync_nopc_wait; /* PT72 */
/* PT77ae: in-world restore of our PC after a host load (asked the host for
   our PC's blob; it is loaded at the sync's client_loc). */
static bool coop_sync_restore_asked;
static int64_t coop_sync_restore_loc;
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
/* PT31: what the owner last got of its own PC's host-side state. */
static unsigned int coop_owner_state[8][3];
static bool coop_owner_state_valid[8];
static unsigned int coop_owner_state_ms[8];

/* PT31: movement-blocking critter flags of a client's own PC are the host's
   (magictech runs there; its end is a no-op on clients). */
#define COOP_OWN_HOST_OCF (OCF_PARALYZED | OCF_STUNNED)

bool coop_lite_can_use_worldmap(void)
{
    if (!tig_net_is_active()) {
        return true;
    }
    return tig_net_is_host();
}

bool coop_lite_worldmap_client_open_ok(bool can_use, bool spectating, bool client_may_propose)
{
    return spectating || can_use || client_may_propose;
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

static bool coop_preshop;

bool coop_lite_client_preshop_active(void)
{
    return coop_preshop;
}

bool coop_lite_is_client_role(void)
{
    if (coop_preshop) {
        return false; /* PT40: the pre-session shop is single player */
    }
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
    /* PT23: a corpse never talks (the host's death may have reached us as
       NPC_POS after the click was queued). */
    if (critter_is_dead(npc_obj)
        || (obj_field_int32_get(npc_obj, OBJ_F_FLAGS) & OF_FLAT) != 0) {
        tig_debug_printf("CoopLite: talk refused — NPC is dead\n");
        return COOP_TALK_BLOCK;
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
    /* PT54c: radius 1 up - never the host's own tile. */
    for (radius = 1; radius <= 8; radius++) {
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

/* PT31: a paralysis/stun left on our own PC (client-side magictech never
   ends here) must not keep it from travelling with the host; the host
   re-asserts it in the PC_POS look if it still holds. */
static int coop_follow_busy_retries;
/* PT32: session-scoped state (cleared in coop_lite_session_reset). */
static unsigned int coop_in_world_ms; /* host: since when settled, follow arming */
static unsigned int coop_mismatch_key;
static unsigned int coop_mismatch_since_ms;
static unsigned int coop_mismatch_follow_ms;

/* PT32: the client's PC location when it SENT the WORLD_REQ. The host samples
   COOP_WORLD_RADIUS around its copy of that PC (a PC_POS old), so pruning is
   measured from here, never from where the client has run to since. */
static int64_t coop_world_req_center;
static bool coop_world_req_center_valid;

static void coop_lite_client_clear_stuck_flags(int64_t pc_obj, const char* why)
{
#ifndef COOP_LITE_TEST_STUBS
    unsigned int ocf;

    if (pc_obj == OBJ_HANDLE_NULL || tig_net_is_host()) {
        return;
    }
    ocf = (unsigned int)obj_field_int32_get(pc_obj, OBJ_F_CRITTER_FLAGS);
    if ((ocf & COOP_OWN_HOST_OCF) != 0) {
        obj_field_int32_set(pc_obj, OBJ_F_CRITTER_FLAGS, (int)(ocf & ~(unsigned int)COOP_OWN_HOST_OCF));
        tig_debug_printf("CoopLite: own PC paralysis/stun 0x%x cleared (%s)\n", ocf & COOP_OWN_HOST_OCF, why);
    }
#else
    (void)pc_obj;
    (void)why;
#endif
}

#ifndef COOP_LITE_TEST_STUBS
static int coop_lite_inv_list_repair(int64_t owner);
#endif

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

#ifndef COOP_LITE_TEST_STUBS
    if (wmap_ui_is_spectating()) {
        wmap_ui_spectate_close(); /* PT73: host arrived */
    }
#endif
    pc_obj = player_get_local_pc_obj();
    /* PT52f: forced travel - close dialog/barter/inventory/char view first
       (PT52i: only when there is a PC to move) */
    if (pc_obj != OBJ_HANDLE_NULL && coop_travel_close_hook != NULL) {
        coop_travel_close_hook();
    }
    slot = sub_529520();
    dest = coop_lite_follow_loc_for_slot(pkt->loc, slot);
    coop_lite_client_clear_stuck_flags(pc_obj, "follow");
    /* PT54c: teleport.c refuses to schedule a critter whose inventory NUM
       != list length - the PC then never reached the new map (client stuck
       "not in world", host timed it out). Repair the list first. */
#ifndef COOP_LITE_TEST_STUBS
    if (pc_obj != OBJ_HANDLE_NULL) {
        coop_lite_inv_list_repair(pc_obj);
    }
#endif

    memset(&teleport_data, 0, sizeof(teleport_data));
    teleport_data.flags = TELEPORT_RENDER_LOCK;
    teleport_data.obj = pc_obj;
    teleport_data.loc = dest;
    teleport_data.map = pkt->map;

    /* The teleport runs later (teleport_ping); carry the other PCs then. */
    coop_follow_carry_pending = true;
    coop_follow_host_loc = pkt->loc;
    coop_lite_applying_follow = true;
    if (!coop_lite_auth_teleport(&teleport_data)) {
        /* PT23: refused (another local teleport pending) — never leave the
           carry wait armed: it stops our PC_POS until the next travel. */
        coop_follow_carry_pending = false;
        /* PT31: never drop the host's travel - retried when the pending
           teleport is done (teleport.c flushes it). */
        if (coop_follow_busy_retries < 8) {
            coop_follow_busy_retries++;
            coop_lite_pending_follow = *pkt;
            coop_lite_pending_follow_valid = true;
        }
        tig_debug_printf("CoopLite: client follow teleport refused (busy) - %s\n",
            coop_lite_pending_follow_valid ? "queued" : "dropped");
    } else {
        /* PT77q: only after a successful teleport, so a refused follow
           does not leave us on the old map with the new generation. */
        coop_map_gen = (uint16_t)pkt->map_gen;
        coop_follow_busy_retries = 0;
    }
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
static void coop_lite_barter_end(int slot, const char* why);
static void coop_lite_host_victim_watch(unsigned int now_ms);
static void coop_lite_host_pc_stats_tick(unsigned int now_ms);
static void coop_lite_host_critter_effects_tick(unsigned int now_ms);
static void coop_lite_client_pc_stats_tick(unsigned int now_ms);
static void coop_lite_host_story_tick(unsigned int now_ms);
static void coop_lite_client_story_tick(unsigned int now_ms);
static void coop_lite_area_tick(unsigned int now_ms); /* PT78 */
static void coop_lite_host_npc_stats(int64_t npc, unsigned int now_ms); /* PT79 */
static void coop_lite_story_reset(void);
static void coop_lite_host_dialog_tick(unsigned int now_ms);
static void coop_lite_party_ui_tick(unsigned int now_ms);
static void coop_lite_dialog_reset(void);
static void coop_lite_host_barter_watch(unsigned int now_ms);
void coop_lite_client_dev_barter_tick(unsigned int now_ms);
void coop_lite_client_dev_loot_tick(unsigned int now_ms);
void coop_lite_client_layout_tick(unsigned int now_ms);
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
/* PT53/PT54 pack layout state. Host: last INVEN_LAYOUT seq applied per slot
   (echoed in INVEN_SYNC flags). Client: hashes, newest seq sent, echo seq
   of the batch being applied. */
static unsigned int coop_layout_seq_applied[8];
static unsigned int coop_inv_echo_seq;
static unsigned int coop_layout_sent_seq;
static unsigned int coop_layout_next_seq;
static unsigned int coop_layout_sent_ms;
static unsigned int coop_layout_host_hash;
/* PT57: other players' packs (by slot) as the host last listed them. */
static unsigned int coop_other_host_hash[8];
static unsigned int coop_other_acked_hash[8];
static bool coop_other_valid[8];

static bool coop_layout_host_valid;
static unsigned int coop_layout_acked_hash;
/* PT37: debounced WORLD_DIRTY after any door change on the host. */
static bool coop_portal_changed;
static unsigned int coop_portal_changed_ms;

/* PT23: cheap counters, logged every 30 s on both sides ("CoopDiag:"), so a
   user's log shows growth (anim slots, time events, queues) and desync. */
typedef struct CoopDiag {
    unsigned int window_ms;
    unsigned int pings;
    unsigned int npc_tx;        /* host: NPC_POS entries sent */
    unsigned int npc_rx;        /* client: NPC_POS entries applied */
    unsigned int npc_rx_unknown; /* client: NPC_POS for an OID we do not have */
    unsigned int npc_req; /* PT26 client: asked the host for such an NPC */
    unsigned int npc_rx_on; /* PT26 client: NPC_POS switched an OF_OFF NPC on */
    unsigned int npc_rx_dead;   /* client: corpse state applied from NPC_POS */
    unsigned int pc_rx;         /* PC_POS received */
    unsigned int pc_tx;         /* client: own PC_POS sent */
    unsigned int pc_drop_map;   /* PC_POS dropped: other map / map generation */
    unsigned int pc_drop_order; /* host: PC_POS dropped: old epoch / seq */
    unsigned int pc_drop_bad;   /* host: PC_POS implausible */
    unsigned int force_tx;      /* host: FORCE corrections sent */
    unsigned int own_corrections; /* client: host moved our PC */
    unsigned int attack_intents;
    unsigned int attack_approaches;
    unsigned int swing_tx; /* host: PT25 strike echoes to owners */
    unsigned int swing_rx; /* client: strike echoes received */
    unsigned int swing_anim; /* PT26 client: swing animations started */
    unsigned int swing_anim_fail; /* PT26 client: ... refused */
    unsigned int npc_sound_tx; /* host: PT25 NPC sounds forwarded */
    unsigned int p10_own;  /* PT27 client: host anim-end poses for our own PC, dropped */
    unsigned int p10_peer; /* ... for another (interpolated) PC, dropped */
    unsigned int p10_npc;  /* ... for an NPC (NPC_POS drives it), dropped */
    unsigned int peer_anim; /* PT27 client: interp frames a peer PC had a local anim run */
    unsigned int interp_lag; /* PT27 client: max tiles a drawn peer PC trailed its newest pose */
    unsigned int max_gap_ms;
    unsigned int last_ping_ms;
} CoopDiag;
static CoopDiag coop_diag;
/* Client: remote PC pose rings, by slot (never the local PC). */
static CoopInterpRing coop_interp[8];
/* Not cleared by session_reset: it fires after the session is gone. */
static bool coop_host_left_menu_armed;
static unsigned int coop_host_left_menu_ms;
static bool coop_wait_host; /* PT62: main menu retries the host */
static void (*coop_return_to_menu_hook)(void);
#ifndef COOP_LITE_TEST_STUBS
static void coop_lite_obj_pending_reset(void);
static void coop_lite_obj_ground_fallback_tick(unsigned int now_ms);
#ifndef COOP_LITE_TEST_STUBS
static void coop_lite_host_door_flush_tick(unsigned int now_ms); /* PT53e */
static void coop_lite_client_hotbar_tick(unsigned int now_ms); /* PT63 */
static void coop_lite_host_hotbar_tick(void); /* PT63 */
static void coop_lite_hotbar_after_load(void); /* PT63 */
static void coop_lite_host_vitals_force(void); /* PT54 */
static void coop_lite_host_pcs_force(void); /* PT54a */
static void coop_lite_show_notice(const char* str, int tf_type); /* PT77ad */
static void coop_lite_client_restore_pc_ask(const PacketCoopSessionSync* pkt); /* PT77ae */
#endif
static void coop_lite_client_interp_tick(unsigned int now_ms);
static void coop_lite_client_world_req_tick(unsigned int now_ms);
static void coop_lite_client_world_refresh_soon(void);
static void coop_lite_world_state_reset(void);
static void coop_lite_host_dev_world_mutate_tick(unsigned int now_ms);
static void coop_lite_host_dev_kill_client_tick(unsigned int now_ms);
static void coop_lite_host_dev_auto_resurrect_tick(unsigned int now_ms);
static void coop_lite_host_dev_gold_pile_tick(unsigned int now_ms);
static void coop_lite_client_dev_gold_pile_tick(unsigned int now_ms);
static void coop_lite_client_pickup_tick(unsigned int now_ms);
static void coop_lite_client_pickup_reset(void);
static void coop_lite_client_door_tick(void);
#endif

#ifndef COOP_LITE_TEST_STUBS
/* Build fingerprint of a PC (base stats, skills, colleges): host and client
   copies of one character must log the same value (PT15: compare the host's
   bundle PC with what the client shows). */
static unsigned int coop_lite_pc_build_hash(int64_t pc)
{
    static const int stats[] = {
        STAT_STRENGTH, STAT_DEXTERITY, STAT_CONSTITUTION, STAT_BEAUTY,
        STAT_INTELLIGENCE, STAT_PERCEPTION, STAT_WILLPOWER, STAT_CHARISMA,
        STAT_LEVEL, STAT_UNSPENT_POINTS, STAT_RACE, STAT_GENDER,
    };
    unsigned int h = 2166136261u;
    size_t i;
    int v;

#define COOP_HASH_INT(x) do { v = (x); h = (h ^ (unsigned int)v) * 16777619u; } while (0)
    for (i = 0; i < sizeof(stats) / sizeof(stats[0]); i++) {
        COOP_HASH_INT(stat_base_get(pc, stats[i]));
    }
    for (i = 0; i < BASIC_SKILL_COUNT; i++) {
        COOP_HASH_INT(basic_skill_points_get(pc, (int)i));
    }
    for (i = 0; i < TECH_SKILL_COUNT; i++) {
        COOP_HASH_INT(tech_skill_points_get(pc, (int)i));
    }
    for (i = 0; i < COLLEGE_COUNT; i++) {
        COOP_HASH_INT(spell_college_level_get(pc, (int)i));
    }
#undef COOP_HASH_INT
    return h;
}
#endif

/* Rejoin: our parked PC (host copy) is on its way; until it replaces the
   menu character, that one has no PC on the host to report for. */
static bool coop_rejoin_pc_wanted;
static unsigned int coop_rejoin_pc_ms; /* PT54: when the parked PC was asked for */

/*
 * Review P0-3: client-side application of host state (inventory syncs,
 * blobs, world state). Packets stay locally suspended as before, and object
 * scripts are muted: item_insert / item_remove / object_destroy would run
 * SAP_INSERT_ITEM / SAP_REMOVE_ITEM / SAP_DESTROY here, effects the host
 * already had. end() restores the exact suspend depth begin() saw, so an
 * unbalanced call inside cannot leak suspension past the scope.
 */
#define COOP_APPLY_STACK 8
static int coop_apply_depth[COOP_APPLY_STACK];
static int coop_apply_top;
static int coop_script_mute;

bool coop_lite_scripts_muted(void)
{
    return coop_script_mute > 0;
}

static void coop_lite_apply_begin(void)
{
    if (coop_apply_top < COOP_APPLY_STACK) {
        coop_apply_depth[coop_apply_top] = tig_net_local_suspend_pause();
        tig_net_local_suspend_resume(coop_apply_depth[coop_apply_top]);
    }
    coop_apply_top++;
    tig_net_local_suspend_begin();
    coop_script_mute++;
}

static void coop_lite_apply_end(void)
{
    if (coop_script_mute > 0) {
        coop_script_mute--;
    }
    if (coop_apply_top <= 0) {
        tig_debug_printf("CoopLite: apply scope underflow\n");
        tig_net_local_suspend_end();
        return;
    }
    coop_apply_top--;
    if (coop_apply_top < COOP_APPLY_STACK) {
        int want = coop_apply_depth[coop_apply_top];
        int have = tig_net_local_suspend_pause();

        if (have != want + 1) {
            tig_debug_printf("CoopLite: apply scope: suspend depth %d, expected %d — restored\n", have, want + 1);
        }
        tig_net_local_suspend_resume(want);
    } else {
        tig_net_local_suspend_end();
    }
}

static void coop_lite_char_rx_reset_all(void);
static void coop_lite_client_attack_reset(void);
#ifndef COOP_LITE_TEST_STUBS
static void coop_lite_session_reset_extra(void);
void coop_lite_travel_reset(void);
void coop_lite_travel_session_reset(void);
#endif

#ifndef COOP_LITE_TEST_STUBS
static void coop_lite_worn_reassert_reset(void);
#endif

void coop_lite_session_reset(void)
{
    int i;

#ifndef COOP_LITE_TEST_STUBS
    coop_lite_worn_reassert_reset(); /* PT34: no stale ring across sessions */
#endif

    /* Review P0-1: half-received bundles must not survive a session. */
    coop_lite_char_rx_reset_all();

    for (i = 0; i < 8; i++) {
        coop_interp_reset(&coop_interp[i]);
    }
    coop_rejoin_pc_wanted = false;
    coop_rejoin_pc_ms = 0;
    /* PT54: pack layout state starts over with the session. */
    memset(coop_layout_seq_applied, 0, sizeof(coop_layout_seq_applied));
    coop_inv_echo_seq = 0;
    coop_layout_sent_seq = 0;
    coop_layout_next_seq = 0;
    coop_layout_host_valid = false;
    coop_layout_host_hash = 0;
    memset(coop_other_valid, 0, sizeof(coop_other_valid)); /* PT57 */
    coop_layout_acked_hash = 0;
    coop_in_world_ms = 0;
    coop_mismatch_key = 0;
    coop_mismatch_since_ms = 0;
    coop_mismatch_follow_ms = 0;
    coop_follow_busy_retries = 0;
    coop_world_req_center_valid = false;

    coop_lite_follow_armed = false;
    coop_lite_pose_order_reset();
    coop_lite_client_attack_reset();
    memset(&coop_diag, 0, sizeof(coop_diag));
#ifndef COOP_LITE_TEST_STUBS
    coop_lite_obj_pending_reset();
    coop_lite_roster_reset();
    coop_lite_world_state_reset();
#endif
    coop_lite_pending_follow_valid = false;
    memset(&coop_lite_pending_follow, 0, sizeof(coop_lite_pending_follow));
    coop_lite_waiting_sync = false;
    coop_lite_session_synced = false;
    coop_lite_rejoin_no_pc = false;
    coop_lite_client_in_session = false;
    coop_client_pos_valid = false;
    coop_last_census_ms = 0;
    coop_pending_sync_valid = false;
    coop_pending_sync_teleport_queued = false;
    coop_pending_sync_retry_ms = 0;
    coop_sync_nopc_wait = false;
    coop_sync_nopc_tries = 0;
    coop_sync_restore_asked = false;
    coop_last_pc_pos_ms = 0;
#ifndef COOP_LITE_TEST_STUBS
    coop_lite_world_reset();
    coop_lite_barter_reset();
    coop_lite_session_reset_extra();
    mp_clear_deferred_packet70();
    memset(coop_reload_raise, 0, sizeof(coop_reload_raise));
#endif
    for (i = 0; i < 8; i++) {
        coop_last_pc_pos_valid[i] = false;
        coop_owner_state_valid[i] = false;
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

#ifndef COOP_LITE_TEST_STUBS
/* PT23: client melee approach (see anim_goal_attack). */
static bool coop_client_attack_pending;
static ObjectID coop_client_attack_target;
static unsigned int coop_client_attack_ms;
static unsigned int coop_client_attack_repath_ms;
static int64_t coop_client_attack_goal_loc;

static void coop_lite_client_attack_reset(void)
{
    coop_client_attack_pending = false;
}

static int coop_lite_client_attack_reach(int64_t attacker)
{
    int range = item_weapon_range(item_wield_get(attacker, ITEM_INV_LOC_WEAPON), attacker);

    return range < 1 ? 1 : range;
}
#endif

#ifdef COOP_LITE_TEST_STUBS
static void coop_lite_client_attack_reset(void)
{
}
#endif

static void coop_lite_client_play_swing(int64_t attacker_obj, int64_t target_obj);
static bool coop_client_swing_click; /* PT27: the click drew a swing; skip its own echo once */

bool coop_lite_client_send_attack_intent(int64_t attacker_obj, int64_t target_obj)
{
#ifndef COOP_LITE_TEST_STUBS
    PacketCoopAttackIntent pkt;
    ObjectID attacker_oid;
    ObjectID target_oid;

    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_ATTACK_INTENT;
    pkt.attack_kind = 0;
    attacker_oid = obj_get_id(attacker_obj);
    target_oid = obj_get_id(target_obj);
    memcpy(pkt.attacker_oid, &attacker_oid, sizeof(attacker_oid));
    memcpy(pkt.target_oid, &target_oid, sizeof(target_oid));
    tig_net_send_app_all(&pkt, sizeof(pkt));
    coop_diag.attack_intents++;

    /* Review M6: the host resolves the hit, but its swing never reached
       the attacker's own screen. Adjacent target: play the swing here
       (cosmetic AG_ANIMATE only — no hit is resolved locally). */
    if (location_dist(obj_field_int64_get(attacker_obj, OBJ_F_LOCATION),
            obj_field_int64_get(target_obj, OBJ_F_LOCATION))
        <= 1) {
        coop_lite_client_play_swing(attacker_obj, target_obj);
        coop_client_swing_click = true;
    }
#else
    (void)attacker_obj;
    (void)target_obj;
#endif
    return true;
}

bool coop_lite_client_attack_approach(int64_t attacker_obj, int64_t target_obj)
{
#ifndef COOP_LITE_TEST_STUBS
    int reach;
    int64_t tloc;

    if (attacker_obj == OBJ_HANDLE_NULL || target_obj == OBJ_HANDLE_NULL) {
        return false;
    }
    reach = coop_lite_client_attack_reach(attacker_obj);
    tloc = obj_field_int64_get(target_obj, OBJ_F_LOCATION);
    if (location_dist(obj_field_int64_get(attacker_obj, OBJ_F_LOCATION), tloc) <= reach) {
        coop_client_attack_pending = false;
        return false;
    }
    if (!anim_goal_run_near_tile(attacker_obj, tloc, reach)) {
        /* No path: let the host decide (old behaviour). */
        coop_client_attack_pending = false;
        return false;
    }
    coop_client_attack_pending = true;
    coop_client_attack_target = obj_get_id(target_obj);
    coop_client_attack_ms = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1;
    coop_client_attack_repath_ms = coop_client_attack_ms;
    coop_client_attack_goal_loc = tloc;
    coop_diag.attack_approaches++;
    tig_debug_printf("CoopLite: attack: approaching target %d tiles away (reach %d)\n",
        (int)location_dist(obj_field_int64_get(attacker_obj, OBJ_F_LOCATION), tloc), reach);
    return true;
#else
    (void)attacker_obj;
    (void)target_obj;
    return false;
#endif
}

#ifndef COOP_LITE_TEST_STUBS
/* Client tick: finish a pending approach (send the intent once in reach). */
static void coop_lite_client_attack_tick(unsigned int now_ms)
{
    int64_t pc = player_get_local_pc_obj();
    int64_t target;
    int64_t tloc;
    int reach;

    if (!coop_client_attack_pending) {
        return;
    }
    target = obj_pool_perm_lookup(coop_client_attack_target);
    if (pc == OBJ_HANDLE_NULL || target == OBJ_HANDLE_NULL || critter_is_dead(target) || critter_is_dead(pc)
        || coop_lite_interval_elapsed(coop_client_attack_ms, now_ms, 8000u)) {
        coop_client_attack_pending = false;
        return;
    }
    reach = coop_lite_client_attack_reach(pc);
    tloc = obj_field_int64_get(target, OBJ_F_LOCATION);
    if (location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION), tloc) <= reach) {
        coop_client_attack_pending = false;
        tig_debug_printf("CoopLite: attack: in reach — intent sent\n");
        coop_lite_client_send_attack_intent(pc, target);
        return;
    }
    /* The target moved: re-path at most twice a second. */
    if (tloc != coop_client_attack_goal_loc
        && coop_lite_interval_elapsed(coop_client_attack_repath_ms, now_ms, 500u)) {
        coop_client_attack_repath_ms = now_ms;
        coop_client_attack_goal_loc = tloc;
        anim_goal_run_near_tile(pc, tloc, reach);
    }
}
#endif

#ifndef COOP_LITE_TEST_STUBS
bool coop_lite_host_chase_to_owner(int64_t attacker, int64_t target);

typedef struct CoopHostIntent {
    bool valid;
    bool retry; /* PT26: AG_ATTACK refused (busy anim): retrying */
    int tries;
    int64_t attacker;
    int64_t target;
    unsigned int ms;
} CoopHostIntent;
static CoopHostIntent coop_host_intent[8];

/* Host resolves AG_ATTACK - combat_dmg / ai_attack run here (event-level). */
static void coop_lite_host_start_attack(int64_t attacker, int64_t target, int slot)
{
    bool ok = anim_goal_attack(attacker, target);
    char tname[128];

    /* PT26 item 2: a refusal that is not "already attacking" is a busy
       animation (hit reaction, the PC_POS walk, a stand-up): one swing and
       then nothing until the next click. Keep trying for 1.5 s. */
    if (!ok && slot >= 1 && slot < 8 && !anim_is_current_goal_type(attacker, AG_ATTACK, NULL)) {
        CoopHostIntent* in = &coop_host_intent[slot];

        if (!(in->valid && in->retry && in->attacker == attacker && in->target == target)) {
            in->valid = true;
            in->retry = true;
            in->tries = 0;
            in->attacker = attacker;
            in->target = target;
            in->ms = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1;
            tig_debug_printf("CoopLite: slot %d attack refused (animating %d, hit %d, moving %d) - retrying\n", slot,
                anim_is_current_goal_type(attacker, AG_ANIMATE, NULL),
                anim_is_current_goal_type(attacker, AG_HIT_BY_WEAPON, NULL) || anim_is_current_goal_type(attacker, AG_KNOCK_DOWN, NULL),
                anim_is_current_goal_type(attacker, AG_MOVE_TO_TILE, NULL)
                    || anim_is_current_goal_type(attacker, AG_RUN_NEAR_TILE, NULL)
                    || anim_is_current_goal_type(attacker, AG_MOVE_NEAR_TILE, NULL));
            return;
        }
        return; /* the tick keeps trying */
    }
    if (slot >= 1 && slot < 8 && coop_host_intent[slot].retry) {
        if (ok) {
            tig_debug_printf("CoopLite: slot %d attack started after %d retries\n", slot, coop_host_intent[slot].tries);
        }
        coop_host_intent[slot].valid = false;
        coop_host_intent[slot].retry = false;
    }

    tname[0] = '\0';
    object_examine(target, attacker, tname);
    tig_debug_printf("CoopLite: slot %d attacks '%s' (dist %d): %s\n", slot, tname,
        (int)location_dist(obj_field_int64_get(attacker, OBJ_F_LOCATION), obj_field_int64_get(target, OBJ_F_LOCATION)),
        ok ? "goal started"
           : (anim_is_current_goal_type(attacker, AG_ATTACK, NULL) ? "goal refused (already attacking)"
                                                                   : "goal refused"));
}

/* Host tick: start a deferred intent once the owner's pose is in reach. */
static void coop_lite_host_intent_tick(unsigned int now_ms)
{
    int slot;

    for (slot = 1; slot < 8; slot++) {
        CoopHostIntent* in = &coop_host_intent[slot];
        int range;

        if (!in->valid) {
            continue;
        }
        if (sub_4A2B60(slot) != in->attacker || !obj_handle_is_valid(in->target)
            || critter_is_dead(in->attacker) || critter_is_dead(in->target)) {
            in->valid = false;
            continue;
        }
        range = item_weapon_range(item_wield_get(in->attacker, ITEM_INV_LOC_WEAPON), in->attacker);
        if (range < 1) {
            range = 1;
        }
        if (in->retry) {
            if (coop_lite_interval_elapsed(in->ms, now_ms, 1500u) || object_dist(in->attacker, in->target) > range) {
                tig_debug_printf("CoopLite: slot %d attack retry gave up (%d tries, %d tiles)\n", slot, in->tries,
                    (int)object_dist(in->attacker, in->target));
                in->valid = false;
                in->retry = false;
                if (object_dist(in->attacker, in->target) > range) {
                    coop_lite_host_chase_to_owner(in->attacker, in->target);
                }
            } else {
                in->tries++;
                coop_lite_host_start_attack(in->attacker, in->target, slot);
            }
            continue;
        }
        if (object_dist(in->attacker, in->target) <= range) {
            in->valid = false;
            coop_lite_host_start_attack(in->attacker, in->target, slot);
        } else if (coop_lite_interval_elapsed(in->ms, now_ms, 1000u)) {
            /* Still apart here: the owner walks, we never do. */
            in->valid = false;
            tig_debug_printf("CoopLite: slot %d attack intent: still %d tiles away here - owner re-approaches\n",
                slot, (int)object_dist(in->attacker, in->target));
            coop_lite_host_chase_to_owner(in->attacker, in->target);
        }
    }
}
#endif

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
    if (coop_lite_party_no_fight(attacker, target)) {
        /* PT77af: a client PC never starts a fight with a co-op player or
           any party companion (the hit made the companions retaliate). */
        tig_debug_printf("CoopLite: slot %d attack intent refused: target is in the co-op party\n", sub_52A530());
        return;
    }
    {
        char tname[128];

        tname[0] = '\0';
        object_examine(target, attacker, tname);
        tig_debug_printf("CoopLite: slot %d attack intent on '%s'\n", sub_52A530(), tname);
    }
    (void)pkt->attack_kind;
    /* PT25: the owner sends the intent once in reach on ITS screen; our
       copy of its pose may still lag a step behind. Starting AG_ATTACK now
       made the host walk the PC itself (FORCE snaps on the owner's side):
       wait up to 1 s for the pose to catch up. */
    {
        int slot = sub_52A530();
        int range = item_weapon_range(item_wield_get(attacker, ITEM_INV_LOC_WEAPON), attacker);

        if (range < 1) {
            range = 1;
        }
        if (slot >= 1 && slot < 8 && object_dist(attacker, target) > range
            && object_dist(attacker, target) <= range + 3) {
            coop_host_intent[slot].valid = true;
            coop_host_intent[slot].retry = false;
            coop_host_intent[slot].attacker = attacker;
            coop_host_intent[slot].target = target;
            coop_host_intent[slot].ms = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1;
            return;
        }
        coop_host_intent[slot >= 1 && slot < 8 ? slot : 0].valid = false;
        /* PT37: far off (stale target on the owner's screen): AG_ATTACK
           would walk the client PC across the map (FORCE snaps x115). Make
           the owner re-approach instead; its next click re-sends. */
        if (slot >= 1 && slot < 8 && object_dist(attacker, target) > range + 3) {
            tig_debug_printf("CoopLite: slot %d attack intent refused: %d tiles away (range %d) - owner re-approaches\n",
                slot, (int)object_dist(attacker, target), range);
            coop_lite_host_chase_to_owner(attacker, target);
            return;
        }
    }
    coop_lite_host_start_attack(attacker, target, sub_52A530());
#else
    (void)attacker;
    (void)target;
#endif
}

static int coop_lite_slot_of_obj(int64_t obj);
static bool coop_lite_client_world_ready(void);
#ifndef COOP_LITE_TEST_STUBS
static unsigned int coop_client_swing_ms; /* last local swing animation */
#endif

/* The owner's swing animation (cosmetic AG_ANIMATE, no hit locally). */
static void coop_lite_client_play_swing(int64_t attacker_obj, int64_t target_obj)
{
#ifndef COOP_LITE_TEST_STUBS
    tig_art_id_t art_id = obj_field_int32_get(attacker_obj, OBJ_F_CURRENT_AID);

    bool moving = anim_is_current_goal_type(attacker_obj, AG_MOVE_TO_TILE, NULL)
        || anim_is_current_goal_type(attacker_obj, AG_RUN_NEAR_TILE, NULL);
    bool ok;

    tig_net_local_suspend_begin();
    /* PT30: resetting the frame of a running cosmetic swing (PT28) froze
       the PC: an echo usually lands while AG_ANIMATE holds the LAST frame,
       and that run then just ends - no swing played. A swing still in
       its frames is left to finish; one that is done (last frame) is
       replaced by a fresh AG_ANIMATE below. */
    if (tig_art_id_anim_get(art_id) == TIG_ART_ANIM_ATTACK
        && anim_is_current_goal_type(attacker_obj, AG_ANIMATE, NULL)) {
        TigArtAnimData data;

        if (tig_art_anim_data(art_id, &data) == TIG_OK && data.num_frames > 1
            && tig_art_id_frame_get(art_id) < data.num_frames - 2) {
            coop_diag.swing_anim++;
            tig_net_local_suspend_end();
            coop_client_swing_ms = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1;
            return;
        }
    }
    /* PT27: a previous cosmetic swing still running would hold this one
       back; restart it (never cut a walk). */
    if (!moving && anim_is_current_goal_type(attacker_obj, AG_ANIMATE, NULL)) {
        /* PT30: PRIORITY_5 did not end the cosmetic AG_ANIMATE (the new
           one then waited behind it, frozen on the last frame). */
        sub_424070(attacker_obj, PRIORITY_HIGHEST, false, true);
        art_id = obj_field_int32_get(attacker_obj, OBJ_F_CURRENT_AID);
    }
    /* PT30 root cause of "swings once": anim_goal_animate keeps the
       current frame. After a swing the art stays on the ATTACK anim's last
       frame, so every later swing started there and ended at once. */
    object_set_current_aid(attacker_obj,
        tig_art_id_frame_set(tig_art_id_rotation_set(art_id, object_rot(attacker_obj, target_obj)), 0));
    ok = anim_goal_animate(attacker_obj, TIG_ART_ANIM_ATTACK);
    if (coop_lite_env("ARCANUM_DEV_SWING_TRACE") != NULL) {
        tig_debug_printf("SWTRACE play: moving %d ok %d frame %d animate %d\n", moving, ok,
            tig_art_id_frame_get(obj_field_int32_get(attacker_obj, OBJ_F_CURRENT_AID)),
            anim_is_current_goal_type(attacker_obj, AG_ANIMATE, NULL));
    }
    if (!ok && !moving) {
        sub_424070(attacker_obj, PRIORITY_HIGHEST, false, true);
        ok = anim_goal_animate(attacker_obj, TIG_ART_ANIM_ATTACK);
    }
    if (ok) {
        coop_diag.swing_anim++;
    } else {
        coop_diag.swing_anim_fail++;
        tig_debug_printf("CoopLite: swing anim refused (animating %d, attacking %d, moving %d)\n",
            anim_is_current_goal_type(attacker_obj, AG_ANIMATE, NULL),
            anim_is_current_goal_type(attacker_obj, AG_ATTACK, NULL),
            anim_is_current_goal_type(attacker_obj, AG_MOVE_TO_TILE, NULL)
                || anim_is_current_goal_type(attacker_obj, AG_RUN_NEAR_TILE, NULL));
    }
    tig_net_local_suspend_end();
    coop_client_swing_ms = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1;
#else
    (void)attacker_obj;
    (void)target_obj;
#endif
}

bool coop_lite_combat_text_allowed(int text_num)
{
    /* 2 "Critical miss", 11 "Dodge!", 12 "Critical hit" */
    return text_num == 2 || text_num == 11 || text_num == 12;
}

bool coop_lite_pose_keeps_swing(bool cur_is_attack, bool animating, bool new_is_stand_or_attack)
{
    return cur_is_attack && animating && new_is_stand_or_attack;
}

#ifndef COOP_LITE_TEST_STUBS
/* PT75: send pkt to every client slot whose PC is within 25 tiles of loc
   (same radius as the PT28 combat sounds), except skip_slot. */
static void coop_lite_send_near(PacketCoopSwing* pkt, int64_t loc, int skip_slot)
{
    int slot;

    for (slot = 1; slot < 8; slot++) {
        int64_t pc;

        if (slot == skip_slot) {
            continue;
        }
        pc = sub_4A2B60(slot);
        if (pc != OBJ_HANDLE_NULL && location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION), loc) <= 25) {
            tig_net_send_app(pkt, sizeof(*pkt), slot);
            coop_diag.swing_tx++;
        }
    }
}
#endif

static void coop_lite_client_play_other_swing(int64_t attacker_obj, int64_t target_obj)
{
#ifndef COOP_LITE_TEST_STUBS
    /* PT75: coop_client_swing_ms belongs to our own PC's swings. */
    unsigned int saved = coop_client_swing_ms;

    coop_lite_client_play_swing(attacker_obj, target_obj);
    coop_client_swing_ms = saved;
#else
    (void)attacker_obj;
    (void)target_obj;
#endif
}

void coop_lite_host_strike(int64_t attacker, int64_t target)
{
#ifndef COOP_LITE_TEST_STUBS
    PacketCoopSwing pkt;
    ObjectID oid;
    int slot;

    if (!tig_net_is_active() || !tig_net_is_host() || !tig_net_is_coop_enabled()) {
        return;
    }
    if (!obj_type_is_critter(obj_field_int32_get(attacker, OBJ_F_TYPE))) {
        return;
    }
    slot = coop_lite_slot_of_obj(attacker);
    /* PT25 item 2: the host's AG_ATTACK keeps swinging for a client PC
       until the target dies (further intents are "refused" meanwhile),
       but only the first swing was ever drawn on the owner's screen. */
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_SWING;
    oid = obj_get_id(attacker);
    memcpy(pkt.attacker_oid, &oid, sizeof(oid));
    oid = obj_get_id(target);
    memcpy(pkt.target_oid, &oid, sizeof(oid));
    if (slot >= 1) {
        pkt.hit = COOP_SWING_OWN;
        tig_net_send_app(&pkt, sizeof(pkt), slot);
        coop_diag.swing_tx++;
    }
    /* PT75 root cause of "1/5 of the swings": every other client only saw
       an NPC's / other PC's strike through the 100 ms pose samples, which
       caught a frame of the short ATTACK anim now and then. */
    pkt.hit = COOP_SWING_OTHER;
    coop_lite_send_near(&pkt, obj_field_int64_get(attacker, OBJ_F_LOCATION), slot);
#else
    (void)attacker;
    (void)target;
#endif
}

void coop_lite_host_combat_text(int64_t obj, int tf_type, int text_num)
{
#ifndef COOP_LITE_TEST_STUBS
    PacketCoopSwing pkt;
    ObjectID oid;

    if (obj == OBJ_HANDLE_NULL || !coop_lite_combat_text_allowed(text_num)
        || !tig_net_is_active() || !tig_net_is_host() || !tig_net_is_coop_enabled()) {
        return;
    }
    /* PT75: tf_add only runs where combat resolves (the host). */
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_SWING;
    pkt.hit = COOP_SWING_TEXT;
    oid = obj_get_id(obj);
    memcpy(pkt.target_oid, &oid, sizeof(oid));
    pkt.text_num = text_num;
    pkt.tf_type = tf_type;
    coop_lite_send_near(&pkt, obj_field_int64_get(obj, OBJ_F_LOCATION), -1);
#else
    (void)obj;
    (void)tf_type;
    (void)text_num;
#endif
}


void coop_lite_host_npc_sound(int sound_id, int64_t npc)
{
#ifndef COOP_LITE_TEST_STUBS
    PacketPlaySound pkt;
    int slot;
    int64_t loc;

    if (sound_id == -1 || npc == OBJ_HANDLE_NULL || !tig_net_is_active() || !tig_net_is_host()
        || !tig_net_is_coop_enabled() || !obj_type_is_critter(obj_field_int32_get(npc, OBJ_F_TYPE))) {
        return;
    }
    /* PT28 item 3: PC attackers too (weapon swing / hit / miss sounds) -
       clients never resolve a hit, so they never played them. */
    /* PT25 item 1: NPC anim-goal sounds only ever played on the host, so
       a wolf charging a client PC far from the host was silent there. */
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = 106;
    pkt.subtype = 1;
    pkt.sound_id = sound_id;
    pkt.loops = 1;
    pkt.oid = obj_get_id(npc);
    loc = obj_field_int64_get(npc, OBJ_F_LOCATION);
    for (slot = 1; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);

        if (pc != OBJ_HANDLE_NULL && location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION), loc) <= 25) {
            tig_net_send_app(&pkt, sizeof(pkt), slot);
            coop_diag.npc_sound_tx++;
        }
    }
#else
    (void)sound_id;
    (void)npc;
#endif
}

bool coop_lite_host_chase_to_owner(int64_t attacker, int64_t target)
{
#ifndef COOP_LITE_TEST_STUBS
    PacketCoopSwing pkt;
    ObjectID oid;
    int slot;
    int range;

    if (!coop_lite_is_remote_slot_pc(attacker) || target == OBJ_HANDLE_NULL) {
        return false;
    }
    range = item_weapon_range(item_wield_get(attacker, ITEM_INV_LOC_WEAPON), attacker);
    if (range < 1) {
        range = 1;
    }
    if (object_dist(attacker, target) <= range) {
        return false;
    }
    slot = coop_lite_slot_of_obj(attacker);
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_SWING;
    pkt.hit = 2;
    oid = obj_get_id(attacker);
    memcpy(pkt.attacker_oid, &oid, sizeof(oid));
    oid = obj_get_id(target);
    memcpy(pkt.target_oid, &oid, sizeof(oid));
    tig_net_send_app(&pkt, sizeof(pkt), slot);
    return true;
#else
    (void)attacker;
    (void)target;
    return false;
#endif
}

void coop_lite_handle_swing(PacketCoopSwing* pkt)
{
#ifndef COOP_LITE_TEST_STUBS
    ObjectID oid;
    int64_t attacker;
    int64_t target;

    if (pkt == NULL || pkt->type != PACKET_COOP_SWING || !tig_net_is_active() || tig_net_is_host()) {
        return;
    }
    if (!coop_lite_client_world_ready()) {
        return;
    }
    memcpy(&oid, pkt->attacker_oid, sizeof(oid));
    attacker = obj_pool_perm_lookup(oid);
    memcpy(&oid, pkt->target_oid, sizeof(oid));
    target = obj_pool_perm_lookup(oid);
    if (pkt->hit == COOP_SWING_TEXT) {
        const char* str;

        if (target == OBJ_HANDLE_NULL || !coop_lite_combat_text_allowed(pkt->text_num)
            || pkt->tf_type < 0 || pkt->tf_type >= TF_TYPE_COUNT) {
            return;
        }
        str = combat_mes_text(pkt->text_num);
        if (str != NULL) {
            tf_add(target, pkt->tf_type, str);
        }
        return;
    }
    if (pkt->hit == COOP_SWING_OTHER) {
        if (attacker == OBJ_HANDLE_NULL || target == OBJ_HANDLE_NULL
            || attacker == player_get_local_pc_obj() || critter_is_dead(attacker)) {
            return;
        }
        coop_diag.swing_rx++;
        coop_lite_client_play_other_swing(attacker, target);
        return;
    }
    if (attacker == OBJ_HANDLE_NULL || attacker != player_get_local_pc_obj()
        || target == OBJ_HANDLE_NULL || critter_is_dead(attacker)) {
        return;
    }
    if (pkt->hit == COOP_SWING_APPROACH) {
        /* Our target stepped away: walk after it and attack again (only
           while we are still fighting). */
        if (!critter_is_dead(target) && combat_critter_is_combat_mode_active(attacker)
            && object_dist(attacker, target) <= 15) { /* PT26: not after a map trip */
            tig_debug_printf("CoopLite: attack: target moved away - re-approaching\n");
            if (!coop_lite_client_attack_approach(attacker, target)) {
                coop_lite_client_send_attack_intent(attacker, target);
            }
        }
        return;
    }
    coop_diag.swing_rx++;
    /* The click already drew the first swing; its strike echo lands
       mid-animation - do not double it. PT27: only that one echo is
       skipped; every later strike plays, however fast the attacker
       swings (the old 700 ms window after ANY swing dropped every other
       strike of a fast attacker). */
    if (coop_client_swing_click && coop_client_swing_ms != 0
        && !coop_lite_interval_elapsed(coop_client_swing_ms, coop_obj_now_ms, 700u)) {
        coop_client_swing_click = false;
        return;
    }
    /* PT28 item 2: the first host echo must not cut our own local swing
       that is still playing (frame reset would restart it mid-way). */
    if (coop_client_swing_click
        && tig_art_id_anim_get(obj_field_int32_get(attacker, OBJ_F_CURRENT_AID)) == TIG_ART_ANIM_ATTACK
        && anim_is_current_goal_type(attacker, AG_ANIMATE, NULL)) {
        coop_client_swing_click = false;
        return;
    }
    coop_client_swing_click = false;
    coop_lite_client_play_swing(attacker, target);
#else
    (void)pkt;
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
    case PACKET_COOP_NPC_REQ: /* PT26 unknown NPC request */
    case PACKET_COOP_DOOR_USE: /* PT37 client door use → host */
    case PACKET_COOP_SKILL_USE: /* PT40 pick lock / disarm intent */
    case PACKET_COOP_BODY_DRAG: /* PT77h corpse drag intent */
    case PACKET_COOP_THROW_INTENT: /* PT77z client throw -> host */
    case PACKET_COOP_CRAFT: /* PT40 schematic combine intent */
    case PACKET_COOP_INVEN_LAYOUT: /* PT53 own-pack layout intent */
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
    case 77: /* PT28 item 7: host spell eye candy → client */
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
    case PACKET_COOP_SWING: /* PT25 host strike echo → owner */
    case PACKET_COOP_SESSION_SYNC: /* host map+PC snapshot → client */
    case PACKET_COOP_INVEN_SYNC: /* host inventory snapshot → client */
    case PACKET_COOP_ROSTER: /* other clients' PCs → client */
    case PACKET_COOP_OBJ_BLOB: /* item blob → client (P1-2) */
    case PACKET_COOP_WORLD_STATE: /* §7 world state → client */
    case PACKET_COOP_STORY_SYNC: /* PT35 quests / rumors / reputation → client */
    case PACKET_COOP_DOOR_STATE: /* PT53e host door state → clients */
    case PACKET_COOP_TRAP_FX: /* PT61 host trap visuals → clients */
    case PACKET_COOP_WMAP_POS: /* PT73 host world-map marker → clients */
    case PACKET_COOP_CRITTER_EFFECTS: /* PT77z host effects + light → clients */
    case PACKET_COOP_PROJECTILE_FX: /* PT77z host missile visual → clients */
    case PACKET_COOP_NPC_STATS: /* PT79 host follower build → clients */
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
    case PACKET_COOP_PC_STATS: /* PT19 PC build: owner→host, host→all */
    case PACKET_COOP_DIALOG_STATE: /* PT36: host view → clients, client vote → host (kind-checked) */
    case PACKET_COOP_PARTY_CTRL: /* PT50: travel check / spell end (op-checked) */
    case PACKET_COOP_TRAVEL_REQ: /* PT57: client ask -> host, refusal -> client (kind-checked) */
    case PACKET_COOP_SAVE_REQ: /* PT61: client save/load ask -> host, result -> client (kind-checked) */
    case PACKET_COOP_HOTBAR: /* PT63: client layout -> host, kept layout -> client */
    case PACKET_COOP_CHAT: /* PT77: client text -> host, host relay -> all */
    case PACKET_COOP_AREA_KNOWN: /* PT78: client set -> host, union -> all */
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
        const char* env = coop_lite_env("ARCANUM_DEV_OFFER_STAT");
        int stat;
        int value;
        int n;

        /* "0:15" or a list "0:15,1:12,b3:2" (bN = basic skill N points). */
        while (env != NULL && *env != '\0') {
            if (*env == 'b' && sscanf(env + 1, "%d:%d%n", &stat, &value, &n) == 2
                && stat >= 0 && stat < BASIC_SKILL_COUNT) {
                tig_net_local_suspend_begin();
                basic_skill_points_set(pc, stat, value);
                tig_net_local_suspend_end();
                tig_debug_printf("CoopLite: dev: offer skill %d = %d (now %d)\n", stat, value,
                    basic_skill_points_get(pc, stat));
                env += n + 1;
            } else if (sscanf(env, "%d:%d%n", &stat, &value, &n) == 2
                && stat >= STAT_STRENGTH && stat <= STAT_CHARISMA) {
                tig_net_local_suspend_begin();
                stat_base_set(pc, stat, value);
                tig_net_local_suspend_end();
                tig_debug_printf("CoopLite: dev: offer stat %d = %d (base now %d, effective %d)\n", stat, value,
                    stat_base_get(pc, stat), stat_level_get(pc, stat));
                env += n;
            } else {
                break;
            }
            if (*env != ',') {
                break;
            }
            env++;
        }
    }
    if (coop_lite_env("ARCANUM_COOP_NO_CHAR_BUNDLE") != NULL && coop_lite_env("ARCANUM_COOP_NO_CHAR_BUNDLE")[0] == '1') {
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
    if (chunk == NULL) { tig_debug_printf("CoopLite: PT52e alloc failed\n"); coop_char_bundle_free(&buf); return; }
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
    tig_debug_printf("CoopLite: v2 offer: sent character bundle (%d bytes, ST %d base / %d effective, build=%08x)\n",
        (int)buf.size, stat_base_get(pc, STAT_STRENGTH), stat_level_get(pc, STAT_STRENGTH),
        coop_lite_pc_build_hash(pc));
    coop_char_bundle_free(&buf);
}
#endif

bool coop_lite_offer_flag_has_oid(int has_oid_field)
{
    return (has_oid_field & COOP_OFFER_F_HAS_OID) != 0;
}

bool coop_lite_offer_flag_rejoin_only(int has_oid_field)
{
    return (has_oid_field & COOP_OFFER_F_REJOIN_ONLY) != 0;
}

bool coop_lite_client_is_in_session(void)
{
    return coop_lite_client_in_session || coop_lite_waiting_sync;
}

bool coop_lite_client_take_rejoin_no_pc(void)
{
    bool v = coop_lite_rejoin_no_pc;

    coop_lite_rejoin_no_pc = false;
    return v;
}

static bool coop_lite_client_send_offer(int basic_prototype, const char* name, bool rejoin_only);

bool coop_lite_client_offer_character(int basic_prototype, const char* name)
{
    return coop_lite_client_send_offer(basic_prototype, name, false);
}

bool coop_lite_client_offer_rejoin(int basic_prototype, const char* name)
{
    return coop_lite_client_send_offer(basic_prototype, name, true);
}

static bool coop_lite_client_send_offer(int basic_prototype, const char* name, bool rejoin_only)
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
            pkt.has_oid = COOP_OFFER_F_HAS_OID;
        }
        coop_lite_load_or_create_token(pkt.token);
        if (pc != OBJ_HANDLE_NULL && !rejoin_only) {
            /* PT69: a rejoin placeholder is never offered as a character. */
            coop_lite_client_send_char_bundle(pc);
        }
    }
#endif
    if (rejoin_only) {
        pkt.has_oid |= COOP_OFFER_F_REJOIN_ONLY;
    }
    coop_lite_rejoin_no_pc = false;
    tig_net_send_app_all(&pkt, sizeof(pkt));
    coop_lite_waiting_sync = true;
    tig_debug_printf("CoopLite: CHAR_OFFER sent slot=%d proto=%d name=%s has_oid=%d%s\n",
        pkt.slot, pkt.basic_prototype, pkt.name, pkt.has_oid, rejoin_only ? " (rejoin only)" : "");
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
        || memcmp(&prev->vis_flags, &cur->vis_flags,
               sizeof(*cur) - offsetof(PacketCoopPcPos, vis_flags)) != 0 /* PT28 */
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
    /* PT32: the newest sample is old (the PC stood still; keyframe resends
       do not refresh t_ms): that motion is over, so start afresh instead of
       interpolating / holding across a gap of seconds. The duplicate return
       above only takes an unchanged pose, a resumed step gets here. */
    if (ring->n == 0 || step_tiles > 1 || step_tiles < 0
        || (unsigned int)(sample->t_ms - ring->s[ring->n - 1].t_ms) > COOP_INTERP_DELAY_MS * 2u) {
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

/* ---- PT35: story sync, pure part (blob codec, merge, chunking) ------------ */

#define COOP_STORY_MAGIC 0x59525453u /* 'STRY' */
#define COOP_STORY_REC 12 /* every record is 12 bytes, 4 for global quests */
#define COOP_STORY_BOTCHED 0x100
#define COOP_STORY_STATES 7 /* QUEST_STATE_COUNT */

static int coop_story_applying;

bool coop_lite_story_write_blocked(void)
{
    return tig_net_is_active() && !tig_net_is_host() && coop_story_applying == 0;
}

#if defined(MAX_GLOBAL_FLAGS) && defined(MAX_GLOBAL_VARS)
#if MAX_GLOBAL_FLAGS != COOP_STORY_GFLAGS || MAX_GLOBAL_VARS != COOP_STORY_GVARS
#error "COOP_STORY_GFLAGS / GVARS must match script.h"
#endif
#endif

void coop_story_clear(CoopStory* s)
{
    memset(s, 0, sizeof(*s));
    memset(s->gquest, COOP_STORY_GQUEST_DEFAULT, sizeof(s->gquest));
}

static void coop_story_put16(unsigned char** p, unsigned int v)
{
    unsigned short x = (unsigned short)v;

    memcpy(*p, &x, 2);
    *p += 2;
}

static void coop_story_put32(unsigned char** p, unsigned int v)
{
    memcpy(*p, &v, 4);
    *p += 4;
}

static void coop_story_put64(unsigned char** p, uint64_t v)
{
    memcpy(*p, &v, 8);
    *p += 8;
}

static unsigned int coop_story_get16(const unsigned char** p)
{
    unsigned short x;

    memcpy(&x, *p, 2);
    *p += 2;
    return x;
}

static unsigned int coop_story_get32(const unsigned char** p)
{
    unsigned int x;

    memcpy(&x, *p, 4);
    *p += 4;
    return x;
}

static uint64_t coop_story_get64(const unsigned char** p)
{
    uint64_t x;

    memcpy(&x, *p, 8);
    *p += 8;
    return x;
}

static void coop_story_put_i32(unsigned char** p, int v)
{
    memcpy(*p, &v, 4);
    *p += 4;
}

static int coop_story_get_i32(const unsigned char** p)
{
    int x;

    memcpy(&x, *p, 4);
    *p += 4;
    return x;
}

size_t coop_story_encode(const CoopStory* s, unsigned char* out, size_t cap)
{
    unsigned char* p = out;
    unsigned int nq = 0;
    unsigned int ng = 0;
    unsigned int nr = 0;
    unsigned int ngf;
    unsigned int ngv;
    unsigned int n;
    int i;
    int nrep;
    size_t need;

    for (i = 0; i < COOP_STORY_QUESTS; i++) {
        nq += s->quest_state[i] != 0;
    }
    for (i = 0; i < COOP_STORY_GQUESTS; i++) {
        ng += s->gquest[i] != COOP_STORY_GQUEST_DEFAULT;
    }
    for (i = 0; i < COOP_STORY_RUMORS; i++) {
        nr += s->rumor_ts[i] != 0 || s->rumor_quelled[i] != 0;
    }
    nrep = s->n_rep < 0 ? 0 : (s->n_rep > COOP_STORY_REPS ? COOP_STORY_REPS : s->n_rep);
    ngf = 0;
    ngv = 0;
    for (i = 0; i < COOP_STORY_GFLAGS; i++) {
        ngf += s->gflag[i] != 0;
    }
    for (i = 0; i < COOP_STORY_GVARS; i++) {
        ngv += s->gvar[i] != 0;
    }
    need = 4 + 2 + (size_t)nq * COOP_STORY_REC + 2 + (size_t)ng * 4 + 2 + (size_t)nr * COOP_STORY_REC
        + 2 + (size_t)nrep * COOP_STORY_REC
        + 2 + (size_t)ngf * 2 + 2 + (size_t)ngv * 6;
    if (need > cap) {
        return 0;
    }
    coop_story_put32(&p, COOP_STORY_MAGIC);
    coop_story_put16(&p, nq);
    for (i = 0; i < COOP_STORY_QUESTS; i++) {
        if (s->quest_state[i] != 0) {
            coop_story_put16(&p, (unsigned int)i);
            coop_story_put16(&p, s->quest_state[i]);
            coop_story_put64(&p, s->quest_ts[i]);
        }
    }
    coop_story_put16(&p, ng);
    for (i = 0; i < COOP_STORY_GQUESTS; i++) {
        if (s->gquest[i] != COOP_STORY_GQUEST_DEFAULT) {
            coop_story_put16(&p, (unsigned int)i);
            coop_story_put16(&p, s->gquest[i]);
        }
    }
    coop_story_put16(&p, nr);
    for (i = 0; i < COOP_STORY_RUMORS; i++) {
        if (s->rumor_ts[i] != 0 || s->rumor_quelled[i] != 0) {
            coop_story_put16(&p, (unsigned int)i);
            coop_story_put16(&p, s->rumor_quelled[i] != 0 ? 1u : 0u);
            coop_story_put64(&p, s->rumor_ts[i]);
        }
    }
    n = (unsigned int)nrep;
    coop_story_put16(&p, n);
    for (i = 0; i < nrep; i++) {
        coop_story_put32(&p, s->rep[i]);
        coop_story_put64(&p, s->rep_ts[i]);
    }
    coop_story_put16(&p, ngf);
    for (i = 0; i < COOP_STORY_GFLAGS; i++) {
        if (s->gflag[i] != 0) {
            coop_story_put16(&p, (unsigned int)i);
        }
    }
    coop_story_put16(&p, ngv);
    for (i = 0; i < COOP_STORY_GVARS; i++) {
        if (s->gvar[i] != 0) {
            coop_story_put16(&p, (unsigned int)i);
            coop_story_put_i32(&p, s->gvar[i]);
        }
    }
    return (size_t)(p - out);
}

bool coop_story_decode(const unsigned char* blob, size_t size, CoopStory* s)
{
    const unsigned char* p = blob;
    const unsigned char* end;
    unsigned int n;
    unsigned int i;

    coop_story_clear(s);
    if (blob == NULL || size < 4 + 2 * 4) {
        return false;
    }
    end = blob + size; /* PT52j: no arithmetic on NULL */
    if (coop_story_get32(&p) != COOP_STORY_MAGIC) {
        return false;
    }
    n = coop_story_get16(&p);
    if (n > COOP_STORY_QUESTS || (size_t)(end - p) < (size_t)n * COOP_STORY_REC) {
        return false;
    }
    for (i = 0; i < n; i++) {
        unsigned int idx = coop_story_get16(&p);
        unsigned int st = coop_story_get16(&p);
        uint64_t ts = coop_story_get64(&p);

        if (idx >= COOP_STORY_QUESTS || st == 0 || (st & ~(unsigned int)COOP_STORY_BOTCHED) >= COOP_STORY_STATES) {
            return false;
        }
        s->quest_state[idx] = (unsigned short)st;
        s->quest_ts[idx] = ts;
    }
    if (end - p < 2) {
        return false;
    }
    n = coop_story_get16(&p);
    if (n > COOP_STORY_GQUESTS || (size_t)(end - p) < (size_t)n * 4) {
        return false;
    }
    for (i = 0; i < n; i++) {
        unsigned int idx = coop_story_get16(&p);
        unsigned int st = coop_story_get16(&p);

        if (idx >= COOP_STORY_GQUESTS || st >= COOP_STORY_STATES) {
            return false;
        }
        s->gquest[idx] = (unsigned char)st;
    }
    if (end - p < 2) {
        return false;
    }
    n = coop_story_get16(&p);
    if (n > COOP_STORY_RUMORS || (size_t)(end - p) < (size_t)n * COOP_STORY_REC) {
        return false;
    }
    for (i = 0; i < n; i++) {
        unsigned int idx = coop_story_get16(&p);
        unsigned int fl = coop_story_get16(&p);
        uint64_t ts = coop_story_get64(&p);

        if (idx >= COOP_STORY_RUMORS || fl > 1) {
            return false;
        }
        s->rumor_ts[idx] = ts;
        s->rumor_quelled[idx] = (unsigned char)fl;
    }
    if (end - p < 2) {
        return false;
    }
    n = coop_story_get16(&p);
    if (n > COOP_STORY_REPS || (size_t)(end - p) < (size_t)n * COOP_STORY_REC) {
        return false;
    }
    for (i = 0; i < n; i++) {
        s->rep[i] = coop_story_get32(&p);
        s->rep_ts[i] = coop_story_get64(&p);
    }
    s->n_rep = (int)n;
    /* PT77y: pre-47 blobs end here; new blobs append sparse gf / gv. */
    if (p == end) {
        return true;
    }
    if (end - p < 2) {
        return false;
    }
    n = coop_story_get16(&p);
    if (n > COOP_STORY_GFLAGS || (size_t)(end - p) < (size_t)n * 2) {
        return false;
    }
    for (i = 0; i < n; i++) {
        unsigned int idx = coop_story_get16(&p);

        if (idx >= COOP_STORY_GFLAGS) {
            return false;
        }
        s->gflag[idx] = 1;
    }
    if (end - p < 2) {
        return false;
    }
    n = coop_story_get16(&p);
    if (n > COOP_STORY_GVARS || (size_t)(end - p) < (size_t)n * 6) {
        return false;
    }
    for (i = 0; i < n; i++) {
        unsigned int idx = coop_story_get16(&p);
        int val = coop_story_get_i32(&p);

        if (idx >= COOP_STORY_GVARS) {
            return false;
        }
        s->gvar[idx] = val;
    }
    return p == end;
}

unsigned int coop_story_blob_sig(const unsigned char* blob, size_t size)
{
    unsigned int h = 2166136261u;
    size_t i;

    for (i = 0; i < size; i++) {
        h = (h ^ blob[i]) * 16777619u;
    }
    return h;
}

void coop_story_merge(CoopStory* dst, const CoopStory* src)
{
    int i;
    int k;

    for (i = 0; i < COOP_STORY_QUESTS; i++) {
        unsigned int a = dst->quest_state[i];
        unsigned int b = src->quest_state[i];
        unsigned int ab = a & ~(unsigned int)COOP_STORY_BOTCHED;
        unsigned int bb = b & ~(unsigned int)COOP_STORY_BOTCHED;

        if (b == 0) {
            continue;
        }
        if (a == 0 || bb > ab || (bb == ab && (b & COOP_STORY_BOTCHED) != 0 && (a & COOP_STORY_BOTCHED) == 0)) {
            dst->quest_state[i] = (unsigned short)b;
            dst->quest_ts[i] = src->quest_ts[i];
        }
    }
    for (i = 0; i < COOP_STORY_RUMORS; i++) {
        if (src->rumor_ts[i] != 0 && (dst->rumor_ts[i] == 0 || src->rumor_ts[i] < dst->rumor_ts[i])) {
            dst->rumor_ts[i] = src->rumor_ts[i];
        }
    }
    for (i = 0; i < src->n_rep && i < COOP_STORY_REPS; i++) {
        for (k = 0; k < dst->n_rep; k++) {
            if (dst->rep[k] == src->rep[i]) {
                break;
            }
        }
        if (k == dst->n_rep && dst->n_rep < COOP_STORY_REPS) {
            dst->rep[dst->n_rep] = src->rep[i];
            dst->rep_ts[dst->n_rep] = src->rep_ts[i];
            dst->n_rep++;
        }
    }
}

int coop_story_chunk_count(size_t size)
{
    size_t n;

    if (size == 0 || size > COOP_STORY_MAX_BLOB) {
        return 0;
    }
    n = (size + COOP_STORY_CHUNK - 1) / COOP_STORY_CHUNK;
    return (int)n;
}

void coop_story_chunk_fill(PacketCoopStorySync* pkt, unsigned int snap, const unsigned char* blob,
    size_t size, int index)
{
    int count = coop_story_chunk_count(size);
    size_t off = (size_t)index * COOP_STORY_CHUNK;
    size_t len = 0;

    memset(pkt, 0, sizeof(*pkt));
    pkt->type = PACKET_COOP_STORY_SYNC;
    pkt->snap = snap;
    pkt->total = (unsigned int)size;
    pkt->sig = coop_story_blob_sig(blob, size);
    pkt->count = (unsigned short)count;
    pkt->index = (unsigned short)index;
    if (index >= 0 && index < count) {
        len = size - off < COOP_STORY_CHUNK ? size - off : COOP_STORY_CHUNK;
        memcpy(pkt->data, blob + off, len);
    }
    pkt->len = (unsigned short)len;
}

bool coop_story_chunk_valid(const PacketCoopStorySync* pkt, int size)
{
    int count;
    unsigned int want;

    if (pkt == NULL || size < (int)offsetof(PacketCoopStorySync, data) || pkt->type != PACKET_COOP_STORY_SYNC) {
        return false;
    }
    count = coop_story_chunk_count(pkt->total);
    if (count <= 0 || pkt->count != count || pkt->index >= count) {
        return false;
    }
    want = pkt->index + 1 < count ? COOP_STORY_CHUNK : pkt->total - (unsigned int)(count - 1) * COOP_STORY_CHUNK;
    return pkt->len == want && (size_t)size >= offsetof(PacketCoopStorySync, data) + want;
}

void coop_story_rx_reset(CoopStoryRx* rx)
{
    memset(rx, 0, offsetof(CoopStoryRx, blob));
}

int coop_story_rx_add(CoopStoryRx* rx, const PacketCoopStorySync* pkt, int size)
{
    unsigned int full;

    if (!coop_story_chunk_valid(pkt, size)) {
        return -1;
    }
    if (rx->count == 0 || rx->snap != pkt->snap) {
        coop_story_rx_reset(rx);
        rx->snap = pkt->snap;
        rx->total = pkt->total;
        rx->sig = pkt->sig;
        rx->count = pkt->count;
    } else if (rx->total != pkt->total || rx->count != pkt->count || rx->sig != pkt->sig) {
        return -1;
    }
    if (rx->complete) {
        return 0; /* a repeat of what we already hold */
    }
    memcpy(rx->blob + (size_t)pkt->index * COOP_STORY_CHUNK, pkt->data, pkt->len);
    rx->mask |= 1u << pkt->index;
    full = rx->count >= 32 ? 0xFFFFFFFFu : (1u << rx->count) - 1u;
    if (rx->mask != full) {
        return 0;
    }
    if (coop_story_blob_sig(rx->blob, rx->total) != rx->sig) {
        coop_story_rx_reset(rx);
        return -1;
    }
    rx->complete = true;
    return 1;
}

/* ---- PT36: dialog viewer + voting, pure part (codec, validation, votes) ---- */

static void coop_dialog_copy(char* dst, size_t cap, const char* src)
{
    size_t n = 0;

    if (src != NULL) {
        n = strlen(src);
        if (n > cap - 1) {
            n = cap - 1;
        }
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

void coop_dialog_fill_state(PacketCoopDialogState* pkt, unsigned int seq, unsigned int line, const char* npc_name,
    const char* reply, int num_options, const char* const* options, const unsigned char* votes)
{
    int i;

    memset(pkt, 0, sizeof(*pkt));
    pkt->type = PACKET_COOP_DIALOG_STATE;
    pkt->kind = COOP_DIALOG_KIND_STATE;
    pkt->action = COOP_DIALOG_ACT_OPEN;
    if (num_options < 0) {
        num_options = 0;
    }
    if (num_options > COOP_DIALOG_MAX_OPTIONS) {
        num_options = COOP_DIALOG_MAX_OPTIONS;
    }
    pkt->num_options = (unsigned char)num_options;
    pkt->seq = seq;
    pkt->line = line;
    coop_dialog_copy(pkt->npc_name, sizeof(pkt->npc_name), npc_name);
    coop_dialog_copy(pkt->reply, sizeof(pkt->reply), reply);
    for (i = 0; i < num_options; i++) {
        coop_dialog_copy(pkt->options[i], sizeof(pkt->options[i]), options != NULL ? options[i] : NULL);
        if (votes != NULL) {
            pkt->votes[i] = votes[i] > COOP_DIALOG_MAX_VOTES ? COOP_DIALOG_MAX_VOTES : votes[i];
        }
    }
}

void coop_dialog_fill_close(PacketCoopDialogState* pkt, unsigned int seq, unsigned int line)
{
    memset(pkt, 0, COOP_DIALOG_HDR_SIZE);
    pkt->type = PACKET_COOP_DIALOG_STATE;
    pkt->kind = COOP_DIALOG_KIND_STATE;
    pkt->action = COOP_DIALOG_ACT_CLOSE;
    pkt->seq = seq;
    pkt->line = line;
}

void coop_dialog_fill_vote(PacketCoopDialogState* pkt, unsigned int line, int option)
{
    memset(pkt, 0, COOP_DIALOG_HDR_SIZE);
    pkt->type = PACKET_COOP_DIALOG_STATE;
    pkt->kind = COOP_DIALOG_KIND_VOTE;
    pkt->vote_index = (signed char)option;
    pkt->line = line;
}

void coop_dialog_fill_start(PacketCoopDialogState* pkt, const unsigned char* oid24)
{
    memset(pkt, 0, COOP_DIALOG_HDR_SIZE + 24);
    pkt->type = PACKET_COOP_DIALOG_STATE;
    pkt->kind = COOP_DIALOG_KIND_START;
    memcpy(pkt->npc_name, oid24, 24);
}

void coop_dialog_fill_choose(PacketCoopDialogState* pkt, unsigned int line, int option)
{
    coop_dialog_fill_vote(pkt, line, option);
    pkt->kind = COOP_DIALOG_KIND_CHOOSE;
}

int coop_lite_party_talk_check(int slot, int speaker_dist, int host_dist, bool busy)
{
    if (slot < 1 || slot >= 8) {
        return 1;
    }
    if (speaker_dist < 0 || speaker_dist > COOP_PARTY_TALK_MAX_DIST) {
        return 2;
    }
    if (host_dist < 0 || host_dist > COOP_PARTY_TALK_MAX_HOST_DIST) {
        return 3;
    }
    return busy ? 4 : 0;
}

bool coop_lite_party_choose_ok(int speaker, int slot, unsigned int line, unsigned int cur_line, int option,
    int num_options)
{
    return speaker >= 1 && speaker < 8 && slot == speaker && line == cur_line && option >= -1
        && option < num_options && option < COOP_DIALOG_MAX_OPTIONS;
}

int coop_dialog_wire_size(const PacketCoopDialogState* pkt)
{
    if (pkt->kind == COOP_DIALOG_KIND_START) {
        return COOP_DIALOG_HDR_SIZE + 24;
    }
    if (pkt->kind == COOP_DIALOG_KIND_STATE && pkt->action == COOP_DIALOG_ACT_OPEN) {
        return (int)sizeof(*pkt);
    }
    return COOP_DIALOG_HDR_SIZE;
}

static bool coop_dialog_str_ok(const char* s, size_t cap)
{
    return memchr(s, '\0', cap) != NULL;
}

bool coop_dialog_packet_valid(const PacketCoopDialogState* pkt, int size, bool from_host)
{
    int i;

    if (pkt == NULL || size < COOP_DIALOG_HDR_SIZE || size > (int)sizeof(*pkt)
        || pkt->type != PACKET_COOP_DIALOG_STATE) {
        return false;
    }
    if (!from_host) {
        /* Client -> host: VOTE / CHOOSE header only, an option index or -1;
           START header + 24-byte OID, everything else zero. */
        if (pkt->kind == COOP_DIALOG_KIND_START) {
            return size == COOP_DIALOG_HDR_SIZE + 24 && pkt->action == 0 && pkt->num_options == 0
                && pkt->seq == 0 && pkt->line == 0 && pkt->vote_index == 0 && pkt->votes[0] == 0
                && pkt->votes[1] == 0 && pkt->votes[2] == 0 && pkt->votes[3] == 0 && pkt->votes[4] == 0
                && pkt->votes[5] == 0 && pkt->votes[6] == 0 && pkt->votes[7] == 0;
        }
        return (pkt->kind == COOP_DIALOG_KIND_VOTE || pkt->kind == COOP_DIALOG_KIND_CHOOSE)
            && size == COOP_DIALOG_HDR_SIZE && pkt->action == 0
            && pkt->num_options == 0 && pkt->seq == 0 && pkt->vote_index >= -1
            && pkt->vote_index < COOP_DIALOG_MAX_OPTIONS && pkt->votes[0] == 0 && pkt->votes[1] == 0
            && pkt->votes[2] == 0 && pkt->votes[3] == 0 && pkt->votes[4] == 0 && pkt->votes[5] == 0
            && pkt->votes[6] == 0 && pkt->votes[7] == 0;
    }
    if (pkt->kind != COOP_DIALOG_KIND_STATE || pkt->vote_index != 0 || pkt->seq == 0) {
        return false;
    }
    if (pkt->action == COOP_DIALOG_ACT_CLOSE) {
        return size == COOP_DIALOG_HDR_SIZE && pkt->num_options == 0;
    }
    if (pkt->action != COOP_DIALOG_ACT_OPEN || size != (int)sizeof(*pkt)
        || pkt->num_options > COOP_DIALOG_MAX_OPTIONS) {
        return false;
    }
    if (!coop_dialog_str_ok(pkt->npc_name, sizeof(pkt->npc_name))
        || !coop_dialog_str_ok(pkt->reply, sizeof(pkt->reply))) {
        return false;
    }
    for (i = 0; i < COOP_DIALOG_MAX_OPTIONS; i++) {
        if (!coop_dialog_str_ok(pkt->options[i], sizeof(pkt->options[i]))) {
            return false;
        }
    }
    for (i = 0; i < 8; i++) {
        if (i == COOP_DIALOG_SPEAKER_AT) {
            if (pkt->votes[i] > 7) {
                return false;
            }
            continue;
        }
        if (pkt->votes[i] > COOP_DIALOG_MAX_VOTES || (i >= pkt->num_options && pkt->votes[i] != 0)) {
            return false;
        }
    }
    return true;
}

void coop_dialog_sanitize(char* s)
{
    for (; *s != '\0'; s++) {
        if ((unsigned char)*s < 0x20 || *s == 0x7F) {
            *s = ' ';
        }
    }
}

void coop_dialog_view_from_packet(CoopDialogView* view, const PacketCoopDialogState* pkt)
{
    int i;

    memset(view, 0, sizeof(*view));
    view->my_vote = -1;
    view->seq = pkt->seq;
    view->line = pkt->line;
    if (pkt->action != COOP_DIALOG_ACT_OPEN) {
        return;
    }
    view->open = true;
    view->num_options = pkt->num_options;
    memcpy(view->votes, pkt->votes, sizeof(view->votes));
    memcpy(view->npc_name, pkt->npc_name, sizeof(view->npc_name));
    memcpy(view->reply, pkt->reply, sizeof(view->reply));
    memcpy(view->options, pkt->options, sizeof(view->options));
    view->npc_name[sizeof(view->npc_name) - 1] = '\0';
    view->reply[sizeof(view->reply) - 1] = '\0';
    coop_dialog_sanitize(view->npc_name);
    coop_dialog_sanitize(view->reply);
    for (i = 0; i < COOP_DIALOG_MAX_OPTIONS; i++) {
        view->options[i][sizeof(view->options[i]) - 1] = '\0';
        coop_dialog_sanitize(view->options[i]);
    }
}

void coop_dialog_votes_reset(CoopDialogVotes* v)
{
    memset(v, 0, sizeof(*v));
    memset(v->vote, -1, sizeof(v->vote));
}

void coop_dialog_votes_new_line(CoopDialogVotes* v, unsigned int line, int num_options)
{
    memset(v->vote, -1, sizeof(v->vote));
    v->open = true;
    v->line = line;
    v->num_options = num_options < 0 ? 0 : num_options > COOP_DIALOG_MAX_OPTIONS ? COOP_DIALOG_MAX_OPTIONS : num_options;
}

bool coop_dialog_vote_apply(CoopDialogVotes* v, int slot, int option, unsigned int line, unsigned int now_ms)
{
    if (!v->open || slot < 1 || slot >= 8 || line != v->line || option < -1 || option >= v->num_options) {
        return false;
    }
    if (v->seen[slot] && !coop_lite_interval_elapsed(v->last_ms[slot], now_ms, COOP_DIALOG_VOTE_INTERVAL_MS)) {
        return false;
    }
    v->seen[slot] = true;
    v->last_ms[slot] = now_ms;
    if (v->vote[slot] == option) {
        return false;
    }
    v->vote[slot] = (signed char)option;
    return true;
}

bool coop_dialog_vote_clear_slot(CoopDialogVotes* v, int slot)
{
    if (slot < 1 || slot >= 8 || v->vote[slot] == -1) {
        return false;
    }
    v->vote[slot] = -1;
    return true;
}

void coop_dialog_votes_tally(const CoopDialogVotes* v, unsigned char out[COOP_DIALOG_MAX_OPTIONS])
{
    int slot;

    memset(out, 0, COOP_DIALOG_MAX_OPTIONS);
    for (slot = 1; slot < 8; slot++) {
        if (v->vote[slot] >= 0 && v->vote[slot] < COOP_DIALOG_MAX_OPTIONS) {
            out[(int)v->vote[slot]]++;
        }
    }
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
    case PACKET_COOP_SWING:
        return sizeof(PacketCoopSwing);
    case PACKET_COOP_BARTER:
        return sizeof(PacketCoopBarter);
    case PACKET_COOP_ROSTER:
        return sizeof(PacketCoopRoster);
    case PACKET_COOP_INVEN_SYNC:
        return offsetof(PacketCoopInvenSync, entries);
    case PACKET_COOP_OBJ_REQ:
    case PACKET_COOP_NPC_REQ:
        return offsetof(PacketCoopObjReq, oid);
    case PACKET_COOP_OBJ_BLOB:
        return offsetof(PacketCoopObjBlob, data);
    case PACKET_COOP_CHAR_BUNDLE:
        return offsetof(PacketCoopCharChunk, data);
    case PACKET_COOP_WORLD_REQ:
        return sizeof(PacketCoopWorldReq);
    case PACKET_COOP_WORLD_STATE:
        return offsetof(PacketCoopWorldState, entries);
    case PACKET_COOP_STORY_SYNC:
        return offsetof(PacketCoopStorySync, data);
    case PACKET_COOP_PC_STATS:
        return sizeof(PacketCoopPcStats);
    case PACKET_COOP_DOOR_USE:
        return sizeof(PacketCoopDoorUse);
    case PACKET_COOP_DOOR_STATE:
        return sizeof(PacketCoopDoorState);
    case PACKET_COOP_SKILL_USE:
        return sizeof(PacketCoopSkillUse);
    case PACKET_COOP_CRAFT:
        return sizeof(PacketCoopCraft);
    case PACKET_COOP_INVEN_LAYOUT:
        return offsetof(PacketCoopInvenLayout, entries);
    case PACKET_COOP_PARTY_CTRL:
        return sizeof(PacketCoopPartyCtrl);
    case PACKET_COOP_TRAVEL_REQ:
        return sizeof(PacketCoopTravelReq);
    case PACKET_COOP_SAVE_REQ:
        return sizeof(PacketCoopSaveReq);
    case PACKET_COOP_TRAP_FX:
        return sizeof(PacketCoopTrapFx);
    case PACKET_COOP_WMAP_POS:
        return sizeof(PacketCoopWmapPos);
    case PACKET_COOP_CHAT:
        return sizeof(PacketCoopChat);
    case PACKET_COOP_BODY_DRAG:
        return sizeof(PacketCoopBodyDrag);
    case PACKET_COOP_CRITTER_EFFECTS:
        return offsetof(PacketCoopCritterEffects, data);
    case PACKET_COOP_THROW_INTENT:
        return sizeof(PacketCoopThrowIntent);
    case PACKET_COOP_PROJECTILE_FX:
        return sizeof(PacketCoopProjectileFx);
    case PACKET_COOP_AREA_KNOWN:
        return sizeof(PacketCoopAreaKnown);
    case PACKET_COOP_NPC_STATS:
        return sizeof(PacketCoopNpcStats);
    case PACKET_COOP_HOTBAR:
        return sizeof(PacketCoopHotbar);
    case PACKET_COOP_DIALOG_STATE:
        return offsetof(PacketCoopDialogState, npc_name);
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

/* PT27 items 1/2: vanilla Packet10 is the host's "anim ended, pose = stand
   frame 0 at loc/offset" snapshot (plus the host run's flags written into
   whatever local run has that AnimID). In co-op every critter already has
   its own pose channel - our PC is driven here, peers come through the
   PC_POS interpolation ring, NPCs through NPC_POS - so applying it cut our
   swing to a stand frame after each host strike and yanked the host's PC
   out of its interpolated path. true: the caller must drop the packet. */
bool coop_lite_cosmetic_swing_speed_scaled(bool net_active, bool is_host, bool is_critter, bool is_attack_anim)
{
    return net_active && !is_host && is_critter && is_attack_anim;
}

bool coop_lite_client_drop_anim_pose(int64_t obj)
{
#ifdef COOP_LITE_TEST_STUBS
    (void)obj;
    return false;
#else
    int type;

    if (obj == OBJ_HANDLE_NULL || !tig_net_is_active() || tig_net_is_host() || !tig_net_is_coop_enabled()) {
        return false;
    }
    type = obj_field_int32_get(obj, OBJ_F_TYPE);
    if (!obj_type_is_critter(type)) {
        return false;
    }
    if (obj == player_get_local_pc_obj()) {
        coop_diag.p10_own++;
    } else if (type == OBJ_TYPE_PC) {
        coop_diag.p10_peer++;
    } else {
        coop_diag.p10_npc++;
    }
    return true;
#endif
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

static void coop_lite_char_rx_reset_all(void)
{
    int i;

    for (i = 0; i < 8; i++) {
        coop_char_rx_free(&coop_char_rx[i]);
    }
}

static void coop_lite_host_rate_reset(int slot);
static bool coop_lite_host_rate_ok(int kind, int slot, int cost);

void coop_lite_host_on_client_disconnect(int slot)
{
    if (slot <= 0 || slot >= 8) {
        return;
    }
    coop_char_rx_free(&coop_char_rx[slot]);
    coop_lite_pt52_slot_gone(slot); /* PT52c */
    coop_lite_host_rate_reset(slot); /* PT52k */
#ifndef COOP_LITE_TEST_STUBS
    coop_lite_barter_end(slot, "disconnected");
#endif
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
    if (coop_lite_client_leave_on_reset(tig_net_is_active(), tig_net_is_host(),
            coop_lite_client_in_session, coop_lite_waiting_sync)) {
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
    if (coop_lite_client_in_session || coop_lite_waiting_sync || tig_net_is_host()) {
        coop_lite_session_reset();
    }
#endif
}

/* PT77f: a client quitting to the menu leaves the host's session whenever
   it is connected - also half-way through a resync (waiting for the
   host's SESSION_SYNC after a host save load). The old in_session-only
   check kept the connection and waiting_sync alive there, so the menu
   thought the client was still in the session and hid Join Host. */
bool coop_lite_client_leave_on_reset(bool active, bool host, bool in_session, bool waiting_sync)
{
    (void)in_session;
    (void)waiting_sync;
    return active && !host;
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

/* PT25: a co-op PC or a follower led by one ("the party side"). */
static bool coop_lite_party_side(int64_t obj)
{
    int64_t leader;

    if (obj == OBJ_HANDLE_NULL) {
        return false;
    }
    if (coop_lite_slot_of_obj(obj) >= 0) {
        return true;
    }
    if (obj_field_int32_get(obj, OBJ_F_TYPE) != OBJ_TYPE_NPC) {
        return false;
    }
    /* PT77af: followers of followers too (Virgil's summoned elemental). */
    {
        int depth;

        leader = critter_leader_get(obj);
        for (depth = 0; depth < 4 && leader != OBJ_HANDLE_NULL; depth++) {
            if (coop_lite_slot_of_obj(leader) >= 0) {
                return true;
            }
            if (obj_field_int32_get(leader, OBJ_F_TYPE) != OBJ_TYPE_NPC) {
                return false;
            }
            leader = critter_leader_get(leader);
        }
    }
    return false;
}

/* PT27 item 6: vanilla ai_heal() only looks at the healer itself, its
   leader and the leader's followers - so Virgil (the host's follower) never
   healed a client's PC. A follower of any co-op PC may also heal the other
   co-op PCs and their followers within 20 tiles. Host only; nothing about
   who leads whom changes. Returns how many candidates went into out. */
int coop_lite_party_heal_targets(int64_t healer, int64_t* out, int max)
{
    int n = 0;
    int slot;
    int64_t own_leader;

    if (!tig_net_is_active() || !tig_net_is_host() || !tig_net_is_coop_enabled() || healer == OBJ_HANDLE_NULL
        || obj_field_int32_get(healer, OBJ_F_TYPE) != OBJ_TYPE_NPC || !coop_lite_party_side(healer)) {
        return 0;
    }
    own_leader = critter_leader_get(healer);
    for (slot = 0; slot < 8 && n < max; slot++) {
        int64_t pc = sub_4A2B60(slot);
        ObjectList objects;
        ObjectNode* node;

        if (pc == OBJ_HANDLE_NULL || pc == own_leader || object_dist(healer, pc) > 20) {
            continue; /* own leader + its followers: vanilla already tried */
        }
        out[n++] = pc;
        object_list_all_followers(pc, &objects);
        for (node = objects.head; node != NULL && n < max; node = node->next) {
            if (node->obj != healer && object_dist(healer, node->obj) <= 20) {
                out[n++] = node->obj;
            }
        }
        object_list_destroy(&objects);
    }
    {
        static unsigned int logged_ms;

        if (coop_lite_interval_elapsed(logged_ms, coop_obj_now_ms, 10000u)) {
            logged_ms = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1;
            tig_debug_printf("CoopLite: AI heal: %d other co-op party members in reach\n", n);
        }
    }
    return n;
}

bool coop_lite_is_remote_slot_pc(int64_t obj)
{
    return obj != OBJ_HANDLE_NULL && tig_net_is_active() && tig_net_is_host() && tig_net_is_coop_enabled()
        && coop_lite_slot_of_obj(obj) >= 1;
}

bool coop_lite_is_party_pc(int64_t obj)
{
    return obj != OBJ_HANDLE_NULL && tig_net_is_active() && tig_net_is_host() && tig_net_is_coop_enabled()
        && coop_lite_slot_of_obj(obj) >= 0;
}

bool coop_lite_party_friendly(int64_t a, int64_t b)
{
    /* PT25: co-op PCs and their followers never turn on each other (a caught
       pickpocket between players, an accidental hit, a follower's
       retaliation). Only between DIFFERENT players' sides or player vs
       player - a PC hitting its own follower keeps the vanilla reaction. */
    if (!tig_net_is_active() || a == OBJ_HANDLE_NULL || b == OBJ_HANDLE_NULL || a == b) {
        return false;
    }
    if (!coop_lite_party_side(a) || !coop_lite_party_side(b)) {
        return false;
    }
    if (obj_field_int32_get(b, OBJ_F_TYPE) == OBJ_TYPE_NPC && critter_leader_get(b) == a) {
        return false; /* own follower: vanilla "watch it!" / jilt logic */
    }
    return true;
}

/* PT77af: AI target selection: nobody on the co-op side (PCs, their
   followers, followers' summons) ever picks another one as a target - own
   followers included (a follower never turns on any co-op player). */
bool coop_lite_party_no_fight(int64_t a, int64_t b)
{
    if (!tig_net_is_active() || !tig_net_is_coop_enabled() || a == OBJ_HANDLE_NULL || b == OBJ_HANDLE_NULL
        || a == b) {
        return false;
    }
    return coop_lite_party_side(a) && coop_lite_party_side(b);
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
    pkt->vis_flags = (unsigned int)obj_field_int32_get(obj, OBJ_F_FLAGS) & COOP_PC_VIS_FLAGS;
    pkt->critter_flags = (unsigned int)obj_field_int32_get(obj, OBJ_F_CRITTER_FLAGS);
    pkt->critter_flags2 = (unsigned int)obj_field_int32_get(obj, OBJ_F_CRITTER_FLAGS2);
    pkt->spell_flags = (unsigned int)obj_field_int32_get(obj, OBJ_F_SPELL_FLAGS);
    pkt->blit_flags = (unsigned int)obj_field_int32_get(obj, OBJ_F_BLIT_FLAGS);
    pkt->blit_color = (unsigned int)obj_field_int32_get(obj, OBJ_F_BLIT_COLOR);
    pkt->blit_alpha = obj_field_int32_get(obj, OBJ_F_BLIT_ALPHA);
    pkt->blit_scale = obj_field_int32_get(obj, OBJ_F_BLIT_SCALE);
}

/* PT28 item 5: client side - take the host's look of a PC (spell flags,
   blit state); when the base art changes (polymorph ends) drop overlays
   left from the old body. Own PC: its owner-driven critter flags (combat
   mode, sleeping ...) are kept, only the look is taken. */
static unsigned int coop_vis_body[8]; /* host's last art type/num per slot */
static bool coop_vis_body_valid[8];

static void coop_lite_client_apply_pc_visual(int64_t obj, const PacketCoopPcPos* pkt, bool own)
{
    unsigned int cur;
    unsigned int body;
    unsigned int prev_body;
    bool prev_valid;
    bool changed = false;
    int slot;
    int i;

    if (tig_net_is_host() || pkt->type != PACKET_COOP_PC_POS) {
        return;
    }
    cur = (unsigned int)obj_field_int32_get(obj, OBJ_F_FLAGS) & COOP_PC_VIS_FLAGS;
    if (cur != pkt->vis_flags) {
        if ((pkt->vis_flags & ~cur) != 0) {
            object_add_flags(obj, pkt->vis_flags & ~cur);
        }
        if ((cur & ~pkt->vis_flags) != 0) {
            object_remove_flags(obj, cur & ~pkt->vis_flags);
        }
        changed = true;
    }
    if (own) {
        unsigned int ocf = (unsigned int)obj_field_int32_get(obj, OBJ_F_CRITTER_FLAGS);
        unsigned int want = (ocf & ~(unsigned int)COOP_OWN_HOST_OCF) | (pkt->critter_flags & COOP_OWN_HOST_OCF);

        if (ocf != want) {
            obj_field_int32_set(obj, OBJ_F_CRITTER_FLAGS, (int)want);
            tig_debug_printf("CoopLite: own PC paralysis/stun from host: 0x%x -> 0x%x\n",
                ocf & COOP_OWN_HOST_OCF, want & COOP_OWN_HOST_OCF);
        }
    } else {
        if ((unsigned int)obj_field_int32_get(obj, OBJ_F_CRITTER_FLAGS) != pkt->critter_flags) {
            obj_field_int32_set(obj, OBJ_F_CRITTER_FLAGS, (int)pkt->critter_flags);
        }
        if ((unsigned int)obj_field_int32_get(obj, OBJ_F_CRITTER_FLAGS2) != pkt->critter_flags2) {
            obj_field_int32_set(obj, OBJ_F_CRITTER_FLAGS2, (int)pkt->critter_flags2);
        }
    }
    if ((unsigned int)obj_field_int32_get(obj, OBJ_F_SPELL_FLAGS) != pkt->spell_flags) {
        obj_field_int32_set(obj, OBJ_F_SPELL_FLAGS, (int)pkt->spell_flags);
    }
    if ((unsigned int)obj_field_int32_get(obj, OBJ_F_BLIT_FLAGS) != pkt->blit_flags
        || (unsigned int)obj_field_int32_get(obj, OBJ_F_BLIT_COLOR) != pkt->blit_color
        || obj_field_int32_get(obj, OBJ_F_BLIT_ALPHA) != pkt->blit_alpha) {
        obj_field_int32_set(obj, OBJ_F_BLIT_FLAGS, (int)pkt->blit_flags);
        obj_field_int32_set(obj, OBJ_F_BLIT_COLOR, (int)pkt->blit_color);
        obj_field_int32_set(obj, OBJ_F_BLIT_ALPHA, pkt->blit_alpha);
        changed = true;
    }
    if (obj_field_int32_get(obj, OBJ_F_BLIT_SCALE) != pkt->blit_scale) {
        object_set_blit_scale(obj, pkt->blit_scale); /* redraws */
    }
    body = ((unsigned int)tig_art_type((tig_art_id_t)pkt->art_id) << 16)
        | (tig_art_num_get((tig_art_id_t)pkt->art_id) & 0xFFFFu);
    slot = pkt->slot >= 0 && pkt->slot < 8 ? pkt->slot : 0;
    prev_valid = coop_vis_body_valid[slot];
    prev_body = coop_vis_body[slot];
    coop_vis_body[slot] = body;
    coop_vis_body_valid[slot] = true;
    if (prev_valid && prev_body != body) {
        /* Base body changed (polymorph on / off): the old body's overlays
           and palette are stale here. */
        for (i = 0; i < 7; i++) {
            if (obj_arrayfield_uint32_get(obj, OBJ_F_OVERLAY_FORE, i) != TIG_ART_ID_INVALID) {
                object_overlay_set(obj, OBJ_F_OVERLAY_FORE, i, TIG_ART_ID_INVALID);
            }
            if (obj_arrayfield_uint32_get(obj, OBJ_F_OVERLAY_BACK, i) != TIG_ART_ID_INVALID) {
                object_overlay_set(obj, OBJ_F_OVERLAY_BACK, i, TIG_ART_ID_INVALID);
            }
        }
        if (own) {
            /* Own PC: the interpolation never sets its art - take the
               host's body (palette included). */
            object_set_current_aid(obj, (tig_art_id_t)pkt->art_id);
        }
        tig_debug_printf("CoopLite: PC visual: base art changed (polymorph) - overlays cleared\n");
        changed = true;
    }
    if (changed) {
        /* Same scale: only redraws the PC's rect. */
        object_set_blit_scale(obj, obj_field_int32_get(obj, OBJ_F_BLIT_SCALE));
    }
}

/* PT37: a position from the host's NEW map reaching us while we are still on
   the old one ("object_get_rect found object with location outside the
   limits" x60 at every follow travel): not applied. */
bool coop_lite_loc_in_map(int64_t loc)
{
#ifndef COOP_LITE_TEST_STUBS
    int64_t lx;
    int64_t ly;

    location_limits_get(&lx, &ly);
    return LOCATION_GET_X(loc) < lx && LOCATION_GET_Y(loc) < ly;
#else
    (void)loc;
    return true;
#endif
}

#ifndef COOP_LITE_TEST_STUBS
/* PT75: true = keep the relayed swing playing, drop this pose's art. */
static bool coop_lite_pose_cuts_swing_obj(int64_t obj, tig_art_id_t new_art)
{
    int new_anim = tig_art_id_anim_get(new_art);

    return coop_lite_pose_keeps_swing(
        tig_art_id_anim_get(obj_field_int32_get(obj, OBJ_F_CURRENT_AID)) == TIG_ART_ANIM_ATTACK,
        anim_is_current_goal_type(obj, AG_ANIMATE, NULL),
        new_anim == TIG_ART_ANIM_STAND || new_anim == TIG_ART_ANIM_ATTACK);
}
#endif

static void coop_lite_apply_pc_pos(int64_t obj, const PacketCoopPcPos* pkt)
{
    if (!coop_lite_loc_in_map(pkt->loc)) {
        return;
    }
    if (obj_field_int64_get(obj, OBJ_F_LOCATION) != pkt->loc
        || obj_field_int32_get(obj, OBJ_F_OFFSET_X) != pkt->offset_x
        || obj_field_int32_get(obj, OBJ_F_OFFSET_Y) != pkt->offset_y) {
        sub_43E770(obj, pkt->loc, pkt->offset_x, pkt->offset_y);
    }
    /* PT25 item 2: on the host, a client PC's attack runs here (the owner
       only plays a cosmetic swing). Its pose stream carried the owner's
       stand art, which overwrote the host's attack animation mid-swing and
       ended AG_ATTACK after one hit - no repeat, no further swings. */
    if (tig_net_is_host()
        && (anim_is_current_goal_type(obj, AG_ATTACK, NULL)
            || anim_is_current_goal_type(obj, AG_ATTEMPT_ATTACK, NULL))) {
        return;
    }
    if ((unsigned int)obj_field_int32_get(obj, OBJ_F_CURRENT_AID) != pkt->art_id
        && (tig_net_is_host() || !coop_lite_pose_cuts_swing_obj(obj, (tig_art_id_t)pkt->art_id))) {
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
            tig_debug_printf("CoopLite: PC census: %s @%d,%d slot=%d%s dist=%d proto=%d flags=0x%x build=%08x ST %d/%d%s\n",
                oid_str,
                (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc),
                slot,
                obj == local_pc ? " (local)" : "",
                (int)location_dist(local_loc, loc),
                sub_49B290(obj),
                (unsigned int)obj_field_int32_get(obj, OBJ_F_FLAGS),
                coop_lite_pc_build_hash(obj),
                stat_base_get(obj, STAT_STRENGTH), stat_level_get(obj, STAT_STRENGTH),
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

/* Review P1-4: an object blob is read only when all its inner lengths stay
   inside the bytes received (obj_read_mem itself trusts them). */
static bool coop_lite_obj_blob_bounded(const uint8_t* data, int size)
{
    static uint8_t kinds[1024];
    static int elem[1024];
    int count = 0;
    int hdr;
    int body;

    hdr = obj_mem_layout(data, size, kinds, elem, 1024, &count);
    if (hdr < 0) {
        tig_debug_printf("CoopLite: blob refused: bad header (%d bytes)\n", size);
        return false;
    }
    body = coop_lite_blob_walk(data + hdr, size - hdr, kinds, elem, count);
    if (body < 0 || hdr + body != size) {
        tig_debug_printf("CoopLite: blob refused: inner lengths (%d fields, %d+%d of %d bytes)\n",
            count, hdr, body, size);
        return false;
    }
    return true;
}

/* Host: the client's own character from its v2 bundle, at loc. NULL when
   there is none or it does not pass the checks (proto offer then). */
/*
 * Review P0-1: undo a refused bundle. object_destroy of a PC does not
 * destroy its inventory (sub_4639E0 only drops invulnerable items), so the
 * old loop was no double free, but it destroyed the PC before items that
 * still named it as parent, and ran object_destroy on items that were read
 * but never postprocessed (handle fields still ids). Now: placed (PC in a
 * sector, items postprocessed) = items first through the normal path, then
 * the PC; otherwise the objects were only read into the pool and are
 * deallocated as they are.
 */
static void coop_lite_bundle_rollback(int64_t* objs, int n, bool placed)
{
    int i;

    if (n > COOP_CHAR_MAX_ITEMS + 1) {
        n = COOP_CHAR_MAX_ITEMS + 1;
    }
    if (placed || objs[0] != OBJ_HANDLE_NULL) {
        for (i = 1; i < n; i++) {
            if (objs[i] != OBJ_HANDLE_NULL) {
                object_destroy(objs[i]);
                objs[i] = OBJ_HANDLE_NULL;
            }
        }
        if (objs[0] != OBJ_HANDLE_NULL) {
            object_destroy(objs[0]);
            objs[0] = OBJ_HANDLE_NULL;
        }
        return;
    }
    for (i = 1; i < n; i++) {
        if (objs[i] != OBJ_HANDLE_NULL) {
            obj_deallocate(objs[i]);
            objs[i] = OBJ_HANDLE_NULL;
        }
    }
}

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
    bool placed = false;

    if (!rx->complete) {
        return OBJ_HANDLE_NULL;
    }
    memset(&blobs, 0, sizeof(blobs));
    memset(objs, 0, sizeof(objs));
    ok = coop_char_bundle_parse(rx->data, (size_t)rx->total, coop_lite_bundle_collect, &blobs,
             &count, NULL, NULL)
        && blobs.n >= 1 && blobs.n == count;
    for (i = 0; ok && i < blobs.n; i++) {
        ok = coop_lite_bundle_blob_ok(blobs.blob[i], i == 0, i == 0 ? want_oid : NULL)
            && coop_lite_obj_blob_bounded(blobs.blob[i], (int)blobs.size[i]);
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
    placed = ok; /* PC in its sector, items postprocessed (parent = PC) */
    if (ok) {
        for (i = 0; i < 8; i++) {
            stats[i] = stat_base_get(pc, STAT_STRENGTH + i);
        }
        ok = coop_lite_char_plausible(stat_level_get(pc, STAT_LEVEL),
            stat_level_get(pc, STAT_EXPERIENCE_POINTS), stats, 8, item_gold_get(pc),
            obj_field_int32_get(pc, OBJ_F_CRITTER_INVENTORY_NUM));
        if (ok && stat_base_get(pc, STAT_EXPERIENCE_POINTS) < level_xp_required(stat_base_get(pc, STAT_LEVEL))) {
            /* A level needs its XP (xp_level.mes); more XP than the level
               is fine (the engine levels up on the next award). */
            tig_debug_printf("CoopLite: v2 offer: slot %d level %d with only %d XP (needs %d)\n",
                slot, stat_base_get(pc, STAT_LEVEL), stat_base_get(pc, STAT_EXPERIENCE_POINTS),
                level_xp_required(stat_base_get(pc, STAT_LEVEL)));
            ok = false;
        }
        if (ok) {
            /* Review PT24 #3: names are strcpy'd into 128-byte buffers by
               object_examine; chargen allows far less. */
            char* pname = NULL;

            obj_field_string_get(pc, OBJ_F_PC_PLAYER_NAME, &pname);
            if (pname != NULL && strlen(pname) >= 64) {
                tig_debug_printf("CoopLite: v2 offer: slot %d bad player name length\n", slot);
                ok = false;
            }
            if (pname != NULL) {
                FREE(pname);
            }
        }
        if (!ok) {
            tig_debug_printf("CoopLite: v2 offer: slot %d character implausible (level %d, xp %d)\n",
                slot, stat_level_get(pc, STAT_LEVEL), stat_level_get(pc, STAT_EXPERIENCE_POINTS));
        }
    }
    if (!ok) {
        coop_lite_bundle_rollback(objs, blobs.n, placed);
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
    tig_debug_printf("CoopLite: v2 offer: slot %d character from bundle (level %d, ST %d base / %d effective, build=%08x, %d items, gold %d)\n",
        slot, stat_level_get(pc, STAT_LEVEL), stat_base_get(pc, STAT_STRENGTH),
        stat_level_get(pc, STAT_STRENGTH), coop_lite_pc_build_hash(pc),
        obj_field_int32_get(pc, OBJ_F_CRITTER_INVENTORY_NUM), item_gold_get(pc));
    coop_char_rx_free(rx);
    return pc;
}
#endif

static void coop_lite_npc_cache_reset(void);

static bool coop_lite_host_spawn_client(int slot)
{
    /* PT37: the shared "already sent" cache must not hide NPCs from a newcomer. */
    coop_lite_npc_cache_reset();
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
    if (client_obj == OBJ_HANDLE_NULL && coop_pending_offers[slot].rejoin_only) {
        /* PT69: rejoin from the client's main menu, but we hold no PC for
           this player (first join, or it is gone): never adopt the
           placeholder - send it to character creation. */
        memset(&sync, 0, sizeof(sync));
        sync.type = PACKET_COOP_SESSION_SYNC;
        sync.flags = COOP_SYNC_F_NO_PC;
        sync.map = map;
        sync.client_slot = slot;
        tig_net_send_app(&sync, sizeof(sync), slot);
        coop_char_rx_free(&coop_char_rx[slot]);
        coop_pending_offers[slot].valid = false;
        tig_debug_printf("CoopLite: slot %d rejoin-only offer, no PC kept - client goes to character creation\n", slot);
        return false;
    }
    if (client_obj == OBJ_HANDLE_NULL) {
        /* v2: the client's own character (stats, skills, spells, gear). */
        client_obj = coop_lite_host_pc_from_bundle(slot, spawn_loc, &client_oid);
    } else {
        /* Review P1-5: we hold this player's PC (parked, or bound by a
           loaded save / a reconnect): ours wins, their bundle is dropped,
           and the client is told to take our copy even when the OIDs
           match (party-wipe reload: the client still has its pre-death
           PC). */
        if (coop_char_rx[slot].complete || coop_char_rx[slot].data != NULL) {
            tig_debug_printf("CoopLite: slot %d: host copy of the PC wins — client bundle discarded\n", slot);
        }
        coop_char_rx_free(&coop_char_rx[slot]);
        rejoined = true;
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
    coop_layout_seq_applied[slot] = 0; /* PT54: a (re)joined client numbers its layouts from 1 */
    coop_lite_host_vitals_force(); /* PT54: host PC alive/dead + HP to the (re)joined client */
    tig_debug_printf("CoopLite: SESSION_SYNC sent slot=%d map=%d client_proto=%d host_proto=%d (companion spawned on host)\n",
        slot, map, sync.client_proto, sync.host_proto);
    coop_lite_host_announce_roster(slot);
    return true;
}

static void coop_lite_client_request_own_pc(const unsigned char oid[24]);

/* PT77l: after a host save reload the client's PC is bound on a freshly
   loaded map without moving, so nothing told wallcheck where the local PC
   is (map_flush resets it). Inside a building the roof stayed drawn over
   the PC - it looked like the client never came back. Make sure the PC is
   on, centre the camera and recalc roofs/walls at its spot. */
static void coop_lite_client_refresh_own_view(int64_t pc)
{
    if (pc == OBJ_HANDLE_NULL) {
        return;
    }
    if ((obj_field_int32_get(pc, OBJ_F_FLAGS) & OF_OFF) != 0) {
        tig_debug_printf("CoopLite: PT77l own PC was OFF after sync - turned on\n");
        object_flags_unset(pc, OF_OFF);
    }
    location_origin_set(obj_field_int64_get(pc, OBJ_F_LOCATION));
    sub_438530(pc);
    gamelib_invalidate_rect(NULL);
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
        /* PT72: after a host reload the sync teleport could finish before
           our PC was bound (or with the PC object only present under its
           OID). The sync used to be consumed here, leaving the client out of
           the world ("vanished") with every later follow queued forever.
           Adopt our PC by OID when it is here; otherwise keep the sync and
           retry from coop_lite_ping. */
        ObjectID want;

        memcpy(&want, pkt->client_pc_oid, sizeof(want));
        local_pc = obj_pool_perm_lookup(want);
        if (local_pc != OBJ_HANDLE_NULL
            && obj_field_int32_get(local_pc, OBJ_F_TYPE) == OBJ_TYPE_PC) {
            tig_debug_printf("CoopLite: PT72 finish sync: own PC adopted by OID\n");
            sub_40DAF0(local_pc);
        } else if (coop_lite_sync_retry_allowed(++coop_sync_nopc_tries)) {
            tig_debug_printf("CoopLite: PT72 finish sync: no local PC yet - retry %d\n", coop_sync_nopc_tries);
            if (coop_lite_sync_restore_due(coop_sync_nopc_tries)) {
                /* PT77ae (review 2): ask the host for our PC and load it
                   here, in the world (no trip to the menu). */
                coop_lite_client_restore_pc_ask(pkt);
            }
            coop_pending_sync = *pkt;
            coop_pending_sync_valid = true;
            coop_pending_sync_teleport_queued = true; /* no re-teleport */
            coop_sync_nopc_wait = true;
            return;
        } else {
            /* PT77ad (review 2): giving up left the client connected but
               PC-less until the player quit by hand. Take the proven way
               back instead: to the main menu (the reset leaves the session,
               the host parks our PC) and the wait-host loop rejoins, which
               restores the parked PC from the host. No in-world PC rebuild. */
            tig_debug_printf("CoopLite: finish sync: no local PC (gave up) - back to the menu to rejoin\n");
            coop_sync_nopc_tries = 0;
            coop_sync_restore_asked = false; /* PT77ae: last resort */
            coop_rejoin_pc_wanted = false;
            coop_lite_show_notice("Не удалось восстановить персонажа после загрузки хоста. Возврат в меню и автоматический перезаход. / Could not restore your character after the host's load. Returning to the menu to rejoin automatically.", TF_TYPE_RED);
            coop_host_left_menu_armed = true;
            coop_host_left_menu_ms = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1;
            coop_wait_host = coop_lite_wait_host_enabled(coop_lite_env("ARCANUM_COOP_WAIT_HOST"));
            return;
        }
    }
    coop_sync_nopc_tries = 0;
    if (coop_sync_restore_asked) {
        /* PT77ae: the PC turned up another way (adopted by OID): drop the
           blob request, it would block door use / pickups meanwhile. */
        coop_sync_restore_asked = false;
        coop_rejoin_pc_wanted = false;
    }
    local_oid = obj_get_id(local_pc);
    if (!coop_lite_oid_raw_equal(&local_oid, pkt->client_pc_oid)) {
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

    /* Camera on our own PC (like SP New Game); roofs/walls too (PT77l). */
    coop_lite_client_refresh_own_view(local_pc);

    /* Stale/duplicate PCs (e.g. leftovers in the client's Save\Current map
       state or earlier placeholder proxies) — remove anything unbound. */
    coop_lite_pc_census(true);

    /* Objects the host created while we were loading. */
    mp_flush_deferred_packet70();
    tig_debug_printf("CoopLite: SESSION_SYNC applied map=%d (after map load)\n", pkt->map);

    if ((pkt->flags & COOP_SYNC_F_REJOINED) != 0) {
        /* Rejoin: the host kept our PC. Take its copy (stats, level, XP)
           instead of the character picked in the menu; the inventory
           follows once it is in place. Also with the same OID (review
           P1-5: after a host reload our live PC is the pre-death one). */
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

/*
 * PT23: the engine runs NPC AI and heartbeats (ai_timeevent_process ->
 * sub_4AD420) only within 30 tiles of player_get_local_pc_obj(), and scales
 * the AI think delay by that same distance. On the host that is the host PC
 * only, so a client PC away from the host met frozen / 5 s-lazy NPCs and
 * nobody fought it. In a host session every slot PC counts.
 */
int64_t coop_lite_ai_nearest_pc_dist(int64_t obj)
{
    int64_t best = object_dist(player_get_local_pc_obj(), obj);
    int64_t d;
    int64_t pc;
    int slot;

    if (!tig_net_is_active() || !tig_net_is_host()) {
        return best;
    }
    for (slot = 1; slot < 8; slot++) {
        pc = sub_4A2B60(slot);
        if (pc == OBJ_HANDLE_NULL || pc == obj) {
            continue;
        }
        d = object_dist(pc, obj);
        if (d >= 0 && (best < 0 || d < best)) {
            best = d;
        }
    }
    return best;
}

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
/* PT37: 2048 (Tarant has 1500+ NPCs). The cache is never pruned, so a full
   one used to re-send every NPC that missed a free probe slot each 100 ms
   tick; it is also cleared on a map change (coop_lite_host_send_npc_pos). */
#define COOP_NPC_CACHE 2048
#define COOP_NPC_PROBES 8
#define COOP_NPC_DEDUPE 512 /* NPCs remembered per tick to skip repeats */
#define COOP_NPC_KEYFRAME_IDLE_MS 5000u /* idle + unchanged: slow refresh */
#define COOP_NPC_KEYFRAME_MS 2000u
#define COOP_NPC_KEYFRAME_QUEUE 64 /* keyframes per tick; changed NPCs go first */
/* PT37: NPC_POS also covers this many tiles around every PC: the host's own
   window rect is smaller than a client's bigger screen. */
#define COOP_NPC_POS_RADIUS 40
typedef struct CoopNpcCacheEntry {
    int64_t obj;
    PacketCoopNpcPosEntry last;
    unsigned int sent_ms;
    uint8_t seen_by_slot; /* PT77y: slots that already got this last state */
} CoopNpcCacheEntry;
static CoopNpcCacheEntry coop_npc_cache[COOP_NPC_CACHE];
static unsigned int coop_npc_pos_ms;
static int coop_npc_cache_map;

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
 * vanished). The sector cache holds 16 sectors (64 in co-op) and evicts the
 * least recently used unlocked one; a remote PC's sector is never drawn
 * locally, so it was the first to go, taking the PC with it. Keep each remote
 * PC's sector
 * locked (one sector per PC, to leave the cache to the view).
 */
typedef struct CoopSectorKeep {
    bool valid;
    int map;
    int count;
    int64_t ids[9]; /* PT77y: 3x3 around the PC */
} CoopSectorKeep;
static CoopSectorKeep coop_sector_keep[8];

static int coop_lite_keep_set(int64_t loc, int64_t* ids)
{
    return coop_lite_keep_ids(loc, ids, 9);
}

static void coop_lite_keep_sector(int slot, int64_t loc)
{
    CoopSectorKeep* k;
    int64_t ids[9];
    int64_t kept[9];
    int n;
    int kn = 0;
    int i;
    int j;
    int map;
    Sector* sector;

    if (slot < 0 || slot >= 8 || loc == 0 || !map_is_valid()) {
        return;
    }
    k = &coop_sector_keep[slot];
    map = map_current_map();
    n = coop_lite_keep_set(loc, ids);
    if (k->valid && k->map == map && k->count == n
        && memcmp(k->ids, ids, sizeof(int64_t) * (size_t)n) == 0) {
        return;
    }
    /* Lock the new set first (so shared sectors never drop to 0). */
    for (i = 0; i < n; i++) {
        if (sector_lock(ids[i], &sector)) {
            kept[kn++] = ids[i];
        }
    }
    if (k->valid && k->map == map) {
        for (j = 0; j < k->count; j++) {
            sector_unlock(k->ids[j]);
        }
    }
    k->valid = kn > 0;
    k->map = map;
    k->count = kn;
    memcpy(k->ids, kept, sizeof(int64_t) * (size_t)kn);
}

static void coop_lite_unkeep_sector(int slot)
{
    int j;

    if (coop_sector_keep[slot].valid
        && map_is_valid()
        && coop_sector_keep[slot].map == map_current_map()) {
        for (j = 0; j < coop_sector_keep[slot].count; j++) {
            sector_unlock(coop_sector_keep[slot].ids[j]);
        }
    }
    coop_sector_keep[slot].valid = false;
    coop_sector_keep[slot].count = 0;
}

static void coop_lite_release_sectors(void)
{
    int slot;

    for (slot = 0; slot < 8; slot++) {
        /* Map changed: the cache was flushed with it, nothing to unlock. */
        coop_lite_unkeep_sector(slot);
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
        || !coop_lite_oid_raw_equal(&coop_slot_pc[slot].oid, oid)) {
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
    coop_lite_auth_teleport(&teleport_data);
}

/*
 * ARCANUM_DEV_GRANT_RESURRECT=1 (set it for host and client): every PC learns
 * Resurrect (the whole Necromantic White college up to it) and gets enough
 * Willpower/Constitution for the fatigue it costs, once per PC. The host
 * grants every bound PC (its copies are the ones that cast); the client
 * grants its own PC so its spell bar shows it.
 */
static int (*coop_hotkey_spell_hook)(int spell);

void coop_lite_set_hotkey_spell_hook(int (*hook)(int spell))
{
    coop_hotkey_spell_hook = hook;
}

static ObjectID coop_dev_granted[8];
static bool coop_dev_granted_valid[8];

/* intgame_dev_click_obj (dev: a real mouse click on an object). */
static bool (*coop_dev_click_obj_hook)(int64_t obj, int button);

void coop_lite_set_dev_click_obj_hook(bool (*hook)(int64_t obj, int button))
{
    coop_dev_click_obj_hook = hook;
}

/* intgame_dev_probe_obj (dev: where a click would pick the object). */
static bool (*coop_dev_probe_obj_hook)(int64_t obj, int* x, int* y);

void coop_lite_set_dev_probe_obj_hook(bool (*hook)(int64_t obj, int* x, int* y))
{
    coop_dev_probe_obj_hook = hook;
}

static bool (*coop_dev_click_hotkey_hook)(int index);
static bool (*coop_dev_loot_hook)(int op);

void coop_lite_set_dev_loot_hook(bool (*hook)(int op))
{
    coop_dev_loot_hook = hook;
}

/* Dev (ARCANUM_DEV_INVEN_DUMP=N): every N s log a PC's inventory list as the
   UI sees it (OBJ_F_CRITTER_INVENTORY_LIST_IDX) with the flags that hide an
   item, and whether each item's parent agrees. */
static bool coop_lite_is_stackable(int64_t item);
static int coop_lite_stack_qty(int64_t item);

static void coop_lite_dev_inven_dump(const char* tag, int64_t pc)
{
    int cnt;
    int i;
    int64_t viewer = player_get_local_pc_obj();
    bool box;

    if (pc == OBJ_HANDLE_NULL) {
        return;
    }
    box = obj_field_int32_get(pc, OBJ_F_TYPE) == OBJ_TYPE_CONTAINER;

    cnt = obj_field_int32_get(pc, box ? OBJ_F_CONTAINER_INVENTORY_NUM : OBJ_F_CRITTER_INVENTORY_NUM);
    tig_debug_printf("CoopLite: dev: inven dump %s: %d items, gold %d%s\n", tag, cnt, item_gold_get(pc),
        !box && critter_is_dead(pc) ? " (dead)" : "");
    for (i = 0; i < cnt; i++) {
        int64_t item = obj_arrayfield_handle_get(pc, box ? OBJ_F_CONTAINER_INVENTORY_LIST_IDX : OBJ_F_CRITTER_INVENTORY_LIST_IDX, i);
        int64_t parent = OBJ_HANDLE_NULL;
        char name[128];
        char oid_str[40];

        name[0] = '\0';
        if (item == OBJ_HANDLE_NULL) {
            tig_debug_printf("CoopLite: dev:   [%d] NULL handle\n", i);
            continue;
        }
        object_examine(item, viewer != OBJ_HANDLE_NULL ? viewer : pc, name);
        objid_id_to_str(oid_str, obj_get_id(item));
        item_parent(item, &parent);
        tig_debug_printf("CoopLite: dev:   [%d] '%s' %s qty %d loc %d flags 0x%x item flags 0x%x parent %s\n",
            i, name, oid_str, coop_lite_is_stackable(item) ? coop_lite_stack_qty(item) : 1,
            item_inventory_location_get(item),
            (unsigned int)obj_field_int32_get(item, OBJ_F_FLAGS),
            (unsigned int)obj_field_int32_get(item, OBJ_F_ITEM_FLAGS),
            parent == pc ? "ok" : "MISMATCH");
    }
}

static unsigned int coop_dev_inven_dump_ms;

static void coop_lite_dev_inven_dump_tick(unsigned int now_ms)
{
    const char* env = coop_lite_env("ARCANUM_DEV_INVEN_DUMP");
    int secs;
    int slot;

    if (env == NULL || (secs = atoi(env)) <= 0) {
        return;
    }
    if (coop_dev_inven_dump_ms != 0 && !coop_lite_interval_elapsed(coop_dev_inven_dump_ms, now_ms, (unsigned int)secs * 1000u)) {
        return;
    }
    coop_dev_inven_dump_ms = now_ms != 0 ? now_ms : 1;
    if (tig_net_is_host()) {
        for (slot = 0; slot < 8; slot++) {
            char tag[32];

            snprintf(tag, sizeof(tag), "host's slot %d PC", slot);
            coop_lite_dev_inven_dump(tag, sub_4A2B60(slot));
        }
    } else {
        coop_lite_dev_inven_dump("my PC", player_get_local_pc_obj());
    }
}

void coop_lite_set_dev_click_hotkey_hook(bool (*hook)(int index))
{
    coop_dev_click_hotkey_hook = hook;
}

static void coop_lite_dev_grant_resurrect(int slot, int64_t pc)
{
    const char* env = coop_lite_env("ARCANUM_DEV_GRANT_RESURRECT");
    ObjectID oid;

    if (env == NULL || env[0] != '1' || pc == OBJ_HANDLE_NULL || slot < 0 || slot >= 8) {
        return;
    }
    oid = obj_get_id(pc);
    if (coop_dev_granted_valid[slot] && objid_is_equal(coop_dev_granted[slot], oid)) {
        /* PT18: keep it on the bar (an interface reset / map load clears
           the hotkeys; assigning is a no-op while it is there). */
        if (pc == player_get_local_pc_obj() && coop_hotkey_spell_hook != NULL && spell_is_known(pc, SPELL_RESURRECT)) {
            coop_hotkey_spell_hook(SPELL_RESURRECT);
        }
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
    /* PT16: and on our hotbar, so a playtest does not need the spell book. */
    if (pc == player_get_local_pc_obj() && coop_hotkey_spell_hook != NULL) {
        tig_debug_printf("CoopLite: dev: Resurrect on hotbar slot %d\n",
            coop_hotkey_spell_hook(SPELL_RESURRECT));
    }
}

/*
 * PT18 GUI: ARCANUM_DEV_GRANT_PICKPOCKET=N (host and client): every PC gets
 * N Pick Pocket skill points (Dexterity raised to 20 so N up to 5 fits),
 * once per PC. The host's copy is the one the steal roll uses; the client
 * sets its own so the character sheet shows it.
 */
static ObjectID coop_dev_pp_granted[8];
static bool coop_dev_pp_granted_valid[8];

static void coop_lite_dev_grant_pickpocket(int slot, int64_t pc)
{
    const char* env = coop_lite_env("ARCANUM_DEV_GRANT_PICKPOCKET");
    ObjectID oid;
    int points;

    if (env == NULL || env[0] == '\0' || pc == OBJ_HANDLE_NULL || slot < 0 || slot >= 8) {
        return;
    }
    oid = obj_get_id(pc);
    if (coop_dev_pp_granted_valid[slot] && objid_is_equal(coop_dev_pp_granted[slot], oid)) {
        return;
    }
    coop_dev_pp_granted[slot] = oid;
    coop_dev_pp_granted_valid[slot] = true;
    points = atoi(env);
    if (points <= 0) {
        points = 5;
    }
    tig_net_local_suspend_begin();
    if (stat_base_get(pc, STAT_DEXTERITY) < 20) {
        stat_base_set(pc, STAT_DEXTERITY, 20);
    }
    basic_skill_points_set(pc, BASIC_SKILL_PICK_POCKET, points);
    tig_net_local_suspend_end();
    tig_debug_printf("CoopLite: dev: slot %d PC Pick Pocket points %d (skill level %d, DEX %d)\n",
        slot, basic_skill_points_get(pc, BASIC_SKILL_PICK_POCKET),
        basic_skill_level(pc, BASIC_SKILL_PICK_POCKET), stat_level_get(pc, STAT_DEXTERITY));
}

static void coop_lite_dev_list_near(const char* why);
static void coop_lite_dev_trigger_tick(unsigned int now_ms);

static void coop_lite_dev_tick(unsigned int now_ms)
{
    const char* env;
    int slot;
    int64_t pc;
    int64_t wolf;
    int64_t loc;

    if (!coop_dev_start_done) {
        env = coop_lite_env("ARCANUM_DEV_START_LOC");
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
                coop_lite_auth_teleport(&teleport_data);
                if (strcmp(env, "merchant") == 0) {
                    coop_dev_merchant_pending = true;
                    coop_dev_merchant_ms = now_ms;
                }
            }
        }
    }
    for (slot = 0; slot < 8; slot++) {
        coop_lite_dev_grant_resurrect(slot, sub_4A2B60(slot));
        coop_lite_dev_grant_pickpocket(slot, sub_4A2B60(slot));
    }
    coop_lite_dev_trigger_tick(now_ms);

    if (coop_dev_merchant_pending && coop_lite_interval_elapsed(coop_dev_merchant_ms, now_ms, 2000u)) {
        coop_dev_merchant_pending = false;
        coop_lite_dev_go_to_merchant();
    }

    if (!coop_dev_save_done && sub_4A2B60(1) != OBJ_HANDLE_NULL) {
        env = coop_lite_env("ARCANUM_DEV_COOP_SAVE");
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
        const char* load_env = coop_lite_env("ARCANUM_DEV_COOP_LOAD");

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

    env = coop_lite_env("ARCANUM_DEV_SPAWN_HOSTILE_NEAR_CLIENT");
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
    env = coop_lite_env("ARCANUM_DEV_CLIENT_COMBAT");
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
   the weapon (28 to backpack), wield it again (28 to the weapon slot).
   =2: the pickup runs the click's AG_PICKUP_ITEM goal instead. */
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
    env = coop_lite_env("ARCANUM_DEV_CLIENT_ITEMS");
    if (env == NULL || (env[0] != '1' && env[0] != '2') || pc == OBJ_HANDLE_NULL) {
        coop_dev_items_stage = env == NULL || (env[0] != '1' && env[0] != '2') ? 5 : coop_dev_items_stage;
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
            if (env[0] == '2') {
                /* =2: the way a click does it (AG_PICKUP_ITEM goal). */
                tig_debug_printf("CoopLite: dev items: pickup goal (as a click) %s\n",
                    anim_goal_pickup_item_as_click(pc, item) ? "started" : "refused");
            } else {
                item_transfer(item, pc); /* client: Packet28 */
            }
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
    env = coop_lite_env("ARCANUM_DEV_EXPORT_CHAR");
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
    env = coop_lite_env("ARCANUM_DEV_TELEPORT_CLIENT_FAR");
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
    coop_lite_auth_teleport(&teleport_data);
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
            coop_lite_host_npc_stats(npc, now_ms); /* PT79 */
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
            if (coop_lite_oid_raw_equal(&now_list[j], &coop_client_followers[i])) {
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
    e->fatigue_damage = critter_fatigue_damage_get(obj);
}

static void coop_lite_npc_cache_reset(void)
{
    memset(coop_npc_cache, 0, sizeof(coop_npc_cache));
    coop_npc_pos_ms = 0;
}

static CoopNpcCacheEntry* coop_lite_npc_cache_slot(int64_t obj)
{
    unsigned int h = (unsigned int)((uint64_t)obj * 2654435761u) % COOP_NPC_CACHE;
    CoopNpcCacheEntry* oldest = NULL;
    unsigned int i;

    for (i = 0; i < COOP_NPC_PROBES; i++) {
        CoopNpcCacheEntry* c = &coop_npc_cache[(h + i) % COOP_NPC_CACHE];
        if (c->obj == obj || c->obj == OBJ_HANDLE_NULL) {
            return c;
        }
        if (oldest == NULL || (int)(oldest->sent_ms - c->sent_ms) > 0) {
            oldest = c;
        }
    }
    /* PT37: all probes taken by others: replace the one sent longest ago. */
    return oldest;
}

/* NPCs within COOP_NPC_POS_RADIUS tiles of `pc`. */
static void coop_lite_npc_list_around(int64_t pc, ObjectList* objects)
{
    int64_t loc = obj_field_int64_get(pc, OBJ_F_LOCATION);
    int64_t x = LOCATION_GET_X(loc);
    int64_t y = LOCATION_GET_Y(loc);
    LocRect rect;

    rect.x1 = x > COOP_NPC_POS_RADIUS ? x - COOP_NPC_POS_RADIUS : 0;
    rect.y1 = y > COOP_NPC_POS_RADIUS ? y - COOP_NPC_POS_RADIUS : 0;
    rect.x2 = x + COOP_NPC_POS_RADIUS;
    rect.y2 = y + COOP_NPC_POS_RADIUS;
    object_list_rect(&rect, OBJ_TM_NPC, objects);
}

/* PT79 (review 2.3a): PCs standing together share one object_list_rect
   (the union of their squares) instead of one 81x81 query each; each NPC
   is still only taken when inside some member's own square, so the result
   is exactly the old per-PC union. Returns the member slot mask. */
#define COOP_NPC_GROUP_TILES 16
static unsigned int coop_lite_npc_list_group(int lead, bool* scanned, ObjectList* objects)
{
    int64_t locs[8] = { 0 };
    unsigned int members = 0;
    int64_t lead_loc;
    int64_t x1 = 0;
    int64_t y1 = 0;
    int64_t x2 = 0;
    int64_t y2 = 0;
    LocRect rect;
    int slot;

    lead_loc = obj_field_int64_get(sub_4A2B60(lead), OBJ_F_LOCATION);
    for (slot = lead; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);
        int64_t x;
        int64_t y;
        int64_t dx;
        int64_t dy;

        if (pc == OBJ_HANDLE_NULL || scanned[slot]) {
            continue;
        }
        locs[slot] = obj_field_int64_get(pc, OBJ_F_LOCATION);
        x = LOCATION_GET_X(locs[slot]);
        y = LOCATION_GET_Y(locs[slot]);
        dx = x - LOCATION_GET_X(lead_loc);
        dy = y - LOCATION_GET_Y(lead_loc);
        if (slot != lead && (dx < -COOP_NPC_GROUP_TILES || dx > COOP_NPC_GROUP_TILES
                                || dy < -COOP_NPC_GROUP_TILES || dy > COOP_NPC_GROUP_TILES)) {
            continue;
        }
        if (members == 0 || x - COOP_NPC_POS_RADIUS < x1) {
            x1 = x - COOP_NPC_POS_RADIUS;
        }
        if (members == 0 || y - COOP_NPC_POS_RADIUS < y1) {
            y1 = y - COOP_NPC_POS_RADIUS;
        }
        if (members == 0 || x + COOP_NPC_POS_RADIUS > x2) {
            x2 = x + COOP_NPC_POS_RADIUS;
        }
        if (members == 0 || y + COOP_NPC_POS_RADIUS > y2) {
            y2 = y + COOP_NPC_POS_RADIUS;
        }
        members |= 1u << slot;
        scanned[slot] = true;
    }
    rect.x1 = x1 > 0 ? x1 : 0;
    rect.y1 = y1 > 0 ? y1 : 0;
    rect.x2 = x2;
    rect.y2 = y2;
    object_list_rect(&rect, OBJ_TM_NPC, objects);
    if ((members & (members - 1)) != 0) {
        /* Several members: keep the per-member squares exact. */
        ObjectNode* node;

        for (node = objects->head; node != NULL; node = node->next) {
            int64_t loc = obj_field_int64_get(node->obj, OBJ_F_LOCATION);
            bool keep = false;

            for (slot = 0; slot < 8 && !keep; slot++) {
                int64_t dx;
                int64_t dy;

                if ((members & (1u << slot)) == 0) {
                    continue;
                }
                dx = LOCATION_GET_X(loc) - LOCATION_GET_X(locs[slot]);
                dy = LOCATION_GET_Y(loc) - LOCATION_GET_Y(locs[slot]);
                keep = dx >= -COOP_NPC_POS_RADIUS && dx <= COOP_NPC_POS_RADIUS
                    && dy >= -COOP_NPC_POS_RADIUS && dy <= COOP_NPC_POS_RADIUS;
            }
            if (!keep) {
                node->obj = OBJ_HANDLE_NULL; /* skipped by the caller; freed as is */
            }
        }
    }
    return members;
}

static unsigned int coop_lite_npc_slots_in_range(int64_t npc)
{
    unsigned int mask = 0;
    int slot;
    int64_t loc;

    if (npc == OBJ_HANDLE_NULL) {
        return 0;
    }
    loc = obj_field_int64_get(npc, OBJ_F_LOCATION);
    for (slot = 0; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);

        if (pc != OBJ_HANDLE_NULL && location_dist(loc, obj_field_int64_get(pc, OBJ_F_LOCATION)) <= COOP_NPC_POS_RADIUS) {
            mask |= 1u << slot;
        }
    }
    return mask;
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
    int64_t done[COOP_NPC_DEDUPE];
    CoopNpcCacheEntry* kf_cache[COOP_NPC_KEYFRAME_QUEUE];
    PacketCoopNpcPosEntry kf_entry[COOP_NPC_KEYFRAME_QUEUE];
    int kf_cnt = 0;
    int done_cnt = 0;
    int k;
    bool any_client = false;
    bool scanned[8] = { false };

    if (coop_portal_changed && coop_lite_interval_elapsed(coop_portal_changed_ms, now_ms, 400u)) {
        coop_portal_changed = false;
        coop_lite_host_world_dirty();
    }
    if (!coop_lite_interval_elapsed(coop_npc_pos_ms, now_ms, 100u)) {
        return;
    }
    coop_npc_pos_ms = now_ms;
    if (coop_npc_cache_map != map_current_map()) {
        coop_npc_cache_map = map_current_map();
        memset(coop_npc_cache, 0, sizeof(coop_npc_cache));
    }

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
        unsigned int members;

        pc = sub_4A2B60(slot);
        if (pc == OBJ_HANDLE_NULL || scanned[slot]) {
            continue;
        }
        members = coop_lite_npc_list_group(slot, scanned, &objects); /* PT79 */
        for (node = objects.head; node != NULL; node = node->next) {
            if (node->obj == OBJ_HANDLE_NULL) {
                continue; /* outside every member's square */
            }
            for (k = 0; k < done_cnt; k++) {
                if (done[k] == node->obj) {
                    break;
                }
            }
            if (k < done_cnt) {
                continue;
            }
            if (done_cnt < COOP_NPC_DEDUPE) {
                done[done_cnt++] = node->obj;
            }
            coop_lite_fill_npc_entry(&e, node->obj);
            c = coop_lite_npc_cache_slot(node->obj);
            {
                unsigned int in_range = coop_lite_npc_slots_in_range(node->obj) | (members & 0xFFu);
                bool unchanged;

                if (c->obj != node->obj) {
                    c->obj = node->obj;
                    c->seen_by_slot = 0;
                    memset(&c->last, 0, sizeof(c->last));
                }
                c->seen_by_slot &= (uint8_t)in_range;
                unchanged = memcmp(&c->last, &e, sizeof(e)) == 0;
                if (!coop_lite_npc_pos_force(c->seen_by_slot, in_range, unchanged)) {
                    /* All in-range slots already have this state: slow keyframe. */
                    unsigned int keyframe = anim_is_idle(node->obj) ? COOP_NPC_KEYFRAME_IDLE_MS
                                                                     : COOP_NPC_KEYFRAME_MS;

                    if (coop_lite_interval_elapsed(c->sent_ms, now_ms, keyframe)
                        && kf_cnt < COOP_NPC_KEYFRAME_QUEUE) {
                        kf_cache[kf_cnt] = c;
                        kf_entry[kf_cnt++] = e;
                    }
                    continue;
                }
                c->last = e;
                c->sent_ms = now_ms;
                c->seen_by_slot = (uint8_t)in_range;
            }
            pkt.entries[pkt.count++] = e;
            coop_diag.npc_tx++;
            if (pkt.count == COOP_NPC_POS_MAX) {
                tig_net_send_pose_all(&pkt, (int)coop_lite_batch_size(pkt.count,
                    COOP_NPC_POS_MAX, sizeof(PacketCoopNpcPosEntry)));
                pkt.count = 0;
            }
        }
        object_list_destroy(&objects);
    }
    for (k = 0; k < kf_cnt; k++) {
        kf_cache[k]->sent_ms = now_ms;
        pkt.entries[pkt.count++] = kf_entry[k];
        coop_diag.npc_tx++;
        if (pkt.count == COOP_NPC_POS_MAX) {
            tig_net_send_pose_all(&pkt, (int)coop_lite_batch_size(pkt.count,
                COOP_NPC_POS_MAX, sizeof(PacketCoopNpcPosEntry)));
            pkt.count = 0;
        }
    }
    if (pkt.count > 0) {
        tig_net_send_pose_all(&pkt, (int)coop_lite_batch_size(pkt.count,
            COOP_NPC_POS_MAX, sizeof(PacketCoopNpcPosEntry)));
    }
}

static void coop_lite_float_damage(int64_t obj, int prev, int now)
{
    char str[32];
    int delta = coop_lite_float_delta(prev, now, object_hp_max(obj));

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

static int64_t coop_lite_get_oid_obj(const uint8_t* oid);

/* PT26: client side of PACKET_COOP_NPC_REQ, at most one ask per OID / 5 s. */
#define COOP_NPC_WANT_MAX 32
static struct {
    uint8_t oid[24];
    unsigned int ms;
} coop_npc_want[COOP_NPC_WANT_MAX];
static int coop_npc_want_next;

static void coop_lite_client_want_npc(const uint8_t* oid)
{
    unsigned int now = coop_obj_now_ms;
    PacketCoopObjReq pkt;
    int i;

    for (i = 0; i < COOP_NPC_WANT_MAX; i++) {
        if (coop_npc_want[i].ms != 0 && coop_lite_oid_raw_equal(coop_npc_want[i].oid, oid)) {
            if (!coop_lite_interval_elapsed(coop_npc_want[i].ms, now, 5000u)) {
                return;
            }
            break;
        }
    }
    if (i == COOP_NPC_WANT_MAX) {
        i = coop_npc_want_next;
        coop_npc_want_next = (coop_npc_want_next + 1) % COOP_NPC_WANT_MAX;
    }
    memcpy(coop_npc_want[i].oid, oid, 24);
    coop_npc_want[i].ms = now != 0 ? now : 1;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_NPC_REQ;
    pkt.count = 1;
    memcpy(pkt.oid[0], oid, 24);
    tig_net_send_app(&pkt, (int)(offsetof(PacketCoopObjReq, oid) + 24u), 0);
    coop_diag.npc_req++;
}

/* PT26 host: re-announce NPCs a client does not have (Packet70, as
   mp_object_create does; NPC_POS then sets its pose, art and HP). */
void coop_lite_handle_npc_req(PacketCoopObjReq* pkt)
{
#ifndef COOP_LITE_TEST_STUBS
    int slot;
    int i;
    int sent = 0;

    if (!tig_net_is_host() || pkt == NULL || pkt->type != PACKET_COOP_NPC_REQ
        || !coop_lite_obj_req_valid(pkt, tig_net_last_message_size())) {
        return;
    }
    slot = sub_52A530();
    if (slot <= 0 || slot >= 8 || sub_4A2B60(slot) == OBJ_HANDLE_NULL) {
        return;
    }
    if (!coop_lite_host_rate_ok(1, slot, pkt->count > 0 ? pkt->count : 1)) {
        return; /* PT52k */
    }
    for (i = 0; i < pkt->count; i++) {
        int64_t npc = coop_lite_get_oid_obj(pkt->oid[i]);
        Packet70 out;
        char name[128];

        if (npc == OBJ_HANDLE_NULL || obj_field_int32_get(npc, OBJ_F_TYPE) != OBJ_TYPE_NPC
            || coop_lite_obj_is_slot_pc(npc)
            || (obj_field_int32_get(npc, OBJ_F_FLAGS) & (OF_DESTROYED | OF_OFF)) != 0) {
            continue;
        }
        memset(&out, 0, sizeof(out));
        out.type = 70;
        out.subtype = 0;
        out.s0.name = sub_49B290(npc);
        out.s0.oid = obj_get_id(npc);
        out.s0.loc = obj_field_int64_get(npc, OBJ_F_LOCATION);
        tig_net_send_app(&out, sizeof(out), slot);
        coop_lite_npc_cache_slot(npc)->sent_ms = 0; /* pose again next tick */
        name[0] = '\0';
        object_examine(npc, sub_4A2B60(slot), name);
        tig_debug_printf("CoopLite: npc req: slot %d lacks '%s' (proto %d): re-announced\n", slot, name,
            out.s0.name);
        sent++;
    }
    (void)sent;
#else
    (void)pkt;
#endif
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
        if (obj == OBJ_HANDLE_NULL) {
            coop_diag.npc_rx_unknown++;
            /* PT26: an NPC the host has and we never got (made by a
               script while its Packet70 could not land, ...): ask. Only
               where our sector is loaded, so a later sector load cannot
               bring a second copy. */
            if (sector_loaded(sector_id_from_loc(e->loc))) {
                coop_lite_client_want_npc(e->oid);
            }
            continue;
        }
        if (obj_field_int32_get(obj, OBJ_F_TYPE) != OBJ_TYPE_NPC
            || coop_lite_obj_is_slot_pc(obj) || !coop_lite_loc_in_map(e->loc)) {
            continue;
        }
        coop_diag.npc_rx++;
        /* PT26: NPC_POS only lists what the host shows (object lists skip
           OF_OFF): one a script switched on stayed hidden here. */
        if ((obj_field_int32_get(obj, OBJ_F_FLAGS) & OF_OFF) != 0) {
            tig_net_local_suspend_begin();
            object_flags_unset(obj, OF_OFF);
            tig_net_local_suspend_end();
            coop_diag.npc_rx_on++;
            tig_debug_printf("CoopLite: NPC_POS: switched on an NPC hidden here\n");
        }
        if (obj_field_int64_get(obj, OBJ_F_LOCATION) != e->loc
            || obj_field_int32_get(obj, OBJ_F_OFFSET_X) != e->offset_x
            || obj_field_int32_get(obj, OBJ_F_OFFSET_Y) != e->offset_y) {
            sub_43E770(obj, e->loc, e->offset_x, e->offset_y);
        }
        if ((unsigned int)obj_field_int32_get(obj, OBJ_F_CURRENT_AID) != e->art_id
            && !coop_lite_pose_cuts_swing_obj(obj, (tig_art_id_t)e->art_id)) {
            object_set_current_aid(obj, (tig_art_id_t)e->art_id);
        }
        flags = obj_field_int32_get(obj, OBJ_F_FLAGS);
        if ((e->flags & COOP_NPC_POS_DEAD) != 0) {
            if ((flags & (OF_FLAT | OF_NO_BLOCK)) != (OF_FLAT | OF_NO_BLOCK)) {
                object_flags_set(obj, OF_FLAT | OF_NO_BLOCK);
                coop_diag.npc_rx_dead++;
            }
        } else if ((flags & OF_FLAT) != 0) {
            object_flags_unset(obj, OF_FLAT | OF_NO_BLOCK);
        }
        /* Review P1-2: the host's HP is the truth (health bars, the
           locally-dead check, and a revive on the host shows up here). */
        if (e->hp_damage >= 0) {
            /* PT77af: alive on the host stays alive here (Magnus). */
            int dmg = coop_lite_npc_rx_hp_damage(e->hp_damage, (e->flags & COOP_NPC_POS_DEAD) != 0,
                object_hp_max(obj));

            if (obj_field_int32_get(obj, OBJ_F_HP_DAMAGE) != dmg) {
                obj_field_int32_set(obj, OBJ_F_HP_DAMAGE, dmg);
            }
        }
        /* PT37: so does fatigue (client-side spell cost / lost regen events
           left companions exhausted here only). Host value wins. */
        if (e->fatigue_damage >= 0
            && obj_field_int32_get(obj, OBJ_F_CRITTER_FATIGUE_DAMAGE) != e->fatigue_damage) {
            obj_field_int32_set(obj, OBJ_F_CRITTER_FATIGUE_DAMAGE, e->fatigue_damage);
        }
        hp = coop_lite_npc_hp_slot(obj);
        if (hp->obj == obj && e->hp_damage > hp->hp_damage) {
            coop_lite_float_damage(obj, hp->hp_damage, e->hp_damage);
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
    str = coop_lite_lang(str); /* PT53 */
    ui_message.type = UI_MSG_TYPE_FEEDBACK;
    ui_message.str = (char*)str;
    ui_display_msg(&ui_message);
    if (pc != OBJ_HANDLE_NULL) {
        tf_add(pc, tf_type, str);
    }
    tig_debug_printf("CoopLite: notice: %s\n", str);
}

/* Host: tell every client, and show it here too. */
static void coop_lite_host_party_notice(int kind, int slot, unsigned int countdown_ms)
{
    PacketCoopPartyState pkt;
    char str[512];

    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_PARTY_STATE;
    pkt.kind = kind;
    pkt.slot = slot;
    pkt.countdown_ms = countdown_ms;
    tig_net_send_app_all(&pkt, sizeof(pkt));

    coop_lite_party_notice_text(kind, countdown_ms, true, str, sizeof(str));
    if (str[0] != '\0') {
        coop_lite_show_notice(str, coop_lite_party_notice_is_good(kind) ? TF_TYPE_GREEN : TF_TYPE_RED);
    }
}

#ifndef COOP_LITE_TEST_STUBS
static bool coop_client_rest_faded;

bool coop_lite_client_rest_refused(void)
{
    if (!tig_net_is_active() || tig_net_is_host()) {
        return false;
    }
    coop_lite_show_notice("Отдыхать или ждать может только лидер группы. / Only the party leader can rest or wait.", TF_TYPE_RED);
    tig_debug_printf("CoopLite: rest/wait refused on the client (the host owns game time)\n");
    return true;
}

void coop_lite_host_world_dirty(void)
{
    PacketCoopPartyState pkt;

    if (!tig_net_is_active() || !tig_net_is_host() || !tig_net_is_coop_enabled()) {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_PARTY_STATE;
    pkt.kind = COOP_PARTY_WORLD_DIRTY;
    tig_net_send_app_all(&pkt, sizeof(pkt));
}

static int coop_rest_extra_hours; /* PT27: hours slept on for a hurt client */

/* PT27 item 5: "Until Healed" only looked at the host's own PC (sleep_ui
   checks sleep_ui_obj's damage), so a host at full health woke the party
   after one hour. true while a living co-op client PC is still hurt - the
   party sleeps on; capped at a week per rest so a PC that cannot heal
   (e.g. a bad stat effect) cannot lock the game asleep. */
bool coop_lite_host_rest_anyone_hurt(void)
{
    int slot;

    if (!tig_net_is_active() || !tig_net_is_host() || coop_rest_extra_hours >= 7 * 24) {
        return false;
    }
    for (slot = 1; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);

        if (pc != OBJ_HANDLE_NULL && !critter_is_dead(pc) && object_hp_damage_get(pc) > 0) {
            coop_rest_extra_hours++;
            tig_debug_printf("CoopLite: rest: slot %d still hurt (hp damage %d) - the party sleeps on\n", slot,
                object_hp_damage_get(pc));
            return true;
        }
    }
    return false;
}

void coop_lite_host_rest_begin(void)
{
    coop_rest_extra_hours = 0;
    if (tig_net_is_active() && tig_net_is_host()) {
        tig_debug_printf("CoopLite: host rests - clients fade out\n");
        coop_lite_host_party_notice(COOP_PARTY_REST_BEGIN, 0, 0);
    }
}

static void coop_lite_host_send_vitals(int64_t obj);

void coop_lite_host_rest_end(void)
{
    if (tig_net_is_active() && tig_net_is_host()) {
        PacketGameTime gt;

        /* the new time first, then wake everyone up */
        memset(&gt, 0, sizeof(gt));
        gt.type = 3;
        gt.game_time = sub_45A7C0().value;
        gt.anim_time = 0; /* forward-only on clients: untouched */
        tig_net_send_app_all(&gt, sizeof(gt));
        tig_debug_printf("CoopLite: host woke up - clients fade in\n");
        {
            int slot;

            for (slot = 1; slot < 8; slot++) {
                coop_lite_host_send_vitals(sub_4A2B60(slot));
            }
        }
        coop_lite_host_party_notice(COOP_PARTY_REST_END, 0, 0);
    }
}

/* PT26 item 4: a slot PC's HP/fatigue straight to everyone. The setters
   only send when the MP lock is off; a heal or regen that ran under it (or
   a heal that found nothing to do because our copy had already healed
   quietly) left the owner's bars behind - sleep "stopped healing". */
static void coop_lite_host_send_vitals(int64_t obj)
{
    Packet100 pkt;

    if (obj == OBJ_HANDLE_NULL || !tig_net_is_active() || !tig_net_is_host()) {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = 100;
    pkt.subtype = 13;
    pkt.d.z.field_8 = obj_get_id(obj);
    pkt.d.z.field_38 = object_hp_damage_get(obj);
    pkt.d.z.field_3C = critter_fatigue_damage_get(obj);
    tig_net_send_app_all(&pkt, sizeof(pkt));
}

static int coop_vitals_sent_hp[8];
static int coop_vitals_sent_fat[8];
static unsigned int coop_vitals_ms;

/* PT54: next vitals tick resends every PC (host PC included) - after a
   save load, a resync or a revive the clients' copies may be stale. */
static int coop_vitals_force_rounds;

static void coop_lite_host_vitals_force(void)
{
    coop_vitals_force_rounds = 3; /* the next three 3 s ticks: a client mid-load still gets one */
    coop_lite_host_pcs_force(); /* PT54a: portraits / body art too */
}

/* PT26: every 3 s, a slot PC whose vitals differ from the last ones we sent.
   PT54: slot 0 (the host PC) too: its death / revive / save-load HP never
   reached the clients, which saw the host dead forever. */
static void coop_lite_host_vitals_tick(unsigned int now_ms)
{
    int slot;

    if (!coop_lite_interval_elapsed(coop_vitals_ms, now_ms, 3000u)) {
        return;
    }
    coop_vitals_ms = now_ms != 0 ? now_ms : 1;
    if (coop_vitals_force_rounds > 0) {
        coop_vitals_force_rounds--;
        for (slot = 0; slot < 8; slot++) {
            coop_vitals_sent_hp[slot] = -1;
            coop_vitals_sent_fat[slot] = -1;
        }
    }
    for (slot = 0; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);
        int hp;
        int fat;

        if (pc == OBJ_HANDLE_NULL) {
            continue;
        }
        hp = object_hp_damage_get(pc);
        fat = critter_fatigue_damage_get(pc);
        if (hp != coop_vitals_sent_hp[slot] || fat != coop_vitals_sent_fat[slot]) {
            coop_vitals_sent_hp[slot] = hp;
            coop_vitals_sent_fat[slot] = fat;
            coop_lite_host_send_vitals(pc);
            if (slot == 0) {
                tig_debug_printf("CoopLite: host PC vitals sent (hp damage %d, %s)\n", hp,
                    critter_is_dead(pc) ? "dead" : "alive");
            }
        }
    }
}

void coop_lite_host_rest_heal(int hours)
{
    int slot;

    if (!tig_net_is_active() || !tig_net_is_host()) {
        return;
    }
    /* What sleep_ui does for the sleeper and its followers, for every
       other co-op PC too; HP/fatigue reach the owners via the usual
       host->client field updates. */
    for (slot = 1; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);
        ObjectList objects;
        ObjectNode* node;

        if (pc == OBJ_HANDLE_NULL || critter_is_dead(pc)) {
            continue;
        }
        critter_resting_heal(pc, hours);
        coop_lite_host_send_vitals(pc);
        object_list_all_followers(pc, &objects);
        for (node = objects.head; node != NULL; node = node->next) {
            critter_resting_heal(node->obj, hours);
            coop_lite_host_send_vitals(node->obj);
        }
        object_list_destroy(&objects);
        tig_debug_printf("CoopLite: rest heal: slot %d +%dh -> hp damage %d, fatigue damage %d\n", slot, hours,
            object_hp_damage_get(pc), critter_fatigue_damage_get(pc));
    }
}

/* PT26 item 5: while the host is on the world map the client saw its PC
   just standing there. A steady dark overlay (the notice stays readable
   under it) until the host closes the map / arrives; it lapses by itself
   if the refreshes stop (host gone, packet lost). */
static bool coop_client_wmap_dimmed;
static unsigned int coop_client_wmap_ms;

static void coop_lite_client_wmap_dim(bool on)
{
    coop_client_wmap_ms = on ? (coop_obj_now_ms != 0 ? coop_obj_now_ms : 1) : 0;
    if (on == coop_client_wmap_dimmed) {
        return;
    }
    coop_client_wmap_dimmed = on;
    if (!on && wmap_ui_is_spectating()) {
        wmap_ui_spectate_close(); /* PT73 */
    }
    if (!coop_client_rest_faded) {
        tig_video_dim(tig_color_make(0, 0, 16), on && !wmap_ui_is_spectating() ? 150 : 0);
    }
    tig_debug_printf("CoopLite: world map overlay %s\n", on ? "on (host travels)" : "off");
}

static void coop_lite_client_wmap_tick(unsigned int now_ms)
{
    if (coop_client_wmap_dimmed && coop_lite_interval_elapsed(coop_client_wmap_ms, now_ms, 8000u)) {
        tig_debug_printf("CoopLite: world map overlay timed out\n");
        coop_lite_client_wmap_dim(false);
    }
}

static bool coop_host_wmap_open;
static unsigned int coop_host_wmap_ms;
static bool coop_host_wmap_pos_have;
static unsigned int coop_host_wmap_pos_ms;
static unsigned int coop_host_wmap_pos_seq;
static int coop_host_wmap_pos_x;
static int coop_host_wmap_pos_y;
static unsigned int coop_client_wmap_pos_seq;

/* PT73 client: show the host's world map read-only, marker at x,y. */
void coop_lite_handle_wmap_pos(PacketCoopWmapPos* pkt)
{
    if (!tig_net_is_active() || tig_net_is_host()
        || !coop_lite_wmap_pos_valid(pkt, tig_net_last_message_size())) {
        return;
    }
    if (!coop_lite_client_world_ready() || coop_client_rest_faded) {
        return;
    }
    if (coop_client_wmap_pos_seq != 0 && (int)(pkt->seq - coop_client_wmap_pos_seq) <= 0) {
        return; /* stale */
    }
    coop_client_wmap_pos_seq = pkt->seq;
    if (!coop_client_wmap_dimmed) {
        coop_lite_client_wmap_dim(true);
    } else {
        coop_client_wmap_ms = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1;
    }
    if (!wmap_ui_is_spectating()) {
        if (wmap_ui_is_created()) {
            return; /* own town map open: leave it */
        }
        if (!wmap_ui_spectate_open()) {
            return;
        }
        tig_video_dim(tig_color_make(0, 0, 16), 0);
        tig_debug_printf("CoopLite: PT73 spectating the host's world map\n");
    }
    wmap_ui_spectate_set(pkt->x, pkt->y);
}

/* PT26 host: tell clients the world map is open (edge + every 3 s). */
static void coop_lite_host_wmap_tick(unsigned int now_ms)
{
    /* PT61: only the world map dims the clients; the location (town) map,
       looking or planning a route on it, does not. */
    bool open = wmap_ui_is_created() && !wmap_ui_is_town_mode();

    if (open && (!coop_host_wmap_open || coop_lite_interval_elapsed(coop_host_wmap_ms, now_ms, 3000u))) {
        PacketCoopPartyState pkt;

        memset(&pkt, 0, sizeof(pkt));
        pkt.type = PACKET_COOP_PARTY_STATE;
        pkt.kind = COOP_PARTY_WMAP_OPEN;
        tig_net_send_app_all(&pkt, sizeof(pkt));
        if (!coop_host_wmap_open) {
            tig_debug_printf("CoopLite: host opened the world map - clients dim\n");
        }
        coop_host_wmap_ms = now_ms != 0 ? now_ms : 1;
    }
    if (open) {
        int x;
        int y;

        if (wmap_ui_world_coords_get(&x, &y)
            && coop_lite_wmap_pos_should_send(coop_host_wmap_pos_have, coop_host_wmap_pos_ms, now_ms,
                coop_host_wmap_pos_x, coop_host_wmap_pos_y, x, y)) {
            PacketCoopWmapPos wp;

            memset(&wp, 0, sizeof(wp));
            wp.type = PACKET_COOP_WMAP_POS;
            wp.seq = ++coop_host_wmap_pos_seq;
            wp.x = x;
            wp.y = y;
            wp.moving = wmap_ui_is_moving() ? 1 : 0;
            tig_net_send_app_all(&wp, sizeof(wp));
            coop_host_wmap_pos_have = true;
            coop_host_wmap_pos_ms = now_ms;
            coop_host_wmap_pos_x = x;
            coop_host_wmap_pos_y = y;
        }
    } else if (coop_host_wmap_open) {
        coop_host_wmap_pos_have = false;
    }
    if (!open && coop_host_wmap_open) {
        PacketCoopPartyState pkt;

        memset(&pkt, 0, sizeof(pkt));
        pkt.type = PACKET_COOP_PARTY_STATE;
        pkt.kind = COOP_PARTY_WMAP_CLOSE;
        tig_net_send_app_all(&pkt, sizeof(pkt));
        tig_debug_printf("CoopLite: host closed the world map - clients undim\n");
    }
    coop_host_wmap_open = open;
}

static void coop_lite_client_rest_fade(bool out)
{
    FadeData fade;

    if (out == coop_client_rest_faded) {
        return;
    }
    coop_client_rest_faded = out;
    memset(&fade, 0, sizeof(fade));
    fade.flags = out ? 0 : FADE_IN;
    fade.duration = 2.0f;
    fade.steps = 48;
    fade.color = tig_color_make(0, 0, 0);
    gfade_run(&fade);
    if (!out && coop_client_wmap_dimmed) {
        tig_video_dim(tig_color_make(0, 0, 16), 150);
    }
}
#else
void coop_lite_handle_wmap_pos(PacketCoopWmapPos* pkt) { (void)pkt; }
bool coop_lite_client_rest_refused(void) { return false; }
void coop_lite_host_rest_begin(void) { }
void coop_lite_host_world_dirty(void) { }
void coop_lite_host_rest_end(void) { }
void coop_lite_host_rest_heal(int hours) { (void)hours; }
bool coop_lite_host_rest_anyone_hurt(void) { return false; }
#endif

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

/*
 * PT33 item 1: the host loaded a save from the menu (not the party-wipe
 * reload). That bumps the map generation, so every client's PC_POS /
 * WORLD_REQ carry a stale key and are dropped, and nothing told the client
 * to start over: it had to be restarted by hand. Take the party-reload route
 * for it too: once the host is back in the world it sends COOP_PARTY_RESYNC,
 * each client resets its session and re-offers its character, exactly as
 * after a wipe reload. As there, the party comes back on its feet.
 */
void coop_lite_host_note_save_loaded(void)
{
    int slot;

    if (!tig_net_is_coop_enabled() || !tig_net_is_host()) {
        return;
    }
    coop_lite_travel_reset(); /* PT52 */
    coop_reload_resync_pending = true;
    for (slot = 1; slot < 8; slot++) {
        if (tig_net_client_is_active(slot)) {
            coop_reload_raise[slot] = true;
        }
    }
    tig_debug_printf("CoopLite: host loaded a save — clients will be resynced\n");
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
    char str[512];

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
            snprintf(str, sizeof(str), "%s пал. / %s has fallen.", name, name);
            tf_add(pc, TF_TYPE_RED, str);
            tig_debug_printf("CoopLite: slot %d PC fell — corpse until resurrected\n", slot);
        } else if ((rose & (1u << slot)) != 0) {
            /* PT17: the owner's client stands its PC up from these vitals
               (coop_lite_client_watch_vitals), whatever its own copy of
               the spell did; re-send them once the spell is done. */
            object_hp_damage_set(pc, object_hp_damage_get(pc)); /* not locked: broadcasts */
            tig_debug_printf("CoopLite: slot %d PC resurrected (hp damage %d, fatigue %d)\n", slot,
                object_hp_damage_get(pc), critter_fatigue_damage_get(pc));
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
    coop_lite_story_reset(); /* clients get a fresh story snapshot */
    /* PT37: clients reload the map from disk; every NPC pose (dead ones
       included) must go out again, not only what changed. */
    memset(coop_npc_cache, 0, sizeof(coop_npc_cache));
    coop_npc_pos_ms = 0;
    coop_lite_follow_armed = true; /* the load had no spawn teleport to arm it */
    multiplayer_bind_slot_pc(0, player_get_local_pc_obj());
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_PARTY_STATE;
    pkt.kind = COOP_PARTY_RESYNC;
    pkt.slot = -1;
    tig_net_send_app_all(&pkt, sizeof(pkt));
    coop_lite_host_vitals_force(); /* PT54 */
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

void coop_lite_local_teleport_finished(void)
{
    /*
     * PT23 (confirmed in a headless map-change run): after a host map change
     * the carry wait could stay armed (coop_lite_after_party_carry not
     * reached), and while it is armed the client sends no PC_POS at all. The
     * host's copy of the client PC then froze at the arrival tile: NPCs
     * fought that ghost, and the host FORCE-corrected the client back to it
     * once a second — the PC "ran off by itself" / slid through walls.
     */
    if (coop_follow_carry_pending) {
        tig_debug_printf("CoopLite: party carry wait cleared after the teleport\n");
        coop_follow_carry_pending = false;
    }
    /* PT31: a host travel refused while this teleport was pending runs now,
       even when this one returned early (the success path flushes too). */
    coop_lite_flush_pending_follow();
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
#ifndef COOP_LITE_TEST_STUBS
    /* PT54c: the follow destination was picked on the OLD map; on the new
       one it can be the host's tile or blocked. Step to a free tile next
       to the host (host PC is already carried there above). */
    if (!tig_net_is_host()) {
        int64_t own = obj_field_int64_get(mover, OBJ_F_LOCATION);

        if (own == host_loc || !coop_lite_loc_is_free_walkable(own)) {
            loc = coop_lite_follow_loc_for_slot(host_loc, sub_529520());
            if (loc != own && loc != host_loc) {
                multiplayer_lock();
                sub_43E770(mover, loc, 0, 0);
                multiplayer_unlock();
                tig_debug_printf("CoopLite: PT54c own PC moved off %d,%d to free tile %d,%d\n",
                    (int)LOCATION_GET_X(own), (int)LOCATION_GET_Y(own),
                    (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc));
            }
        }
    }
#endif
}

/* PT54c: teleport.c hook - in co-op repair an inventory whose NUM does not
   match its list instead of refusing the teleport. */
bool coop_lite_teleport_inv_fix(int64_t obj)
{
    if (!tig_net_is_coop_enabled() || !tig_net_is_active() || obj == OBJ_HANDLE_NULL) {
        return false;
    }
#ifndef COOP_LITE_TEST_STUBS
    tig_debug_printf("CoopLite: PT54c teleport: inventory NUM/list mismatch repaired\n");
    coop_lite_inv_list_repair(obj);
    return true;
#else
    return false;
#endif
}

/* Client, on COOP_PARTY_RESYNC: the host reloaded — re-offer our PC. */
static void coop_lite_client_resync(void)
{
    int64_t pc = player_get_local_pc_obj();
    char name[64];
    int proto;

    if (pc == OBJ_HANDLE_NULL) {
        /* PT77f: used to just return, leaving the client in limbo with no
           PC and no way back. Ask the host for our kept PC instead (it
           sends us to character creation when it holds none). */
        tig_debug_printf("CoopLite: resync - no local PC, sending a rejoin-only offer\n");
        coop_lite_session_reset();
        coop_lite_show_notice("Лидер группы загрузился. Переподключение... / The party leader reloaded. Rejoining...", TF_TYPE_WHITE);
        coop_lite_client_send_offer(BP_GENERIC_PC, "Companion", true);
        return;
    }
    coop_lite_session_reset();
#ifndef COOP_LITE_TEST_STUBS
    {
        /* PT61: what a real load's map_reset does first (magictech, anims).
           The rejoin skipped it: every host reload left all spell locks
           behind and re-sent maintained spells added more, until
           "magictech_id_new_lock: ran out of id's" quit the client. */
        int locks;
        int freed;

        multiplayer_lock();
        tig_net_local_suspend_begin();
        anim_goal_interrupt_all_goals();
        locks = magictech_coop_locks_in_use();
        freed = magictech_coop_free_stale(true);
        tig_net_local_suspend_end();
        multiplayer_unlock();
        tig_debug_printf("CoopLite: PT61 resync reset: anims interrupted, magictech %d lock(s) freed (%d in use)\n",
            freed, locks);
    }
#endif
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
    coop_lite_show_notice("Лидер группы загрузился. Переподключение... / The party leader reloaded. Rejoining...", TF_TYPE_WHITE);
    coop_lite_client_offer_character(proto, name);
}

void coop_lite_handle_party_state(PacketCoopPartyState* pkt)
{
    char str[512];

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
#ifndef COOP_LITE_TEST_STUBS
    if (pkt->kind == COOP_PARTY_REST_END) {
        coop_lite_client_rest_fade(false);
        return;
    }
    if (pkt->kind == COOP_PARTY_WORLD_DIRTY) {
        coop_lite_client_world_refresh_soon();
        return;
    }
    if (pkt->kind == COOP_PARTY_WMAP_CLOSE) {
        coop_lite_client_wmap_dim(false);
        return;
    }
    if (pkt->kind == COOP_PARTY_WMAP_OPEN) {
        /* the notice once, then only the overlay is kept alive */
        bool first = !coop_client_wmap_dimmed;

        coop_lite_client_wmap_dim(true);
        if (!first) {
            return;
        }
    }
#endif
    coop_lite_party_notice_text(pkt->kind, pkt->countdown_ms, false, str, sizeof(str));
    if (str[0] != '\0') {
        coop_lite_show_notice(str, coop_lite_party_notice_is_good(pkt->kind) ? TF_TYPE_GREEN : TF_TYPE_RED);
    }
#ifndef COOP_LITE_TEST_STUBS
    if (pkt->kind == COOP_PARTY_REST_BEGIN) {
        coop_lite_client_rest_fade(true);
    }
#endif
}

static bool coop_remote_fallen[8];

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
            coop_remote_fallen[slot] = false;
            continue;
        }
        dmg = object_hp_damage_get(pc);
        if (coop_slot_hp_valid[slot] && dmg > coop_slot_hp_damage[slot]) {
            coop_lite_float_damage(pc, coop_slot_hp_damage[slot], dmg);
        }
        coop_slot_hp_damage[slot] = dmg;
        coop_slot_hp_valid[slot] = true;

        /* PT18 GUI: a peer's PC (the host's, or another client's) that dies
           here only gets its hit points; poses of dead PCs are not
           interpolated, so it stayed standing. Lay it down / stand it up
           locally, like our own. */
        if (pc != local_pc && critter_is_dead(pc) != coop_remote_fallen[slot]) {
            coop_remote_fallen[slot] = critter_is_dead(pc);
            multiplayer_lock();
            tig_net_local_suspend_begin();
            if (coop_remote_fallen[slot]) {
                sub_424070(pc, PRIORITY_HIGHEST, false, true);
            }
            coop_lite_set_corpse_pose(pc, coop_remote_fallen[slot]);
            tig_net_local_suspend_end();
            multiplayer_unlock();
            tig_debug_printf("CoopLite: slot %d PC (peer) %s here\n", slot,
                coop_remote_fallen[slot] ? "fell — corpse pose" : "stood up");
        }
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
        coop_lite_show_notice("Вы пали. Спутник может вас воскресить. / You have fallen. A companion can resurrect you.", TF_TYPE_RED);
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
        coop_lite_show_notice("Вас воскресили. / You have been resurrected.", TF_TYPE_GREEN);
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
        static unsigned int log_n; /* PT55 */
        if (tig_net_log_budget(&log_n)) {
            tig_debug_printf("CoopLite: CHAR_OFFER payload slot=%d != sender %d (using sender)\n", pkt->slot, slot);
        }
    }
    if (!coop_lite_pc_proto_plausible(pkt->basic_prototype)) {
        static unsigned int log_n; /* PT55 */
        if (tig_net_log_budget(&log_n)) {
            tig_debug_printf("CoopLite: CHAR_OFFER rejected - bad proto %d (%u so far)\n", pkt->basic_prototype, log_n);
        }
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
    coop_pending_offers[slot].has_oid = coop_lite_offer_flag_has_oid(pkt->has_oid);
    coop_pending_offers[slot].rejoin_only = coop_lite_offer_flag_rejoin_only(pkt->has_oid);
    memcpy(coop_pending_offers[slot].oid, pkt->pc_oid, sizeof(coop_pending_offers[slot].oid));
    coop_pending_offers[slot].token_hash = coop_lite_token_hash(pkt->token);
    tig_debug_printf("CoopLite: CHAR_OFFER from slot=%d proto=%d name=%s has_oid=%d\n",
        slot, pkt->basic_prototype, coop_pending_offers[slot].name, pkt->has_oid);

#ifndef COOP_LITE_TEST_STUBS
    /* PT77n: the same player (same token) came back on a new slot while its
       old connection had not timed out yet (log: slot 2 joined, slot 1 left
       4 s later and parked the PC slot 2 now used -> "parked PC is gone",
       character creation). Drop the stale slot first so its PC is parked
       and this offer takes it back. */
    {
        int other;

        for (other = 1; other < 8; other++) {
            if (other != slot && coop_pending_offers[slot].token_hash != 0
                && coop_slot_token_hash[other] == coop_pending_offers[slot].token_hash) {
                tig_debug_printf("CoopLite: PT77n slot %d is a stale connection of the player on slot %d - dropping it\n",
                    other, slot);
                if (!tig_net_kick_slot(other)) {
                    coop_lite_host_on_client_disconnect(other);
                    coop_lite_host_remove_left_pc(other, sub_4A2B60(other));
                }
            }
        }
    }
    if (coop_lite_client_in_world()) {
        coop_lite_host_spawn_client(slot);
    } else {
        tig_debug_printf("CoopLite: offer queued until host enters world\n");
    }
#endif
}

bool coop_lite_test_pending_offer_rejoin_only(int slot)
{
    return slot > 0 && slot < 8 && coop_pending_offers[slot].valid && coop_pending_offers[slot].rejoin_only;
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
    /* PT37: the host's map is the truth; when we already stand on it (the
       host reloaded a save) reload it from disk too, or killed / moved NPCs
       of the old session stay as ghosts. */
    teleport_data.flags = TELEPORT_RENDER_LOCK | TELEPORT_FORCE_RELOAD;
    teleport_data.obj = player_get_local_pc_obj();
    /* PT77ae (review 2a): as a forced follow (PT52f / PT73) - close the
       world-map spectate and inventory / loot / barter / dialog / char view
       before the map reload; they hold handles of the old map. */
#ifndef COOP_LITE_TEST_STUBS
    if (wmap_ui_is_spectating()) {
        wmap_ui_spectate_close();
    }
#endif
    if (teleport_data.obj != OBJ_HANDLE_NULL && coop_travel_close_hook != NULL) {
        coop_travel_close_hook();
    }
    teleport_data.loc = coop_pending_sync.client_loc;
    teleport_data.map = coop_pending_sync.map;
    coop_lite_client_clear_stuck_flags(teleport_data.obj, "sync");
    coop_lite_applying_follow = true;
    ok = coop_lite_auth_teleport(&teleport_data);
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
    if ((pkt->flags & COOP_SYNC_F_NO_PC) != 0) {
        /* PT69: our rejoin found no PC on the host: create one (New Game). */
        if (coop_lite_waiting_sync) {
            coop_lite_waiting_sync = false;
            coop_lite_rejoin_no_pc = true;
            tig_debug_printf("CoopLite: host keeps no PC for us - character creation needed\n");
        }
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
    {
        char* map_name = NULL;
        char path[TIG_MAX_PATH];

        if (map_get_name(pkt->map, &map_name) && map_name != NULL) {
            snprintf(path, sizeof(path), "Save\\Current\\maps\\%s", map_name);
            if (map_is_valid() && map_current_map() == pkt->map) {
                /* PT63: never empty the folder of the OPEN map. map_flush in
                   map_open_in_game rewrote it, while the sector cache read
                   from it - the 07239681 client crash (dead inventory
                   handles, then obj_find_remove on NULL in map_open). Wipe it
                   between map_close and the reload instead. */
                snprintf(coop_map_wipe_path, sizeof(coop_map_wipe_path), "%s", path);
                coop_carry_mdy_path[0] = '\0';
                coop_carry_mdy_started = false;
                tig_debug_printf("CoopLite: PT63 stale state of the open map %s - wiped after it closes\n", path);
            } else if (tig_file_is_directory(path)) {
                tig_debug_printf("CoopLite: clearing stale client map state %s\n", path);
                tig_file_empty_directory(path);
            }
        }
    }
#endif

    coop_lite_client_request_sync_teleport();
    /* Binding happens in coop_lite_client_on_teleport_done (teleport.c). */
}


void coop_lite_map_open_after_close(const char* save_path)
{
#ifdef COOP_LITE_TEST_STUBS
    (void)save_path;
    coop_map_wipe_path[0] = '\0';
    coop_carry_mdy_path[0] = '\0';
    coop_carry_mdy_started = false;
#else
    if (save_path == NULL || coop_map_wipe_path[0] == '\0') {
        return;
    }
    if (SDL_strcasecmp(save_path, coop_map_wipe_path) == 0) {
        tig_debug_printf("CoopLite: PT63 clearing stale client map state %s (map closed)\n", save_path);
        tig_file_empty_directory(save_path);
        /* PT77aa: teleport already wrote the carried local PC (and anything
           else on the teleport list) to a side file. This wipe would have
           deleted dest\mobile.mdy, and finish_sync then found no local PC
           (client log: "PT72 finish sync: no local PC", then vanish / menu). */
        if (coop_carry_mdy_started && coop_carry_mdy_path[0] != '\0') {
            char dest[TIG_MAX_PATH];
            TigFileInfo info;

            snprintf(dest, sizeof(dest), "%s\\mobile.mdy", save_path);
            memset(&info, 0, sizeof(info));
            if (tig_file_exists(coop_carry_mdy_path, &info)) {
                tig_file_remove(dest);
                if (tig_file_rename(coop_carry_mdy_path, dest) == 0
                    || tig_file_copy(coop_carry_mdy_path, dest)) {
                    tig_file_remove(coop_carry_mdy_path);
                    tig_debug_printf("CoopLite: PT77aa restored carried mobile.mdy after the map wipe\n");
                } else {
                    tig_debug_printf("CoopLite: PT77aa failed to restore carried mobile.mdy after the map wipe\n");
                }
            }
        }
    }
    coop_map_wipe_path[0] = '\0';
    coop_carry_mdy_path[0] = '\0';
    coop_carry_mdy_started = false;
#endif
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

#ifndef COOP_LITE_TEST_STUBS
static void coop_lite_diag_tick(unsigned int now_ms)
{
    int slot;
    int kept = 0;
    int remote_far = -1;
    int64_t local_pc = player_get_local_pc_obj();

    if (coop_diag.last_ping_ms != 0 && now_ms - coop_diag.last_ping_ms > coop_diag.max_gap_ms) {
        coop_diag.max_gap_ms = now_ms - coop_diag.last_ping_ms;
    }
    coop_diag.last_ping_ms = now_ms;
    coop_diag.pings++;
    if (coop_diag.window_ms == 0) {
        coop_diag.window_ms = now_ms != 0 ? now_ms : 1;
        return;
    }
    if (!coop_lite_interval_elapsed(coop_diag.window_ms, now_ms, 30000u)) {
        return;
    }
    for (slot = 0; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);

        if (coop_sector_keep[slot].valid) {
            kept += coop_sector_keep[slot].count;
        }
        if (pc != OBJ_HANDLE_NULL && local_pc != OBJ_HANDLE_NULL && pc != local_pc) {
            int d = (int)object_dist(local_pc, pc);

            if (d > remote_far) {
                remote_far = d;
            }
        }
    }
    {
        char te_types[160];

        timeevent_debug_types(te_types, sizeof(te_types));
        tig_debug_printf("CoopDiag: timeevent types %s\n", te_types);
    }
    tig_debug_printf("CoopDiag: %s map=%d pings=%u max_gap=%ums anim_runs=%d timeevents=%d deferred70=%d "
                     "kept_sectors=%d far_pc=%d npc_tx=%u npc_rx=%u npc_unknown=%u npc_req=%u npc_on=%u npc_dead=%u pc_rx=%u pc_tx=%u "
                     "pc_drop=%u/%u/%u carry=%d "
                     "force_tx=%u own_corr=%u atk=%u approach=%u swing=%u/%u anim=%u/%u npc_snd=%u interp=%d/%d p10=%u/%u/%u peer_anim=%u interp_lag=%u\n",
        tig_net_is_host() ? "host" : "client", map_current_map(), coop_diag.pings, coop_diag.max_gap_ms,
        anim_debug_active_runs(), timeevent_debug_count(), mp_deferred_packet70_pending(),
        kept, remote_far, coop_diag.npc_tx, coop_diag.npc_rx, coop_diag.npc_rx_unknown, coop_diag.npc_req, coop_diag.npc_rx_on, coop_diag.npc_rx_dead,
        coop_diag.pc_rx, coop_diag.pc_tx, coop_diag.pc_drop_map, coop_diag.pc_drop_order, coop_diag.pc_drop_bad,
        coop_follow_carry_pending ? 1 : 0, coop_diag.force_tx, coop_diag.own_corrections, coop_diag.attack_intents,
        coop_diag.attack_approaches, coop_diag.swing_tx, coop_diag.swing_rx, coop_diag.swing_anim, coop_diag.swing_anim_fail, coop_diag.npc_sound_tx, coop_interp[0].n, coop_interp[1].n,
        coop_diag.p10_own, coop_diag.p10_peer, coop_diag.p10_npc, coop_diag.peer_anim, coop_diag.interp_lag);
    memset(&coop_diag, 0, sizeof(coop_diag));
    coop_diag.window_ms = now_ms != 0 ? now_ms : 1;
    coop_diag.last_ping_ms = now_ms;
}
#endif

/* PT25 item 2: auto-attack and always-run default to on in a co-op
   session (once per run - the player may still turn them off). */
static void coop_lite_force_auto_settings(void)
{
#ifndef COOP_LITE_TEST_STUBS
    static bool done;

    if (done || player_get_local_pc_obj() == OBJ_HANDLE_NULL) {
        return;
    }
    done = true;
    tig_debug_printf("CoopLite: auto attack %d -> 1, always run %d -> 1 (co-op default)\n",
        settings_get_value(&settings, AUTO_ATTACK_KEY), settings_get_value(&settings, ALWAYS_RUN_KEY));
    combat_auto_attack_set(true);
    set_always_run(true);
#endif
}

/* ---- PT47: auto-follow / guard (F toggles; any left click cancels) ------ */

#ifndef COOP_LITE_TEST_STUBS
static ObjectID coop_follow_oid;
static bool coop_follow_on;
static unsigned int coop_follow_ms;

void coop_lite_follow_cancel(void)
{
    if (coop_follow_on) {
        coop_follow_on = false;
        tig_debug_printf("CoopLite: follow: off\n");
    }
}

bool coop_lite_follow_toggle(void)
{
    int64_t pc = player_get_local_pc_obj();
    int64_t best = OBJ_HANDLE_NULL;
    int64_t best_d = 0;
    ObjectList objects;
    ObjectNode* node;

    if (coop_follow_on) {
        coop_lite_follow_cancel();
        return true;
    }
    if (pc == OBJ_HANDLE_NULL || !tig_net_is_active() || !tig_net_is_coop_enabled()) { /* PT52d */
        return false;
    }
    object_list_vicinity(pc, OBJ_TM_PC, &objects);
    for (node = objects.head; node != NULL; node = node->next) {
        int64_t d;

        if (node->obj == pc || coop_lite_slot_of_obj(node->obj) < 0 || critter_is_dead(node->obj)) {
            continue;
        }
        d = location_dist(obj_field_int64_get(node->obj, OBJ_F_LOCATION), obj_field_int64_get(pc, OBJ_F_LOCATION));
        if (best == OBJ_HANDLE_NULL || d < best_d) {
            best = node->obj;
            best_d = d;
        }
    }
    object_list_destroy(&objects);
    if (best != OBJ_HANDLE_NULL) {
        coop_follow_oid = obj_get_id(best);
        coop_follow_on = true;
        coop_follow_ms = 0;
        tig_debug_printf("CoopLite: follow: on (slot %d's PC, %d tiles)\n", coop_lite_slot_of_obj(best), (int)best_d);
        return true;
    }
    return false; /* PT52e: nobody to follow, key not consumed */
}

static void coop_lite_follow_tick(unsigned int now_ms)
{
    int64_t pc;
    int64_t target;
    int64_t d;

    if (!coop_follow_on || !coop_lite_interval_elapsed(coop_follow_ms, now_ms, 700u)) {
        return;
    }
    coop_follow_ms = now_ms;
    pc = player_get_local_pc_obj();
    target = obj_pool_perm_lookup(coop_follow_oid);
    if (pc == OBJ_HANDLE_NULL || target == OBJ_HANDLE_NULL || critter_is_dead(pc) || critter_is_dead(target)
        || coop_lite_slot_of_obj(target) < 0 || map_is_clearing_objects()) {
        coop_lite_follow_cancel();
        return;
    }
    if (anim_is_current_goal_type(pc, AG_ATTACK, NULL)) {
        return;
    }
    d = location_dist(obj_field_int64_get(target, OBJ_F_LOCATION), obj_field_int64_get(pc, OBJ_F_LOCATION));
    if (d > 25) {
        coop_lite_follow_cancel(); /* lost (other map / too far) */
        return;
    }
    if (d <= 4) {
        /* Guard: an NPC whose combat focus is the target, close by. */
        ObjectList objects;
        ObjectNode* node;
        int64_t foe = OBJ_HANDLE_NULL;

        object_list_vicinity(target, OBJ_TM_NPC, &objects);
        for (node = objects.head; node != NULL; node = node->next) {
            if (!critter_is_dead(node->obj)
                && obj_field_handle_get(node->obj, OBJ_F_NPC_COMBAT_FOCUS) == target
                && location_dist(obj_field_int64_get(node->obj, OBJ_F_LOCATION),
                       obj_field_int64_get(target, OBJ_F_LOCATION)) <= 6) {
                foe = node->obj;
                break;
            }
        }
        object_list_destroy(&objects);
        if (foe != OBJ_HANDLE_NULL) {
            /* PT52: the normal attack goal; on a client it goes through
               coop_lite_client_attack_approach (walk in, then intent). */
            anim_goal_attack(pc, foe);
        }
        return;
    }
    anim_goal_move_to_tile(pc, obj_field_int64_get(target, OBJ_F_LOCATION));
}
#else
void coop_lite_follow_cancel(void) { }
bool coop_lite_follow_toggle(void) { return false; }
#endif

static void coop_travel_tick(unsigned int now_ms);
#ifndef COOP_LITE_TEST_STUBS
static void coop_world_full_tick(unsigned int now_ms);
#endif

/* PT63: O(1) gate for the per-object hooks in object.c (flags / HP /
   destroy). Clients and solo games skip the type reads and the net checks
   that every object_flags_set call paid since PT60b. */
bool coop_lite_world_hooks_on;

void coop_lite_world_hooks_refresh(void)
{
    int depth = tig_net_local_suspend_pause();

#ifdef COOP_LITE_TEST_STUBS
    coop_lite_world_hooks_on = tig_net_is_active() && tig_net_is_host() && tig_net_is_coop_enabled();
#else
    coop_lite_world_hooks_on = tig_net_is_active() && tig_net_is_host() && tig_net_is_coop_enabled()
        && !gamelib_in_load() && !map_is_clearing_objects();
#endif
    tig_net_local_suspend_resume(depth);
}

void coop_lite_ping(unsigned int now_ms)
{
    coop_obj_now_ms = now_ms;
    coop_lite_world_hooks_refresh();
    coop_travel_tick(now_ms);
#ifndef COOP_LITE_TEST_STUBS
    int slot;
    int64_t obj;
    PacketCoopPcPos pkt;

    coop_lite_follow_tick(now_ms);
    coop_world_full_tick(now_ms); /* PT51 */

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
    coop_lite_lobby_tick(now_ms); /* PT77: launcher lobby IPC */
    coop_lite_diag_tick(now_ms);

    if (!tig_net_is_host()) {
        coop_lite_obj_ground_fallback_tick(now_ms);
        coop_lite_client_interp_tick(now_ms);
        if (coop_lite_env("ARCANUM_DEV_SWING_TRACE") != NULL && coop_client_swing_ms != 0
            && !coop_lite_interval_elapsed(coop_client_swing_ms, now_ms, 1500u)) {
            int64_t me = player_get_local_pc_obj();
            static unsigned int last_aid;

            if (me != OBJ_HANDLE_NULL && (unsigned int)obj_field_int32_get(me, OBJ_F_CURRENT_AID) != last_aid) {
                tig_art_id_t a = obj_field_int32_get(me, OBJ_F_CURRENT_AID);

                last_aid = (unsigned int)a;
                tig_debug_printf("SWTRACE +%u anim %d frame %d animate %d attack %d\n",
                    now_ms - coop_client_swing_ms, tig_art_id_anim_get(a), tig_art_id_frame_get(a),
                    anim_is_current_goal_type(me, AG_ANIMATE, NULL), anim_is_current_goal_type(me, AG_ATTACK, NULL));
            }
        }
        /* PT72: map is loaded but our PC was not bound yet - retry the
           finish step only (no new teleport). */
        if (coop_pending_sync_valid && coop_sync_nopc_wait
            && coop_lite_interval_elapsed(coop_pending_sync_retry_ms, now_ms, 250u)) {
            coop_pending_sync_retry_ms = now_ms;
            coop_sync_nopc_wait = false;
            coop_lite_client_on_teleport_done();
        }
        /* Client: retry a SESSION_SYNC teleport that could not be queued. */
        if (coop_pending_sync_valid && !coop_pending_sync_teleport_queued
            && coop_lite_interval_elapsed(coop_pending_sync_retry_ms, now_ms, 250u)) {
            coop_pending_sync_retry_ms = now_ms;
            coop_lite_client_request_sync_teleport();
        }

        if (!coop_lite_client_world_ready()) {
            /* Map load / teleport: queued pickups are for the old place. */
            coop_lite_client_pickup_reset();
            return;
        }

        /* Client camera: every frame (not the 50 ms pose tick) so the
           scroll follows the PC smoothly. */
        coop_lite_client_follow_own_pc(coop_lite_camera_target());
        coop_lite_client_watch_vitals();
        coop_lite_client_wmap_tick(now_ms);
        coop_lite_force_auto_settings();
        coop_lite_keep_remote_sectors();
        if (coop_lite_client_in_session) {
            coop_lite_client_pc_stats_tick(now_ms);
            coop_lite_client_story_tick(now_ms);
            coop_lite_area_tick(now_ms); /* PT78 */
        }
        if (coop_lite_client_in_session
            && coop_lite_interval_elapsed(coop_roster_reconcile_ms, now_ms, 1000u)) {
            coop_roster_reconcile_ms = now_ms;
            coop_lite_client_roster_reconcile();
        }
        if (coop_lite_client_in_session) {
            coop_lite_dev_grant_resurrect(sub_529520(), player_get_local_pc_obj());
            coop_lite_dev_grant_pickpocket(sub_529520(), player_get_local_pc_obj());
            coop_lite_dev_trigger_tick(now_ms);
            coop_lite_client_dev_barter_tick(now_ms);
            coop_lite_dev_export_tick(now_ms);
            coop_lite_client_dev_combat_tick(now_ms);
            coop_lite_client_dev_items_tick(now_ms);
            coop_lite_client_dev_gold_pile_tick(now_ms);
            coop_lite_dev_inven_dump_tick(now_ms);
            coop_lite_client_dev_loot_tick(now_ms);
            coop_lite_client_world_req_tick(now_ms);
            coop_lite_client_pickup_tick(now_ms);
            coop_lite_client_layout_tick(now_ms); /* PT53 */
            coop_lite_client_door_tick();
            coop_lite_client_attack_tick(now_ms);
            coop_lite_client_rejoin_tick(now_ms); /* PT54 */
            coop_lite_client_drag_tick(); /* PT54 */
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
                    coop_diag.pc_tx++;
                }
            }
        }

        coop_lite_client_hotbar_tick(now_ms); /* PT63 */
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
    /* PT31: a game loaded from the menu has no spawn teleport to arm follow,
       so the host's first real travel was swallowed as the "initial spawn"
       and the client never followed (PT31 log: map key dropped for minutes).
       Settled in the world for 1.5 s = it is not a spawn any more. */
    if (!coop_lite_follow_armed) {
        if (coop_in_world_ms == 0) {
            coop_in_world_ms = now_ms ? now_ms : 1u;
        } else if (coop_lite_interval_elapsed(coop_in_world_ms, now_ms, 1500u)) {
            coop_lite_follow_armed = true;
            coop_in_world_ms = 0;
            tig_debug_printf("CoopLite: follow armed (host settled in the world)\n");
        }
    } else {
        coop_in_world_ms = 0; /* armed some other way: do not leave a stale start */
    }
    coop_lite_client_follow_own_pc(coop_lite_camera_target());
    coop_lite_keep_remote_sectors();
    coop_lite_dev_tick(now_ms);
    coop_lite_host_dev_far_tick(now_ms);
    coop_lite_dev_export_tick(now_ms);
    coop_lite_host_roster_tick(now_ms);
    coop_lite_host_party_inven_tick(now_ms);
    coop_lite_host_dev_give_weapon_tick(now_ms);
    coop_lite_host_dev_world_mutate_tick(now_ms);
    coop_lite_host_dev_kill_client_tick(now_ms);
    coop_lite_host_intent_tick(now_ms);
#ifndef COOP_LITE_TEST_STUBS
    coop_lite_host_door_flush_tick(now_ms); /* PT53e */
    coop_lite_host_hotbar_tick(); /* PT63 */
#endif
    coop_lite_host_dev_auto_resurrect_tick(now_ms);
    coop_lite_host_dev_gold_pile_tick(now_ms);
    coop_lite_dev_inven_dump_tick(now_ms);
    coop_lite_host_barter_watch(now_ms);
    coop_lite_host_victim_watch(now_ms);
    coop_lite_host_pc_stats_tick(now_ms);
    coop_lite_host_critter_effects_tick(now_ms);
    coop_lite_host_story_tick(now_ms);
    coop_lite_area_tick(now_ms); /* PT78 */
    coop_lite_host_dialog_tick(now_ms);
    coop_lite_party_ui_tick(now_ms);

    {
        Uint64 t0 = SDL_GetTicksNS();
        coop_lite_host_send_followers(now_ms);
        coop_lite_host_send_npc_pos(now_ms);
        coop_lite_host_watch_party(now_ms);
        coop_lite_host_vitals_tick(now_ms);
        coop_lite_host_wmap_tick(now_ms);
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
            {
                /* PT31: the owner never gets this echo, so a host-side
                   paralysis/stun/stone on its PC (or its end) never reached
                   it. Send the state to the owner on change, and every 2 s
                   while a blocking flag is set (reliable, not FORCE: the
                   client takes only the look + these flags from it). */
                unsigned int st[3];

                st[0] = pkt.critter_flags & COOP_OWN_HOST_OCF;
                st[1] = pkt.spell_flags;
                st[2] = pkt.vis_flags;
                if (!coop_owner_state_valid[slot]
                    || memcmp(st, coop_owner_state[slot], sizeof(st)) != 0
                    || (st[0] != 0 && coop_lite_interval_elapsed(coop_owner_state_ms[slot], now_ms, 2000u))) {
                    memcpy(coop_owner_state[slot], st, sizeof(st));
                    coop_owner_state_valid[slot] = true;
                    coop_owner_state_ms[slot] = now_ms;
                    tig_net_send_app(&pkt, sizeof(pkt), slot);
                }
            }
        } else if (slot > 0) {
            /* Host moved an owner-driven PC (knockback, follow teleport,
               refused jump …) — the owner must apply this one: reliable. */
            pkt.flags |= COOP_PC_POS_FORCE;
            coop_diag.force_tx++;
            if (!coop_pose_force_active[slot]) {
                /* PT25 diagnostics: why the owner gets corrected. */
                tig_debug_printf("CoopLite: FORCE slot %d: host %d,%d off %d,%d vs owner %d,%d off %d,%d goal %d%s%s\n", slot,
                    (int)LOCATION_GET_X(pkt.loc), (int)LOCATION_GET_Y(pkt.loc), pkt.offset_x, pkt.offset_y,
                    coop_client_applied_valid[slot] ? (int)LOCATION_GET_X(coop_client_applied[slot].loc) : -1,
                    coop_client_applied_valid[slot] ? (int)LOCATION_GET_Y(coop_client_applied[slot].loc) : -1,
                    coop_client_applied_valid[slot] ? coop_client_applied[slot].offset_x : 0,
                    coop_client_applied_valid[slot] ? coop_client_applied[slot].offset_y : 0,
                    anim_primary_goal_type(obj), /* PT77af: what moves the host copy */
                    anim_is_current_goal_type(obj, AG_ATTACK, NULL) ? " [AG_ATTACK]" : "",
                    coop_client_applied_valid[slot] ? "" : " [no owner pose]");
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

#ifndef COOP_LITE_TEST_STUBS
/* PT31: self-heal for a missed FOLLOW_HOST. The host's own PC keeps
   announcing its map key; when it stays different from ours for 3 s (no
   travel of ours in flight), follow it from the key (map index + load
   generation) and its location. Retried every 5 s while it persists. */
static void coop_lite_client_map_mismatch(const PacketCoopPcPos* pkt)
{
    PacketCoopFollowHost fol;
    unsigned int now = coop_obj_now_ms;

    if (tig_net_is_host() || pkt->slot != 0 || !coop_lite_client_in_world()
        || coop_follow_carry_pending || coop_pending_sync_valid || coop_lite_pending_follow_valid) {
        coop_mismatch_since_ms = 0;
        return;
    }
    if (coop_mismatch_since_ms == 0 || coop_mismatch_key != (unsigned int)pkt->map) {
        coop_mismatch_key = (unsigned int)pkt->map;
        coop_mismatch_since_ms = now ? now : 1u;
        return;
    }
    if (!coop_lite_interval_elapsed(coop_mismatch_since_ms, now, 3000u)
        || (coop_mismatch_follow_ms != 0 && !coop_lite_interval_elapsed(coop_mismatch_follow_ms, now, 5000u))) {
        return;
    }
    coop_mismatch_follow_ms = now ? now : 1u;
    memset(&fol, 0, sizeof(fol));
    fol.type = PACKET_COOP_FOLLOW_HOST;
    fol.map = (int)((unsigned int)pkt->map & 0xFFFFu);
    fol.map_gen = (unsigned int)pkt->map >> 16;
    fol.loc = pkt->loc;
    tig_debug_printf("CoopLite: host is on map key %08x for 3 s, ours differs - following it\n",
        (unsigned int)pkt->map);
    coop_lite_handle_follow_packet(&fol);
}
#endif

#ifndef COOP_LITE_TEST_STUBS
/* PT42: a client PC stepped from `from` to `to` (the pose stream may skip
   tiles): the host springs the first armed floor trap on that line, as
   anim's walk does for a local mover. */
/* ---- PT43: party portraits ---- */
static void (*coop_party_ui_refresh_hook)(void);
static void (*coop_party_ui_update_hook)(void);
static unsigned int coop_party_ui_ms;
static int64_t coop_party_ui_have[8];
static int coop_party_ui_num;
static unsigned int coop_party_ui_sig;

void coop_lite_set_party_ui_hooks(void (*refresh)(void), void (*update)(void))
{
    coop_party_ui_refresh_hook = refresh;
    coop_party_ui_update_hook = update;
}

int coop_lite_party_teammates(int64_t* out, int max)
{
    int64_t local_pc;
    int slot;
    int num = 0;
    int depth;

    if (out == NULL || max <= 0 || !tig_net_is_coop_enabled() || !map_is_valid()) {
        return 0;
    }
    if (max > 8) {
        max = 8;
    }
    local_pc = player_get_local_pc_obj();
    depth = tig_net_local_suspend_pause(); /* slot table reads need the live session */
    for (slot = 0; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);

        if (pc == OBJ_HANDLE_NULL || !obj_handle_is_valid(pc)
            || obj_field_int32_get(pc, OBJ_F_TYPE) != OBJ_TYPE_PC
            || (obj_field_int32_get(pc, OBJ_F_FLAGS) & (OF_DESTROYED | OF_OFF | OF_INVENTORY)) != 0) {
            continue;
        }
        if (coop_lite_teammate_accept(pc, local_pc, out, num, max)) {
            out[num++] = pc;
        }
    }
    tig_net_local_suspend_resume(depth);
    return num;
}

bool coop_lite_is_teammate_pc(int64_t obj)
{
    int64_t list[8];
    int n = coop_lite_party_teammates(list, 8);
    int i;

    for (i = 0; i < n; i++) {
        if (list[i] == obj) {
            return true;
        }
    }
    return false;
}

/* Join / leave / death: rebuild the column; HP / fatigue: redraw. */
static void coop_lite_party_ui_tick(unsigned int now_ms)
{
    int64_t list[8];
    unsigned int sig = 2166136261u;
    bool changed;
    int n;
    int i;

    if (!coop_lite_interval_elapsed(coop_party_ui_ms, now_ms, 300u)) {
        return;
    }
    coop_party_ui_ms = now_ms;
    n = coop_lite_party_teammates(list, 8);
    changed = n != coop_party_ui_num || memcmp(list, coop_party_ui_have, sizeof(list[0]) * (size_t)n) != 0;
    for (i = 0; i < n; i++) {
        sig = (sig ^ (unsigned int)object_hp_current(list[i])) * 16777619u;
        sig = (sig ^ (unsigned int)critter_fatigue_current(list[i])) * 16777619u;
        sig = (sig ^ (critter_is_dead(list[i]) ? 1u : 0u)) * 16777619u;
    }
    if (changed) {
        memcpy(coop_party_ui_have, list, sizeof(list[0]) * (size_t)n);
        coop_party_ui_num = n;
        coop_party_ui_sig = sig;
        tig_debug_printf("CoopLite: party ui: %d teammate portrait(s)\n", n);
        if (coop_party_ui_refresh_hook != NULL) {
            coop_party_ui_refresh_hook();
        }
    } else if (sig != coop_party_ui_sig) {
        coop_party_ui_sig = sig;
        if (coop_party_ui_update_hook != NULL) {
            coop_party_ui_update_hook();
        }
    }
}

static void coop_lite_host_pc_trap_walk(int slot, int64_t pc, int64_t from, int64_t to)
{
    int64_t loc = from;
    int steps;
    unsigned int spell_flags;

    if (from == to || pc == OBJ_HANDLE_NULL || critter_is_dead(pc)) {
        return;
    }
    spell_flags = (unsigned int)obj_field_int32_get(pc, OBJ_F_SPELL_FLAGS);
    if ((spell_flags & (OSF_FLOATING | OSF_BODY_OF_AIR)) != 0) {
        return;
    }
    for (steps = 0; steps < COOP_TRAP_WALK_MAX && loc != to; steps++) {
        ObjectList traps;
        ObjectNode* node;
        int64_t next;

        if (!location_in_dir(loc, location_rot(loc, to), &next) || next == loc) {
            break;
        }
        loc = next;
        object_list_location(loc, OBJ_TM_TRAP, &traps);
        for (node = traps.head; node != NULL; node = node->next) {
            if (trap_type(node->obj) != TRAP_TYPE_INVALID) {
                break;
            }
        }
        if (node != NULL) {
            int64_t trap = node->obj;

            object_list_destroy(&traps);
            tig_debug_printf("CoopLite: trap: slot %d PC springs a floor trap at %d,%d\n", slot,
                (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc));
            object_script_execute(pc, trap, trap, SAP_USE, 0);
            coop_lite_host_world_dirty();
            return;
        }
        object_list_destroy(&traps);
    }
}
#endif

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
        if (coop_diag.pc_drop_map++ == 0) {
            tig_debug_printf("CoopLite: PC_POS for map key %08x dropped (ours %08x)\n",
                (unsigned int)pkt->map, (unsigned int)coop_lite_pose_map_key(map_current_map(), coop_map_gen));
        }
        coop_lite_client_map_mismatch(pkt);
        return;
    }
    if (pkt->slot == 0) {
        coop_mismatch_since_ms = 0; /* the host's map is ours again */
    }
    memcpy(&oid, pkt->oid, sizeof(oid));
    coop_diag.pc_rx++;
    if (tig_net_is_host()) {
        sender = sub_52A530();
        /* Review M3: ownership before anything (sector loads included). */
        if (sender <= 0 || sender >= 8 || !coop_slot_pc[sender].valid
            || !coop_lite_oid_raw_equal(&coop_slot_pc[sender].oid, &oid)) {
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
            if (coop_diag.pc_drop_order++ == 0) {
                tig_debug_printf("CoopLite: PC_POS slot %d epoch %u seq %u dropped (have epoch %u)\n",
                    sender, (unsigned int)pkt->epoch, (unsigned int)pkt->seq,
                    (unsigned int)coop_pose_rx[sender].epoch);
            }
            return;
        }
        if (!coop_lite_pc_pos_plausible(pkt->map, coop_lite_pose_map_key(map_current_map(), coop_map_gen),
                location_dist(obj_field_int64_get(obj, OBJ_F_LOCATION), pkt->loc),
                tig_art_type((tig_art_id_t)pkt->art_id),
                tig_art_type(obj_field_int32_get(obj, OBJ_F_CURRENT_AID)))) {
            /* Refused: the next host pose for this PC goes out FORCE so the
               owner snaps back to where the host has it. */
            coop_diag.pc_drop_bad++;
            tig_debug_printf("CoopLite: PC_POS from slot %d implausible (step %d) — corrected\n",
                sender, (int)location_dist(obj_field_int64_get(obj, OBJ_F_LOCATION), pkt->loc));
            coop_client_applied_valid[sender] = false;
            coop_last_pc_pos_valid[sender] = false;
            return;
        }
        {
            int64_t from_loc = obj_field_int64_get(obj, OBJ_F_LOCATION);

            coop_lite_apply_pc_pos(obj, pkt);
            coop_lite_host_pc_trap_walk(sender, obj, from_loc, pkt->loc);
        }
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
        /* Our own movement echoed back — we are authoritative for it. The
           look (spells cast on us) is the host's (PT28 item 5). */
        multiplayer_lock();
        coop_lite_client_apply_pc_visual(obj, pkt, true);
        multiplayer_unlock();
        return;
    }

    multiplayer_lock();
    coop_lite_client_apply_pc_visual(obj, pkt, obj == player_get_local_pc_obj());
    if (obj == player_get_local_pc_obj()) {
        /* Host correction of our PC: position only. Its art would be a
           stand frame, and a stand art ends our running move goal. */
        if (obj_field_int64_get(obj, OBJ_F_LOCATION) != pkt->loc
            || obj_field_int32_get(obj, OBJ_F_OFFSET_X) != pkt->offset_x
            || obj_field_int32_get(obj, OBJ_F_OFFSET_Y) != pkt->offset_y) {
            coop_diag.own_corrections++;
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

        /* PT28 item 4: a stand pose ends the move: snap to it now, no
           smooth tail behind the delay. */
        if (tig_art_id_anim_get((tig_art_id_t)pkt->art_id) == TIG_ART_ANIM_STAND) {
            ring->s[0] = smp;
            ring->n = 1;
            coop_lite_apply_pc_pos(obj, pkt);
        } else
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

static int coop_lite_popcount32(unsigned int v)
{
    int n = 0;

    while (v != 0) {
        v &= v - 1;
        n++;
    }
    return n;
}

int coop_lite_blob_walk(const uint8_t* p, int size, const uint8_t* kinds, const int* elem, int n)
{
    int at = 0;
    int i;

#define COOP_NEED(k) \
    do { \
        if ((k) < 0 || size - at < (k)) { \
            return -1; \
        } \
    } while (0)

    if (p == NULL || size < 0 || n < 0) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        uint8_t presence;

        if (kinds[i] == OBJ_MEM_K_I32) {
            COOP_NEED(4);
            at += 4;
            continue;
        }
        COOP_NEED(1);
        presence = p[at++];
        if (presence == 0) {
            continue;
        }
        switch (kinds[i]) {
        case OBJ_MEM_K_I64:
            COOP_NEED(8);
            at += 8;
            break;
        case OBJ_MEM_K_HANDLE:
            COOP_NEED(24);
            at += 24;
            break;
        case OBJ_MEM_K_STRING: {
            int len;

            COOP_NEED(4);
            memcpy(&len, p + at, 4);
            at += 4;
            if (len < 0 || len > COOP_BLOB_STR_MAX) {
                return -1;
            }
            COOP_NEED(len + 1);
            if (p[at + len] != 0) {
                return -1;
            }
            at += len + 1;
            break;
        }
        case OBJ_MEM_K_ARRAY: {
            int hdr[3]; /* element size, count, bitset id */
            int words;
            int bits = 0;
            int w;

            COOP_NEED(12);
            memcpy(hdr, p + at, 12);
            at += 12;
            if (hdr[0] != elem[i] || hdr[0] <= 0 || hdr[1] < 0
                || hdr[1] > COOP_BLOB_SA_MAX_BYTES / hdr[0]) {
                return -1;
            }
            COOP_NEED(hdr[0] * hdr[1]);
            at += hdr[0] * hdr[1];
            COOP_NEED(4);
            memcpy(&words, p + at, 4);
            at += 4;
            if (words < 0 || words > COOP_BLOB_BITSET_MAX) {
                return -1;
            }
            COOP_NEED(4 * words);
            for (w = 0; w < words; w++) {
                unsigned int v;

                memcpy(&v, p + at + 4 * w, 4);
                bits += coop_lite_popcount32(v);
            }
            at += 4 * words;
            /* Each set bit names a stored element. */
            if (bits > hdr[1]) {
                return -1;
            }
            break;
        }
        default:
            return -1;
        }
    }
#undef COOP_NEED
    return at;
}

unsigned int coop_lite_keyring_hash(const unsigned int* keys, int count)
{
    unsigned int h = 2166136261u;
    int i;

    if (count <= 0) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        h = (h ^ keys[i]) * 16777619u;
    }
    h = (h ^ (unsigned int)count) * 16777619u;
    return h != 0 ? h : 1;
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
        if (pkt->entries[i].kind < COOP_WORLD_PORTAL || pkt->entries[i].kind > COOP_WORLD_KIND_MAX) {
            return false;
        }
    }
    return true;
}

bool coop_lite_teammate_accept(int64_t obj, int64_t local_pc, const int64_t* have, int num, int max)
{
    int i;

    if (obj == OBJ_HANDLE_NULL || obj == local_pc || num >= max) {
        return false;
    }
    for (i = 0; i < num; i++) {
        if (have[i] == obj) {
            return false;
        }
    }
    return true;
}

unsigned int coop_lite_world_trap_flags_merge(unsigned int have, unsigned int host)
{
    if ((host & COOP_WORLD_OF_TRAP_SPOTTED) != 0) {
        have |= COOP_WORLD_OF_TRAP_SPOTTED;
        have &= ~COOP_WORLD_OF_DONTDRAW;
    }
    return have;
}

bool coop_lite_world_req_due(int last_key, int key, unsigned int ready_ms,
    unsigned int last_req_ms, unsigned int now_ms)
{
    if (key != last_key) {
        /* Let the new map's sectors settle first. PT77ad: 1500 -> 500 ms;
           entries for sectors not loaded yet are skipped by the apply and
           come again with the sector-change request. */
        return coop_lite_interval_elapsed(ready_ms, now_ms, 500u);
    }
    return coop_lite_interval_elapsed(last_req_ms, now_ms, 30000u);
}

bool coop_lite_world_req_sector_due(int64_t last_sector, int64_t sector,
    unsigned int last_req_ms, unsigned int now_ms)
{
    return sector != last_sector && coop_lite_interval_elapsed(last_req_ms, now_ms, 3000u);
}

bool coop_lite_rate_take(CoopRateBucket* b, unsigned int now_ms, int cost, int cap, int per_sec)
{
    unsigned int dt;
    long long milli;

    if (b == NULL || cost < 0 || cap <= 0 || per_sec <= 0) {
        return false;
    }
    if (!b->init) {
        b->init = true;
        b->last_ms = now_ms;
        b->milli = cap * 1000;
    }
    dt = now_ms - b->last_ms; /* wraps fine */
    if (dt > 3600000u) {
        dt = 3600000u;
    }
    b->last_ms = now_ms;
    milli = (long long)b->milli + (long long)dt * per_sec;
    if (milli > (long long)cap * 1000) {
        milli = (long long)cap * 1000;
    }
    if (milli < (long long)cost * 1000) {
        b->milli = (int)milli;
        return false;
    }
    b->milli = (int)(milli - (long long)cost * 1000);
    return true;
}

/* PT52k: kind 0 OBJ_REQ (cost = OIDs), 1 NPC_REQ, 2 door/skill/craft. */
static CoopRateBucket coop_host_rate[3][8];
static unsigned int coop_host_rate_drops[3][8];

static bool coop_lite_host_rate_ok(int kind, int slot, int cost)
{
    static const int caps[3] = { COOP_RATE_OBJ_CAP, COOP_RATE_NPC_CAP, COOP_RATE_ACT_CAP };
    static const int rates[3] = { COOP_RATE_OBJ_PER_SEC, COOP_RATE_NPC_PER_SEC, COOP_RATE_ACT_PER_SEC };

    if (kind < 0 || kind >= 3 || slot <= 0 || slot >= 8) {
        return false;
    }
    if (coop_lite_rate_take(&coop_host_rate[kind][slot], coop_obj_now_ms, cost, caps[kind], rates[kind])) {
        return true;
    }
    if ((coop_host_rate_drops[kind][slot]++ & 63u) == 0) {
        tig_debug_printf("CoopLite: PT52k slot %d request kind %d over rate, dropped (%u)\n",
            slot, kind, coop_host_rate_drops[kind][slot]);
    }
    return false;
}

static void coop_lite_host_rate_reset(int slot)
{
    int k;

    if (slot <= 0 || slot >= 8) {
        return;
    }
    for (k = 0; k < 3; k++) {
        memset(&coop_host_rate[k][slot], 0, sizeof(coop_host_rate[k][slot]));
        coop_host_rate_drops[k][slot] = 0;
    }
}

static unsigned int coop_oid_set_hash(const uint8_t oid[24])
{
    unsigned int h = 2166136261u;
    int i;

    for (i = 0; i < 24; i++) {
        h = (h ^ oid[i]) * 16777619u;
    }
    return h;
}

void coop_oid_set_reset(CoopOidSet* set)
{
    set->count = 0;
    set->overflow = false;
    memset(set->slot, 0, sizeof(set->slot));
}

/* PT52k: hash lookup (was a linear scan: O(n^2) per snapshot). */
static int coop_oid_set_find(const CoopOidSet* set, const uint8_t oid[24], bool* found)
{
    unsigned int i = coop_oid_set_hash(oid) & (COOP_OID_SET_HASH - 1);

    while (set->slot[i] != 0) {
        if (coop_lite_oid_raw_equal(set->oid[set->slot[i] - 1], oid)) {
            *found = true;
            return (int)i;
        }
        i = (i + 1) & (COOP_OID_SET_HASH - 1);
    }
    *found = false;
    return (int)i;
}

void coop_oid_set_add(CoopOidSet* set, const uint8_t oid[24])
{
    bool found;
    int i = coop_oid_set_find(set, oid, &found);

    if (found) {
        return;
    }
    if (set->count >= COOP_OID_SET_MAX) {
        set->overflow = true;
        return;
    }
    memcpy(set->oid[set->count], oid, 24);
    set->count++;
    set->slot[i] = (uint16_t)set->count;
}

bool coop_oid_set_has(const CoopOidSet* set, const uint8_t oid[24])
{
    bool found;

    (void)coop_oid_set_find(set, oid, &found);
    return found;
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

    /* The engine's caps: STAT_LEVEL max 51, STAT_EXPERIENCE_POINTS 2e9. */
    if (level < 1 || level > COOP_CHAR_MAX_LEVEL || xp < 0 || xp > 2000000000 || gold < 0 || gold > 1000000
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
    if (pkt == NULL || (pkt->type != PACKET_COOP_OBJ_REQ && pkt->type != PACKET_COOP_NPC_REQ)
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
    return coop_lite_oid_raw_equal(pkt->data + 4 + 24, pkt->oid);
}

void coop_lite_obj_blob_ground_loc_put(PacketCoopObjBlob* pkt, int64_t loc)
{
    memset(pkt->owner_oid, 0, sizeof(pkt->owner_oid));
    memcpy(pkt->owner_oid, &loc, sizeof(loc));
}

int64_t coop_lite_obj_blob_ground_loc_get(const PacketCoopObjBlob* pkt)
{
    int64_t loc;

    memcpy(&loc, pkt->owner_oid, sizeof(loc));
    return loc;
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
    return (pkt->flags & ~(COOP_INVEN_BATCH_FIRST | COOP_INVEN_BATCH_LAST) & 0xFFFF) == 0; /* PT54: high bits = echo seq */
}

bool coop_lite_client_transfer_flags_allowed(unsigned int flags)
{
    /* 0x01 = move into the target, 0x20 = whole stack (no quantity box),
       0x02 = drop on the ground (P1-3; host checks the item is the PC's). */
    if ((flags & 0x02u) != 0) {
        return (flags & ~0x22u) == 0;
    }
    if ((flags & (COOP_XFER_STEAL | COOP_XFER_PLANT)) != 0) {
        return flags == (0x01u | COOP_XFER_STEAL) || flags == (0x01u | COOP_XFER_PLANT);
    }
    return (flags & 0x01u) != 0 && (flags & ~0x21u) == 0;
}

bool coop_lite_throw_allowed(bool net_active, bool is_host, bool coop)
{
    return !(net_active && !is_host && coop);
}

bool coop_lite_client_throw_denied(void)
{
    return !coop_lite_throw_allowed(tig_net_is_active(), tig_net_is_host(), tig_net_is_coop_enabled());
}

int coop_lite_target_kind(bool is_container, bool locked, bool is_critter, bool dead, bool is_self, int dist)
{
    if (is_self || dist < 0 || dist > COOP_TARGET_REACH) {
        return COOP_TARGET_NONE;
    }
    if (is_container) {
        return locked ? COOP_TARGET_NONE : COOP_TARGET_LOOT;
    }
    if (is_critter) {
        return dead ? COOP_TARGET_LOOT : COOP_TARGET_STEAL;
    }
    return COOP_TARGET_NONE;
}

unsigned int coop_lite_pc_build_sig(const CoopPcBuild* b)
{
    const unsigned char* p = (const unsigned char*)b;
    unsigned int h = 2166136261u;
    size_t i;

    for (i = 0; i < sizeof(*b); i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static bool coop_lite_pcb_rise(int have, int want, int cap, int* cost)
{
    if (want < have) {
        return false;
    }
    if (want > have) {
        if (want > cap) {
            return false;
        }
        *cost += want - have;
    }
    return true;
}

bool coop_lite_pc_build_spend_ok(const CoopPcBuild* have, const CoopPcBuild* want)
{
    int cost = 0;
    int i;

    if (have == NULL || want == NULL) {
        return false;
    }
    for (i = 0; i < COOP_PCB_PRIMARY; i++) {
        if (!coop_lite_pcb_rise(have->stats[i], want->stats[i], 20, &cost)) {
            return false;
        }
    }
    /* Skills: points in the low 6 bits, training above (host/dialog only). */
    for (i = 0; i < COOP_PCB_BASIC; i++) {
        if ((have->basic[i] & ~63) != (want->basic[i] & ~63)
            || !coop_lite_pcb_rise(have->basic[i] & 63, want->basic[i] & 63, 20, &cost)) {
            return false;
        }
    }
    for (i = 0; i < COOP_PCB_TECH; i++) {
        if ((have->tech[i] & ~63) != (want->tech[i] & ~63)
            || !coop_lite_pcb_rise(have->tech[i] & 63, want->tech[i] & 63, 20, &cost)) {
            return false;
        }
    }
    /* The mastery college comes from a quest (host), never a spend. */
    if (have->spell_tech[COOP_PCB_MASTERY] != want->spell_tech[COOP_PCB_MASTERY]) {
        return false;
    }
    for (i = 0; i < COOP_PCB_COLLEGES; i++) {
        if (!coop_lite_pcb_rise(have->spell_tech[i], want->spell_tech[i], 5, &cost)) {
            return false;
        }
    }
    for (i = COOP_PCB_DEGREE0; i < COOP_PCB_SPELL_TECH; i++) {
        if (!coop_lite_pcb_rise(have->spell_tech[i], want->spell_tech[i], 7, &cost)) {
            return false;
        }
    }
    if (want->stats[COOP_PCB_STAT_UNSPENT] < 0
        || have->stats[COOP_PCB_STAT_UNSPENT] - want->stats[COOP_PCB_STAT_UNSPENT] != cost) {
        return false;
    }
    return true;
}

int coop_lite_xp_split(int xp, int n, int carry[8], int out[8])
{
    int i;

    if (n <= 0 || n > 8 || xp <= 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        int owed = xp + carry[i];

        out[i] = owed / n;
        carry[i] = owed % n;
    }
    return n;
}

/* PT52j: pure (outside the stub ifdef) so tests can check every notice. */
void coop_lite_party_notice_text(int kind, unsigned int countdown_ms, bool local_is_host,
    char* buf, size_t size)
{
    switch (kind) {
    case COOP_PARTY_HOST_DOWN:
        snprintf(buf, size, "%s", local_is_host
                ? "Вы пали. Спутники могут вас воскресить. / You have fallen. Your companions can resurrect you."
                : "Лидер группы пал! Воскресите его, чтобы продолжить. / The party leader has fallen! Resurrect them to continue.");
        break;
    case COOP_PARTY_HOST_UP:
        snprintf(buf, size, "%s", local_is_host ? "Вас воскресили. / You have been resurrected." : "Лидер группы снова на ногах. / The party leader is back on their feet.");
        break;
    case COOP_PARTY_WIPE:
        snprintf(buf, size, "Вся группа пала. Загрузка последнего сохранения через %u с... / The whole party has fallen. Reloading the last save in %u seconds...",
            (countdown_ms + 999u) / 1000u, (countdown_ms + 999u) / 1000u); /* PT52i */
        break;
    case COOP_PARTY_WIPE_CANCEL:
        snprintf(buf, size, "%s", "Кто-то поднялся — загрузка отменена. / Someone got back up - reload cancelled.");
        break;
    case COOP_PARTY_RELOADING:
        snprintf(buf, size, "%s", "Загружаем последнее сохранение лидера... / Reloading the party leader's last save...");
        break;
    case COOP_PARTY_RESYNC:
        snprintf(buf, size, "%s", "Лидер группы загрузился. Переподключение... / The party leader reloaded. Rejoining...");
        break;
    case COOP_PARTY_NO_SAVE:
        snprintf(buf, size, "%s", "Нет сохранения — группа поднимается. / No save to reload - the party gets back up.");
        break;
    case COOP_PARTY_CAUGHT_THIEF:
        snprintf(buf, size, "%s", "Пойман на краже! / Caught pickpocketing!");
        break;
    case COOP_PARTY_STEAL_FAILED:
        snprintf(buf, size, "%s", "Кража не удалась. / Your pickpocket attempt failed.");
        break;
    case COOP_PARTY_STEAL_OK:
        snprintf(buf, size, "%s", "Получилось. / You succeed.");
        break;
    case COOP_PARTY_SKILL_FAILED:
        snprintf(buf, size, "%s", "Не получилось. / You failed.");
        break;
    case COOP_PARTY_TALK_REFUSED:
        snprintf(buf, size, "%s", "Для разговора лидер группы должен быть рядом и свободен. / The party leader must be close by (and free) to talk.");
        break;
    case COOP_PARTY_REST_BEGIN:
        snprintf(buf, size, "%s", local_is_host ? "" : "Лидер группы отдыхает — время идёт для всех. / The party leader rests - time passes for everyone.");
        break;
    case COOP_PARTY_WMAP_OPEN:
        snprintf(buf, size, "%s", local_is_host ? ""
                : "Лидер группы на карте мира — вы путешествуете вместе. Подождите... / The party leader is on the world map - you travel together. Please wait...");
        break;
    default:
        buf[0] = '\0';
        break;
    }
    {
        /* PT53: English unless ARCANUM_COOP_LANG=ru. */
        const char* en = coop_lite_lang(buf);

        if (en != buf) {
            memmove(buf, en, strlen(en) + 1);
        }
    }
}

bool coop_lite_party_notice_is_good(int kind)
{
    return kind == COOP_PARTY_HOST_UP || kind == COOP_PARTY_STEAL_OK;
}

bool coop_lite_target_transfer_ok(int kind, unsigned int flags, bool from_target, bool worn)
{
    bool steal_bits = (flags & (COOP_XFER_STEAL | COOP_XFER_PLANT)) != 0;

    if (!coop_lite_client_transfer_flags_allowed(flags) || (flags & 0x02u) != 0) {
        return false;
    }
    switch (kind) {
    case COOP_TARGET_LOOT:
        return !steal_bits;
    case COOP_TARGET_STEAL:
        /* Vanilla: worn items cannot be pickpocketed (inven_ui refuses the
           drag); stealing takes out of the target, planting puts in. */
        if (from_target) {
            return flags == (0x01u | COOP_XFER_STEAL) && !worn;
        }
        return flags == (0x01u | COOP_XFER_PLANT);
    default:
        return false;
    }
}

#ifndef COOP_LITE_TEST_STUBS
#include "game/obj_flags.h"

typedef struct CoopBarterSession {
    bool valid;
    ObjectID npc;
    /* PT18: a loot / pickpocket window (same lifecycle as barter). */
    bool target_valid;
    int target_kind;
    ObjectID target;
} CoopBarterSession;
static CoopBarterSession coop_barter[8];

static bool coop_inv_active;

static void coop_lite_barter_reset(void)
{
    memset(coop_barter, 0, sizeof(coop_barter));
    coop_inv_active = false;
}

/* Review P1-1: a barter session ends with the dialog, but also when the
   client disconnects, the map changes, either side dies or they part. */
static void coop_lite_barter_end(int slot, const char* why)
{
    if (slot > 0 && slot < 8 && coop_barter[slot].valid) {
        coop_barter[slot].valid = false;
        tig_debug_printf("CoopLite: barter: slot %d session ended (%s)\n", slot, why);
    }
    if (slot > 0 && slot < 8 && coop_barter[slot].target_valid) {
        coop_barter[slot].target_valid = false;
        tig_debug_printf("CoopLite: loot: slot %d %s window ended (%s)\n", slot,
            coop_barter[slot].target_kind == COOP_TARGET_STEAL ? "pickpocket" : "loot", why);
    }
}

/* PT18: what `pc` may open on `target` right now (COOP_TARGET_*). */
static int coop_lite_target_kind_of(int64_t pc, int64_t target)
{
    int type;
    bool is_container;
    bool is_critter;

    if (pc == OBJ_HANDLE_NULL || target == OBJ_HANDLE_NULL || critter_is_dead(pc)
        || (obj_field_int32_get(target, OBJ_F_FLAGS) & (OF_DESTROYED | OF_OFF)) != 0) {
        return COOP_TARGET_NONE;
    }
    type = obj_field_int32_get(target, OBJ_F_TYPE);
    is_container = type == OBJ_TYPE_CONTAINER;
    is_critter = obj_type_is_critter(type);
    return coop_lite_target_kind(is_container,
        is_container && object_locked_get(target),
        is_critter,
        is_critter && critter_is_dead(target),
        target == pc,
        (int)location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION), obj_field_int64_get(target, OBJ_F_LOCATION)));
}

static int64_t coop_lite_target_obj(int slot)
{
    if (slot <= 0 || slot >= 8 || !coop_barter[slot].target_valid) {
        return OBJ_HANDLE_NULL;
    }
    return obj_pool_perm_lookup(coop_barter[slot].target);
}

static unsigned int coop_barter_watch_ms;
static bool coop_lite_barter_partner_ok(int64_t pc, int64_t npc);

static void coop_lite_host_barter_watch(unsigned int now_ms)
{
    int slot;

    if (!coop_lite_interval_elapsed(coop_barter_watch_ms, now_ms, 1000u)) {
        return;
    }
    coop_barter_watch_ms = now_ms;
    for (slot = 1; slot < 8; slot++) {
        if (coop_barter[slot].valid
            && !coop_lite_barter_partner_ok(sub_4A2B60(slot), obj_pool_perm_lookup(coop_barter[slot].npc))) {
            coop_lite_barter_end(slot, "partner gone, dead or too far");
        }
        if (coop_barter[slot].target_valid
            && coop_lite_target_kind_of(sub_4A2B60(slot), coop_lite_target_obj(slot)) != coop_barter[slot].target_kind) {
            coop_lite_barter_end(slot, "target gone, changed (died / locked) or too far");
        }
    }
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

static int coop_lite_stack_qty(int64_t item)
{
    return obj_field_int32_get(item, obj_field_int32_get(item, OBJ_F_TYPE) == OBJ_TYPE_GOLD
            ? OBJ_F_GOLD_QUANTITY
            : OBJ_F_AMMO_QUANTITY);
}

/*
 * PT17b: vanilla AI (ai_look_for_item_func) lets any nearby NPC take items
 * lying around. In a coop session that ate players' drops (the clients'
 * copies lingered, clicks on them got nothing back), so on the host an item
 * a PC drops is marked NPC-proof until a PC picks it up again.
 */
void coop_lite_mark_player_drop(int64_t item)
{
    /* PT18 (user decision + Argon #3 pt 3): vanilla again — NPCs may take
       what players drop. Only the dev item tests keep their piles. */
    if (coop_lite_env("ARCANUM_DEV_GOLD_PILE") == NULL && coop_lite_env("ARCANUM_DEV_CLIENT_ITEMS") == NULL) {
        return;
    }
    if (item == OBJ_HANDLE_NULL || !tig_net_is_coop_enabled()
        || (tig_net_is_active() && !tig_net_is_host())
        || (obj_field_int32_get(item, OBJ_F_FLAGS) & OF_DESTROYED) != 0) {
        return;
    }
    obj_field_int32_set(item, OBJ_F_ITEM_FLAGS,
        obj_field_int32_get(item, OBJ_F_ITEM_FLAGS) | (int)OIF_NO_NPC_PICKUP);
}

void coop_lite_clear_player_drop(int64_t item)
{
    if (item == OBJ_HANDLE_NULL || !tig_net_is_coop_enabled()
        || !obj_handle_is_valid(item)
        || (obj_field_int32_get(item, OBJ_F_FLAGS) & OF_DESTROYED) != 0
        || (obj_field_int32_get(item, OBJ_F_ITEM_FLAGS) & (int)OIF_NO_NPC_PICKUP) == 0) {
        return;
    }
    obj_field_int32_set(item, OBJ_F_ITEM_FLAGS,
        obj_field_int32_get(item, OBJ_F_ITEM_FLAGS) & ~(int)OIF_NO_NPC_PICKUP);
}

/*
 * PT17b: an item left the ground at the host through a pickup goal (an NPC,
 * the host's PC). Clients drop their copy now instead of at the next world
 * state (up to 10 s of a clickable ghost). A client PC's own pickup keeps
 * its copy for its INVEN_SYNC, unless it is a stack (totals only).
 */
void coop_lite_host_ground_item_taken(int64_t taker, const void* oid_ptr, bool stack)
{
    PacketObjectDestroy gone;
    int slot;

    if (oid_ptr == NULL || !tig_net_is_coop_enabled() || !tig_net_is_active() || !tig_net_is_host()) {
        return;
    }
    memset(&gone, 0, sizeof(gone));
    gone.type = 72;
    memcpy(&gone.oid, oid_ptr, sizeof(gone.oid));
    slot = taker != OBJ_HANDLE_NULL ? coop_lite_slot_of_obj(taker) : -1;
    if (slot > 0 && !stack) {
        tig_net_send_app_except(&gone, sizeof(gone), slot);
    } else {
        tig_net_send_app_all(&gone, sizeof(gone));
    }
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

static unsigned int coop_lite_item_keyring_hash(int64_t item)
{
    unsigned int keys[64];
    int n;
    int i;

    if (obj_field_int32_get(item, OBJ_F_TYPE) != OBJ_TYPE_KEY_RING) {
        return 0;
    }
    n = obj_arrayfield_length_get(item, OBJ_F_KEY_RING_LIST_IDX);
    if (n > 64) {
        n = 64;
    }
    for (i = 0; i < n; i++) {
        keys[i] = obj_arrayfield_uint32_get(item, OBJ_F_KEY_RING_LIST_IDX, i);
    }
    return coop_lite_keyring_hash(keys, n);
}

static void coop_lite_fill_item_state(PacketCoopInvenEntry* e, int64_t item)
{
    e->keyring_hash = coop_lite_item_keyring_hash(item);
    e->hp_damage = obj_field_int32_get(item, OBJ_F_HP_DAMAGE);
    e->mana_store = obj_field_int32_get(item, OBJ_F_ITEM_MANA_STORE);
    e->spell_mana_store = obj_field_int32_get(item, OBJ_F_ITEM_SPELL_MANA_STORE);
    e->state_flags = coop_lite_inven_state_flags(false,
        tig_art_item_id_destroyed_get(obj_field_int32_get(item, OBJ_F_CURRENT_AID)) != 0);
}

/* PT18 (Argon #3 pt 4c): what the host last told slot N about its own PC.
   Anything else that changes that PC's inventory on the host (the host
   looting its corpse or picking its pocket, another client's steal, a
   script) differs from it, and the slot gets a resync — gold included. */
static unsigned int coop_slot_inven_sig[8];
static bool coop_slot_inven_sig_valid[8];

static unsigned int coop_lite_pc_inven_sig(int64_t pc)
{
    unsigned int h = 2166136261u;
    int cnt;
    int i;
    int t;

#define COOP_SIG_INT(x) do { h = (h ^ (unsigned int)(x)) * 16777619u; } while (0)
    COOP_SIG_INT(item_gold_get(pc));
    for (t = 0; t < COOP_INVEN_AMMO_TYPES; t++) {
        COOP_SIG_INT(item_ammo_quantity_get(pc, t));
    }
    cnt = obj_field_int32_get(pc, OBJ_F_CRITTER_INVENTORY_NUM);
    COOP_SIG_INT(cnt);
    for (i = 0; i < cnt; i++) {
        int64_t item = obj_arrayfield_handle_get(pc, OBJ_F_CRITTER_INVENTORY_LIST_IDX, i);
        ObjectID oid;
        const unsigned char* p;
        size_t k;

        if (item == OBJ_HANDLE_NULL || coop_lite_is_stackable(item)) {
            continue;
        }
        oid = obj_get_id(item);
        p = (const unsigned char*)&oid;
        for (k = 0; k < sizeof(oid); k++) {
            COOP_SIG_INT(p[k]);
        }
        COOP_SIG_INT(item_inventory_location_get(item));
    }
#undef COOP_SIG_INT
    return h;
}

/* ---- PT19: PC build sync (PACKET_COOP_PC_STATS) -------------------------- */

COOP_STATIC_ASSERT(sizeof(PacketCoopWorldState) <= TIG_NET_MAX_PACKET, "world chunk fits a packet");
COOP_STATIC_ASSERT(sizeof(PacketCoopObjBlob) <= TIG_NET_MAX_PACKET, "obj blob fits a packet");
COOP_STATIC_ASSERT(sizeof(PacketCoopPcStats) <= TIG_NET_MAX_PACKET, "PC stats fit a packet");
COOP_STATIC_ASSERT(COOP_PCB_STATS == STAT_COUNT, "PCB stats");
COOP_STATIC_ASSERT(COOP_PCB_BASIC == BASIC_SKILL_COUNT, "PCB basic skills");
COOP_STATIC_ASSERT(COOP_PCB_TECH == TECH_SKILL_COUNT, "PCB tech skills");
COOP_STATIC_ASSERT(COOP_PCB_COLLEGES == COLLEGE_COUNT, "PCB colleges");
COOP_STATIC_ASSERT(COOP_PCB_SPELL_TECH == COLLEGE_COUNT + 1 + TECH_COUNT, "PCB spell/tech");
COOP_STATIC_ASSERT(COOP_PCB_STAT_LEVEL == STAT_LEVEL && COOP_PCB_STAT_XP == STAT_EXPERIENCE_POINTS
        && COOP_PCB_STAT_ALIGNMENT == STAT_ALIGNMENT && COOP_PCB_STAT_FATE == STAT_FATE_POINTS
        && COOP_PCB_STAT_UNSPENT == STAT_UNSPENT_POINTS,
    "PCB stat indices");
COOP_STATIC_ASSERT(COOP_NPC_STATS_COUNT == STAT_COUNT && STAT_LEVEL == 17, "PT79 NPC_STATS stat layout");

static void coop_lite_pc_build_fill(int64_t pc, CoopPcBuild* b)
{
    int i;
    int n;

    memset(b, 0, sizeof(*b));
    for (i = 0; i < COOP_PCB_STATS; i++) {
        b->stats[i] = obj_arrayfield_int32_get(pc, OBJ_F_CRITTER_STAT_BASE_IDX, i);
    }
    for (i = 0; i < COOP_PCB_BASIC; i++) {
        b->basic[i] = obj_arrayfield_int32_get(pc, OBJ_F_CRITTER_BASIC_SKILL_IDX, i);
    }
    for (i = 0; i < COOP_PCB_TECH; i++) {
        b->tech[i] = obj_arrayfield_int32_get(pc, OBJ_F_CRITTER_TECH_SKILL_IDX, i);
    }
    for (i = 0; i < COOP_PCB_SPELL_TECH; i++) {
        b->spell_tech[i] = (int)obj_arrayfield_uint32_get(pc, OBJ_F_CRITTER_SPELL_TECH_IDX, i);
    }
    if (obj_field_int32_get(pc, OBJ_F_TYPE) == OBJ_TYPE_PC) {
        n = obj_arrayfield_length_get(pc, OBJ_F_PC_SCHEMATICS_FOUND_IDX);
        if (n > COOP_PCB_SCHEMATICS) {
            n = COOP_PCB_SCHEMATICS;
        }
        b->num_schematics = n;
        for (i = 0; i < n; i++) {
            b->schematics[i] = (int)obj_arrayfield_uint32_get(pc, OBJ_F_PC_SCHEMATICS_FOUND_IDX, i);
        }
    }
}

#define COOP_PCB_APPLY_FULL 0
#define COOP_PCB_APPLY_OWNER 1 /* host → owner: host-authoritative fields only */
#define COOP_PCB_APPLY_SPEND 2 /* owner → host: an accepted spend */

static void coop_lite_pc_build_apply(int64_t pc, const CoopPcBuild* b, int mode)
{
    int i;

    if (mode == COOP_PCB_APPLY_OWNER) {
        int local_level = obj_arrayfield_int32_get(pc, OBJ_F_CRITTER_STAT_BASE_IDX, STAT_LEVEL);
        /* PT56: poison too - the host poisons and cures (antidotes run
           there); our copy stayed poisoned after an antidote. */
        static const int host_stats[] = { STAT_LEVEL, STAT_EXPERIENCE_POINTS, STAT_ALIGNMENT, STAT_FATE_POINTS,
            STAT_POISON_LEVEL };

        if (obj_arrayfield_int32_get(pc, OBJ_F_CRITTER_STAT_BASE_IDX, STAT_POISON_LEVEL) != b->stats[STAT_POISON_LEVEL]) {
            tig_debug_printf("CoopLite: PC stats: poison %d -> %d (host)\n",
                obj_arrayfield_int32_get(pc, OBJ_F_CRITTER_STAT_BASE_IDX, STAT_POISON_LEVEL), b->stats[STAT_POISON_LEVEL]);
        }
        for (i = 0; i < (int)(sizeof(host_stats) / sizeof(host_stats[0])); i++) {
            obj_arrayfield_int32_set(pc, OBJ_F_CRITTER_STAT_BASE_IDX, host_stats[i], b->stats[host_stats[i]]);
        }
        {
            CoopPcBuild mine;
            bool caught_up;

            coop_lite_pc_build_fill(pc, &mine);
            /* The host's unspent count is the truth once it has every spend
               of ours (same primaries, skills, spells, degrees); while one
               is in flight, keep ours. A level-up always brings its points. */
            caught_up = memcmp(mine.stats, b->stats, sizeof(int) * COOP_PCB_PRIMARY) == 0
                && memcmp(mine.basic, b->basic, sizeof(mine.basic)) == 0
                && memcmp(mine.tech, b->tech, sizeof(mine.tech)) == 0
                && memcmp(mine.spell_tech, b->spell_tech, sizeof(mine.spell_tech)) == 0;
            if (caught_up || b->stats[STAT_LEVEL] > local_level) {
                obj_arrayfield_int32_set(pc, OBJ_F_CRITTER_STAT_BASE_IDX, STAT_UNSPENT_POINTS, b->stats[STAT_UNSPENT_POINTS]);
            }
            /* PT47: training (bits >= 6) is host-only (trainers); take it on
               every tick, not just the FORCE send, keeping our points. */
            for (i = 0; i < COOP_PCB_BASIC; i++) {
                int v = coop_lite_skill_merge_training(mine.basic[i], b->basic[i]);
                if (v != mine.basic[i]) {
                    obj_arrayfield_int32_set(pc, OBJ_F_CRITTER_BASIC_SKILL_IDX, i, v);
                }
            }
            for (i = 0; i < COOP_PCB_TECH; i++) {
                int v = coop_lite_skill_merge_training(mine.tech[i], b->tech[i]);
                if (v != mine.tech[i]) {
                    obj_arrayfield_int32_set(pc, OBJ_F_CRITTER_TECH_SKILL_IDX, i, v);
                }
            }
        }
        return;
    }
    for (i = 0; i < COOP_PCB_STATS; i++) {
        if (mode == COOP_PCB_APPLY_SPEND && i >= COOP_PCB_PRIMARY && i != STAT_UNSPENT_POINTS) {
            continue;
        }
        obj_arrayfield_int32_set(pc, OBJ_F_CRITTER_STAT_BASE_IDX, i, b->stats[i]);
    }
    for (i = 0; i < COOP_PCB_BASIC; i++) {
        obj_arrayfield_int32_set(pc, OBJ_F_CRITTER_BASIC_SKILL_IDX, i, b->basic[i]);
    }
    for (i = 0; i < COOP_PCB_TECH; i++) {
        obj_arrayfield_int32_set(pc, OBJ_F_CRITTER_TECH_SKILL_IDX, i, b->tech[i]);
    }
    for (i = 0; i < COOP_PCB_SPELL_TECH; i++) {
        obj_arrayfield_uint32_set(pc, OBJ_F_CRITTER_SPELL_TECH_IDX, i, (unsigned int)b->spell_tech[i]);
    }
    if (mode == COOP_PCB_APPLY_FULL && obj_field_int32_get(pc, OBJ_F_TYPE) == OBJ_TYPE_PC) {
        for (i = 0; i < b->num_schematics && i < COOP_PCB_SCHEMATICS; i++) {
            obj_arrayfield_uint32_set(pc, OBJ_F_PC_SCHEMATICS_FOUND_IDX, i, (unsigned int)b->schematics[i]);
        }
    }
}

/*
 * PT20 party XP (host). Kill, damage and quest XP all go through
 * critter_give_xp; split it among the living, connected slot PCs (all on
 * the host's map in coop-lite: map travel takes everyone). Dead PCs get
 * nothing, as in vanilla. Level-ups happen here (level_recalc) and reach
 * the owners with the PC build sync (packet 144).
 */
bool coop_lite_host_share_xp(int64_t earner, int xp)
{
    static int carry_by_slot[8]; /* XP owed in n-ths (per-hit XP is tiny) */
    int64_t pcs[8];
    int slots[8];
    int carry[8];
    int amounts[8];
    int n = 0;
    bool in_party = false;
    int slot;
    int i;

    if (!tig_net_is_host() || xp <= 0) {
        return false;
    }
    for (slot = 0; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);

        if (pc == OBJ_HANDLE_NULL || (slot > 0 && !tig_net_client_is_active(slot))) {
            continue;
        }
        if (pc == earner) {
            in_party = true;
        }
        if (critter_is_dead(pc)
            || (obj_field_int32_get(pc, OBJ_F_FLAGS) & (OF_OFF | OF_DESTROYED)) != 0) {
            continue;
        }
        slots[n] = slot;
        carry[n] = carry_by_slot[slot];
        pcs[n++] = pc;
    }
    if (!in_party) {
        return false; /* not a party PC: vanilla */
    }
    if (n == 0) {
        tig_debug_printf("CoopLite: party xp: %d XP, nobody alive — dropped\n", xp);
        return true;
    }
    if (n == 1 && pcs[0] == earner) {
        return false; /* alone: vanilla */
    }
    coop_lite_xp_split(xp, n, carry, amounts);
    for (i = 0; i < n; i++) {
        carry_by_slot[slots[i]] = carry[i];
        critter_award_xp(pcs[i], amounts[i]);
        tig_debug_printf("CoopLite: party xp: %d of %d XP -> slot %d (xp now %d, lvl %d)\n", amounts[i], xp,
            slots[i], stat_base_get(pcs[i], STAT_EXPERIENCE_POINTS),
            stat_base_get(pcs[i], STAT_LEVEL));
    }
    return true;
}

static void coop_lite_pc_stats_send(int slot, int64_t pc, unsigned int flags, int to_slot)
{
    PacketCoopPcStats pkt;
    ObjectID oid = obj_get_id(pc);
    int depth;

    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_PC_STATS;
    pkt.slot = slot;
    pkt.flags = coop_lite_pcs_portrait_pack(flags, obj_field_int32_get(pc, OBJ_F_CRITTER_PORTRAIT));
    memcpy(pkt.oid, &oid, sizeof(pkt.oid));
    pkt.base_aid = (unsigned int)obj_field_int32_get(pc, OBJ_F_AID); /* PT54a */
    coop_lite_pc_build_fill(pc, &pkt.build);
    depth = tig_net_local_suspend_pause();
    if (to_slot >= 0) {
        tig_net_send_app(&pkt, sizeof(pkt), to_slot);
    } else {
        tig_net_send_app_all(&pkt, sizeof(pkt));
    }
    tig_net_local_suspend_resume(depth);
}

static unsigned int coop_pcs_sig[8];
static int64_t coop_pcs_obj[8];
static unsigned int coop_pcs_ms;
static int coop_pcs_force_left; /* PT54a */
static unsigned int coop_pcs_force_ms;

static void coop_lite_host_pcs_force(void)
{
    coop_pcs_force_left = 4;
    coop_pcs_force_ms = 0;
}

/* Host: every 500 ms, broadcast any slot PC whose build changed (and all of
   them when the roster changes, so a new client gets everyone). */
static void coop_lite_host_pc_stats_tick(unsigned int now_ms)
{
    int slot;
    bool roster_changed = false;

    if (!coop_lite_interval_elapsed(coop_pcs_ms, now_ms, 500u)) {
        return;
    }
    coop_pcs_ms = now_ms;
    for (slot = 0; slot < 8; slot++) {
        if (sub_4A2B60(slot) != coop_pcs_obj[slot]) {
            roster_changed = true;
        }
    }
    /* PT54a: after a join / rejoin / host load every slot's PC stats
       (portrait + body art) go out again every 3 s, 4 times: the first
       copy often reaches a client whose world is not loaded yet. */
    if (coop_pcs_force_left > 0 && coop_lite_interval_elapsed(coop_pcs_force_ms, now_ms, 3000u)) {
        coop_pcs_force_ms = now_ms;
        coop_pcs_force_left--;
        roster_changed = true;
    }
    for (slot = 0; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);
        CoopPcBuild b;
        unsigned int sig;

        if (roster_changed) {
            coop_pcs_obj[slot] = pc;
            coop_pcs_sig[slot] = 0;
        }
        if (pc == OBJ_HANDLE_NULL) {
            continue;
        }
        /* PT33: a PC that holds the XP for its next level gets it (and its
           auto-level scheme) whether or not it is dead; the XP split only
           pays the living, but nothing else may leave a dead PC un-levelled. */
        if (obj_field_int32_get(pc, OBJ_F_TYPE) == OBJ_TYPE_PC
            && (slot == 0 || tig_net_client_is_active(slot))
            && stat_base_get(pc, STAT_LEVEL) < stat_level_max(pc, STAT_LEVEL)
            && level_experience_points_to_next_level(pc) <= 0) {
            tig_debug_printf("CoopLite: PC stats: slot %d holds XP for level %d%s — level-up\n", slot,
                stat_base_get(pc, STAT_LEVEL) + 1, critter_is_dead(pc) ? " (dead)" : "");
            level_recalc(pc);
        }
        coop_lite_pc_build_fill(pc, &b);
        sig = coop_lite_pc_build_sig(&b)
            ^ ((unsigned int)obj_field_int32_get(pc, OBJ_F_CRITTER_PORTRAIT) * 0x9E3779B1u) /* PT47 */
            ^ ((unsigned int)obj_field_int32_get(pc, OBJ_F_AID) * 0x85EBCA77u); /* PT54a */
        if (sig == 0) {
            sig = 1; /* PT52g: 0 = none yet */
        }
        if (sig == coop_pcs_sig[slot]) {
            continue;
        }
        coop_pcs_sig[slot] = sig;
        coop_lite_pc_stats_send(slot, pc, 0, -1);
        tig_debug_printf("CoopLite: PC stats: slot %d build sent (lvl %d, xp %d, unspent %d, max hp %d, sig %08x)\n",
            slot, b.stats[STAT_LEVEL], b.stats[STAT_EXPERIENCE_POINTS], b.stats[STAT_UNSPENT_POINTS],
            object_hp_max(pc), sig);
    }
}

#define COOP_FX_CACHE 512
#define COOP_FX_KEYFRAME_MS 5000u
typedef struct CoopFxCacheEntry {
    int64_t obj;
    unsigned int sig;
    unsigned int sent_ms;
} CoopFxCacheEntry;
static CoopFxCacheEntry coop_fx_cache[COOP_FX_CACHE];
static unsigned int coop_fx_ms;
static int coop_fx_map;

static CoopFxCacheEntry* coop_lite_fx_cache_slot(int64_t obj)
{
    unsigned int h = (unsigned int)((uint64_t)obj * 2654435761u) % COOP_FX_CACHE;
    unsigned int i;
    CoopFxCacheEntry* oldest = NULL;

    for (i = 0; i < 8; i++) {
        CoopFxCacheEntry* c = &coop_fx_cache[(h + i) % COOP_FX_CACHE];

        if (c->obj == obj || c->obj == OBJ_HANDLE_NULL) {
            return c;
        }
        if (oldest == NULL || (int)(oldest->sent_ms - c->sent_ms) > 0) {
            oldest = c;
        }
    }
    return oldest;
}

static void coop_lite_host_critter_effects_send(int64_t obj)
{
    PacketCoopCritterEffects pkt;
    ObjectID oid;
    int n;
    int nc;
    int i;
    unsigned int effects[COOP_CRITTER_EFFECTS_MAX];
    unsigned int causes[COOP_CRITTER_EFFECTS_MAX];
    unsigned int light_flags;
    unsigned int light_aid;
    unsigned int light_color;
    unsigned int sig;
    CoopFxCacheEntry* c;

    if (obj == OBJ_HANDLE_NULL || !obj_type_is_critter(obj_field_int32_get(obj, OBJ_F_TYPE))) {
        return;
    }
    n = obj_arrayfield_length_get(obj, OBJ_F_CRITTER_EFFECTS_IDX);
    nc = obj_arrayfield_length_get(obj, OBJ_F_CRITTER_EFFECT_CAUSE_IDX);
    if (nc < n) {
        n = nc;
    }
    if (n < 0) {
        n = 0;
    }
    if (n > COOP_CRITTER_EFFECTS_MAX) {
        n = COOP_CRITTER_EFFECTS_MAX;
    }
    for (i = 0; i < n; i++) {
        effects[i] = obj_arrayfield_uint32_get(obj, OBJ_F_CRITTER_EFFECTS_IDX, i);
        causes[i] = obj_arrayfield_uint32_get(obj, OBJ_F_CRITTER_EFFECT_CAUSE_IDX, i);
    }
    light_flags = (unsigned int)obj_field_int32_get(obj, OBJ_F_LIGHT_FLAGS);
    light_aid = (unsigned int)obj_field_int32_get(obj, OBJ_F_LIGHT_AID);
    light_color = (unsigned int)obj_field_int32_get(obj, OBJ_F_LIGHT_COLOR);
    sig = coop_lite_critter_effects_sig(n, effects, causes, light_flags, light_aid, light_color);
    c = coop_lite_fx_cache_slot(obj);
    if (c->obj == obj && c->sig == sig
        && !coop_lite_interval_elapsed(c->sent_ms, coop_obj_now_ms, COOP_FX_KEYFRAME_MS)) {
        return;
    }
    memset(&pkt, 0, offsetof(PacketCoopCritterEffects, data));
    pkt.type = PACKET_COOP_CRITTER_EFFECTS;
    pkt.count = n;
    oid = obj_get_id(obj);
    memcpy(pkt.oid, &oid, sizeof(oid));
    pkt.light_flags = light_flags;
    pkt.light_aid = light_aid;
    pkt.light_color = light_color;
    if (n > 0) {
        memcpy(pkt.data, effects, (size_t)n * sizeof(unsigned int));
        memcpy(pkt.data + n, causes, (size_t)n * sizeof(unsigned int));
    }
    tig_net_send_app_all(&pkt, coop_lite_critter_effects_size(n));
    c->obj = obj;
    c->sig = sig;
    c->sent_ms = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1;
}

static void coop_lite_host_critter_effects_tick(unsigned int now_ms)
{
    int slot;

    if (!tig_net_is_active() || !tig_net_is_host()) {
        return;
    }
    if (!coop_lite_interval_elapsed(coop_fx_ms, now_ms, 500u)) {
        return;
    }
    coop_fx_ms = now_ms;
    if (coop_fx_map != map_current_map()) {
        coop_fx_map = map_current_map();
        memset(coop_fx_cache, 0, sizeof(coop_fx_cache));
    }
    for (slot = 0; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);
        ObjectList objects;
        ObjectNode* node;

        if (pc == OBJ_HANDLE_NULL) {
            continue;
        }
        coop_lite_host_critter_effects_send(pc);
        coop_lite_npc_list_around(pc, &objects);
        for (node = objects.head; node != NULL; node = node->next) {
            coop_lite_host_critter_effects_send(node->obj);
        }
        object_list_destroy(&objects);
    }
}

void coop_lite_handle_critter_effects(PacketCoopCritterEffects* pkt)
{
    int64_t obj;
    int i;
    int n;
    const unsigned int* effects;
    const unsigned int* causes;

    if (!tig_net_is_active() || tig_net_is_host()
        || !coop_lite_critter_effects_valid(pkt, tig_net_last_message_size())
        || map_is_clearing_objects()) {
        return;
    }
    obj = coop_lite_get_oid_obj(pkt->oid);
    if (obj == OBJ_HANDLE_NULL || !obj_type_is_critter(obj_field_int32_get(obj, OBJ_F_TYPE))) {
        return;
    }
    n = pkt->count;
    effects = pkt->data;
    causes = pkt->data + n;
    multiplayer_lock();
    coop_lite_apply_begin();
    obj_arrayfield_length_set(obj, OBJ_F_CRITTER_EFFECTS_IDX, 0);
    obj_arrayfield_length_set(obj, OBJ_F_CRITTER_EFFECT_CAUSE_IDX, 0);
    for (i = 0; i < n; i++) {
        obj_arrayfield_uint32_set(obj, OBJ_F_CRITTER_EFFECTS_IDX, i, effects[i]);
        obj_arrayfield_uint32_set(obj, OBJ_F_CRITTER_EFFECT_CAUSE_IDX, i, causes[i]);
    }
    object_set_light(obj, pkt->light_flags, (tig_art_id_t)pkt->light_aid, (tig_color_t)pkt->light_color);
    anim_speed_recalc(obj);
    ui_refresh_health_bar(obj);
    ui_refresh_fatigue_bar(obj);
    coop_lite_apply_end();
    multiplayer_unlock();
}

static unsigned int coop_client_pcs_sig;
static unsigned int coop_client_pcs_ms;
static int coop_client_pcs_resend = 4; /* PT54a */
static unsigned int coop_client_pcs_resend_ms;

/* Client: our own build changed (character points spent) → tell the host. */
static void coop_lite_client_pc_stats_tick(unsigned int now_ms)
{
    int64_t pc = player_get_local_pc_obj();
    CoopPcBuild b;
    unsigned int sig;

    if (pc == OBJ_HANDLE_NULL || !coop_lite_interval_elapsed(coop_client_pcs_ms, now_ms, 500u)) {
        return;
    }
    coop_client_pcs_ms = now_ms;
    coop_lite_pc_build_fill(pc, &b);
    sig = coop_lite_pc_build_sig(&b)
        ^ ((unsigned int)obj_field_int32_get(pc, OBJ_F_CRITTER_PORTRAIT) * 0x9E3779B1u) /* PT54a */
        ^ ((unsigned int)obj_field_int32_get(pc, OBJ_F_AID) * 0x85EBCA77u);
    if (sig == 0) {
        sig = 1; /* PT52g: 0 = none yet */
    }
    if (coop_client_pcs_resend > 0 && coop_lite_interval_elapsed(coop_client_pcs_resend_ms, now_ms, 3000u)) {
        /* PT54a: the join-time send can land before the host's world is
           ready; repeat it a few times so portrait / body art arrive. */
        coop_client_pcs_resend_ms = now_ms;
        coop_client_pcs_resend--;
        coop_client_pcs_sig = sig;
        coop_lite_pc_stats_send(sub_529520(), pc, 0, -1);
        return;
    }
    if (coop_client_pcs_sig == 0) {
        coop_client_pcs_sig = sig; /* the join bundle carried this one */
        coop_lite_pc_stats_send(sub_529520(), pc, 0, -1); /* PT47: portrait */
        return;
    }
    if (sig == coop_client_pcs_sig) {
        return;
    }
    coop_client_pcs_sig = sig;
    coop_lite_pc_stats_send(sub_529520(), pc, 0, -1);
    tig_debug_printf("CoopLite: PC stats: my build changed — sent to the host (unspent %d)\n",
        b.stats[STAT_UNSPENT_POINTS]);
}

/* ---- PT35: story sync, engine part ---------------------------------------- */

static CoopStory coop_story_host;
static CoopStory coop_story_slot;
static CoopStory coop_story_other;
static PcQuestState coop_story_qflat[COOP_STORY_QUESTS];
static uint64_t coop_story_rflat[MAX_RUMORS];
static unsigned char coop_story_txblob[COOP_STORY_MAX_BLOB];
static CoopStoryRx coop_story_rx;
static unsigned int coop_story_snap;
static unsigned int coop_story_host_ms;
static unsigned int coop_story_sent_sig[8];
static unsigned int coop_story_sent_ms[8];
static int64_t coop_story_sent_pc[8];
static bool coop_story_sent_valid[8];
static unsigned int coop_story_applied_sig;
static bool coop_story_applied_valid;
static bool coop_story_pending;
static unsigned int coop_story_client_ms;

static void coop_lite_story_reset(void)
{
    coop_story_rx_reset(&coop_story_rx);
    coop_story_pending = false;
    coop_story_applied_valid = false;
    memset(coop_story_sent_valid, 0, sizeof(coop_story_sent_valid));
}

/* One PC's quest / rumor / reputation arrays (the globals are read apart). */
static void coop_story_read_pc(int64_t pc, CoopStory* s)
{
    int i;
    int n;

    coop_story_clear(s);
    if (pc == OBJ_HANDLE_NULL || obj_field_int32_get(pc, OBJ_F_TYPE) != OBJ_TYPE_PC) {
        return;
    }
    memset(coop_story_qflat, 0, sizeof(coop_story_qflat));
    obj_arrayfield_pc_quest_copy_to_flat(pc, OBJ_F_PC_QUEST_IDX, COOP_STORY_QUESTS - 1, coop_story_qflat);
    for (i = 0; i < COOP_STORY_QUESTS; i++) {
        unsigned int st = (unsigned int)coop_story_qflat[i].state;

        if (st != 0 && st <= 0x1FFu && (st & ~(unsigned int)COOP_STORY_BOTCHED) < COOP_STORY_STATES) {
            s->quest_state[i] = (unsigned short)st;
            s->quest_ts[i] = coop_story_qflat[i].datetime.value;
        }
    }
    memset(coop_story_rflat, 0, sizeof(coop_story_rflat));
    obj_arrayfield_pc_rumor_copy_to_flat(pc, OBJ_F_PC_RUMOR_IDX, MAX_RUMORS - 1, coop_story_rflat);
    for (i = 0; i < COOP_STORY_RUMORS; i++) {
        s->rumor_ts[i] = coop_story_rflat[i];
    }
    n = obj_arrayfield_length_get(pc, OBJ_F_PC_REPUTATION_IDX);
    if (n > COOP_STORY_REPS) {
        n = COOP_STORY_REPS;
    }
    for (i = 0; i < n; i++) {
        s->rep[i] = obj_arrayfield_uint32_get(pc, OBJ_F_PC_REPUTATION_IDX, i);
        s->rep_ts[i] = (uint64_t)obj_arrayfield_int64_get(pc, OBJ_F_PC_REPUTATION_TS_IDX, i);
    }
    s->n_rep = n;
}

static void coop_story_read_globals(CoopStory* s)
{
    int i;

    for (i = 0; i < COOP_STORY_GQUESTS; i++) {
        int st = quest_global_state_raw_get(i);

        s->gquest[i] = (unsigned char)(st >= 0 && st < COOP_STORY_STATES ? st : COOP_STORY_GQUEST_DEFAULT);
    }
    for (i = 0; i < COOP_STORY_RUMORS; i++) {
        s->rumor_quelled[i] = rumor_qstate_raw_get(i) ? 1 : 0;
    }
    for (i = 0; i < COOP_STORY_GFLAGS; i++) {
        s->gflag[i] = script_global_flag_get(i) ? 1 : 0;
    }
    for (i = 0; i < COOP_STORY_GVARS; i++) {
        s->gvar[i] = script_global_var_get(i);
    }
}

static void coop_story_send_to(int slot, const unsigned char* blob, size_t size)
{
    PacketCoopStorySync pkt;
    int count = coop_story_chunk_count(size);
    int i;
    int depth;

    coop_story_snap++;
    depth = tig_net_local_suspend_pause();
    for (i = 0; i < count; i++) {
        coop_story_chunk_fill(&pkt, coop_story_snap, blob, size, i);
        tig_net_send_app(&pkt, (int)(offsetof(PacketCoopStorySync, data) + pkt.len), slot);
    }
    tig_net_local_suspend_resume(depth);
}

/* Host: every 500 ms, rebuild each client's journal (the story PC's record
   folded with the client PC's own, plus the global quest / rumor bits) and
   send it when its signature changed, the client joined or its PC changed,
   or 30 s passed (cheap insurance for a client that lost its copy). */
static void coop_lite_host_story_tick(unsigned int now_ms)
{
    int slot;
    int64_t host_pc;
    bool read_host = false;

    if (!coop_lite_interval_elapsed(coop_story_host_ms, now_ms, 500u)) {
        return;
    }
    coop_story_host_ms = now_ms;
    host_pc = coop_lite_host_pc_obj();
    for (slot = 1; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);
        size_t size;
        unsigned int sig;

        if (pc == OBJ_HANDLE_NULL || !tig_net_client_is_active(slot)) {
            coop_story_sent_valid[slot] = false;
            continue;
        }
        if (host_pc == OBJ_HANDLE_NULL) {
            continue;
        }
        if (!read_host) {
            coop_story_read_pc(host_pc, &coop_story_host);
            coop_story_read_globals(&coop_story_host);
            read_host = true;
        }
        memcpy(&coop_story_slot, &coop_story_host, sizeof(coop_story_slot));
        if (pc != host_pc) {
            coop_story_read_pc(pc, &coop_story_other);
            coop_story_merge(&coop_story_slot, &coop_story_other);
        }
        size = coop_story_encode(&coop_story_slot, coop_story_txblob, sizeof(coop_story_txblob));
        if (size == 0) {
            continue;
        }
        sig = coop_story_blob_sig(coop_story_txblob, size);
        if (coop_story_sent_valid[slot] && sig == coop_story_sent_sig[slot] && pc == coop_story_sent_pc[slot]
            && !coop_lite_interval_elapsed(coop_story_sent_ms[slot], now_ms, 30000u)) {
            continue;
        }
        coop_story_sent_valid[slot] = true;
        coop_story_sent_sig[slot] = sig;
        coop_story_sent_pc[slot] = pc;
        coop_story_sent_ms[slot] = now_ms;
        coop_story_send_to(slot, coop_story_txblob, size);
        tig_debug_printf("CoopLite: story: slot %d snapshot sent (%d bytes, %d chunks, sig %08x)\n", slot,
            (int)size, coop_story_chunk_count(size), sig);
    }
}

/* Client: write the host's story into our own PC and the global tables. */
static bool coop_story_apply_local(const CoopStory* s)
{
    int64_t pc = player_get_local_pc_obj();
    int i;
    int n;
    int cur;
    int changed = 0;
    int added = 0;

    if (pc == OBJ_HANDLE_NULL || obj_field_int32_get(pc, OBJ_F_TYPE) != OBJ_TYPE_PC) {
        return false;
    }
    multiplayer_lock();
    coop_lite_apply_begin();
    coop_story_applying++;

    memset(coop_story_qflat, 0, sizeof(coop_story_qflat));
    obj_arrayfield_pc_quest_copy_to_flat(pc, OBJ_F_PC_QUEST_IDX, COOP_STORY_QUESTS - 1, coop_story_qflat);
    for (i = 0; i < COOP_STORY_QUESTS; i++) {
        unsigned int have = (unsigned int)coop_story_qflat[i].state;

        if (have != s->quest_state[i] || (s->quest_state[i] != 0 && coop_story_qflat[i].datetime.value != s->quest_ts[i])) {
            PcQuestState q;

            memset(&q, 0, sizeof(q));
            q.state = s->quest_state[i];
            q.datetime.value = s->quest_ts[i];
            obj_arrayfield_pc_quest_set(pc, OBJ_F_PC_QUEST_IDX, i, &q);
            changed++;
            added += s->quest_state[i] != 0;
        }
    }
    for (i = 0; i < COOP_STORY_GQUESTS; i++) {
        if (quest_global_state_raw_get(i) != s->gquest[i]) {
            quest_global_state_raw_set(i, s->gquest[i]);
            changed++;
        }
    }
    memset(coop_story_rflat, 0, sizeof(coop_story_rflat));
    obj_arrayfield_pc_rumor_copy_to_flat(pc, OBJ_F_PC_RUMOR_IDX, MAX_RUMORS - 1, coop_story_rflat);
    for (i = 0; i < COOP_STORY_RUMORS; i++) {
        if (coop_story_rflat[i] != s->rumor_ts[i]) {
            obj_arrayfield_int64_set(pc, OBJ_F_PC_RUMOR_IDX, i, (int64_t)s->rumor_ts[i]);
            changed++;
            added += s->rumor_ts[i] != 0;
        }
        if (rumor_qstate_raw_get(i) != (s->rumor_quelled[i] != 0)) {
            rumor_qstate_raw_set(i, s->rumor_quelled[i] != 0);
            changed++;
        }
    }
    cur = obj_arrayfield_length_get(pc, OBJ_F_PC_REPUTATION_IDX);
    n = s->n_rep;
    if (cur > n) {
        /* PT54d: length_set removes ONE key; drop every key >= n */
        for (i = cur + 64 - 1; i >= n; i--) {
            obj_arrayfield_length_set(pc, OBJ_F_PC_REPUTATION_IDX, i);
            obj_arrayfield_length_set(pc, OBJ_F_PC_REPUTATION_TS_IDX, i);
        }
        changed++;
        cur = n;
    }
    for (i = 0; i < n; i++) {
        if (i >= cur || obj_arrayfield_uint32_get(pc, OBJ_F_PC_REPUTATION_IDX, i) != s->rep[i]
            || (uint64_t)obj_arrayfield_int64_get(pc, OBJ_F_PC_REPUTATION_TS_IDX, i) != s->rep_ts[i]) {
            obj_arrayfield_uint32_set(pc, OBJ_F_PC_REPUTATION_IDX, i, s->rep[i]);
            obj_arrayfield_int64_set(pc, OBJ_F_PC_REPUTATION_TS_IDX, i, (int64_t)s->rep_ts[i]);
            changed++;
            added += i >= cur;
        }
    }
    /* PT77y: raw flag/var writes only; script_global_* does not run scripts. */
    for (i = 0; i < COOP_STORY_GFLAGS; i++) {
        int want = s->gflag[i] != 0;

        if (script_global_flag_get(i) != want) {
            script_global_flag_set(i, want);
            changed++;
        }
    }
    for (i = 0; i < COOP_STORY_GVARS; i++) {
        if (script_global_var_get(i) != s->gvar[i]) {
            script_global_var_set(i, s->gvar[i]);
            changed++;
        }
    }

    coop_story_applying--;
    coop_lite_apply_end();
    multiplayer_unlock();
    if (added > 0) {
        ui_toggle_primary_button(UI_PRIMARY_BUTTON_LOGBOOK, true);
    }
    if (changed > 0) {
        logbook_ui_coop_reload(); /* an open journal shows it now */
    }
    tig_debug_printf("CoopLite: story: applied (%d field changes, %d new entries)\n", changed, added);
    return true;
}

static void coop_lite_client_story_tick(unsigned int now_ms)
{
    if (!coop_story_pending || !coop_lite_interval_elapsed(coop_story_client_ms, now_ms, 250u)) {
        return;
    }
    coop_story_client_ms = now_ms;
    if (!coop_story_rx.complete) {
        coop_story_pending = false;
        return;
    }
    if (coop_story_applied_valid && coop_story_rx.sig == coop_story_applied_sig) {
        coop_story_pending = false;
        return;
    }
    if (!coop_story_decode(coop_story_rx.blob, coop_story_rx.total, &coop_story_host)) {
        tig_debug_printf("CoopLite: story: snapshot %08x does not parse - dropped\n", coop_story_rx.sig);
        coop_story_rx_reset(&coop_story_rx);
        coop_story_pending = false;
        return;
    }
    if (coop_story_apply_local(&coop_story_host)) {
        coop_story_applied_sig = coop_story_rx.sig;
        coop_story_applied_valid = true;
        coop_story_pending = false;
    }
}

void coop_lite_handle_story_sync(PacketCoopStorySync* pkt)
{
    int r;

    if (pkt == NULL || !tig_net_is_active() || tig_net_is_host()) {
        return;
    }
    r = coop_story_rx_add(&coop_story_rx, pkt, tig_net_last_message_size());
    if (r < 0) {
        tig_debug_printf("CoopLite: story: bad chunk dropped\n");
    } else if (r > 0) {
        coop_story_pending = true; /* applied by the client tick, once the world is ready */
        tig_debug_printf("CoopLite: story: snapshot %u complete (%u bytes)\n", coop_story_rx.snap, coop_story_rx.total);
    }
}

/* ---- PT36: dialog viewer + voting, game side ------------------------------- */

static CoopDialogVotes coop_dialog_votes;
static PacketCoopDialogState coop_dialog_tx; /* what the host last published */
static unsigned int coop_dialog_seq;
static unsigned int coop_dialog_line;
static unsigned int coop_dialog_sent_seq[8];
static CoopDialogView coop_dialog_view;
static unsigned int coop_dialog_rx_seq;
static bool coop_dialog_rx_seen;
static unsigned int coop_dialog_vote_ms;
static bool coop_dialog_vote_sent;
static bool coop_dialog_votes_init;
static void (*coop_dialog_show_hook)(const CoopDialogView* view);
static void (*coop_dialog_close_hook)(void);
static void (*coop_dialog_host_refresh_hook)(void);
/* PT41 party dialog (host): the client slot whose PC is talking, -1 none. */
static int coop_party_slot = -1;
static int64_t coop_party_pc;
static unsigned int coop_party_start_ms[8];
static unsigned int coop_party_choose_ms;
static bool (*coop_party_choose_hook)(int option);
/* client */
static unsigned int coop_party_talk_ms;
static bool coop_party_talk_sent;

void coop_lite_set_party_choose_hook(bool (*choose)(int option))
{
    coop_party_choose_hook = choose;
}

int64_t coop_lite_party_dialog_pc(void)
{
    return coop_party_slot > 0 ? coop_party_pc : OBJ_HANDLE_NULL;
}

static bool coop_dialog_is_speaker(void)
{
    return coop_dialog_view.open && coop_dialog_view.votes[COOP_DIALOG_SPEAKER_AT] != 0
        && coop_dialog_view.votes[COOP_DIALOG_SPEAKER_AT] == sub_529520();
}

void coop_lite_set_dialog_view_hooks(void (*show)(const CoopDialogView* view), void (*close)(void),
    void (*host_refresh)(void))
{
    coop_dialog_show_hook = show;
    coop_dialog_close_hook = close;
    coop_dialog_host_refresh_hook = host_refresh;
}

static void coop_lite_dialog_reset(void)
{
    if (coop_dialog_view.open && coop_dialog_close_hook != NULL) {
        coop_dialog_close_hook();
    }
    memset(&coop_dialog_view, 0, sizeof(coop_dialog_view));
    memset(&coop_dialog_tx, 0, sizeof(coop_dialog_tx));
    memset(coop_dialog_sent_seq, 0, sizeof(coop_dialog_sent_seq));
    coop_dialog_rx_seen = false;
    coop_dialog_rx_seq = 0;
    coop_dialog_vote_sent = false;
    coop_dialog_seq = 0;
    coop_dialog_votes_init = true;
    coop_dialog_votes_reset(&coop_dialog_votes);
    coop_party_slot = -1;
    coop_party_pc = OBJ_HANDLE_NULL;
    coop_party_choose_ms = 0;
    memset(coop_party_start_ms, 0, sizeof(coop_party_start_ms));
    coop_party_talk_sent = false;
}

static void coop_dialog_votes_ensure(void)
{
    if (!coop_dialog_votes_init) {
        coop_dialog_votes_init = true;
        coop_dialog_votes_reset(&coop_dialog_votes);
    }
}

/* Host: republish the current dialog with the current vote counts. */
static void coop_dialog_host_republish(void)
{
    unsigned char tally[COOP_DIALOG_MAX_OPTIONS];
    int i;

    coop_dialog_votes_tally(&coop_dialog_votes, tally);
    for (i = 0; i < COOP_DIALOG_MAX_OPTIONS; i++) {
        coop_dialog_tx.votes[i] = i < coop_dialog_tx.num_options ? tally[i] : 0;
    }
    coop_dialog_tx.seq = ++coop_dialog_seq;
    if (coop_party_slot > 0 && coop_dialog_show_hook != NULL) {
        /* the host watches a client's party dialog in the same panel */
        coop_dialog_view_from_packet(&coop_dialog_view, &coop_dialog_tx);
        coop_dialog_show_hook(&coop_dialog_view);
    }
}

void coop_lite_host_dialog_state(const char* npc_name, const char* reply, int num_options, const char* const* options)
{
    if (!tig_net_is_active() || !tig_net_is_host()) {
        return;
    }
    coop_dialog_votes_ensure();
    coop_dialog_line++;
    coop_dialog_votes_new_line(&coop_dialog_votes, coop_dialog_line, num_options);
    coop_dialog_fill_state(&coop_dialog_tx, 0, coop_dialog_line, npc_name, reply, num_options, options, NULL);
    coop_dialog_tx.votes[COOP_DIALOG_SPEAKER_AT] = (unsigned char)(coop_party_slot > 0 ? coop_party_slot : 0);
    coop_dialog_host_republish();
}

void coop_lite_host_dialog_closed(void)
{
    if (!tig_net_is_active() || !tig_net_is_host() || !coop_dialog_votes.open) {
        return;
    }
    coop_dialog_votes.open = false;
    coop_dialog_fill_close(&coop_dialog_tx, ++coop_dialog_seq, coop_dialog_line);
    if (coop_dialog_view.open && coop_dialog_close_hook != NULL) {
        coop_dialog_close_hook();
    }
    memset(&coop_dialog_view, 0, sizeof(coop_dialog_view));
}

void coop_lite_party_dialog_ended(int64_t pc_obj)
{
    if (coop_party_slot <= 0 || pc_obj != coop_party_pc) {
        return;
    }
    tig_debug_printf("CoopLite: party dialog: slot %d ended\n", coop_party_slot);
    coop_party_slot = -1;
    coop_party_pc = OBJ_HANDLE_NULL;
    coop_lite_host_dialog_closed();
}

static void coop_lite_host_slot_notice(int slot, int kind);

static void coop_lite_party_refuse(int slot, int64_t pc, int reason)
{
    tig_debug_printf("CoopLite: party dialog: slot %d refused (%d)\n", slot, reason);
    if (pc != OBJ_HANDLE_NULL) {
        coop_lite_host_slot_notice(slot, COOP_PARTY_TALK_REFUSED);
    }
}

/* Host: a client asks to talk to a story NPC. The dialog runs here with the
   client's PC (its stats drive the checks); everybody watches via the viewer. */
static void coop_lite_host_handle_party_start(const PacketCoopDialogState* pkt)
{
    int slot = sub_52A530();
    unsigned int now_ms = (unsigned int)(SDL_GetTicksNS() / 1000000u);
    int64_t pc;
    int64_t npc;
    int64_t host;
    int r;

    if (slot < 1 || slot >= 8) {
        return;
    }
    if (coop_party_start_ms[slot] != 0
        && !coop_lite_interval_elapsed(coop_party_start_ms[slot], now_ms, COOP_PARTY_TALK_INTERVAL_MS)) {
        return;
    }
    coop_party_start_ms[slot] = now_ms != 0 ? now_ms : 1;
    if (!map_is_valid() || map_is_clearing_objects()) {
        return;
    }
    pc = sub_4A2B60(slot);
    npc = coop_lite_get_oid_obj((const uint8_t*)pkt->npc_name);
    host = player_get_local_pc_obj();
    if (pc == OBJ_HANDLE_NULL || npc == OBJ_HANDLE_NULL || host == OBJ_HANDLE_NULL
        || obj_field_int32_get(npc, OBJ_F_TYPE) != OBJ_TYPE_NPC || critter_is_dead(npc) || critter_is_dead(pc)
        || critter_is_unconscious(pc)) {
        coop_lite_party_refuse(slot, pc, 5);
        return;
    }
    r = coop_lite_party_talk_check(slot, (int)object_dist(pc, npc), (int)object_dist(host, npc),
        coop_party_slot > 0 || coop_dialog_votes.open || combat_critter_is_combat_mode_active(host));
    if (r != 0) {
        coop_lite_party_refuse(slot, pc, r);
        return;
    }
    coop_party_slot = slot;
    coop_party_pc = pc;
    tig_debug_printf("CoopLite: party dialog: slot %d starts\n", slot);
    object_script_execute(pc, npc, OBJ_HANDLE_NULL, SAP_DIALOG, 0);
    if (!coop_dialog_votes.open && coop_party_slot == slot) {
        /* no dialog window (bark / one-liner): nothing to drive */
        coop_party_slot = -1;
        coop_party_pc = OBJ_HANDLE_NULL;
        tig_debug_printf("CoopLite: party dialog: slot %d got no dialog\n", slot);
    }
}

static void coop_lite_host_handle_party_choose(const PacketCoopDialogState* pkt)
{
    int slot = sub_52A530();
    unsigned int now_ms = (unsigned int)(SDL_GetTicksNS() / 1000000u);

    if (!coop_dialog_votes.open
        || !coop_lite_party_choose_ok(coop_party_slot, slot, pkt->line, coop_dialog_line, pkt->vote_index,
            coop_dialog_votes.num_options)) {
        tig_debug_printf("CoopLite: party dialog: choose from slot %d refused\n", slot);
        return;
    }
    if (coop_party_choose_ms != 0
        && !coop_lite_interval_elapsed(coop_party_choose_ms, now_ms, COOP_DIALOG_VOTE_INTERVAL_MS)) {
        return;
    }
    coop_party_choose_ms = now_ms != 0 ? now_ms : 1;
    tig_debug_printf("CoopLite: party dialog: slot %d chooses %d\n", slot, (int)pkt->vote_index);
    if (pkt->vote_index < 0) {
        if (coop_dialog_end_hook != NULL) {
            coop_dialog_end_hook(coop_party_pc, 0);
        }
        coop_lite_party_dialog_ended(coop_party_pc);
        return;
    }
    if (coop_party_choose_hook != NULL) {
        coop_party_choose_hook(pkt->vote_index);
    }
}

bool coop_lite_client_party_talk(int64_t npc_obj, int64_t pc_obj)
{
    PacketCoopDialogState pkt;
    uint8_t oid[24];
    unsigned int now_ms = (unsigned int)(SDL_GetTicksNS() / 1000000u);
    int depth;

    if (!tig_net_is_coop_client() || npc_obj == OBJ_HANDLE_NULL || !player_is_local_pc_obj(pc_obj)) {
        return false;
    }
    if (coop_party_talk_sent && !coop_lite_interval_elapsed(coop_party_talk_ms, now_ms, COOP_PARTY_TALK_INTERVAL_MS)) {
        return true;
    }
    coop_party_talk_sent = true;
    coop_party_talk_ms = now_ms;
    coop_lite_put_oid(oid, npc_obj);
    coop_dialog_fill_start(&pkt, oid);
    depth = tig_net_local_suspend_pause();
    tig_net_send_app(&pkt, COOP_DIALOG_HDR_SIZE + 24, 0);
    tig_net_local_suspend_resume(depth);
    tig_debug_printf("CoopLite: party dialog: start sent to host\n");
    return true;
}

bool coop_lite_client_dialog_leave(void)
{
    PacketCoopDialogState pkt;
    int depth;

    if (!coop_lite_is_client_role() || !coop_dialog_is_speaker()) {
        return false;
    }
    coop_dialog_fill_choose(&pkt, coop_dialog_view.line, -1);
    depth = tig_net_local_suspend_pause();
    tig_net_send_app(&pkt, COOP_DIALOG_HDR_SIZE, 0);
    tig_net_local_suspend_resume(depth);
    return true;
}

int coop_lite_host_dialog_vote_count(int option)
{
    unsigned char tally[COOP_DIALOG_MAX_OPTIONS];

    if (!coop_dialog_votes.open || option < 0 || option >= COOP_DIALOG_MAX_OPTIONS) {
        return 0;
    }
    coop_dialog_votes_tally(&coop_dialog_votes, tally);
    return tally[option];
}

/* Host: push the current state to every client that has not seen this seq;
   forget votes of clients that left. */
static void coop_lite_host_dialog_tick(unsigned int now_ms)
{
    int slot;
    int depth;
    bool changed = false;

    (void)now_ms;
    if (coop_party_slot > 0 && !tig_net_client_is_active(coop_party_slot)) {
        int64_t pc = coop_party_pc;

        tig_debug_printf("CoopLite: party dialog: speaker slot %d left\n", coop_party_slot);
        if (coop_dialog_end_hook != NULL) {
            coop_dialog_end_hook(pc, 0);
        }
        coop_lite_party_dialog_ended(pc);
    }
    if (coop_dialog_seq == 0) {
        return;
    }
    for (slot = 1; slot < 8; slot++) {
        if (!tig_net_client_is_active(slot)) {
            coop_dialog_sent_seq[slot] = 0;
            if (coop_dialog_votes.open && coop_dialog_vote_clear_slot(&coop_dialog_votes, slot)) {
                changed = true;
            }
        }
    }
    if (changed) {
        coop_dialog_host_republish();
        if (coop_dialog_host_refresh_hook != NULL) {
            coop_dialog_host_refresh_hook();
        }
    }
    depth = tig_net_local_suspend_pause();
    for (slot = 1; slot < 8; slot++) {
        if (!tig_net_client_is_active(slot) || coop_dialog_sent_seq[slot] == coop_dialog_tx.seq) {
            continue;
        }
        coop_dialog_sent_seq[slot] = coop_dialog_tx.seq;
        if (!coop_dialog_votes.open && coop_dialog_tx.action != COOP_DIALOG_ACT_CLOSE) {
            continue;
        }
        tig_net_send_app(&coop_dialog_tx, coop_dialog_wire_size(&coop_dialog_tx), slot);
    }
    tig_net_local_suspend_resume(depth);
}

static void coop_lite_host_handle_dialog_vote(const PacketCoopDialogState* pkt)
{
    int slot = sub_52A530();
    unsigned int now_ms = (unsigned int)(SDL_GetTicksNS() / 1000000u);

    if (slot < 1 || slot >= 8 || !coop_dialog_votes.open) {
        return;
    }
    if (!coop_dialog_vote_apply(&coop_dialog_votes, slot, pkt->vote_index, pkt->line, now_ms)) {
        return;
    }
    coop_dialog_host_republish();
    if (coop_dialog_host_refresh_hook != NULL) {
        coop_dialog_host_refresh_hook();
    }
    tig_debug_printf("CoopLite: dialog: slot %d votes option %d\n", slot, (int)pkt->vote_index);
}

void coop_lite_handle_dialog_state(PacketCoopDialogState* pkt)
{
    int size;

    if (pkt == NULL || !tig_net_is_active()) {
        return;
    }
    size = tig_net_last_message_size();
    if (tig_net_is_host()) {
        if (!coop_dialog_packet_valid(pkt, size, false)) {
            tig_debug_printf("CoopLite: dialog: bad vote from slot %d dropped\n", sub_52A530());
            return;
        }
        if (pkt->kind == COOP_DIALOG_KIND_START) {
            coop_lite_host_handle_party_start(pkt);
        } else if (pkt->kind == COOP_DIALOG_KIND_CHOOSE) {
            coop_lite_host_handle_party_choose(pkt);
        } else {
            coop_lite_host_handle_dialog_vote(pkt);
        }
        return;
    }
    if (!coop_dialog_packet_valid(pkt, size, true)) {
        tig_debug_printf("CoopLite: dialog: bad state packet dropped\n");
        return;
    }
    if (coop_dialog_rx_seen && (int)(pkt->seq - coop_dialog_rx_seq) <= 0) {
        return; /* stale or repeated */
    }
    coop_dialog_rx_seen = true;
    coop_dialog_rx_seq = pkt->seq;
    if (pkt->action == COOP_DIALOG_ACT_CLOSE) {
        if (coop_dialog_view.open && coop_dialog_close_hook != NULL) {
            coop_dialog_close_hook();
        }
        memset(&coop_dialog_view, 0, sizeof(coop_dialog_view));
        return;
    }
    {
        int my_vote = coop_dialog_view.open && coop_dialog_view.line == pkt->line ? coop_dialog_view.my_vote : -1;

        coop_dialog_view_from_packet(&coop_dialog_view, pkt);
        coop_dialog_view.my_vote = my_vote < coop_dialog_view.num_options ? my_vote : -1;
        if (coop_dialog_is_speaker()) {
            size_t n = strlen(coop_dialog_view.npc_name);

            snprintf(coop_dialog_view.npc_name + n, sizeof(coop_dialog_view.npc_name) - n, "%s",
                " - you answer (Esc: leave)");
        }
    }
    if (coop_dialog_show_hook != NULL) {
        coop_dialog_show_hook(&coop_dialog_view);
    }
}

bool coop_lite_client_dialog_vote(int option)
{
    PacketCoopDialogState pkt;
    unsigned int now_ms = (unsigned int)(SDL_GetTicksNS() / 1000000u);
    int idx;
    int depth;

    if (!coop_lite_is_client_role() || !tig_net_is_active() || !coop_dialog_view.open || option < 0
        || option >= coop_dialog_view.num_options) {
        return false;
    }
    if (coop_dialog_vote_sent && !coop_lite_interval_elapsed(coop_dialog_vote_ms, now_ms, COOP_DIALOG_VOTE_INTERVAL_MS)) {
        return false;
    }
    coop_dialog_vote_sent = true;
    coop_dialog_vote_ms = now_ms;
    if (coop_dialog_is_speaker()) {
        /* PT41: the speaker's click is the answer itself */
        idx = option;
        coop_dialog_fill_choose(&pkt, coop_dialog_view.line, idx);
    } else {
        idx = option == coop_dialog_view.my_vote ? -1 : option;
        coop_dialog_fill_vote(&pkt, coop_dialog_view.line, idx);
    }
    depth = tig_net_local_suspend_pause();
    tig_net_send_app(&pkt, COOP_DIALOG_HDR_SIZE, 0);
    tig_net_local_suspend_resume(depth);
    coop_dialog_view.my_vote = idx;
    if (coop_dialog_show_hook != NULL) {
        coop_dialog_show_hook(&coop_dialog_view);
    }
    return true;
}

static unsigned int coop_victim_watch_ms;

static void coop_lite_host_sync_slot_inven(int slot);

static void coop_lite_host_victim_watch(unsigned int now_ms)
{
    int slot;

    if (!coop_lite_interval_elapsed(coop_victim_watch_ms, now_ms, 250u)) {
        return;
    }
    coop_victim_watch_ms = now_ms;
    for (slot = 1; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);

        if (pc == OBJ_HANDLE_NULL) {
            coop_slot_inven_sig_valid[slot] = false;
            continue;
        }
        if (coop_slot_inven_sig_valid[slot] && coop_lite_pc_inven_sig(pc) != coop_slot_inven_sig[slot]) {
            tig_debug_printf("CoopLite: inven: slot %d PC's inventory changed on the host (loot / steal / script) - resync, gold %d\n",
                slot, item_gold_get(pc));
            coop_lite_host_sync_slot_inven(slot);
        }
    }
}

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
    for (o = 0; o < num_owners; o++) {
        if (owners[o] != OBJ_HANDLE_NULL && owners[o] == sub_4A2B60(slot)) {
            coop_slot_inven_sig[slot] = coop_lite_pc_inven_sig(owners[o]);
            coop_slot_inven_sig_valid[slot] = true;
        }
    }
    pkt = (PacketCoopInvenSync*)MALLOC(sizeof(*pkt));
    if (pkt == NULL) { tig_debug_printf("CoopLite: PT52e alloc failed\n"); return; }
    for (o = 0; o < num_owners; o++) {
        int64_t owner = owners[o];

        /* PT52f: skip an owner destroyed since the list was built */
        if (owner == OBJ_HANDLE_NULL || !obj_handle_is_valid(owner)
            || (obj_field_int32_get(owner, OBJ_F_FLAGS) & OF_DESTROYED) != 0) {
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

                /* PT26: a container's stacks travel as items (OID +
                   quantity); a critter's by total (gold/ammo fields). A
                   container's piles have no such field, and only the first
                   pile of one prototype was ever counted. */
                /* PT57: a critter's stacks are listed too (E_STACK), for
                   their grid cell only: the client maps them to its own
                   local stack of that type; quantity still by total. */
                if (pkt->count == COOP_INVEN_CHUNK_ENTRIES) {
                    pkt->flags = first ? COOP_INVEN_BATCH_FIRST : 0;
                    first = false;
                    pkt->flags |= coop_lite_inven_echo_flags(coop_layout_seq_applied[slot]); /* PT54 */
                    tig_net_send_app(pkt, (int)coop_lite_inven_sync_size(pkt->count), slot);
                    pkt->count = 0;
                }
                e = &pkt->entries[pkt->count++];
                coop_lite_put_oid(e->oid, items[idx]);
                e->proto = sub_49B290(items[idx]);
                e->inventory_location = item_inventory_location_get(items[idx]);
                e->item_flags = (unsigned int)obj_field_int32_get(items[idx], OBJ_F_ITEM_FLAGS);
                coop_lite_fill_item_state(e, items[idx]);
                if (coop_lite_is_stackable(items[idx])) {
                    e->state_flags = coop_lite_inven_state_flags(true, false);
                    e->mana_store = coop_lite_stack_qty(items[idx]);
                }
                if (coop_lite_env("ARCANUM_DEV_INVEN_TRACE") != NULL) {
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
            pkt->flags |= coop_lite_inven_echo_flags(coop_layout_seq_applied[slot]); /* PT54 */
                    tig_net_send_app(pkt, (int)coop_lite_inven_sync_size(pkt->count), slot);
        }
        item_list_free(items);
    }
    if (!first && num_owners > 0 && owners[num_owners - 1] == OBJ_HANDLE_NULL) {
        /* Last owner vanished: close the batch with an empty owner-less chunk. */
        memset(pkt, 0, offsetof(PacketCoopInvenSync, entries));
        pkt->type = PACKET_COOP_INVEN_SYNC;
        pkt->flags = COOP_INVEN_BATCH_LAST;
        pkt->flags |= coop_lite_inven_echo_flags(coop_layout_seq_applied[slot]); /* PT54 */
        tig_net_send_app(pkt, (int)coop_lite_inven_sync_size(0), slot);
    }
    FREE(pkt);
}

/* Host: re-send what a slot's barter window shows. */
static void coop_lite_host_sync_slot_inven(int slot)
{
    int64_t owners[4];
    int n = 0;
    int64_t pc = sub_4A2B60(slot);
    int64_t npc = OBJ_HANDLE_NULL;
    int64_t target = coop_lite_target_obj(slot);

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
    if (target != OBJ_HANDLE_NULL && target != pc && target != npc) {
        owners[n++] = target;
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
    const char* env = coop_lite_env("ARCANUM_DEV_CLIENT_ITEMS");
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

/* PT77m: is this container some merchant's stock (an NPC's substitute
   inventory)? A client could drag store goods to the barter window's drop
   slot (refused on the host, but the item lay on ITS ground) and pick that
   copy up: the pickup (Packet28) looted the store chest in reach for free. */
static bool coop_lite_container_is_store(int64_t container, int64_t pc)
{
    ObjectList npcs;
    ObjectNode* node;
    bool store = false;
    int slot;

    if (container == OBJ_HANDLE_NULL || obj_field_int32_get(container, OBJ_F_TYPE) != OBJ_TYPE_CONTAINER) {
        return false;
    }
    for (slot = 0; slot < 8; slot++) {
        if (coop_barter[slot].valid
            && coop_lite_store_of(obj_pool_perm_lookup(coop_barter[slot].npc)) == container) {
            return true;
        }
    }
    /* Prefer the chest itself: the store can be up to 20 tiles from the NPC. */
    object_list_vicinity(container, OBJ_TM_NPC, &npcs);
    for (node = npcs.head; node != NULL; node = node->next) {
        if (coop_lite_store_of(node->obj) == container) {
            store = true;
            break;
        }
    }
    object_list_destroy(&npcs);
    if (!store && pc != OBJ_HANDLE_NULL && pc != container) {
        object_list_vicinity(pc, OBJ_TM_NPC, &npcs);
        for (node = npcs.head; node != NULL; node = node->next) {
            if (coop_lite_store_of(node->obj) == container) {
                store = true;
                break;
            }
        }
        object_list_destroy(&npcs);
    }
    return store;
}

static bool coop_lite_parent_holds_merchant_stock(int64_t parent, int64_t near_obj)
{
    int typ;

    if (parent == OBJ_HANDLE_NULL) {
        return false;
    }
    typ = obj_field_int32_get(parent, OBJ_F_TYPE);
    if (typ == OBJ_TYPE_CONTAINER) {
        return coop_lite_container_is_store(parent, near_obj);
    }
    /* Equipped / carried by a living merchant, not a companion. */
    if (typ == OBJ_TYPE_NPC
        && !critter_is_dead(parent)
        && critter_pc_leader_get(parent) == OBJ_HANDLE_NULL
        && coop_lite_npc_is_merchant(parent)) {
        return true;
    }
    return false;
}

bool coop_lite_refuse_merchant_stock_drop(int64_t item)
{
    int64_t parent = OBJ_HANDLE_NULL;
    int64_t near_obj;

    if (item == OBJ_HANDLE_NULL
        || !tig_net_is_active()
        || !tig_net_is_coop_enabled()) {
        return false;
    }
    if (!item_parent(item, &parent) || parent == OBJ_HANDLE_NULL) {
        return false;
    }
    near_obj = player_get_local_pc_obj();
    if (near_obj == OBJ_HANDLE_NULL) {
        near_obj = parent;
    }
    if (!coop_lite_parent_holds_merchant_stock(parent, near_obj)) {
        return false;
    }
    tig_debug_printf("CoopLite: refused drop of a merchant's item (barter drop-slot)\n");
    return true;
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
    bool ground_stack = false;
    ObjectID picked_oid;
    const char* what = "?";

    memset(&picked_oid, 0, sizeof(picked_oid));
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
            /* Review #2 pt 3: a stack merges into ours and is destroyed;
               keep its id, the picker's copy must go too. */
            picked_oid = obj_get_id(item);
            ground_stack = coop_lite_is_stackable(item);
        } else if (parent != pc) {
            /* Loot from an unlocked container in reach; never from critters. */
            if (obj_field_int32_get(parent, OBJ_F_TYPE) != OBJ_TYPE_CONTAINER
                || object_locked_get(parent)
                || !coop_lite_item_near(pc, obj_field_int64_get(parent, OBJ_F_LOCATION))) {
                break;
            }
            if (coop_lite_container_is_store(parent, pc)) {
                tig_debug_printf("CoopLite: item op: slot %d tried to take a merchant's stock for free - refused\n", slot);
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
        /* PT20 review: the price is the host's (inven_ui sub_5786C0), never
           the client's number (cost 0 identified for free). */
        int price = spell_money(SPELL_DIVINE_MAGICK);

        what = "identify";
        item = obj_pool_perm_lookup(pkt->item_oid);
        if (item != OBJ_HANDLE_NULL && obj_pool_perm_lookup(pkt->pc_oid) == pc
            && item_parent(item, &parent) && parent == pc
            && !item_is_identified(item)
            && coop_lite_barter_partner_ok(pc, npc)
            && price > 0 && price <= item_gold_get(pc)) {
            item_perform_identify_service(item, npc, pc, price);
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

        if (!ground_stack) {
            coop_lite_clear_player_drop(item); /* back in a PC's hands */
        }
        memset(&gone, 0, sizeof(gone));
        gone.type = 72;
        gone.oid = picked_oid;
        if (ground_stack) {
            /* INVEN_SYNC carries gold/ammo as totals, so nothing would
               remove the picker's ground copy (a clickable ghost pile). */
            tig_net_send_app_all(&gone, sizeof(gone));
        } else {
            /* The picker's INVEN_SYNC moves its own copy in by OID. */
            tig_net_send_app_except(&gone, sizeof(gone), slot);
        }
    }
}
#else
bool coop_lite_refuse_merchant_stock_drop(int64_t item)
{
    (void)item;
    return false;
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
        coop_lite_barter_end(slot, "closed by the client");
        return;
    }
    if (pkt->kind == COOP_BARTER_SYNC) {
        coop_barter[slot].valid = false;
        coop_barter[slot].target_valid = false;
        coop_lite_host_sync_slot_inven(slot);
        return;
    }
    if (pkt->kind == COOP_BARTER_OPEN_LOOT || pkt->kind == COOP_BARTER_OPEN_STEAL) {
        /* PT18: loot (container, any corpse) / pickpocket (living critter,
           NPC or PC) window. Inventory, not dialog: no merchant rule. */
        int64_t target = coop_lite_get_oid_obj(pkt->npc_oid);
        int want = pkt->kind == COOP_BARTER_OPEN_LOOT ? COOP_TARGET_LOOT : COOP_TARGET_STEAL;
        int kind = coop_lite_target_kind_of(pc, target);
        char tname[128];

        coop_barter[slot].valid = false;
        coop_barter[slot].target_valid = false;
        tname[0] = '\0';
        if (target != OBJ_HANDLE_NULL) {
            object_examine(target, pc, tname);
        }
        if (kind != want) {
            tig_debug_printf("CoopLite: loot: slot %d refused %s window on '%s' (allowed: %d)\n", slot,
                want == COOP_TARGET_STEAL ? "pickpocket" : "loot", tname, kind);
            coop_lite_host_sync_slot_inven(slot);
            return;
        }
        if (obj_field_int32_get(target, OBJ_F_TYPE) == OBJ_TYPE_CONTAINER
            && (obj_field_int32_get(target, OBJ_F_CONTAINER_FLAGS) & OCOF_INVEN_SPAWN_ONCE) != 0) {
            /* PT33 item 4: first open of auto-generated loot. The host rolls
               it (the client's own roll is gone), so the sync below carries
               the stock instead of an empty container. */
            sub_463E20(target);
            tig_debug_printf("CoopLite: loot: slot %d first open: host generated the container's stock\n", slot);
        }
        coop_barter[slot].target_valid = true;
        coop_barter[slot].target_kind = kind;
        coop_barter[slot].target = obj_get_id(target);
        tig_debug_printf("CoopLite: loot: slot %d opened %s window on '%s' (%d items)\n", slot,
            kind == COOP_TARGET_STEAL ? "pickpocket" : "loot", tname,
            obj_field_int32_get(target, obj_field_int32_get(target, OBJ_F_TYPE) == OBJ_TYPE_CONTAINER
                    ? OBJ_F_CONTAINER_INVENTORY_NUM
                    : OBJ_F_CRITTER_INVENTORY_NUM));
        coop_lite_host_sync_slot_inven(slot);
        return;
    }
    if (pkt->kind != COOP_BARTER_OPEN) {
        return;
    }
    npc = coop_lite_get_oid_obj(pkt->npc_oid);
    if (!coop_lite_barter_partner_ok(pc, npc)) {
        tig_debug_printf("CoopLite: barter: slot %d refused (not a merchant / too far / dead)\n", slot);
        if (npc != OBJ_HANDLE_NULL) {
            /* PT23: resend this NPC's state on the next NPC_POS tick (a
               client that talks to a corpse has a stale copy of it). */
            CoopNpcCacheEntry* c = coop_lite_npc_cache_slot(npc);

            if (c->obj == npc) {
                c->sent_ms = coop_obj_now_ms - 5000u;
                memset(&c->last, 0, sizeof(c->last));
            }
        }
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

#ifndef COOP_LITE_TEST_STUBS
/* PT18: a notice for one slot (0 = the host itself). */
static void coop_lite_host_slot_notice(int slot, int kind)
{
    PacketCoopPartyState pkt;
    char str[512];

    if (slot == 0) {
        coop_lite_party_notice_text(kind, 0, true, str, sizeof(str));
        if (str[0] != '\0') {
            coop_lite_show_notice(str, coop_lite_party_notice_is_good(kind) ? TF_TYPE_GREEN : TF_TYPE_RED);
        }
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_PARTY_STATE;
    pkt.kind = kind;
    pkt.slot = slot;
    tig_net_send_app(&pkt, sizeof(pkt), slot);
}

/*
 * PT18: Packet81 between a client PC and its loot / pickpocket target.
 * Loot (container, corpse): the inventory window's own transfer, like
 * barter at price 0. Pickpocket: the host rolls — against an NPC the vanilla
 * skill invocation (item move, alignment, detection and ai_attack); against
 * a PC the same odds, and a failure is "Пойман на краже! / Caught pickpocketing!" for both.
 */
static void coop_lite_host_target_transfer(int slot, int64_t pc, int64_t target, struct Packet81* pkt,
    int64_t item, int64_t from, int64_t to, int qty)
{
    int kind = coop_barter[slot].target_kind;
    bool from_target = from == target;
    int64_t parent = OBJ_HANDLE_NULL;
    bool ok = false;
    const char* result = "refused";
    int victim_slot = coop_lite_slot_of_obj(target);
    char iname[128];

    iname[0] = '\0';
    if (item != OBJ_HANDLE_NULL) {
        object_examine(item, pc, iname);
    }
    if (coop_lite_target_kind_of(pc, target) != kind
        || item == OBJ_HANDLE_NULL
        || !item_parent(item, &parent) || parent != from
        || qty <= 0
        || !coop_lite_target_transfer_ok(kind, (unsigned int)pkt->field_8, from_target,
            IS_WEAR_INV_LOC(item_inventory_location_get(item)))) {
        /* refused */
    } else if (kind == COOP_TARGET_LOOT) {
        Packet81* copy = (Packet81*)MALLOC(sizeof(*copy));
        if (copy == NULL) { tig_debug_printf("CoopLite: PT52e alloc failed\n"); coop_lite_host_sync_slot_inven(slot); return; }

        *copy = *pkt;
        copy->field_5C = 0;
        copy->field_70 = obj_get_id(target);
        copy->field_88 = obj_get_id(target);
        sub_460960(copy); /* inven_ui's transfer (sub_578EA0); frees `copy` */
        ok = coop_lite_is_stackable(item) || (item_parent(item, &parent) && parent == to);
        if (coop_lite_loot_retry_auto_slot(ok, coop_lite_is_stackable(item),
                item_parent(item, &parent) && parent == from)) {
            /* PT46: the client picked a grid cell / slot of its own copy of
               the pack; on the host that cell is taken (packs drift), so
               the vanilla move failed. Let the host place it. */
            ok = item_transfer(item, to);
            if (ok) {
                tig_debug_printf("CoopLite: loot: slot %d item placed by the host (client's cell was taken)\n", slot);
            }
        }
        result = ok ? "done" : "failed";
        if (!ok && item_parent(item, &parent) && parent == from) {
            /* PT53: say why (worn-on-corpse / too heavy / no room on the
               host's copy of the pack), so a refusal is diagnosable. */
            tig_debug_printf("CoopLite: loot: slot %d why: remove %d, insert %d, src loc %d\n", slot,
                item_check_remove(item), item_check_insert(item, to, NULL), item_inventory_location_get(item));
        }
    } else if (obj_field_int32_get(target, OBJ_F_TYPE) != OBJ_TYPE_PC) {
        SkillInvocation si;

        skill_invocation_init(&si);
        sub_4440E0(pc, &(si.source));
        sub_4440E0(target, &(si.target));
        sub_4440E0(item, &(si.item));
        si.skill = SKILL_PICK_POCKET;
        if (!from_target) {
            si.flags |= SKILL_INVOCATION_0x02; /* plant */
        }
        if (coop_lite_env("ARCANUM_DEV_STEAL_FORCE") != NULL) {
            si.flags |= SKILL_INVOCATION_FORCED; /* dev: test the success path */
        }
        skill_invocation_run(&si);
        ok = (si.flags & SKILL_INVOCATION_SUCCESS) != 0;
        result = ok ? "succeeded" : "failed";
        /* gui3 BUG C: the roll ran here, so the thief's own "Получилось. / You succeed." /
           failure line has to be sent (vanilla prints it on the host only). */
        coop_lite_host_slot_notice(slot, ok ? COOP_PARTY_STEAL_OK : COOP_PARTY_STEAL_FAILED);
    } else if (coop_lite_party_gift(true, true, from_target)) {
        /* PT45: putting an item into another player's pack is a gift, not
           a plant: no roll, always lands (host-authoritative, both synced). */
        int t = obj_field_int32_get(item, OBJ_F_TYPE);

        if (t == OBJ_TYPE_GOLD) {
            ok = item_gold_transfer(from, to, qty, item);
        } else if (t == OBJ_TYPE_AMMO) {
            ok = item_ammo_transfer(from, to, qty, obj_field_int32_get(item, OBJ_F_AMMO_TYPE), item);
        } else {
            ok = item_transfer(item, to);
        }
        result = ok ? "given" : "failed";
        coop_lite_host_slot_notice(slot, ok ? COOP_PARTY_STEAL_OK : COOP_PARTY_SKILL_FAILED);
    } else {
        SkillInvocation si;
        int effectiveness;
        int difficulty;

        skill_invocation_init(&si);
        sub_4440E0(pc, &(si.source));
        sub_4440E0(target, &(si.target));
        sub_4440E0(item, &(si.item));
        si.skill = SKILL_PICK_POCKET;
        effectiveness = basic_skill_effectiveness(pc, BASIC_SKILL_PICK_POCKET, target);
        difficulty = skill_invocation_difficulty(&si);
        if (difficulty + random_between(1, 100) <= effectiveness || coop_lite_env("ARCANUM_DEV_STEAL_FORCE") != NULL) {
            int t = obj_field_int32_get(item, OBJ_F_TYPE);

            if (t == OBJ_TYPE_GOLD) {
                ok = item_gold_transfer(from, to, qty > 100 ? 100 : qty, item);
            } else if (t == OBJ_TYPE_AMMO) {
                ok = item_ammo_transfer(from, to, qty > 20 ? 20 : qty, obj_field_int32_get(item, OBJ_F_AMMO_TYPE), item);
            } else {
                ok = item_transfer(item, to);
            }
            result = ok ? "succeeded" : "failed";
            coop_lite_host_slot_notice(slot, ok ? COOP_PARTY_STEAL_OK : COOP_PARTY_STEAL_FAILED);
        } else {
            result = "caught";
            coop_lite_host_slot_notice(slot, COOP_PARTY_CAUGHT_THIEF);
            if (victim_slot >= 0 && victim_slot != slot) {
                coop_lite_host_slot_notice(victim_slot, COOP_PARTY_CAUGHT_THIEF);
            }
        }
    }
    tig_debug_printf("CoopLite: loot: slot %d %s '%s' %s target: %s\n", slot,
        kind == COOP_TARGET_STEAL ? (from_target ? "steals" : "plants") : (from_target ? "takes" : "puts"),
        iname, from_target ? "from" : "into", result);
    coop_lite_host_sync_slot_inven(slot);
    if (victim_slot > 0 && victim_slot != slot) {
        coop_lite_host_sync_slot_inven(victim_slot);
    }
}
#endif

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
    if (critter_is_dead(pc)) {
        /* PT18: a corpse does not rearrange or drop its things. */
        tig_debug_printf("CoopLite: inven: slot %d transfer refused (PC is dead)\n", slot);
        coop_lite_host_sync_slot_inven(slot);
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

        int64_t pile = OBJ_HANDLE_NULL;
        int t = item != OBJ_HANDLE_NULL ? obj_field_int32_get(item, OBJ_F_TYPE) : -1;

        if (coop_lite_client_transfer_flags_allowed((unsigned int)pkt->field_8)
            && item != OBJ_HANDLE_NULL && from == pc
            && item_parent(item, &parent) && parent == pc) {
            int64_t loc = obj_field_int64_get(pc, OBJ_F_LOCATION);

            multiplayer_lock();
            if (t == OBJ_TYPE_GOLD) {
                /* Review #2 pt 4: a gold pile of qty, as inven_ui does on
                   the host (transfer out, then a new pile; Packet70 + the
                   blob give clients the real amount). */
                if (qty > 0 && qty <= item_gold_get(pc)
                    && item_gold_transfer(pc, OBJ_HANDLE_NULL, qty, item)) {
                    pile = item_gold_create(qty, loc);
                    ok = pile != OBJ_HANDLE_NULL;
                    coop_lite_mark_player_drop(pile);
                }
            } else if (t == OBJ_TYPE_AMMO) {
                int ammo_type = obj_field_int32_get(item, OBJ_F_AMMO_TYPE);

                if (qty > 0 && qty <= item_ammo_quantity_get(pc, ammo_type)
                    && item_ammo_transfer(pc, OBJ_HANDLE_NULL, qty, ammo_type, item)) {
                    pile = item_ammo_create(qty, ammo_type, loc);
                    ok = pile != OBJ_HANDLE_NULL;
                    coop_lite_mark_player_drop(pile);
                }
            } else {
                ok = item_drop(item);
                pile = item;
            }
            multiplayer_unlock();
        }
        tig_debug_printf("CoopLite: inven: slot %d drop %s%s\n", slot,
            t == OBJ_TYPE_GOLD ? "gold " : (t == OBJ_TYPE_AMMO ? "ammo " : ""), ok ? "done" : "refused");
        if (ok && pile == item) {
            coop_lite_host_announce_ground_item(item); /* stacks: mp_object_create did */
        }
        coop_lite_host_sync_slot_inven(slot);
        return;
    }
    {
        int64_t target = coop_lite_target_obj(slot);

        if (target != OBJ_HANDLE_NULL && ((from == target && to == pc) || (from == pc && to == target))) {
            coop_lite_host_target_transfer(slot, pc, target, pkt, item, from, to, qty);
            return;
        }
    }
    /* PT28 item 1: equip slot <-> backpack on the client's own PC is legal
       with no barter / loot session (the window's merchant and store fields
       may still name whatever the client last had open). */
    if (from == pc && to == pc && item != OBJ_HANDLE_NULL
        && coop_lite_client_transfer_flags_allowed((unsigned int)pkt->field_8)
        && (((unsigned int)pkt->field_8) & (COOP_XFER_STEAL | COOP_XFER_PLANT)) == 0
        && item_parent(item, &parent) && parent == pc && qty > 0) {
        copy = (Packet81*)MALLOC(sizeof(*copy));
        if (copy == NULL) { tig_debug_printf("CoopLite: PT52e alloc failed\n"); coop_lite_host_sync_slot_inven(slot); return; }
        *copy = *pkt;
        copy->field_5C = 0;
        copy->field_70.type = OID_TYPE_NULL;
        copy->field_88.type = OID_TYPE_NULL;
        tig_debug_printf("CoopLite: inven: slot %d own move (equip/backpack) to loc %d\n", slot, pkt->field_58);
        sub_460960(copy); /* frees `copy` */
        /* The host's copy is the truth now: the resync below carries the new
           slot, and the victim watch takes it as the known state. */
        coop_lite_host_sync_slot_inven(slot);
        coop_slot_inven_sig[slot] = coop_lite_pc_inven_sig(pc);
        coop_slot_inven_sig_valid[slot] = true;
        return;
    }
    if (!coop_lite_client_transfer_flags_allowed((unsigned int)pkt->field_8)
        || (((unsigned int)pkt->field_8) & (COOP_XFER_STEAL | COOP_XFER_PLANT)) != 0
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
        if (npc != OBJ_HANDLE_NULL) {
            /* PT41 diagnosis: which barter check failed */
            tig_debug_printf("CoopLite: barter: slot %d refused: item=%d parent_ok=%d qty=%d partner_ok=%d npc_oid_ok=%d store_ok=%d\n",
                slot, item != OBJ_HANDLE_NULL ? 1 : 0,
                item != OBJ_HANDLE_NULL && item_parent(item, &parent) && parent == from ? 1 : 0, qty,
                coop_lite_barter_partner_ok(pc, npc) ? 1 : 0, obj_pool_perm_lookup(pkt->field_70) == npc ? 1 : 0,
                pkt->field_88.type == OID_TYPE_NULL
                        || obj_pool_perm_lookup(pkt->field_88) == (store != OBJ_HANDLE_NULL ? store : npc)
                    ? 1
                    : 0);
        }
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
            tig_debug_printf("CoopLite: barter: slot %d merchant will not sell that\n", slot);
            coop_lite_host_sync_slot_inven(slot);
            return;
        }
        if (cost <= 0) {
            tig_debug_printf("CoopLite: barter: slot %d merchant will not buy that\n", slot);
            coop_lite_host_sync_slot_inven(slot);
            return;
        }
        if (item_gold_get(to == pc ? pc : npc) < cost * qty) {
            tig_debug_printf("CoopLite: barter: slot %d not enough gold (%d < %d)\n", slot,
                item_gold_get(to == pc ? pc : npc), cost * qty);
            coop_lite_host_sync_slot_inven(slot);
            return;
        }
    }

    copy = (Packet81*)MALLOC(sizeof(*copy));
    if (copy == NULL) { tig_debug_printf("CoopLite: PT52e alloc failed\n"); coop_lite_host_sync_slot_inven(slot); return; }
    *copy = *pkt;
    copy->field_5C = cost;
    tig_debug_printf("CoopLite: inven: slot %d transfer qty %d cost %d\n", slot, qty, cost);
    if (npc != OBJ_HANDLE_NULL && from != to) {
        tig_debug_printf("CoopLite: barter: slot %d %s qty %d for %d each (pc gold %d, merchant gold %d)\n", slot,
            to == pc ? "buys" : "sells", qty, cost, item_gold_get(pc), item_gold_get(npc));
    }
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
        tig_debug_printf("CoopLite: barter: client request kind %d sent\n", kind);
    } else {
        tig_debug_printf("CoopLite: barter: client request kind %d NOT sent (no session)\n", kind);
    }
    tig_net_local_suspend_resume(depth);
#else
    (void)pc_obj;
    (void)npc_obj;
    (void)kind;
#endif
}

void coop_lite_client_request_steal(int64_t pc_obj, int64_t target_obj, int64_t item_obj, bool plant)
{
#ifndef COOP_LITE_TEST_STUBS
    Packet81 pkt;
    int depth;
    int t;

    if (pc_obj == OBJ_HANDLE_NULL || target_obj == OBJ_HANDLE_NULL || item_obj == OBJ_HANDLE_NULL
        || !tig_net_is_active() || tig_net_is_host()) {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = 81;
    pkt.field_8 = (int)(0x01u | (plant ? COOP_XFER_PLANT : COOP_XFER_STEAL));
    pkt.field_10 = obj_get_id(item_obj);
    coop_lite_client_map_stack_oid(&(pkt.field_10));
    pkt.field_28 = obj_get_id(plant ? pc_obj : target_obj);
    pkt.field_40 = obj_get_id(plant ? target_obj : pc_obj);
    pkt.field_58 = -1;
    pkt.field_5C = -1;
    t = obj_field_int32_get(item_obj, OBJ_F_TYPE);
    pkt.field_60 = t == OBJ_TYPE_GOLD
        ? item_gold_get(item_obj)
        : (t == OBJ_TYPE_AMMO ? obj_field_int32_get(item_obj, OBJ_F_AMMO_QUANTITY) : 1);
    pkt.field_68 = sub_529520();
    pkt.field_70 = obj_get_id(target_obj);
    pkt.field_88 = obj_get_id(target_obj);
    pkt.field_A0 = obj_get_id(pc_obj);
    depth = tig_net_local_suspend_pause();
    tig_net_send_app_all(&pkt, sizeof(pkt));
    tig_net_local_suspend_resume(depth);
    {
        char iname[128];
        char tname[128];

        iname[0] = tname[0] = '\0';
        object_examine(item_obj, pc_obj, iname);
        object_examine(target_obj, pc_obj, tname);
        /* gui3: name the item and the direction, so a panel mix-up is visible. */
        tig_debug_printf("CoopLite: loot: asked the host to %s an item ('%s' %s '%s')\n",
            plant ? "plant" : "steal", iname, plant ? "from my PC into" : "into my PC from", tname);
    }
#else
    (void)pc_obj;
    (void)target_obj;
    (void)item_obj;
    (void)plant;
#endif
}

bool coop_lite_client_local_loot(int64_t source_obj)
{
#ifndef COOP_LITE_TEST_STUBS
    return coop_lite_is_client_role() && source_obj != OBJ_HANDLE_NULL && source_obj == player_get_local_pc_obj();
#else
    (void)source_obj;
    return false;
#endif
}

bool coop_lite_client_opens_inven_locally(int mode, int64_t pc_obj)
{
#ifndef COOP_LITE_TEST_STUBS
    /* INVEN_UI_MODE_INVENTORY = 0, INVEN_UI_MODE_BARTER = 1, PT18:
       INVEN_UI_MODE_LOOT = 2, INVEN_UI_MODE_STEAL = 3 (the host validates
       the target with COOP_BARTER_OPEN_LOOT / _STEAL). */
    if (!coop_lite_is_client_role()) {
        return false;
    }
    if (mode < 0 || mode > 3) {
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
#ifndef COOP_LITE_TEST_STUBS
static void coop_lite_client_layout_note_host(int64_t local_pc);
static int coop_lite_layout_nearest_cell(int64_t pc, int64_t item, int want); /* PT54 */
static bool coop_lite_client_layout_hold(void)
{
    if (coop_layout_sent_seq != 0 && coop_lite_interval_elapsed(coop_layout_sent_ms, coop_obj_now_ms, 5000u)) {
        tig_debug_printf("CoopLite: inven: layout %u not echoed in 5 s - host copy taken\n", coop_layout_sent_seq);
        coop_layout_sent_seq = 0; /* no echo (refused before apply): the host wins */
    }
    return !coop_lite_inven_echo_reached(coop_layout_sent_seq, coop_inv_echo_seq);
}
#endif
static int coop_inv_owner_cnt;
static PacketCoopInvenEntry coop_inv_entry[COOP_INV_BUF_ENTRIES];
static int coop_inv_entry_cnt;
static bool coop_inv_overflow;

static bool coop_lite_inv_listed(const CoopInvBufOwner* o, ObjectID oid)
{
    int i;

    for (i = o->first; i < o->first + o->count; i++) {
        if (coop_lite_oid_raw_equal(coop_inv_entry[i].oid, &oid)) {
            return true;
        }
    }
    return false;
}

/* PT27 item 3: an item we destroyed (an owner's sync no longer listed it -
   the host had just dropped it or moved it into a chest) stays in the
   object pool, OF_DESTROYED, until the map closes. Its OID then looked
   "known" to the next ground/container listing, which skipped it forever:
   the host's drop never appeared here. Free it for real so the listing
   fetches the host's object afresh. Returns OBJ_HANDLE_NULL when freed. */
static int coop_world_reclaimed; /* PT27: destroyed copies freed for a refetch */

static int64_t coop_lite_client_reclaim_destroyed(int64_t obj)
{
#ifndef COOP_LITE_TEST_STUBS
    if (obj == OBJ_HANDLE_NULL || tig_net_is_host()
        || (obj_field_int32_get(obj, OBJ_F_FLAGS) & OF_DESTROYED) == 0
        || (obj_field_int32_get(obj, OBJ_F_FLAGS) & OF_INVENTORY) != 0
        || !item_is_item(obj)) {
        return obj;
    }
    sub_43CF70(obj);
    object_delete(obj);
    coop_world_reclaimed++;
    return OBJ_HANDLE_NULL;
#else
    return obj;
#endif
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
    const char* env = coop_lite_env("ARCANUM_COOP_NO_OBJ_BLOB");

    return env == NULL || env[0] != '1';
}

static CoopObjPending* coop_lite_obj_pending_find(const uint8_t* oid)
{
    int i;

    for (i = 0; i < COOP_OBJ_PENDING_MAX; i++) {
        if (coop_obj_pending[i].used && coop_lite_oid_raw_equal(coop_obj_pending[i].oid, oid)) {
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

/* PT27 item 3: true when a ground object at loc can live here. An item the
   host announced (Packet70) or listed far away, in a sector this client
   has not loaded, used to be created anyway: it sat in the object pool,
   known by its OID, but in no loaded sector's list - drawn nowhere. When
   the host later dropped it next to us, the world state found it "known"
   and never fetched it again: it stayed invisible for good. */
bool coop_lite_client_ground_loc_live(int64_t loc)
{
#ifndef COOP_LITE_TEST_STUBS
    return loc != 0 && sector_loaded(sector_id_from_loc(loc));
#else
    (void)loc;
    return true;
#endif
}

bool coop_lite_client_want_ground_blob(const unsigned char oid[24], int64_t loc, int proto)
{
    CoopObjPending* p;

    if (tig_net_is_host() || !coop_lite_obj_blobs_enabled()) {
        return false;
    }
    if (!coop_lite_client_ground_loc_live(loc)) {
        /* Far away: nothing to show. The world state lists it again once
           it lies near our PC. */
        return true;
    }
    if ((p = coop_lite_obj_pending_find(oid)) != NULL) {
        if (p->ground) {
            p->ground_loc = loc; /* where the host has it now */
            p->ground_proto = proto;
        }
        return true;
    }
    if (!coop_lite_obj_blob_defer(oid)) {
        return false;
    }
    p = coop_lite_obj_pending_find(oid);
    if (p == NULL) { /* PT53c */
        return false;
    }
    p->ground = true;
    p->ground_loc = loc;
    p->ground_proto = proto;
    coop_lite_obj_req_flush();
    return true;
}

static bool coop_lite_interp_step_mode(void)
{
    static int mode = -1;

    if (mode < 0) {
        const char* v = coop_lite_env("ARCANUM_DEV_INTERP");

        mode = (v != NULL && strcmp(v, "lerp") == 0) ? 0 : 1;
        tig_debug_printf("CoopLite: remote PC motion: %s\n", mode ? "stepwise (default)" : "lerp (ARCANUM_DEV_INTERP=lerp)");
    }
    return mode == 1;
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
        {
            int run = anim_find_first(obj);

            if (run != -1) {
                static int logged;

                coop_diag.peer_anim++;
                if (logged++ < 10) {
                    tig_debug_printf("CoopLite: interp: slot %d PC has a local anim run (goal %d)\n", slot,
                        anim_run_info[run].goals[0].type);
                }
            }
        }
        kind = coop_interp_pick(ring, now_ms - COOP_INTERP_DELAY_MS, &ia, &ib, &num, &den);
        if (kind == 0) {
            continue;
        }
        /* PT31 part B: the engine moves a sprite in discrete steps - the art
           frame and the offset advance together (sub_4305D0). Lerping the
           offset between samples while the frame is the newer sample's
           smears the torso over the leg frames. Default: show the newest
           sample that is due, whole (loc, offset, frame), like the engine
           would. ARCANUM_DEV_INTERP=lerp (with ARCANUM_DEV=1) brings the old
           smooth lerp back for comparison. */
        if (kind == 2 && coop_lite_interp_step_mode()) {
            kind = 1;
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
        {
            unsigned int lag = (unsigned int)location_dist(loc, ring->s[ring->n - 1].loc);

            if (lag > coop_diag.interp_lag) {
                coop_diag.interp_lag = lag;
            }
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
        if (proto_obj == OBJ_HANDLE_NULL || obj_pool_perm_lookup(oid) != OBJ_HANDLE_NULL
            || !coop_lite_client_ground_loc_live(p->ground_loc)) {
            continue;
        }
        tig_debug_printf("CoopLite: obj blob: ground item timed out, using the proto\n");
        multiplayer_lock();
        coop_lite_apply_begin();
        object_create_ex(proto_obj, p->ground_loc, oid, &obj);
        coop_lite_apply_end();
        multiplayer_unlock();
    }
}

/* PT77ae: an item just landed on the ground at the host. Clients near it
   get its blob in this tick (was: a host-UI drop announced nothing - the
   clients saw it at the next WORLD_STATE, 3.5..30 s later; item_drop_ex
   only raised WORLD_DIRTY). Far clients learn it from WORLD_STATE. */
void coop_lite_host_item_landed(int64_t item)
{
#ifndef COOP_LITE_TEST_STUBS
    PacketCoopObjBlob* out;
    uint8_t* data = NULL;
    int size = 0;
    int64_t loc;
    int slot;
    int sent = 0;

    if (item == OBJ_HANDLE_NULL || !tig_net_is_coop_enabled() || !tig_net_is_active() || !tig_net_is_host()
        || !item_is_item(item)
        || (obj_field_int32_get(item, OBJ_F_FLAGS) & (OF_INVENTORY | OF_DESTROYED)) != 0) {
        return;
    }
    loc = obj_field_int64_get(item, OBJ_F_LOCATION);
    for (slot = 1; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);

        if (pc != OBJ_HANDLE_NULL && location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION), loc) <= COOP_DROP_PUSH_RADIUS) {
            break;
        }
    }
    if (slot >= 8) {
        return;
    }
    sub_442050(&data, &size, item);
    if (data == NULL || size <= 0 || size > COOP_OBJ_BLOB_MAX) {
        if (data != NULL) {
            FREE(data);
        }
        return;
    }
    out = (PacketCoopObjBlob*)MALLOC(sizeof(*out));
    if (out != NULL) {
        memset(out, 0, offsetof(PacketCoopObjBlob, data));
        out->type = PACKET_COOP_OBJ_BLOB;
        out->size = size;
        out->flags = COOP_OBJ_BLOB_F_GROUND;
        coop_lite_put_oid(out->oid, item);
        coop_lite_obj_blob_ground_loc_put(out, loc);
        memcpy(out->data, data, (size_t)size);
        for (; slot < 8; slot++) {
            int64_t pc = sub_4A2B60(slot);

            if (pc == OBJ_HANDLE_NULL
                || location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION), loc) > COOP_DROP_PUSH_RADIUS) {
                continue;
            }
            tig_net_send_app(out, (int)(offsetof(PacketCoopObjBlob, data) + (size_t)size), slot);
            sent++;
        }
        FREE(out);
    }
    FREE(data);
    if (sent > 0) {
        tig_debug_printf("CoopLite: obj blob: dropped item pushed to %d client(s) (%d bytes)\n", sent, size);
    }
#else
    (void)item;
#endif
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
    if (!coop_lite_host_rate_ok(0, slot, pkt->count > 0 ? pkt->count : 1)) {
        return; /* PT52k */
    }
    out = (PacketCoopObjBlob*)MALLOC(sizeof(*out));
    if (out == NULL) { tig_debug_printf("CoopLite: PT52e alloc failed\n"); return; }
    for (i = 0; i < pkt->count; i++) {
        int64_t item = coop_lite_get_oid_obj(pkt->oid[i]);
        int64_t parent = OBJ_HANDLE_NULL;
        uint8_t* data = NULL;
        int size = 0;

        if (item != OBJ_HANDLE_NULL && item == sub_4A2B60(slot)) {
            /* Rejoin: the sender's own (restored) PC. */
            parent = OBJ_HANDLE_NULL;
        } else if (item != OBJ_HANDLE_NULL && item_is_item(item)
            && (obj_field_int32_get(item, OBJ_F_FLAGS) & (OF_INVENTORY | OF_DESTROYED)) == 0) {
            /* A ground item (Packet70 announcement), stacks included:
               on the ground nothing merges them (review P1-2). */
            parent = OBJ_HANDLE_NULL;
        } else if (item == OBJ_HANDLE_NULL || !item_is_item(item)
            || (coop_lite_is_stackable(item)
                && !(item_parent(item, &parent) && parent != OBJ_HANDLE_NULL
                    && obj_field_int32_get(parent, OBJ_F_TYPE) == OBJ_TYPE_CONTAINER))
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

static int coop_rejoin_pc_tries;

static void coop_lite_client_request_own_pc(const unsigned char oid[24])
{
    memcpy(coop_rejoin_pc_oid, oid, 24);
    coop_rejoin_pc_wanted = true;
    coop_rejoin_pc_ms = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1u;
    coop_rejoin_pc_tries = 0;
    if (coop_lite_obj_pending_find(oid) == NULL) {
        coop_lite_obj_blob_defer(oid);
    }
    coop_lite_obj_req_flush();
    tig_debug_printf("CoopLite: rejoin: asking the host for our parked PC\n");
}

/* PT77ae: SESSION_SYNC found no local PC: ask the host for the blob of the
   PC it keeps for our slot (the existing rejoin request), to be loaded at
   the sync's client_loc by coop_lite_client_restore_pc_in_world. */
static void coop_lite_client_restore_pc_ask(const PacketCoopSessionSync* pkt)
{
    CoopObjPending* p = coop_lite_obj_pending_find(pkt->client_pc_oid);

    if (p != NULL && coop_lite_interval_elapsed(p->asked_ms, coop_obj_now_ms, 900u)) {
        p->used = false; /* unanswered: ask again */
    }
    if (!coop_sync_restore_asked) {
        tig_debug_printf("CoopLite: PT77ae no local PC after the host load - restoring it in the world\n");
    }
    coop_sync_restore_asked = true;
    coop_sync_restore_loc = pkt->client_loc;
    coop_lite_client_request_own_pc(pkt->client_pc_oid);
}

static void coop_lite_client_restore_pc_in_world(PacketCoopObjBlob* pkt)
{
    int64_t pc = OBJ_HANDLE_NULL;
    int64_t loc = coop_sync_restore_loc;
    int slot = sub_529520();
    int t;

    if (slot <= 0 || slot >= 8 || !map_is_valid() || map_is_clearing_objects() || loc == 0
        || !coop_lite_client_ground_loc_live(loc) || !coop_lite_obj_blob_bounded(pkt->data, pkt->size)) {
        tig_debug_printf("CoopLite: PT77ae restore: map or place not ready - asking again\n");
        return;
    }
    if (coop_lite_get_oid_obj(pkt->oid) != OBJ_HANDLE_NULL) {
        return; /* it is here after all: finish sync adopts it by OID */
    }
    multiplayer_lock();
    coop_lite_apply_begin();
    if (!sub_4420D0(pkt->data, &pc, loc) || pc == OBJ_HANDLE_NULL
        || obj_field_int32_get(pc, OBJ_F_TYPE) != OBJ_TYPE_PC) {
        if (pc != OBJ_HANDLE_NULL) {
            object_destroy(pc);
        }
        coop_lite_apply_end();
        multiplayer_unlock();
        tig_debug_printf("CoopLite: PT77ae restore: PC blob rejected\n");
        return;
    }
    /* As the rejoin adopt: item handles do not resolve here, the pack
       comes with the next INVEN_SYNC. */
    obj_field_int32_set(pc, OBJ_F_CRITTER_INVENTORY_NUM, 0);
    coop_lite_inv_list_clear(pc);
    obj_field_handle_set(pc, OBJ_F_CRITTER_GOLD, OBJ_HANDLE_NULL);
    for (t = 0; t < COOP_INVEN_AMMO_TYPES; t++) {
        obj_field_handle_set(pc, coop_ammo_fld[t], OBJ_HANDLE_NULL);
    }
    object_flags_unset(pc, OF_OFF);
    sub_40DAF0(pc);
    multiplayer_bind_slot_pc(slot, pc);
    coop_lite_apply_end();
    multiplayer_unlock();

    coop_sync_restore_asked = false;
    coop_client_pos_valid = false;
    coop_lite_client_refresh_own_view(pc);
    tig_debug_printf("CoopLite: PT77ae restore: our PC is back in the world (level %d, %d bytes)\n",
        stat_level_get(pc, STAT_LEVEL), pkt->size);
    coop_lite_client_request_barter(pc, OBJ_HANDLE_NULL, COOP_BARTER_SYNC);
    /* The pending SESSION_SYNC's next PT72 retry binds the party as usual. */
}

/* PT54: a held item the last sync destroyed or replaced is dropped. */
void coop_lite_client_drag_tick(void)
{
    if (inven_ui_drag_cancel_if_stale()) {
        tig_debug_printf("CoopLite: inven: held item was replaced by a sync - drag dropped\n");
    }
}

/* PT54: the parked-PC blob can be lost (it came while the world was not
   ready, or the request was dropped). coop_rejoin_pc_wanted then stayed true
   forever and silently blocked every door use and pickup the client sent.
   Ask again every 3 s; after 4 tries keep the PC we have and resync its pack. */
void coop_lite_client_rejoin_tick(unsigned int now_ms)
{
    int64_t pc;

    if (!coop_rejoin_pc_wanted || !coop_lite_interval_elapsed(coop_rejoin_pc_ms, now_ms, 3000u)) {
        return;
    }
    coop_rejoin_pc_ms = now_ms != 0 ? now_ms : 1u;
    if (++coop_rejoin_pc_tries <= 4) {
        if (coop_lite_obj_pending_find(coop_rejoin_pc_oid) == NULL) {
            coop_lite_obj_blob_defer(coop_rejoin_pc_oid);
        }
        coop_lite_obj_req_flush();
        tig_debug_printf("CoopLite: rejoin: parked PC not here yet - asking again (%d)\n", coop_rejoin_pc_tries);
        return;
    }
    coop_rejoin_pc_wanted = false;
    pc = player_get_local_pc_obj();
    tig_debug_printf("CoopLite: rejoin: no parked PC blob after 4 tries - keeping ours, pack resync\n");
    if (pc != OBJ_HANDLE_NULL) {
        coop_lite_client_request_barter(pc, OBJ_HANDLE_NULL, COOP_BARTER_SYNC);
    }
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
    /* PT30 crash after a party wipe: when the client kept its own PC (it
       had died and was revived for the reload) that PC already carries the
       parked PC's OID. Loading the blob made a second object with the same
       OID and then destroyed the "old" one - the client crashed. Our PC is
       the parked PC: keep it, only refresh the inventory. */
    {
        ObjectID want;
        ObjectID have = obj_get_id(old_pc);

        memcpy(&want, pkt->oid, sizeof(want));
        if (objid_is_equal(want, have) || obj_pool_perm_lookup(want) != OBJ_HANDLE_NULL) {
            tig_debug_printf("CoopLite: rejoin: parked PC is already here - kept\n");
            coop_lite_client_request_barter(old_pc, OBJ_HANDLE_NULL, COOP_BARTER_SYNC);
            return;
        }
    }
    loc = obj_field_int64_get(old_pc, OBJ_F_LOCATION);
    multiplayer_lock();
    coop_lite_apply_begin();
    if (!sub_4420D0(pkt->data, &pc, loc) || pc == OBJ_HANDLE_NULL
        || obj_field_int32_get(pc, OBJ_F_TYPE) != OBJ_TYPE_PC) {
        if (pc != OBJ_HANDLE_NULL) {
            object_destroy(pc);
        }
        coop_lite_apply_end();
        multiplayer_unlock();
        tig_debug_printf("CoopLite: rejoin: PC blob rejected — keeping the menu character\n");
        coop_lite_client_request_barter(old_pc, OBJ_HANDLE_NULL, COOP_BARTER_SYNC);
        return;
    }
    /* Its item handles did not resolve here (the items come with the next
       INVEN_SYNC as blobs): start from an empty inventory. */
    obj_field_int32_set(pc, OBJ_F_CRITTER_INVENTORY_NUM, 0);
    /* PT54d: length_set(0) only removed key 0 - the parked PC's other
       unresolved handles stayed (log: "NUM 0 != list length 17"). */
    coop_lite_inv_list_clear(pc);
    obj_field_handle_set(pc, OBJ_F_CRITTER_GOLD, OBJ_HANDLE_NULL);
    for (t = 0; t < COOP_INVEN_AMMO_TYPES; t++) {
        obj_field_handle_set(pc, coop_ammo_fld[t], OBJ_HANDLE_NULL);
    }
    object_flags_unset(pc, OF_OFF);
    sub_40DAF0(pc);
    multiplayer_bind_slot_pc(slot, pc);
    object_destroy(old_pc);
    coop_lite_apply_end();
    multiplayer_unlock();

    coop_client_pos_valid = false;
    coop_lite_client_refresh_own_view(pc);
    tig_debug_printf("CoopLite: rejoin: parked PC restored (level %d, %d bytes)\n",
        stat_level_get(pc, STAT_LEVEL), pkt->size);
    coop_lite_client_request_barter(pc, OBJ_HANDLE_NULL, COOP_BARTER_SYNC);
}

static bool coop_lite_oid_bytes_set(const uint8_t* bytes);


/* PT79 (review 1.3): OIDs the host's ground push just put on the floor
   here. An INVEN_SYNC batch built before that drop (still in flight or
   applied later) would pick the item back up into the dropper's pack, and
   the next batch then destroyed it. Such a batch leaves them alone ~3 s. */
#define COOP_GROUNDED_RING 32
#define COOP_GROUNDED_MS 3000u
static struct {
    uint8_t oid[24];
    unsigned int ms;
} coop_grounded[COOP_GROUNDED_RING];
static int coop_grounded_next;

static void coop_lite_grounded_note(const uint8_t oid[24])
{
    memcpy(coop_grounded[coop_grounded_next].oid, oid, 24);
    coop_grounded[coop_grounded_next].ms = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1;
    coop_grounded_next = (coop_grounded_next + 1) % COOP_GROUNDED_RING;
}

static bool coop_lite_grounded_recent(const uint8_t oid[24])
{
    int i;

    for (i = 0; i < COOP_GROUNDED_RING; i++) {
        if (coop_grounded[i].ms != 0 && memcmp(coop_grounded[i].oid, oid, 24) == 0
            && !coop_lite_interval_elapsed(coop_grounded[i].ms, coop_obj_now_ms, COOP_GROUNDED_MS)) {
            return true;
        }
    }
    return false;
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
    if ((pkt->flags & COOP_OBJ_BLOB_F_GROUND) != 0) {
        /* PT77ae: pushed ground drop (asked or not). */
        int64_t gloc = coop_lite_obj_blob_ground_loc_get(pkt);
        int64_t have;

        if (p != NULL) {
            p->used = false;
        }
        if (!coop_lite_client_world_ready() || gloc == 0) {
            return;
        }
        have = coop_lite_get_oid_obj(pkt->oid);
        if (have != OBJ_HANDLE_NULL) {
            /* Our copy (e.g. still in the dropper's pack here): to the
               ground, keeping its state - as a Packet70 does. */
            unsigned int hf = (unsigned int)obj_field_int32_get(have, OBJ_F_FLAGS);

            if (item_is_item(have) && (hf & OF_INVENTORY) != 0 && (hf & OF_DESTROYED) == 0
                && have != inven_ui_drag_item_obj_get()) {
                multiplayer_lock();
                tig_net_local_suspend_begin();
                item_remove(have);
                object_drop(have, gloc);
                tig_net_local_suspend_end();
                multiplayer_unlock();
                coop_lite_grounded_note(pkt->oid);
            }
            return;
        }
        if (!coop_lite_client_ground_loc_live(gloc) || !coop_lite_obj_blob_bounded(pkt->data, pkt->size)) {
            return;
        }
        multiplayer_lock();
        coop_lite_apply_begin();
        if (!sub_4420D0(pkt->data, &item, gloc) || item == OBJ_HANDLE_NULL || !item_is_item(item)) {
            if (item != OBJ_HANDLE_NULL) {
                object_destroy(item);
            }
            item = OBJ_HANDLE_NULL;
        }
        coop_lite_apply_end();
        multiplayer_unlock();
        if (item != OBJ_HANDLE_NULL) {
            coop_lite_grounded_note(pkt->oid);
        }
        tig_debug_printf("CoopLite: obj blob: pushed ground drop %s (%d bytes)\n",
            item != OBJ_HANDLE_NULL ? "loaded" : "rejected", pkt->size);
        return;
    }
    if (p == NULL) {
        /* Unasked (or already timed out and created from the proto). */
        return;
    }
    /* PT25: asked for as a ground item (Packet70 / world state), but the
       host now has it in an inventory (picked up, planted, transferred):
       follow the host's answer, not our request. It used to be loaded at
       the old ground spot with OF_INVENTORY set ("objlist_insert_internal()
       called with inventory item") and was then visible nowhere. */
    ground = p->ground && !coop_lite_oid_bytes_set(pkt->owner_oid);
    ground_loc = p->ground_loc;
    p->used = false;
    if ((pkt->flags & COOP_OBJ_BLOB_F_GONE) != 0) {
        /* An inventory item is asked for again by the next sync that
           lists it; a ground item just isn't there any more. */
        return;
    }
    if (coop_sync_restore_asked && coop_rejoin_pc_wanted
        && coop_lite_oid_raw_equal(pkt->oid, coop_rejoin_pc_oid)
        && player_get_local_pc_obj() == OBJ_HANDLE_NULL) {
        /* PT77ae: before world_ready (that needs a local PC). On failure
           the next PT72 retry asks again. */
        coop_rejoin_pc_wanted = false;
        coop_lite_client_restore_pc_in_world(pkt);
        return;
    }
    if (!coop_lite_client_world_ready()) {
        return;
    }
    if (coop_lite_get_oid_obj(pkt->oid) != OBJ_HANDLE_NULL
        && !(coop_rejoin_pc_wanted && coop_lite_oid_raw_equal(pkt->oid, coop_rejoin_pc_oid)
            && coop_lite_get_oid_obj(pkt->oid) == local_pc)) {
        return;
    }
    if (!coop_lite_obj_blob_bounded(pkt->data, pkt->size)) {
        return; /* review P1-4 */
    }
    if (ground && !coop_lite_client_ground_loc_live(ground_loc)) {
        tig_debug_printf("CoopLite: obj blob: ground item in a sector not loaded here - skipped\n");
        return;
    }
    if (ground) {
        multiplayer_lock();
        coop_lite_apply_begin();
        if (!sub_4420D0(pkt->data, &item, ground_loc) || item == OBJ_HANDLE_NULL || !item_is_item(item)) {
            if (item != OBJ_HANDLE_NULL) {
                object_destroy(item);
            }
            item = OBJ_HANDLE_NULL;
        }
        coop_lite_apply_end();
        multiplayer_unlock();
        tig_debug_printf("CoopLite: obj blob: ground item %s (%d bytes)\n",
            item != OBJ_HANDLE_NULL ? "loaded" : "rejected", pkt->size);
        return;
    }
    if (coop_rejoin_pc_wanted && coop_lite_oid_raw_equal(pkt->oid, coop_rejoin_pc_oid)) {
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
    coop_lite_apply_begin();
    if (obj_read_mem(pkt->data, &item) && item != OBJ_HANDLE_NULL) {
        obj_load_postprocess(item);
        if (!item_is_item(item)
            || (coop_lite_is_stackable(item) && obj_field_int32_get(owner, OBJ_F_TYPE) != OBJ_TYPE_CONTAINER)) {
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
    coop_lite_apply_end();
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
        multiplayer_lock();
        coop_lite_inv_sanitize(owner); /* PT53b */
        multiplayer_unlock();
        /* PT13: an open inventory window showed an empty grid for items
           that arrived after it was drawn. */
        ui_update_inven(owner);
    }
#else
    (void)pkt;
#endif
}

static uint8_t coop_host_gold_oid[24];
static uint8_t coop_host_ammo_oid[COOP_INVEN_AMMO_TYPES][24];

/* PT18 GUI: the host's gold/ammo stack OIDs of other critters too (the
   host's PC, another client's, an NPC): looting or picking the gold of a
   remote PC named our local stack copy, which the host does not know
   ("takes '' from target: refused"). */
typedef struct CoopStackMap {
    bool valid;
    ObjectID owner;
    uint8_t gold[24];
    uint8_t ammo[COOP_INVEN_AMMO_TYPES][24];
} CoopStackMap;
static CoopStackMap coop_stack_map[16];
static int coop_stack_map_next;

static void coop_lite_client_remember_stacks(int64_t owner, const uint8_t* gold, const uint8_t (*ammo)[24])
{
    ObjectID oid = obj_get_id(owner);
    int i;
    CoopStackMap* e = NULL;

    for (i = 0; i < 16; i++) {
        if (coop_stack_map[i].valid && objid_is_equal(coop_stack_map[i].owner, oid)) {
            e = &coop_stack_map[i];
            break;
        }
    }
    if (e == NULL) {
        e = &coop_stack_map[coop_stack_map_next];
        coop_stack_map_next = (coop_stack_map_next + 1) % 16;
    }
    e->valid = true;
    e->owner = oid;
    memcpy(e->gold, gold, 24);
    memcpy(e->ammo, ammo, sizeof(e->ammo));
}

/* A 24-byte wire OID is set (memcpy: the byte arrays are not aligned). */
static bool coop_lite_oid_bytes_set(const uint8_t* bytes)
{
    ObjectID oid;

    memcpy(&oid, bytes, sizeof(oid));
    return oid.type != OID_TYPE_NULL;
}

void coop_lite_client_map_stack_oid(void* oid_ptr)
{
    ObjectID* oid = (ObjectID*)oid_ptr;
    int64_t pc = player_get_local_pc_obj();
    int64_t obj;
    int t;

    if (oid == NULL || pc == OBJ_HANDLE_NULL || !tig_net_is_active() || tig_net_is_host()
        || oid->type == OID_TYPE_NULL) {
        return;
    }
    obj = obj_pool_perm_lookup(*oid);
    if (obj == OBJ_HANDLE_NULL) {
        return;
    }
    if (obj == obj_field_handle_get(pc, OBJ_F_CRITTER_GOLD)) {
        if (coop_lite_oid_bytes_set(coop_host_gold_oid)) {
            memcpy(oid, coop_host_gold_oid, sizeof(*oid));
        }
        return;
    }
    for (t = 0; t < COOP_INVEN_AMMO_TYPES; t++) {
        if (obj == obj_field_handle_get(pc, coop_ammo_fld[t])) {
            if (coop_lite_oid_bytes_set(coop_host_ammo_oid[t])) {
                memcpy(oid, coop_host_ammo_oid[t], sizeof(*oid));
            }
            return;
        }
    }
    {
        /* Another critter's stack (loot / pickpocket of a PC or NPC). */
        int64_t parent = OBJ_HANDLE_NULL;
        ObjectID parent_oid;
        int i;

        if (!item_parent(obj, &parent) || parent == OBJ_HANDLE_NULL
            || !obj_type_is_critter(obj_field_int32_get(parent, OBJ_F_TYPE))) {
            return;
        }
        parent_oid = obj_get_id(parent);
        for (i = 0; i < 16; i++) {
            const uint8_t* src = NULL;

            if (!coop_stack_map[i].valid || !objid_is_equal(coop_stack_map[i].owner, parent_oid)) {
                continue;
            }
            if (obj == obj_field_handle_get(parent, OBJ_F_CRITTER_GOLD)) {
                src = coop_stack_map[i].gold;
            } else {
                for (t = 0; t < COOP_INVEN_AMMO_TYPES; t++) {
                    if (obj == obj_field_handle_get(parent, coop_ammo_fld[t])) {
                        src = coop_stack_map[i].ammo[t];
                    }
                }
            }
            if (src != NULL && coop_lite_oid_bytes_set(src)) {
                memcpy(oid, src, sizeof(*oid));
            }
            return;
        }
    }
}

/*
 * PT33 item 3: the host's copy of this client's PC lists a worn item (weapon,
 * shield, ...) in the backpack while the client still wears it. The client
 * is the source of truth for what its own PC wears, so the inventory sync no
 * longer takes it off; the worn slot is asserted to the host with the same
 * own-move Packet81 a drag in the window sends, so the host's copy (and its
 * combat) catch up. Rate-limited: a host that refuses is not hammered.
 */
#define COOP_WORN_REASSERT_SLOTS 8
#define COOP_WORN_REASSERT_TRIES 2
static struct {
    ObjectID oid;
    unsigned int ms;
    int tries;
} coop_worn_reassert[COOP_WORN_REASSERT_SLOTS];
static int coop_worn_reassert_next;

static void coop_lite_worn_reassert_reset(void)
{
    memset(coop_worn_reassert, 0, sizeof(coop_worn_reassert));
    coop_worn_reassert_next = 0;
}

/* True when the host's entries of this owner put a different item at `loc`:
   the slot is not ours to keep (PT34). */
static bool coop_lite_inv_slot_claimed(int o, int self_idx, int loc)
{
    int k;

    for (k = coop_inv_owner[o].first; k < coop_inv_owner[o].first + coop_inv_owner[o].count; k++) {
        if (k != self_idx && coop_inv_entry[k].inventory_location == loc) {
            return true;
        }
    }
    return false;
}

/* True: the worn item stays worn (asserted to the host, or asked a moment
   ago). False: the host listed it in the backpack COOP_WORN_REASSERT_TRIES
   times in a row, so it meant it (script, curse, a stat debuff): obey it. */
static bool coop_lite_client_reassert_worn(int64_t pc, int64_t item, int loc)
{
    Packet81 pkt;
    ObjectID oid;
    unsigned int now = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1;
    int depth;
    int i;

    if (pc == OBJ_HANDLE_NULL || item == OBJ_HANDLE_NULL) {
        return false;
    }
    oid = obj_get_id(item);
    for (i = 0; i < COOP_WORN_REASSERT_SLOTS; i++) {
        if (coop_worn_reassert[i].ms != 0 && objid_is_equal(coop_worn_reassert[i].oid, oid)) {
            break;
        }
    }
    if (i < COOP_WORN_REASSERT_SLOTS) {
        unsigned int age = (unsigned int)(now - coop_worn_reassert[i].ms);

        if (age > 30000u) {
            coop_worn_reassert[i].tries = 0; /* not "in a row" any more */
        } else if (age < 4000u) {
            return true; /* asked a moment ago; the host's answer is in flight */
        } else if (coop_worn_reassert[i].tries >= COOP_WORN_REASSERT_TRIES) {
            memset(&coop_worn_reassert[i], 0, sizeof(coop_worn_reassert[i]));
            tig_debug_printf("CoopLite: inven: host keeps the item at loc %d unworn, obeying\n", loc);
            return false;
        }
    } else {
        i = coop_worn_reassert_next;
        coop_worn_reassert_next = (coop_worn_reassert_next + 1) % COOP_WORN_REASSERT_SLOTS;
        coop_worn_reassert[i].tries = 0;
    }
    coop_worn_reassert[i].oid = oid;
    coop_worn_reassert[i].ms = now;
    coop_worn_reassert[i].tries++;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = 81;
    pkt.field_8 = 0x01;
    pkt.field_10 = oid;
    pkt.field_28 = obj_get_id(pc);
    pkt.field_40 = obj_get_id(pc);
    pkt.field_58 = loc;
    pkt.field_5C = 0;
    pkt.field_60 = 1;
    pkt.field_68 = sub_529520();
    pkt.field_70.type = OID_TYPE_NULL;
    pkt.field_88.type = OID_TYPE_NULL;
    pkt.field_A0 = obj_get_id(pc);
    depth = tig_net_local_suspend_pause();
    if (tig_net_is_active() && !tig_net_is_host()) {
        tig_net_send_app_all(&pkt, sizeof(pkt));
    }
    tig_net_local_suspend_resume(depth);
    tig_debug_printf("CoopLite: inven: kept worn item at loc %d, told the host (try %d)\n", loc,
        coop_worn_reassert[i].tries);
    return true;
}

/* PT39: the inventory NUM field must equal the list length; every walker
   (inven_ui, item_list_get, delete loops) trusts NUM. */
static void coop_lite_fix_inven_num(int64_t owner)
{
    int num_fld = OBJ_F_CRITTER_INVENTORY_NUM;
    int list_fld = OBJ_F_CRITTER_INVENTORY_LIST_IDX;
    int len;

    if (obj_field_int32_get(owner, OBJ_F_TYPE) == OBJ_TYPE_CONTAINER) {
        num_fld = OBJ_F_CONTAINER_INVENTORY_NUM;
        list_fld = OBJ_F_CONTAINER_INVENTORY_LIST_IDX;
    }
    len = obj_arrayfield_length_get(owner, list_fld);
    if (obj_field_int32_get(owner, num_fld) != len) {
        tig_debug_printf("CoopLite: inven: NUM %d != list length %d, fixed\n",
            obj_field_int32_get(owner, num_fld), len);
        obj_field_int32_set(owner, num_fld, len);
    }
}

/* PT48: a parked-PC restore can leave the inventory list with null or
   duplicate handles while items still name the PC as parent: those items
   were invisible on the client and the host's sync thought them "there". */
/* PT54d: an object array field is a SPARSE keyed array (sa.c):
   obj_arrayfield_length_get() is the number of PRESENT keys, and
   obj_arrayfield_length_set(obj, fld, k) only REMOVES KEY k (vanilla only
   ever calls it with the last index after shifting). Coop code that used it
   to "truncate to k" left keys k+1.. alive: stale/duplicate handles, holes
   (null reads), count != NUM, and item_insert (writes at index NUM)
   overwriting or leaving gaps. These helpers read/write the list densely. */
#define COOP_ARR_SCAN_SLACK 256

/* Reads keys 0..count+slack; fills by_key (NULL for absent/null keys).
   Returns the number of keys scanned (caller frees *out). */
static int coop_lite_arr_read_keys(int64_t obj, int fld, int64_t** out)
{
    int count = obj_arrayfield_length_get(obj, fld);
    int limit;
    int i;
    int64_t* h;

    *out = NULL;
    if (count < 0 || count > 4096) {
        count = count < 0 ? 0 : 4096;
    }
    limit = count + COOP_ARR_SCAN_SLACK;
    h = (int64_t*)MALLOC(sizeof(*h) * (size_t)limit);
    if (h == NULL) {
        return 0;
    }
    for (i = 0; i < limit; i++) {
        h[i] = obj_arrayfield_handle_get(obj, fld, i);
    }
    *out = h;
    return limit;
}

/* Rewrites the field as exactly h[0..k-1]; every key below `scanned` is
   removed first (length_set == remove key). */
static void coop_lite_arr_rewrite(int64_t obj, int fld, const int64_t* h, int k, int scanned)
{
    int i;

    for (i = scanned - 1; i >= 0; i--) {
        obj_arrayfield_length_set(obj, fld, i);
    }
    for (i = 0; i < k; i++) {
        obj_arrayfield_obj_set(obj, fld, i, h[i]);
    }
}

static void coop_lite_inv_list_clear(int64_t owner)
{
    int64_t* h;
    int scanned = coop_lite_arr_read_keys(owner, OBJ_F_CRITTER_INVENTORY_LIST_IDX, &h);

    if (h != NULL) {
        coop_lite_arr_rewrite(owner, OBJ_F_CRITTER_INVENTORY_LIST_IDX, h, 0, scanned);
        FREE(h);
    }
}

static int coop_lite_inv_list_fld(int64_t owner)
{
    return obj_field_int32_get(owner, OBJ_F_TYPE) == OBJ_TYPE_CONTAINER
        ? OBJ_F_CONTAINER_INVENTORY_LIST_IDX
        : OBJ_F_CRITTER_INVENTORY_LIST_IDX;
}

static int coop_lite_inv_num_fld(int64_t owner)
{
    return obj_field_int32_get(owner, OBJ_F_TYPE) == OBJ_TYPE_CONTAINER
        ? OBJ_F_CONTAINER_INVENTORY_NUM
        : OBJ_F_CRITTER_INVENTORY_NUM;
}

/* PT54d self-check: NUM == count, keys 0..count-1 present and non-null,
   nothing beyond, no duplicates. Logs once per mutation name. */
#define COOP_INV_CHECK_NAMES 24
static const char* coop_inv_check_logged[COOP_INV_CHECK_NAMES];

static bool coop_lite_inv_selfcheck(int64_t owner, const char* what)
{
    int64_t* h;
    int scanned;
    int count;
    int num;
    int code;
    int i;

    if (owner == OBJ_HANDLE_NULL || !obj_handle_is_valid(owner)) {
        return true;
    }
    count = obj_arrayfield_length_get(owner, coop_lite_inv_list_fld(owner));
    num = obj_field_int32_get(owner, coop_lite_inv_num_fld(owner));
    scanned = coop_lite_arr_read_keys(owner, coop_lite_inv_list_fld(owner), &h);
    if (h == NULL) {
        return true;
    }
    code = coop_lite_inv_keys_check(h, scanned, count, num);
    FREE(h);
    if (code == COOP_INV_KEYS_OK) {
        return true;
    }
    for (i = 0; i < COOP_INV_CHECK_NAMES; i++) {
        if (coop_inv_check_logged[i] == what) {
            return false;
        }
        if (coop_inv_check_logged[i] == NULL) {
            coop_inv_check_logged[i] = what;
            break;
        }
    }
    tig_debug_printf("CoopLite: PT54d inven self-check FAILED after '%s': %s (NUM %d, count %d)\n",
        what, coop_lite_inv_keys_check_name(code), num, count);
    return false;
}

/* PT48: a parked-PC restore can leave the inventory list with null or
   duplicate handles while items still name the PC as parent: those items
   were invisible on the client and the host's sync thought them "there".
   PT54d: reads every key (sparse) and rewrites densely. */
static int coop_lite_inv_list_repair(int64_t owner)
{
    int list_fld = coop_lite_inv_list_fld(owner);
    int num_fld = coop_lite_inv_num_fld(owner);
    int count = obj_arrayfield_length_get(owner, list_fld);
    int num = obj_field_int32_get(owner, num_fld);
    int64_t* h;
    int scanned;
    int present = 0;
    int i;
    int k;
    int dropped;

    scanned = coop_lite_arr_read_keys(owner, list_fld, &h);
    if (h == NULL) {
        if (scanned == 0 && count > 0) {
            tig_debug_printf("CoopLite: inven: list repair alloc failed (%d)\n", count);
        }
        return 0;
    }
    if (coop_lite_inv_keys_check(h, scanned, count, num) == COOP_INV_KEYS_OK) {
        FREE(h);
        return 0;
    }
    for (i = 0; i < scanned; i++) {
        int64_t parent = OBJ_HANDLE_NULL;

        if (h[i] == OBJ_HANDLE_NULL) {
            continue;
        }
        present++;
        if (!obj_handle_is_valid(h[i])) {
            h[i] = OBJ_HANDLE_NULL; /* PT52g */
        } else if (!obj_type_is_item(obj_field_int32_get(h[i], OBJ_F_TYPE)) /* PT53b */
            || (obj_field_int32_get(h[i], OBJ_F_FLAGS) & OF_DESTROYED) != 0
            || !item_parent(h[i], &parent) || parent != owner) {
            h[i] = OBJ_HANDLE_NULL;
        }
    }
    k = coop_lite_inv_compact_handles(h, scanned);
    coop_lite_arr_rewrite(owner, list_fld, h, k, scanned);
    obj_field_int32_set(owner, num_fld, k);
    dropped = present - k;
    tig_debug_printf("CoopLite: inven: list repaired (%d bad entries dropped; NUM %d count %d -> %d)\n",
        dropped, num, count, k);
    FREE(h);
    return dropped > 0 ? dropped : 0;
}

/* PT53b: everything the inventory UI / weight code will iterate must be a
   live item whose parent is the owner, at a legal location. Bounded. */
int coop_lite_inv_sanitize(int64_t owner)
{
    int bad;
    int n;
    int i;
    int critter;

    if (owner == OBJ_HANDLE_NULL || !obj_handle_is_valid(owner)) {
        return 0;
    }
    critter = obj_type_is_critter(obj_field_int32_get(owner, OBJ_F_TYPE));
    if (!critter && obj_field_int32_get(owner, OBJ_F_TYPE) != OBJ_TYPE_CONTAINER) {
        return 0;
    }
    bad = coop_lite_inv_list_repair(owner);
    if (!critter) {
        return bad;
    }
    n = obj_arrayfield_length_get(owner, OBJ_F_CRITTER_INVENTORY_LIST_IDX);
    for (i = 0; i < n && i < 4096; i++) {
        int64_t it = obj_arrayfield_handle_get(owner, OBJ_F_CRITTER_INVENTORY_LIST_IDX, i);
        int loc = item_inventory_location_get(it);

        if ((loc >= 0 && loc < 120) || (loc >= 1000 && loc <= 1008) || (loc >= 2000 && loc <= 2009)) {
            continue;
        }
        {
            int fresh = item_find_free_inv_loc_for_insertion(it, owner);

            tig_debug_printf("CoopLite: inven: sanitize: bad loc %d -> %d\n", loc, fresh);
            obj_field_int32_set(it, OBJ_F_ITEM_INV_LOCATION, fresh >= 0 && fresh < 120 ? fresh : 0);
            bad++;
        }
    }
    return bad;
}

/* PT53b: remove every occurrence of a handle from an owner's list. */
static void coop_lite_inv_list_drop(int64_t owner, int64_t item)
{
    int list_fld;
    int64_t* h;
    int scanned;
    int i;
    int k = 0;
    bool hit = false;

    if (!obj_handle_is_valid(owner)) {
        return;
    }
    list_fld = coop_lite_inv_list_fld(owner);
    scanned = coop_lite_arr_read_keys(owner, list_fld, &h);
    if (h == NULL) {
        return;
    }
    for (i = 0; i < scanned; i++) {
        if (h[i] == item) {
            hit = true;
        } else if (h[i] != OBJ_HANDLE_NULL) {
            h[k++] = h[i];
        }
    }
    if (hit) {
        /* PT54d: dense rewrite (length_set only removes one key) */
        coop_lite_arr_rewrite(owner, list_fld, h, k, scanned);
        obj_field_int32_set(owner, coop_lite_inv_num_fld(owner), k);
        coop_lite_inv_selfcheck(owner, "list_drop");
    }
    FREE(h);
}

static bool coop_lite_inv_list_has(int64_t owner, int64_t item)
{
    int list_fld = obj_field_int32_get(owner, OBJ_F_TYPE) == OBJ_TYPE_CONTAINER
        ? OBJ_F_CONTAINER_INVENTORY_LIST_IDX
        : OBJ_F_CRITTER_INVENTORY_LIST_IDX;
    int n = obj_arrayfield_length_get(owner, list_fld);
    int i;

    for (i = 0; i < n; i++) {
        if (obj_arrayfield_handle_get(owner, list_fld, i) == item) {
            return true;
        }
    }
    return false;
}

#define COOP_GRID_FIX_MAX 256
/* PT57: this critter's own (local) gold / ammo stack of the kind the
   host prototype `proto` is, or null. */
static int64_t coop_lite_local_stack_for(int64_t owner, int proto)
{
    int64_t proto_obj = proto > 0 ? sub_4685A0(proto) : OBJ_HANDLE_NULL;
    int64_t stack;
    int64_t parent = OBJ_HANDLE_NULL;
    int fld;

    if (proto_obj == OBJ_HANDLE_NULL) {
        return OBJ_HANDLE_NULL;
    }
    if (obj_field_int32_get(proto_obj, OBJ_F_TYPE) == OBJ_TYPE_AMMO) {
        int t = obj_field_int32_get(proto_obj, OBJ_F_AMMO_TYPE);

        if (t < 0 || t >= COOP_INVEN_AMMO_TYPES) {
            return OBJ_HANDLE_NULL;
        }
        fld = coop_ammo_fld[t];
    } else if (obj_field_int32_get(proto_obj, OBJ_F_TYPE) == OBJ_TYPE_GOLD) {
        fld = OBJ_F_CRITTER_GOLD;
    } else {
        return OBJ_HANDLE_NULL;
    }
    stack = obj_field_handle_get(owner, fld);
    if (stack == OBJ_HANDLE_NULL || !obj_handle_is_valid(stack)
        || (obj_field_int32_get(stack, OBJ_F_FLAGS) & OF_DESTROYED) != 0
        || !item_parent(stack, &parent) || parent != owner) {
        return OBJ_HANDLE_NULL;
    }
    return stack;
}

static int coop_lite_layout_nearest_cell(int64_t pc, int64_t item, int want);

/* PT79a: puts a freshly made gold / ammo stack into a critter mirroring
   the host: no weight / room / script checks (the host already holds it).
   A full grid gets cell 0; the PT57 pass below moves it to the host's. */
static void coop_lite_client_force_stack(int64_t owner, int64_t stack)
{
    int loc;

    if (owner == OBJ_HANDLE_NULL || stack == OBJ_HANDLE_NULL) {
        return;
    }
    loc = item_find_free_inv_loc_for_insertion(stack, owner);
    if (loc < 0) {
        loc = 0;
    }
    object_pickup(stack, owner);
    item_insert(stack, owner, loc);
}

static void coop_lite_client_apply_inven_batch(void)
{
    int64_t rings[16];
    int num_rings = 0;
    int refreshed = 0;
    int64_t owner_obj[COOP_INV_BUF_OWNERS];
    int64_t* doomed = NULL; /* PT53c: no fixed cap */
    int64_t* doomed_parent = NULL;
    int doomed_cap = 0;
    int num_doomed = 0;
    int64_t local_pc = player_get_local_pc_obj();
    int moved = 0;
    int created = 0;
    int o;
    int i;
    int t;
    static int64_t grid_item[COOP_GRID_FIX_MAX];
    static int grid_loc[COOP_GRID_FIX_MAX];
    int num_grid = 0;

    multiplayer_lock();
    coop_lite_apply_begin();

    for (o = 0; o < coop_inv_owner_cnt; o++) {
        owner_obj[o] = coop_lite_get_oid_obj(coop_inv_owner[o].head.owner_oid);
        if (owner_obj[o] == OBJ_HANDLE_NULL) {
            /* PT54: diagnosable (a whole owner's sync was dropped silently). */
            static unsigned int logged[16];
            static int nlogged;
            unsigned int key = coop_lite_inven_layout_term(coop_inv_owner[o].head.owner_oid, 0);
            int k;

            for (k = 0; k < nlogged && logged[k] != key; k++) {
            }
            if (k == nlogged) {
                char oid_str[40];
                ObjectID oid;

                logged[nlogged < 16 ? nlogged++ : 15] = key;
                memcpy(&oid, coop_inv_owner[o].head.owner_oid, sizeof(oid));
                objid_id_to_str(oid_str, oid);
                tig_debug_printf("CoopLite: inven sync: owner %s not here - its %d items skipped\n", oid_str,
                    coop_inv_owner[o].count);
            }
        }
    }

    /* Unlisted (non-stack) items: gone, unless another owner lists them. */
    for (o = 0; o < coop_inv_owner_cnt; o++) {
        int64_t* items;
        int cnt;

        if (owner_obj[o] == OBJ_HANDLE_NULL) {
            continue;
        }
        coop_lite_inv_list_repair(owner_obj[o]);
        cnt = item_list_get(owner_obj[o], &items);
        for (i = 0; i < cnt; i++) {
            /* PT26: a container's stacks are listed by OID too. */
            if (coop_lite_is_stackable(items[i])
                && obj_type_is_critter(obj_field_int32_get(owner_obj[o], OBJ_F_TYPE))) {
                continue;
            }
            if (!coop_lite_inv_listed(&coop_inv_owner[o], obj_get_id(items[i]))) {
                if (num_doomed == doomed_cap) {
                    int new_cap = doomed_cap != 0 ? doomed_cap * 2 : 64;
                    int64_t* nd = (int64_t*)realloc(doomed, sizeof(*nd) * (size_t)new_cap);
                    int64_t* np;

                    if (nd == NULL) {
                        tig_debug_printf("CoopLite: inven sync: doomed list alloc failed (%d)\n", new_cap);
                        continue;
                    }
                    doomed = nd;
                    np = (int64_t*)realloc(doomed_parent, sizeof(*np) * (size_t)new_cap);
                    if (np == NULL) {
                        tig_debug_printf("CoopLite: inven sync: doomed list alloc failed (%d)\n", new_cap);
                        continue;
                    }
                    doomed_parent = np;
                    doomed_cap = new_cap;
                }
                doomed_parent[num_doomed] = owner_obj[o];
                doomed[num_doomed++] = items[i];
            }
        }
        item_list_free(items);
    }

    /* PT44b: the loot window lists item handles of its target; this batch
       is about to destroy some of them (stale copies the host no longer
       has). Close it first instead of leaving dangling handles on screen. */
    if (inven_ui_is_created() && inven_ui_drag_item_obj_get() != OBJ_HANDLE_NULL) {
        for (i = 0; i < num_doomed; i++) {
            /* PT53: only a doomed item in hand (drag) dangles; the
               slot grids are rebuilt by ui_update_inven at the end, the
               same as for the PC's own pane. Keep the window open. */
            /* PT53c: whatever pane the drag came from. */
            if (doomed[i] == inven_ui_drag_item_obj_get()) {
                tig_debug_printf("CoopLite: inven: inventory window closed (host removed the dragged item)\n");
                inven_ui_destroy();
                break;
            }
        }
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
            int64_t item;
            int64_t parent = OBJ_HANDLE_NULL;
            int loc = e->inventory_location;
            int free_loc;
            int k;

            if ((e->state_flags & COOP_INVEN_E_STACK) != 0
                && obj_type_is_critter(obj_field_int32_get(owner, OBJ_F_TYPE))) {
                continue; /* PT57: a critter's gold / ammo cell: placed after the totals */
            }
            item = coop_lite_client_reclaim_destroyed(coop_lite_get_oid_obj(e->oid));

            /* PT53a: an OID listed twice in one owner (corrupt host list)
               would be inserted twice: only the first entry counts. */
            for (k = coop_inv_owner[o].first; k < i; k++) {
                if (coop_lite_oid_raw_equal(coop_inv_entry[k].oid, e->oid)) {
                    break;
                }
            }
            if (k < i) {
                tig_debug_printf("CoopLite: inven sync: duplicate entry skipped\n");
                continue;
            }
            if (item != OBJ_HANDLE_NULL
                && (!obj_handle_is_valid(item) || !obj_type_is_item(obj_field_int32_get(item, OBJ_F_TYPE)))) {
                /* PT53b: the OID maps to a non-item / dead handle: never
                   insert it; ask for the real object instead. */
                tig_debug_printf("CoopLite: inven sync: non-item handle for OID refused\n");
                coop_lite_obj_blob_defer(e->oid);
                continue;
            }
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
            } else if (item_parent(item, &parent) && parent == owner
                && (obj_field_int32_get(item, OBJ_F_FLAGS) & OF_INVENTORY) != 0
                && !coop_lite_inv_list_has(owner, item)) {
                /* PT48: names the owner as parent but missing from its list
                   (invisible): put the handle back, then place it below. */
                int list_fld = obj_field_int32_get(owner, OBJ_F_TYPE) == OBJ_TYPE_CONTAINER
                    ? OBJ_F_CONTAINER_INVENTORY_LIST_IDX
                    : OBJ_F_CRITTER_INVENTORY_LIST_IDX;

                coop_lite_inv_list_repair(owner); /* PT54d: dense before append */
                obj_arrayfield_obj_set(owner, list_fld, obj_arrayfield_length_get(owner, list_fld), item);
                coop_lite_fix_inven_num(owner);
                coop_lite_inv_selfcheck(owner, "orphan_relist");
                tig_debug_printf("CoopLite: inven: orphaned item re-listed in its owner\n");
                moved++;
                goto flags;
            } else if (item_parent(item, &parent) && parent == owner) {
                /* Already there; our own PC keeps its local backpack
                   arrangement, but what is worn/wielded follows the host
                   (P1-3: host-side combat uses the host's equipment). */
                int cur = item_inventory_location_get(item);

                if ((obj_field_int32_get(item, OBJ_F_FLAGS) & OF_INVENTORY) == 0) {
                    /* PT39: also lying on the ground (a local throw/drop of an
                       item the host still has in the pack): take it back. */
                    object_pickup(item, owner);
                    moved++;
                }
                if (owner != local_pc && cur != loc) {
                    item_remove(item);
                    item_insert(item, owner, loc);
                } else if (owner == local_pc && cur != loc && IS_WEAR_INV_LOC(cur) && !IS_WEAR_INV_LOC(loc)
                    && !coop_lite_inv_slot_claimed(o, i, cur)
                    && coop_lite_client_reassert_worn(owner, item, cur)) {
                    /* PT33 item 3: keep what we wear; tell the host. PT34:
                       not when another item of the batch takes that slot,
                       and not after the host said no twice. */
                } else if (owner == local_pc && cur != loc && cur >= 0 && cur < 120 && loc >= 0 && loc < 120
                    && !coop_lite_client_layout_hold()) {
                    /* PT54: the host's grid cell wins (the echo of our own
                       layout, or its nearest legal cell): placed below,
                       after every such item is out (swaps). */
                    if (num_grid < COOP_GRID_FIX_MAX) {
                        grid_item[num_grid] = item;
                        grid_loc[num_grid++] = loc;
                    }
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

            if (parent == OBJ_HANDLE_NULL && (obj_field_int32_get(item, OBJ_F_FLAGS) & OF_INVENTORY) == 0
                && coop_lite_grounded_recent(e->oid)) {
                continue; /* PT79: the host just dropped it; this batch is older */
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
            if ((e->state_flags & COOP_INVEN_E_STACK) != 0 && coop_lite_is_stackable(item)
                && coop_lite_stack_qty(item) != e->mana_store && e->mana_store > 0) {
                obj_field_int32_set(item, obj_field_int32_get(item, OBJ_F_TYPE) == OBJ_TYPE_GOLD
                        ? OBJ_F_GOLD_QUANTITY
                        : OBJ_F_AMMO_QUANTITY,
                    e->mana_store);
                refreshed++;
            }
            if ((e->item_flags & OIF_IDENTIFIED) != 0) {
                obj_field_int32_set(item, OBJ_F_ITEM_FLAGS,
                    obj_field_int32_get(item, OBJ_F_ITEM_FLAGS) | OIF_IDENTIFIED);
            }
            /* Review P1-3: durability and charges follow the host. */
            if ((e->state_flags & COOP_INVEN_E_STATE) != 0) {
                if (obj_field_int32_get(item, OBJ_F_HP_DAMAGE) != e->hp_damage) {
                    obj_field_int32_set(item, OBJ_F_HP_DAMAGE, e->hp_damage);
                    refreshed++;
                }
                if (obj_field_int32_get(item, OBJ_F_ITEM_MANA_STORE) != e->mana_store) {
                    obj_field_int32_set(item, OBJ_F_ITEM_MANA_STORE, e->mana_store);
                    refreshed++;
                }
                if (obj_field_int32_get(item, OBJ_F_ITEM_SPELL_MANA_STORE) != e->spell_mana_store) {
                    obj_field_int32_set(item, OBJ_F_ITEM_SPELL_MANA_STORE, e->spell_mana_store);
                    refreshed++;
                }
                /* PT77o: the "broken" art bit follows the host both ways
                   (a bust on one side only showed up as an intact item
                   arriving broken after a gift). */
                {
                    int fix = coop_lite_inven_broken_fix(e->state_flags,
                        tig_art_item_id_destroyed_get(obj_field_int32_get(item, OBJ_F_CURRENT_AID)) != 0);

                    if (fix >= 0) {
                        object_set_current_aid(item,
                            tig_art_item_id_destroyed_set(obj_field_int32_get(item, OBJ_F_CURRENT_AID), fix));
                        obj_field_int32_set(item, OBJ_F_ITEM_INV_AID,
                            tig_art_item_id_destroyed_set(obj_field_int32_get(item, OBJ_F_ITEM_INV_AID), fix));
                        tig_debug_printf("CoopLite: inven: item broken state %s (host)\n", fix ? "set" : "cleared");
                        refreshed++;
                    }
                }
                /* Keys added to (or taken off) a ring: fetch the host's ring. */
                if (coop_lite_item_keyring_hash(item) != e->keyring_hash && num_rings < 16
                    && !coop_lite_handle_list_contains(rings, num_rings, item)) { /* PT53c: listed by two owners */
                    rings[num_rings++] = item;
                }
            }
        }
    }

    /* Rings whose keys differ: drop ours, the host's comes as a blob. */
    for (i = 0; i < num_rings; i++) {
        int64_t parent = OBJ_HANDLE_NULL;
        ObjectID oid = obj_get_id(rings[i]);

        if (item_parent(rings[i], &parent) && parent != OBJ_HANDLE_NULL) {
            item_remove(rings[i]);
        }
        /* PT53b: a ring item_remove missed stayed listed after the
           deallocate - a dangling (type 0) handle the inventory UI spun on. */
        for (o = 0; o < coop_inv_owner_cnt; o++) {
            if (owner_obj[o] != OBJ_HANDLE_NULL) {
                coop_lite_inv_list_drop(owner_obj[o], rings[i]);
            }
        }
        if (local_pc != OBJ_HANDLE_NULL) {
            coop_lite_inv_list_drop(local_pc, rings[i]);
        }
        /* PT53c: a ring unlisted elsewhere may also be doomed: never touch it after the free. */
        coop_lite_handle_list_clear(doomed, num_doomed, rings[i]);
        obj_deallocate(rings[i]);
        coop_lite_obj_blob_defer((const uint8_t*)&oid);
        refreshed++;
    }

    /* Doomed items nobody re-homed. */
    {
        int destroyed = 0;

        for (i = 0; i < num_doomed; i++) {
            int64_t parent = OBJ_HANDLE_NULL;

            if (doomed[i] != OBJ_HANDLE_NULL && item_parent(doomed[i], &parent) && parent == doomed_parent[i]) {
                object_destroy(doomed[i]);
                destroyed++;
            }
        }
        num_doomed = destroyed;
    }

    /* PT54: our own backpack order follows the host. All moved items out
       first (swaps), then each into the host's cell, or (taken here by
       something the host does not list yet) the nearest free one. */
    if (num_grid > 0 && local_pc != OBJ_HANDLE_NULL) {
        int placed = 0;

        for (i = 0; i < num_grid; i++) {
            item_remove(grid_item[i]);
        }
        for (i = 0; i < num_grid; i++) {
            int slots[960];
            int loc = grid_loc[i];

            item_inventory_slots_get(local_pc, slots);
            if (!item_inventory_slots_has_room_for(grid_item[i], local_pc, loc, slots)) {
                int near_loc = coop_lite_layout_nearest_cell(local_pc, grid_item[i], loc);

                loc = near_loc >= 0 ? near_loc : item_find_free_inv_loc_for_insertion(grid_item[i], local_pc);
                if (loc < 0) {
                    loc = grid_loc[i];
                }
            }
            item_insert(grid_item[i], local_pc, loc);
            placed++;
        }
        moved += placed;
    }

    /* Stacks by total. */
    for (o = 0; o < coop_inv_owner_cnt; o++) {
        int64_t owner = owner_obj[o];
        int d;

        if (owner == OBJ_HANDLE_NULL
            || !obj_type_is_critter(obj_field_int32_get(owner, OBJ_F_TYPE))) {
            continue; /* PT26: containers' stacks came as listed items */
        }
        d = coop_inv_owner[o].head.gold - item_gold_get(owner);
        if (d > 0 && obj_field_handle_get(owner, OBJ_F_CRITTER_GOLD) == OBJ_HANDLE_NULL) {
            /* PT79a: a NEW stack must not go through item_transfer: it
               checks carry weight and free cells, so a merchant (heavy,
               full grid) refused it - its gold read 0 here and every sale
               said "I cannot pay more than 0". */
            coop_lite_client_force_stack(owner,
                item_gold_create(d, obj_field_int64_get(owner, OBJ_F_LOCATION)));
        } else if (d > 0) {
            item_gold_transfer(OBJ_HANDLE_NULL, owner, d, OBJ_HANDLE_NULL);
        } else if (d < 0) {
            item_gold_transfer(owner, OBJ_HANDLE_NULL, -d, OBJ_HANDLE_NULL);
        }
        coop_lite_inv_destroy_stray(coop_inv_owner[o].head.gold_oid);
        if (owner == player_get_local_pc_obj()) {
            /* Review #2 pt 4: our stacks have local OIDs; requests about
               them must name the host's. */
            memcpy(coop_host_gold_oid, coop_inv_owner[o].head.gold_oid, 24);
            memcpy(coop_host_ammo_oid, coop_inv_owner[o].head.ammo_oid, sizeof(coop_host_ammo_oid));
        } else if (obj_type_is_critter(obj_field_int32_get(owner, OBJ_F_TYPE))) {
            coop_lite_client_remember_stacks(owner, coop_inv_owner[o].head.gold_oid,
                (const uint8_t (*)[24])coop_inv_owner[o].head.ammo_oid);
        }
        for (t = 0; t < COOP_INVEN_AMMO_TYPES; t++) {
            d = coop_inv_owner[o].head.ammo[t] - item_ammo_quantity_get(owner, t);
            if (d > 0 && obj_field_handle_get(owner, coop_ammo_fld[t]) == OBJ_HANDLE_NULL) {
                coop_lite_client_force_stack(owner,
                    item_ammo_create(d, t, obj_field_int64_get(owner, OBJ_F_LOCATION))); /* PT79a */
            } else if (d > 0) {
                item_ammo_transfer(OBJ_HANDLE_NULL, owner, d, t, OBJ_HANDLE_NULL);
            } else if (d < 0) {
                item_ammo_transfer(owner, OBJ_HANDLE_NULL, -d, t, OBJ_HANDLE_NULL);
            }
            coop_lite_inv_destroy_stray(coop_inv_owner[o].head.ammo_oid[t]);
        }
    }

    /* PT57: gold / ammo / arrows follow the host's grid cell. The host's
       stack OIDs are not ours (stacks merge), so each host stack is mapped
       to this owner's local stack of the same kind (gold, or the ammo type
       of the host prototype). Our own PC keeps its cell while a layout of
       ours is in flight (same rule as the other items). */
    for (o = 0; o < coop_inv_owner_cnt; o++) {
        int64_t owner = owner_obj[o];

        if (owner == OBJ_HANDLE_NULL || !obj_type_is_critter(obj_field_int32_get(owner, OBJ_F_TYPE))
            || (owner == local_pc && coop_lite_client_layout_hold())) {
            continue;
        }
        for (i = coop_inv_owner[o].first; i < coop_inv_owner[o].first + coop_inv_owner[o].count; i++) {
            PacketCoopInvenEntry* e = &coop_inv_entry[i];
            int64_t stack = coop_lite_local_stack_for(owner, e->proto);
            int want = e->inventory_location;
            int cur;

            if ((e->state_flags & COOP_INVEN_E_STACK) == 0 || stack == OBJ_HANDLE_NULL) {
                continue;
            }
            cur = item_inventory_location_get(stack);
            if (cur == want || cur < 0 || cur >= 120 || want < 0 || want >= 120) {
                continue;
            }
            item_remove(stack);
            {
                int slots[960];

                item_inventory_slots_get(owner, slots);
                if (!item_inventory_slots_has_room_for(stack, owner, want, slots)) {
                    int near_loc = coop_lite_layout_nearest_cell(owner, stack, want);

                    want = near_loc >= 0 ? near_loc : cur;
                }
            }
            item_insert(stack, owner, want);
            moved++;
        }
    }

    for (o = 0; o < coop_inv_owner_cnt; o++) {
        if (owner_obj[o] != OBJ_HANDLE_NULL) {
            coop_lite_inv_list_repair(owner_obj[o]); /* PT53a: no duplicates survive */
            coop_lite_fix_inven_num(owner_obj[o]);
        }
    }

    coop_lite_apply_end();
    multiplayer_unlock();
    coop_lite_obj_req_flush();

    { /* PT54d */
        int oc;
        for (oc = 0; oc < coop_inv_owner_cnt; oc++) {
            if (owner_obj[oc] != OBJ_HANDLE_NULL) {
                coop_lite_inv_selfcheck(owner_obj[oc], "inven_sync_apply");
            }
        }
    }
    /* §7 sends one batch per container: only log containers that changed. */
    if (refreshed != 0) {
        tig_debug_printf("CoopLite: inven sync: %d item fields refreshed (durability/charges/keys)\n", refreshed);
    }
    if (moved != 0 || created != 0 || num_doomed != 0 || coop_inv_owner_cnt != 1 || owner_obj[0] == OBJ_HANDLE_NULL
        || obj_field_int32_get(owner_obj[0], OBJ_F_TYPE) != OBJ_TYPE_CONTAINER) {
        tig_debug_printf("CoopLite: inven sync: %d owners, %d entries, %d moved, %d created, %d removed, gold %d\n",
            coop_inv_owner_cnt, coop_inv_entry_cnt, moved, created, num_doomed,
            local_pc != OBJ_HANDLE_NULL ? item_gold_get(local_pc) : -1);
    }
    coop_lite_client_layout_note_host(local_pc); /* PT53 */
    ui_update_inven(OBJ_HANDLE_NULL);
    free(doomed);
    free(doomed_parent);
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
        if (o == NULL || !coop_lite_oid_raw_equal(o->head.owner_oid, pkt->owner_oid)) {
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
        coop_inv_echo_seq = coop_lite_inven_echo_seq(pkt->flags); /* PT54 */
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

            if (!coop_lite_oid_raw_equal(&old_oid, &oid)) {
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
    const char* env = coop_lite_env("ARCANUM_DEV_OPEN_INVEN");
    int64_t pc = player_get_local_pc_obj();

    if (env == NULL || (env[0] != '1' && env[0] != '2' && env[0] != '3') || pc == OBJ_HANDLE_NULL) {
        return;
    }
    if (coop_dev_open_inven_done && (env[0] == '2' || env[0] == '3') && !coop_dev_dropbox_done
        && coop_lite_interval_elapsed(coop_dev_open_inven_ms, now_ms, 24000u)) {
        int64_t* items;
        int64_t pick = OBJ_HANDLE_NULL;
        int cnt;
        int i;

        coop_dev_dropbox_done = true;
        cnt = item_list_get(pc, &items);
        for (i = 0; i < cnt && pick == OBJ_HANDLE_NULL; i++) {
            /* =3 (PT18): a worn item instead, the weapon if there is one. */
            if (!coop_lite_is_stackable(items[i])
                && (env[0] == '3') == IS_WEAR_INV_LOC(item_inventory_location_get(items[i]))) {
                pick = items[i];
            }
        }
        if (env[0] == '3' && item_wield_get(pc, ITEM_INV_LOC_WEAPON) != OBJ_HANDLE_NULL) {
            pick = item_wield_get(pc, ITEM_INV_LOC_WEAPON);
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
            tig_debug_printf("CoopLite: dev: drop box: dropping one item, loc %d (%d carried)\n",
                item_inventory_location_get(pick), obj_field_int32_get(pc, OBJ_F_CRITTER_INVENTORY_NUM));
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
    const char* env = coop_lite_env("ARCANUM_DEV_CLIENT_STEP");
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
    env = coop_lite_env("ARCANUM_DEV_CLIENT_BARTER");
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
    tig_debug_printf("CoopLite: the host left the session (in session %d)\n", coop_lite_client_in_session);
    /* PT57: also while still joining / loading (it used to return here and
       the client sat on a dead session). Keep the player's progress: the
       session's state of this PC goes to its character bundle first. */
    if (coop_lite_client_in_session && player_get_local_pc_obj() != OBJ_HANDLE_NULL
        && coop_lite_client_world_ready()) {
        coop_lite_export_local_character_impl();
    }
    coop_lite_show_notice("Связь с хостом потеряна (вышел, вылет или таймаут). Возврат в главное меню; «Войти к хосту» (J/Enter). / Lost connection to the host (left, crashed or timed out). Returning to the main menu; use Join Host there (J or Enter).", TF_TYPE_RED);
    coop_host_left_menu_armed = true;
    coop_host_left_menu_ms = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1;
    /* PT62: wait in the main menu and rejoin when the host is back (its
       launcher restarts it from the last save). ARCANUM_COOP_WAIT_HOST=0 off. */
    coop_wait_host = coop_lite_wait_host_enabled(coop_lite_env("ARCANUM_COOP_WAIT_HOST"));
#endif
}

bool coop_lite_wait_host_enabled(const char* env)
{
    return env == NULL || env[0] != '0';
}

bool coop_lite_wait_host_active(void)
{
    return coop_wait_host;
}

void coop_lite_wait_host_set(bool on)
{
    coop_wait_host = on;
}

bool coop_lite_autoload_wanted(const char* env)
{
    return env != NULL && (strcmp(env, "latest") == 0 || strcmp(env, "1") == 0);
}

bool coop_lite_host_left_menu_due(bool armed, unsigned int armed_ms, unsigned int now_ms)
{
    /* A few seconds so the notice can be read. */
    return armed && coop_lite_interval_elapsed(armed_ms, now_ms, 2500u);
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
        if (coop_parked[i].used && coop_lite_oid_raw_equal(coop_parked[i].oid, oid)) {
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
    {
        /* PT63: players' hotbars, after the parked block (older builds stop before it) */
        static uint8_t hb[8 + COOP_HOTBAR_KEPT_MAX * COOP_HOTBAR_SAVE_ENTRY];

        size = coop_lite_hotbar_serialize(hb, (int)sizeof(hb));
        if (size < 0 || tig_file_fwrite(hb, (size_t)size, 1, stream) != 1) {
            return false;
        }
        tig_debug_printf("CoopLite: save: %d player hotbar(s) kept\n", coop_lite_hotbar_kept_count());
    }
    return true;
}

bool coop_lite_load_appendix(TigFile* stream)
{
    uint8_t buf[COOP_PARKED_SAVE_MAX];
    uint32_t count;
    int pos;
    int i;

    coop_lite_parked_reset();
    coop_lite_hotbar_kept_reset(); /* PT63 */
    coop_lite_hotbar_after_load();
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
    {
        static uint8_t hb[8 + COOP_HOTBAR_KEPT_MAX * COOP_HOTBAR_SAVE_ENTRY];
        uint32_t hn;

        coop_lite_hotbar_kept_reset();
        coop_lite_hotbar_after_load();
        pos = tig_file_ftell(stream);
        if (tig_file_fread(hb, 8, 1, stream) != 1 || *(uint32_t*)hb != COOP_HOTBAR_SAVE_MAGIC) {
            tig_file_fseek(stream, pos, SEEK_SET); /* saved before PT63 */
            return true;
        }
        memcpy(&hn, hb + 4, 4);
        if (hn > COOP_HOTBAR_KEPT_MAX
            || (hn > 0 && tig_file_fread(hb + 8, (size_t)hn * COOP_HOTBAR_SAVE_ENTRY, 1, stream) != 1)
            || !coop_lite_hotbar_deserialize(hb, 8 + (int)hn * COOP_HOTBAR_SAVE_ENTRY)) {
            return false;
        }
        tig_debug_printf("CoopLite: load: %d player hotbar(s) restored\n", (int)hn);
    }
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
#ifndef COOP_LITE_TEST_STUBS
        int slot;

        for (slot = 1; slot < 8; slot++) {
            coop_lite_barter_end(slot, "map change");
        }
#endif
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
#define COOP_WORLD_FULL_CONTAINERS 8
#define COOP_WORLD_NPC_DEAD 0x1

static unsigned int coop_world_req_host_ms[8];
static bool coop_world_req_host_seen[8];

/* PT17b: ground items each slot was last told about, to say where one went
   when it drops out of the list (picked up by whom, destroyed, moved). */
#define COOP_WORLD_GONE_MAX 64
static ObjectID coop_world_prev_ground[8][COOP_WORLD_GONE_MAX];
static int coop_world_prev_ground_cnt[8];
static int coop_world_prev_ground_key[8];
static ObjectID coop_world_cur_ground[COOP_WORLD_GONE_MAX];
static int coop_world_cur_ground_cnt;

static void coop_lite_host_world_ground_report(int slot, int64_t center, int key)
{
    int i;
    int j;

    if (coop_world_prev_ground_key[slot] != key) {
        coop_world_prev_ground_cnt[slot] = 0; /* another map: nothing to compare */
        coop_world_prev_ground_key[slot] = key;
    }

    for (i = 0; i < coop_world_prev_ground_cnt[slot]; i++) {
        bool still = false;
        int64_t obj;
        int64_t parent = OBJ_HANDLE_NULL;
        char oid_str[80];
        char who_str[80];

        for (j = 0; j < coop_world_cur_ground_cnt; j++) {
            if (objid_is_equal(coop_world_prev_ground[slot][i], coop_world_cur_ground[j])) {
                still = true;
                break;
            }
        }
        if (still) {
            continue;
        }
        objid_id_to_str(oid_str, coop_world_prev_ground[slot][i]);
        obj = obj_pool_perm_lookup(coop_world_prev_ground[slot][i]);
        if (obj == OBJ_HANDLE_NULL || (obj_field_int32_get(obj, OBJ_F_FLAGS) & OF_DESTROYED) != 0) {
            tig_debug_printf("CoopLite: ground item %s left the ground: destroyed\n", oid_str);
        } else if (item_parent(obj, &parent) && parent != OBJ_HANDLE_NULL) {
            int ps = coop_lite_slot_of_obj(parent);

            objid_id_to_str(who_str, obj_get_id(parent));
            tig_debug_printf("CoopLite: ground item %s left the ground: now carried by %s %s%s\n", oid_str,
                obj_field_int32_get(parent, OBJ_F_TYPE) == OBJ_TYPE_NPC ? "NPC" : (ps >= 0 ? "PC" : "container"),
                who_str, ps == 0 ? " (host PC)" : "");
        } else {
            tig_debug_printf("CoopLite: ground item %s left the list: %d tiles from slot %d's PC, flags 0x%x\n",
                oid_str, (int)location_dist(obj_field_int64_get(obj, OBJ_F_LOCATION), center), slot,
                obj_field_int32_get(obj, OBJ_F_FLAGS));
        }
    }
    memcpy(coop_world_prev_ground[slot], coop_world_cur_ground, sizeof(ObjectID) * (size_t)coop_world_cur_ground_cnt);
    coop_world_prev_ground_cnt[slot] = coop_world_cur_ground_cnt;
}
static ObjectID coop_dev_mutate_dagger;
static bool coop_dev_mutate_dagger_valid;

static void coop_lite_world_flush(PacketCoopWorldState* out, int slot, bool last, bool* first)
{
    out->flags = (out->flags & (COOP_WORLD_F_FULL | COOP_WORLD_F_TRUNCATED)) | (*first ? COOP_WORLD_F_FIRST : 0u) | (last ? COOP_WORLD_F_LAST : 0u);
    tig_net_send_app(out, coop_lite_world_state_size(out->count), slot);
    *first = false;
    out->count = 0;
}
#endif

void coop_lite_handle_pc_stats(PacketCoopPcStats* pkt)
{
#ifndef COOP_LITE_TEST_STUBS
    ObjectID oid;
    int64_t pc;

    if (pkt == NULL || pkt->type != PACKET_COOP_PC_STATS || pkt->slot < 0 || pkt->slot >= 8
        || !tig_net_is_active() || map_is_clearing_objects()) {
        return;
    }
    memcpy(&oid, pkt->oid, sizeof(oid));
    pc = obj_pool_perm_lookup(oid);
    if (pc == OBJ_HANDLE_NULL || obj_field_int32_get(pc, OBJ_F_TYPE) != OBJ_TYPE_PC) {
        return;
    }
    {
        /* PT47: portrait rides in flags' high bits (no layout change). */
        int portrait = coop_lite_pcs_portrait_unpack(pkt->flags);
        static int logged[8]; /* PT53e: one line per slot per new id */

        if (portrait > 0 && logged[pkt->slot] != portrait) {
            logged[pkt->slot] = portrait;
            tig_debug_printf("CoopLite: portrait: slot %d id %d (have %d)%s\n", pkt->slot, portrait,
                obj_field_int32_get(pc, OBJ_F_CRITTER_PORTRAIT), pc == player_get_local_pc_obj() ? " own PC" : "");
        }
        if (portrait > 0 && pc != player_get_local_pc_obj()
            && (!tig_net_is_host() || sub_4A2B60(sub_52A530()) == pc)
            && obj_field_int32_get(pc, OBJ_F_CRITTER_PORTRAIT) != portrait) {
            obj_field_int32_set(pc, OBJ_F_CRITTER_PORTRAIT, portrait);
            tig_debug_printf("CoopLite: portrait: slot %d id %d applied\n", pkt->slot, portrait);
            if (coop_party_ui_refresh_hook != NULL) {
                coop_party_ui_refresh_hook();
            }
        }
        /* PT54a: body art (race / gender / look) both ways, same rule. */
        if (pc != player_get_local_pc_obj()
            && (!tig_net_is_host() || sub_4A2B60(sub_52A530()) == pc)
            && coop_lite_base_aid_accept(pkt->base_aid, (unsigned int)obj_field_int32_get(pc, OBJ_F_AID),
                tig_art_type((tig_art_id_t)pkt->base_aid) == TIG_ART_TYPE_CRITTER)) {
            obj_field_int32_set(pc, OBJ_F_AID, (int)pkt->base_aid);
            tig_debug_printf("CoopLite: PC stats: slot %d base art -> %08x applied\n", pkt->slot, pkt->base_aid);
        }
    }
    if (tig_net_is_host()) {
        CoopPcBuild have;
        int sender = sub_52A530();

        if (sender != pkt->slot || sub_4A2B60(sender) != pc) {
            tig_debug_printf("CoopLite: PC stats: from slot %d for slot %d's PC — dropped\n", sender, pkt->slot);
            return;
        }
        coop_lite_pc_build_fill(pc, &have);
        if (memcmp(&have, &pkt->build, sizeof(have)) == 0) {
            return;
        }
        if (!coop_lite_pc_build_spend_ok(&have, &pkt->build)) {
            tig_debug_printf("CoopLite: PC stats: slot %d spend REJECTED (unspent %d -> %d, STR %d -> %d) — resending ours\n",
                sender, have.stats[STAT_UNSPENT_POINTS], pkt->build.stats[STAT_UNSPENT_POINTS],
                have.stats[STAT_STRENGTH], pkt->build.stats[STAT_STRENGTH]);
            coop_lite_pc_stats_send(sender, pc, COOP_PC_STATS_F_FORCE, sender);
            return;
        }
        coop_lite_pc_build_apply(pc, &pkt->build, COOP_PCB_APPLY_SPEND);
        tig_debug_printf("CoopLite: PC stats: slot %d spend accepted (unspent %d -> %d, STR %d, max hp %d)\n",
            sender, have.stats[STAT_UNSPENT_POINTS], stat_base_get(pc, STAT_UNSPENT_POINTS),
            stat_base_get(pc, STAT_STRENGTH), object_hp_max(pc));
        return; /* the 500 ms watch rebroadcasts it */
    }
    {
        bool own = pkt->slot == sub_529520() && pc == player_get_local_pc_obj();
        bool force = (pkt->flags & COOP_PC_STATS_F_FORCE) != 0;
        int old_level = stat_base_get(pc, STAT_LEVEL);
        /* PT20: a level-up on the host may have run an auto-level scheme
           (pregens have one) that spent the new points there; take it all. */
        bool levelled = own && pkt->build.stats[STAT_LEVEL] > old_level;
        int mode = own && !force && !levelled ? COOP_PCB_APPLY_OWNER : COOP_PCB_APPLY_FULL;
        CoopPcBuild now;

        multiplayer_lock();
        coop_lite_apply_begin();
        coop_lite_pc_build_apply(pc, &pkt->build, mode);
        coop_lite_apply_end();
        multiplayer_unlock();
        if (own) {
            coop_lite_pc_build_fill(pc, &now);
            coop_client_pcs_sig = coop_lite_pc_build_sig(&now); /* not a spend of ours */
            if (stat_base_get(pc, STAT_LEVEL) > old_level) {
                /* PT20: the host levelled us (level_recalc ran there); the
                   vanilla level-up notice and the character button. */
                UiMessage ui_message;
                char str[1] = { '\0' };

                ui_message.type = UI_MSG_TYPE_LEVEL;
                ui_message.str = str;
                ui_message.field_8 = stat_base_get(pc, STAT_LEVEL);
                ui_message.field_C = auto_level_scheme_get(pc) != 0 ? -1 : stat_base_get(pc, STAT_UNSPENT_POINTS);
                ui_display_msg(&ui_message);
                ui_toggle_primary_button(UI_PRIMARY_BUTTON_CHAR, true);
                ui_refresh_health_bar(pc);
                ui_refresh_fatigue_bar(pc);
                tig_debug_printf("CoopLite: PC stats: level up %d -> %d (unspent %d)\n", old_level,
                    stat_base_get(pc, STAT_LEVEL), stat_base_get(pc, STAT_UNSPENT_POINTS));
            }
        }
        coop_lite_pc_build_fill(pc, &now);
        tig_debug_printf("CoopLite: PC stats: slot %d applied (%s), sig %08x, STR %d unspent %d, max hp %d\n",
            pkt->slot, force ? "forced" : (levelled ? "level-up, full" : (own ? "owner fields" : "full")),
            coop_lite_pc_build_sig(&now),
            stat_base_get(pc, STAT_STRENGTH), stat_base_get(pc, STAT_UNSPENT_POINTS), object_hp_max(pc));
    }
#else
    (void)pkt;
#endif
}

static int coop_world_full_key[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
static int64_t coop_world_full_pc[8];
static bool coop_world_full_active[8];
static bool coop_world_full_need_walk[8]; /* PT52k: object walk queued for the tick */
static int coop_world_full_cursor[8];
static unsigned int coop_world_full_ms[8];

#ifndef COOP_LITE_TEST_STUBS
/* PT52: the full snapshot pages over an ObjectID list taken in one pass at
   its start (was: re-walking the live pool and skipping *cursor objects,
   which shifted when objects were created/destroyed mid-dump, so the
   client pruned live objects). Gone/picked-up objects are skipped at send. */
#define COOP_WORLD_FULL_MAX_OIDS 16384
static ObjectID* coop_world_full_oids[8];
static int coop_world_full_oid_cnt[8];
static int64_t coop_world_full_sectors[8][COOP_WORLD_FULL_SECTORS];
static int coop_world_full_sector_cnt[8];
static bool coop_world_full_first[8]; /* PT52c */
static bool coop_world_full_trunc[8]; /* PT79: list cut at COOP_WORLD_FULL_MAX_OIDS */
#define COOP_WORLD_FULL_FLAGS(slot) (COOP_WORLD_F_FULL | (coop_world_full_trunc[slot] ? COOP_WORLD_F_TRUNCATED : 0u))

static void coop_world_full_free(int slot)
{
    if (coop_world_full_oids[slot] != NULL) {
        FREE(coop_world_full_oids[slot]);
    }
    coop_world_full_oids[slot] = NULL;
    coop_world_full_oid_cnt[slot] = 0;
    coop_world_full_sector_cnt[slot] = 0;
    coop_world_full_trunc[slot] = false;
}

/* PT60b: breakable scenery the snapshot lists: anything not invulnerable,
   plus what the host busted (busted = invulnerable), damaged or turned off.
   The client prunes only scenery that is still breakable on its side. */
static bool coop_world_scenery_listed(int64_t obj, int type)
{
    unsigned int flags;

    if (type != OBJ_TYPE_SCENERY) {
        return false;
    }
    flags = (unsigned int)obj_field_int32_get(obj, OBJ_F_FLAGS);
    return (flags & OF_INVULNERABLE) == 0 || (flags & OF_OFF) != 0
        || (obj_field_int32_get(obj, OBJ_F_SCENERY_FLAGS) & OSCF_BUSTED) != 0
        || obj_field_int32_get(obj, OBJ_F_HP_DAMAGE) > 0;
}

static int64_t* coop_world_scope_dst;
static int* coop_world_scope_n;
static int coop_world_scope_cap;
static bool coop_world_scope_cb(Sector* sector);

/* PT77ad: full snapshot order - NPCs and portals first (dead NPCs and
   doors before ground items / containers). */
static bool coop_lite_world_full_early_type(int type)
{
    return type == OBJ_TYPE_NPC || type == OBJ_TYPE_PORTAL;
}

static bool coop_world_full_snapshot(int slot)
{
    int64_t obj;
    int iter;
    int n = 0;
    int n_late = 0;
    ObjectID* late;

    coop_world_full_free(slot);
    coop_world_full_first[slot] = true;
    /* Lock the 3x3 around every PC before we snapshot, then remember exactly
       those loaded sectors. Emitting a later sector_enumerate (after more
       sectors lock) marked neighbours for prune without listing their
       corpses. */
    {
        int64_t host_pc = sub_4A2B60(0);

        if (host_pc != OBJ_HANDLE_NULL) {
            coop_lite_keep_sector(0, obj_field_int64_get(host_pc, OBJ_F_LOCATION));
        }
        coop_lite_keep_remote_sectors();
    }
    coop_world_scope_dst = coop_world_full_sectors[slot];
    coop_world_full_sector_cnt[slot] = 0;
    coop_world_scope_n = &coop_world_full_sector_cnt[slot];
    coop_world_scope_cap = COOP_WORLD_FULL_SECTORS;
    sector_enumerate(coop_world_scope_cb);
    coop_world_scope_dst = NULL;
    coop_world_scope_n = NULL;
    coop_world_full_oids[slot] = (ObjectID*)MALLOC(sizeof(ObjectID) * COOP_WORLD_FULL_MAX_OIDS);
    if (coop_world_full_oids[slot] == NULL) {
        return false; /* PT52d: caller falls back to the radius reply */
    }
    /* PT77ad (review 3b): NPCs and portals first, so dead NPCs and door
       states land in the first pages, not after every ground item. The
       rest goes to a scratch list and is appended (stable); without the
       scratch memory everything stays in walk order. */
    late = (ObjectID*)MALLOC(sizeof(ObjectID) * COOP_WORLD_FULL_MAX_OIDS);
    if (obj_inst_first(&obj, &iter)) {
        do {
            int type;

            if ((obj_field_int32_get(obj, OBJ_F_FLAGS) & (OF_DESTROYED | OF_INVENTORY)) != 0) {
                continue;
            }
            type = obj_field_int32_get(obj, OBJ_F_TYPE);
            if (type != OBJ_TYPE_PORTAL && type != OBJ_TYPE_CONTAINER && type != OBJ_TYPE_NPC
                && type != OBJ_TYPE_TRAP && !coop_world_scenery_listed(obj, type) && (type < OBJ_TYPE_WEAPON || type > OBJ_TYPE_GENERIC)) {
                continue;
            }
            if (n + n_late >= COOP_WORLD_FULL_MAX_OIDS) {
                /* PT79 (review 1.1): the client's seen-set holds exactly this
                   many, so it never overflowed and its full prune destroyed
                   every listed-sector object past the cap. Flag the pages. */
                coop_world_full_trunc[slot] = true;
                tig_debug_printf("CoopLite: world: full snapshot for slot %d cut at %d objects - client will not prune\n",
                    slot, COOP_WORLD_FULL_MAX_OIDS);
                break;
            }
            if (late != NULL && !coop_lite_world_full_early_type(type)) {
                late[n_late++] = obj_get_id(obj);
            } else {
                coop_world_full_oids[slot][n++] = obj_get_id(obj);
            }
        } while (obj_inst_next(&obj, &iter));
    }
    if (late != NULL) {
        memcpy(&coop_world_full_oids[slot][n], late, sizeof(ObjectID) * (size_t)n_late);
        n += n_late;
        FREE(late);
    }
    coop_world_full_oid_cnt[slot] = n;
    return true;
}

static bool coop_world_full_next(int slot, int* cursor, int64_t* obj_ptr)
{
    while (*cursor < coop_world_full_oid_cnt[slot]) {
        int64_t obj = obj_pool_perm_lookup(coop_world_full_oids[slot][(*cursor)++]);

        if (obj != OBJ_HANDLE_NULL) {
            *obj_ptr = obj;
            return true;
        }
    }
    return false;
}

static bool coop_world_scope_cb(Sector* sector)
{
    if (sector == NULL || coop_world_scope_dst == NULL || coop_world_scope_n == NULL) {
        return true;
    }
    (void)coop_lite_sector_list_add(coop_world_scope_dst, coop_world_scope_n, coop_world_scope_cap, sector->id);
    return true;
}

/* PT51: one WORLD_STATE pass. full = whole loaded map (no radius), paged:
   resumes after *cursor visited objects and stops after max_chunks chunks
   (built at send time, so a page never carries older state than what the
   live event packets already sent). Returns true when the snapshot is done. */
static bool coop_world_host_emit(int slot, int key, int64_t pc, bool full, int* cursor, int max_chunks)
{

    PacketCoopWorldState* out;
    int64_t containers[COOP_WORLD_MAX_CONTAINERS];
    int num_containers = 0;
    int64_t center;
    int64_t obj;
    int iter;
    bool first = true;
    int n_portal = 0;
    int n_container = 0;
    int n_npc = 0;
    int n_item = 0;
    int n_trap = 0;
    Uint64 t0;

    int chunks = 0;
    bool done = true;

    /* PT52c: FIRST stays pending until a page really goes out */
    first = full ? coop_world_full_first[slot] : true;
    t0 = SDL_GetTicksNS();
    center = obj_field_int64_get(pc, OBJ_F_LOCATION);
    out = (PacketCoopWorldState*)MALLOC(sizeof(*out));
    if (out == NULL) {
        tig_debug_printf("CoopLite: PT52e alloc failed\n");
        return false; /* PT52i: not done; the full tick retries the page */
    }
    memset(out, 0, offsetof(PacketCoopWorldState, entries));
    out->type = PACKET_COOP_WORLD_STATE;
    out->map = key;
    if (!full) {
        coop_world_cur_ground_cnt = 0;
    }

    /* PT77y: first page of a full snapshot lists every host-loaded sector
       so the client prunes emptied ones that have no object entries. */
    if (full && first) {
        int si;
        int scope_n = coop_world_full_sector_cnt[slot];

        for (si = 0; si < scope_n; si++) {
            CoopWorldEntry* e = &out->entries[out->count];

            memset(e, 0, sizeof(*e));
            e->kind = COOP_WORLD_SECTOR_SCOPE;
            e->loc = LOCATION_MAKE(SECTOR_X(coop_world_full_sectors[slot][si]) * 64,
                SECTOR_Y(coop_world_full_sectors[slot][si]) * 64);
            if (++out->count == COOP_WORLD_CHUNK) {
                out->flags = COOP_WORLD_FULL_FLAGS(slot);
                coop_lite_world_flush(out, slot, false, &first);
                chunks++;
            }
        }
    }

    if (full ? coop_world_full_next(slot, cursor, &obj) : obj_inst_first(&obj, &iter)) {
        do {
            unsigned int flags = obj_field_int32_get(obj, OBJ_F_FLAGS);
            int type;
            int64_t loc;
            CoopWorldEntry* e;
            ObjectID oid;

            /* PT52b: also at most COOP_WORLD_FULL_CONTAINERS containers per
               page/tick: each one is a reliable INVEN_SYNC batch. */
            if (full && (chunks >= max_chunks || num_containers >= COOP_WORLD_FULL_CONTAINERS)) {
                (*cursor)--; /* not consumed: first entry of the next page */
                done = false;
                break;
            }
            if ((flags & (OF_DESTROYED | OF_INVENTORY)) != 0) {
                continue;
            }
            type = obj_field_int32_get(obj, OBJ_F_TYPE);
            if (type != OBJ_TYPE_PORTAL && type != OBJ_TYPE_CONTAINER && type != OBJ_TYPE_NPC
                && type != OBJ_TYPE_TRAP && !coop_world_scenery_listed(obj, type)
                && (type < OBJ_TYPE_WEAPON || type > OBJ_TYPE_GENERIC)) {
                continue;
            }
            loc = obj_field_int64_get(obj, OBJ_F_LOCATION);
            /* PT31: dead / switched-off NPCs map-wide (a rejoining client's
               save still has them alive), the living ones in reach (so the
               client can drop what the host destroyed). */
            if (full) {
                /* PT51: whole map */
            } else if (type == OBJ_TYPE_NPC && (critter_is_dead(obj) || (flags & OF_OFF) != 0)) {
                /* PT37: a window (COOP_WORLD_DEAD_RADIUS), not the whole map; the 100 ms
                   NPC_POS stream (DEAD flag) and the next WORLD_REQ cover the rest. */
                if (location_dist(loc, center) > COOP_WORLD_DEAD_RADIUS) {
                    continue;
                }
            } else if (location_dist(loc, center) > COOP_WORLD_RADIUS) {
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
                e->hp_damage = obj_field_int32_get(obj, OBJ_F_HP_DAMAGE); /* PT60 */
                e->reserved[0] = trap_type(obj) != TRAP_TYPE_INVALID ? COOP_WORLD_TRAP_ARMED : 0u;
                n_portal++;
                break;
            case OBJ_TYPE_CONTAINER:
                e->kind = COOP_WORLD_CONTAINER;
                e->of_flags = flags & COOP_WORLD_PORTAL_OF_MASK;
                e->kind_flags = (unsigned int)obj_field_int32_get(obj, OBJ_F_CONTAINER_FLAGS);
                e->reserved[0] = trap_type(obj) != TRAP_TYPE_INVALID ? COOP_WORLD_TRAP_ARMED : 0u;
                /* PT77ad (review 5a): contents only near the PC; opening
                   any container syncs it (COOP_BARTER_OPEN_LOOT). */
                if (num_containers < COOP_WORLD_MAX_CONTAINERS
                    && location_dist(loc, center) <= COOP_WORLD_CONTENTS_RADIUS) {
                    containers[num_containers++] = obj;
                }
                n_container++;
                break;
            case OBJ_TYPE_TRAP:
                e->kind = COOP_WORLD_TRAP;
                e->of_flags = flags & COOP_WORLD_TRAP_OF_MASK;
                e->kind_flags = trap_type(obj) != TRAP_TYPE_INVALID ? COOP_WORLD_TRAP_ARMED : 0u;
                n_trap++;
                break;
            case OBJ_TYPE_SCENERY: /* PT60b */
                e->kind = COOP_WORLD_SCENERY;
                e->of_flags = flags & COOP_WORLD_SCENERY_OF_MASK;
                e->kind_flags = (unsigned int)obj_field_int32_get(obj, OBJ_F_SCENERY_FLAGS);
                e->hp_damage = obj_field_int32_get(obj, OBJ_F_HP_DAMAGE);
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
                if (!full && coop_world_cur_ground_cnt < COOP_WORLD_GONE_MAX) {
                    coop_world_cur_ground[coop_world_cur_ground_cnt++] = oid;
                }
                e->kind_flags = coop_lite_is_stackable(obj) ? 1u : 0u;
                n_item++;
                if (coop_lite_env("ARCANUM_COOP_WORLD_TRACE") != NULL) {
                    char oid_str[80];

                    objid_id_to_str(oid_str, oid);
                    tig_debug_printf("CoopLite: world trace: host ground item %s at %d,%d (client PC %d,%d)\n",
                        oid_str, (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc),
                        (int)LOCATION_GET_X(center), (int)LOCATION_GET_Y(center));
                }
                break;
            }
            if (++out->count == COOP_WORLD_CHUNK) {
                out->flags = full ? COOP_WORLD_FULL_FLAGS(slot) : 0u;
                coop_lite_world_flush(out, slot, false, &first);
                chunks++;
            }
        } while (full ? coop_world_full_next(slot, cursor, &obj) : obj_inst_next(&obj, &iter));
    }
    if (done || out->count > 0) {
        out->flags = full ? COOP_WORLD_FULL_FLAGS(slot) : 0u;
        if (done) {
            coop_lite_world_flush(out, slot, true, &first);
        } else {
            coop_lite_world_flush(out, slot, false, &first);
        }
    }
    if (full) {
        coop_world_full_first[slot] = first;
    }
    FREE(out);
    if (!full) {
        coop_lite_host_world_ground_report(slot, center, key);
    }
    if (coop_dev_mutate_dagger_valid && coop_lite_env("ARCANUM_COOP_WORLD_TRACE") != NULL) {
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
    if (full && !done) {
        return false;
    }
    tig_debug_printf("CoopLite: world state%s → slot %d: %d portals, %d containers, %d npcs, %d ground items, %d traps (%.2f ms)\n",
        full ? " (full map, last page)" : "", slot, n_portal, n_container, n_npc, n_item, n_trap, (double)(SDL_GetTicksNS() - t0) / 1e6);
    return true;
}
#endif

#ifndef COOP_LITE_TEST_STUBS
/* PT51 host: page pending full-map snapshots (rate-limited, one page per
   slot per COOP_WORLD_FULL_PAGE_MS). Cancel on PC change / map change; the
   client's next WORLD_REQ (map load) starts a fresh one. */
static void coop_world_full_tick(unsigned int now_ms)
{
    int slot;
    int key;
    bool walked = false;

    if (!tig_net_is_active() || !tig_net_is_host()) {
        return;
    }
    key = coop_lite_pose_map_key(map_current_map(), coop_map_gen);
    for (slot = 1; slot < 8; slot++) {
        int64_t pc;

        pc = sub_4A2B60(slot);
        if (pc == OBJ_HANDLE_NULL) {
            /* disconnected: a rejoin gets a full snapshot again */
            coop_world_full_active[slot] = false;
            coop_world_full_free(slot);
            coop_world_full_key[slot] = -1;
            continue;
        }
        if (!coop_world_full_active[slot]) {
            continue;
        }
        if (pc != coop_world_full_pc[slot] || coop_world_full_key[slot] != key) {
            coop_world_full_active[slot] = false;
            coop_world_full_free(slot);
            coop_world_full_key[slot] = -1;
            continue;
        }
        if (coop_world_full_need_walk[slot]) {
            if (walked) {
                continue; /* PT52k: next tick */
            }
            walked = true;
            coop_world_full_need_walk[slot] = false;
            if (!coop_world_full_snapshot(slot)) {
                coop_world_full_active[slot] = false;
                coop_world_full_key[slot] = -1;
                coop_world_full_first[slot] = false; /* PT52i */
                tig_debug_printf("CoopLite: PT52d full snapshot alloc failed, radius reply\n");
                (void)coop_world_host_emit(slot, key, pc, false, NULL, 0);
            } else {
                tig_debug_printf("CoopLite: PT51 full-map world snapshot → slot %d (map key %d)\n", slot, key);
            }
            continue;
        }
        if (coop_world_full_ms[slot] != 0
            && !coop_lite_interval_elapsed(coop_world_full_ms[slot], now_ms, COOP_WORLD_FULL_PAGE_MS)) {
            continue;
        }
        coop_world_full_ms[slot] = now_ms != 0 ? now_ms : 1;
        if (coop_world_host_emit(slot, key, pc, true, &coop_world_full_cursor[slot], 1)) {
            coop_world_full_active[slot] = false;
            coop_world_full_free(slot);
        }
    }
}
#endif

#ifndef COOP_LITE_TEST_STUBS
static void coop_lite_host_walls_to_slot(int slot); /* PT61 */
#endif

void coop_lite_handle_world_req(PacketCoopWorldReq* pkt)
{
#ifndef COOP_LITE_TEST_STUBS
    int64_t pc;
    int slot;
    int key;

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
    /* PT52d: a new map key / PC is never rate-limited (first full snapshot) */
    if (coop_world_req_host_seen[slot]
        && coop_world_full_key[slot] == key && coop_world_full_pc[slot] == pc
        && !coop_lite_interval_elapsed(coop_world_req_host_ms[slot], coop_obj_now_ms, 3000u)) {
        return;
    }
    coop_world_req_host_seen[slot] = true;
    coop_world_req_host_ms[slot] = coop_obj_now_ms;
    coop_lite_host_walls_to_slot(slot); /* PT61 */

    /* PT51: first request for this map (join, rejoin, map/area entry) gets
       the whole-map snapshot, paged from the tick. No radius reply while a
       full stream runs: its FIRST would reset the client's snapshot. */
    if (coop_world_full_active[slot]) {
        return;
    }
    if (coop_world_full_key[slot] != key || coop_world_full_pc[slot] != pc) {
        coop_world_full_key[slot] = key;
        coop_world_full_pc[slot] = pc;
        coop_world_full_active[slot] = true;
        coop_world_full_cursor[slot] = 0;
        coop_world_full_ms[slot] = 0;
        /* PT52k: the whole-map object walk runs from the tick, at most one
           per tick, so 7 simultaneous joins do not stall one host frame. */
        coop_world_full_need_walk[slot] = true;
        tig_debug_printf("CoopLite: PT52k full-map snapshot queued → slot %d (map key %d)\n", slot, key);
        return;
    }
    (void)coop_world_host_emit(slot, key, pc, false, NULL, 0);
#else
    (void)pkt;
#endif
}


#ifndef COOP_LITE_TEST_STUBS
/* Review P0-3: was MALLOC'd once and reused (never freed at exit). Static
   now; 24 KB. */
static CoopOidSet coop_world_seen_set;
static CoopOidSet* const coop_world_seen = &coop_world_seen_set;
/* PT31: NPCs the host listed (dead map-wide, living in reach). */
static CoopOidSet coop_world_npc_seen_set;
static CoopOidSet* const coop_world_npc_seen = &coop_world_npc_seen_set;
static bool coop_world_rx_active;
static int coop_world_rx_map;
static bool coop_world_rx_full; /* PT51: this snapshot is the full-map one */
static int64_t coop_world_rx_sectors[COOP_WORLD_FULL_SECTORS];
static int coop_world_rx_sector_cnt;
static bool coop_world_rx_sector_overflow;
static bool coop_world_rx_trunc; /* PT79: host cut the full list - no prune */
static int coop_world_stats[6]; /* per kind: applied changes */
static int coop_world_req_key = -1;
static unsigned int coop_world_req_ms;
static int64_t coop_world_req_sector = -1; /* PT77ad */
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
static int coop_world_fetched; /* unknown ground items asked for, this snapshot */
static bool coop_world_log_pending;
static int coop_world_pruned;
static int coop_world_ghosts; /* PT27: unseen pool copies dropped for a refetch */

static void coop_lite_world_apply(const CoopWorldEntry* e)
{
    int64_t obj;
    int type;

    if (e->kind == COOP_WORLD_SECTOR_SCOPE) {
        return; /* loc already added to the prune set in handle_world_state */
    }
    obj = coop_lite_get_oid_obj(e->oid);

    if (e->kind == COOP_WORLD_GROUND_ITEM) {
        obj = coop_lite_client_reclaim_destroyed(obj);
    }

    if (e->kind == COOP_WORLD_CONTAINER || e->kind == COOP_WORLD_PORTAL || e->kind == COOP_WORLD_SCENERY) {
        coop_oid_set_add(coop_world_seen, e->oid); /* PT26: junk piles prune too; PT60: doors */
    }
    if (e->kind == COOP_WORLD_GROUND_ITEM) {
        coop_oid_set_add(coop_world_seen, e->oid);
    }
    if (e->kind == COOP_WORLD_NPC) {
        coop_oid_set_add(coop_world_npc_seen, e->oid); /* PT31 */
    }
    if (e->kind == COOP_WORLD_TRAP) {
        coop_oid_set_add(coop_world_seen, e->oid); /* PT42: unlisted floor traps prune */
    }
    if (obj == OBJ_HANDLE_NULL) {
        /* Unknown ground item: asked for after the batch (a request sent
           while packets are suspended would be lost). */
        if (e->kind == COOP_WORLD_GROUND_ITEM
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
        }
        if (type == OBJ_TYPE_PORTAL && e->hp_damage >= 0
            && obj_field_int32_get(obj, OBJ_F_HP_DAMAGE) != e->hp_damage) {
            obj_field_int32_set(obj, OBJ_F_HP_DAMAGE, e->hp_damage); /* PT60: door HP */
            coop_world_stats[e->kind]++;
        } else if (type == OBJ_TYPE_CONTAINER
            && (unsigned int)obj_field_int32_get(obj, OBJ_F_CONTAINER_FLAGS) != e->kind_flags) {
            obj_field_int32_set(obj, OBJ_F_CONTAINER_FLAGS, (int)e->kind_flags);
            coop_world_stats[e->kind]++;
        }
        if ((e->reserved[0] & COOP_WORLD_TRAP_ARMED) == 0 && trap_type(obj) != TRAP_TYPE_INVALID) {
            /* PT41: disarmed (or sprung) on the host */
            trap_remove(obj);
            coop_world_stats[e->kind]++;
            tig_debug_printf("CoopLite: world: trap removed (host state)\n");
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
        } else if ((e->kind_flags & COOP_WORLD_NPC_DEAD) == 0 && critter_is_dead(obj)) {
            /* Review P1-2: alive (resurrected) at the host, a corpse here. */
            obj_field_int32_set(obj, OBJ_F_HP_DAMAGE, e->hp_damage > 0 ? e->hp_damage : 0);
            object_flags_unset(obj, OF_FLAT | OF_NO_BLOCK);
            object_set_current_aid(obj, (tig_art_id_t)e->aid);
            coop_world_stats[e->kind]++;
        }
        coop_lite_world_apply_flags(obj, e->of_flags, COOP_WORLD_NPC_OF_MASK, e->kind);
        break;
    case COOP_WORLD_WALL: /* PT61: never added to the prune set (walls are never pruned) */
        if (type != OBJ_TYPE_WALL) {
            return;
        }
        if (e->aid != 0 && (unsigned int)obj_field_int32_get(obj, OBJ_F_CURRENT_AID) != e->aid) {
            object_set_current_aid(obj, (tig_art_id_t)e->aid);
        }
        coop_lite_world_apply_flags(obj, e->of_flags, COOP_WORLD_WALL_OF_MASK, COOP_WORLD_PORTAL);
        break;
    case COOP_WORLD_SCENERY: /* PT60b */
        if (type != OBJ_TYPE_SCENERY) {
            return;
        }
        if ((unsigned int)obj_field_int32_get(obj, OBJ_F_CURRENT_AID) != e->aid) {
            object_set_current_aid(obj, (tig_art_id_t)e->aid);
            coop_world_stats[e->kind]++;
        }
        coop_lite_world_apply_flags(obj, e->of_flags, COOP_WORLD_SCENERY_OF_MASK, e->kind);
        if ((unsigned int)obj_field_int32_get(obj, OBJ_F_SCENERY_FLAGS) != e->kind_flags) {
            if ((e->kind_flags & OSCF_BUSTED) != 0
                && (obj_field_int32_get(obj, OBJ_F_SCENERY_FLAGS) & OSCF_BUSTED) == 0) {
                tig_debug_printf("CoopLite: object: scenery busted on the host - applied here\n");
            }
            obj_field_int32_set(obj, OBJ_F_SCENERY_FLAGS, (int)e->kind_flags);
            coop_world_stats[e->kind]++;
        }
        if (e->hp_damage >= 0 && obj_field_int32_get(obj, OBJ_F_HP_DAMAGE) != e->hp_damage) {
            obj_field_int32_set(obj, OBJ_F_HP_DAMAGE, e->hp_damage);
            coop_world_stats[e->kind]++;
        }
        break;
    case COOP_WORLD_TRAP:
        if (type != OBJ_TYPE_TRAP) {
            return;
        }
        if ((e->kind_flags & COOP_WORLD_TRAP_ARMED) == 0) {
            /* disarmed / spent at the host */
            object_destroy(obj);
            coop_world_stats[e->kind]++;
            tig_debug_printf("CoopLite: world: floor trap dropped (spent at host)\n");
            return;
        }
        {
            unsigned int have = (unsigned int)obj_field_int32_get(obj, OBJ_F_FLAGS);
            unsigned int want = coop_lite_world_trap_flags_merge(have, e->of_flags);

            if ((want & ~have) != 0) {
                object_flags_set(obj, want & ~have);
            }
            if ((have & ~want) != 0) {
                object_flags_unset(obj, have & ~want);
            }
            if (want != have) {
                coop_world_stats[e->kind]++;
            }
        }
        break;
    case COOP_WORLD_GROUND_ITEM:
        if (!item_is_item(obj)) {
            return;
        }
        if ((obj_field_int32_get(obj, OBJ_F_FLAGS) & OF_INVENTORY) != 0) {
            /* Carried here, on the ground at the host. PT25: when the
               carrier is not us (the host's PC dropped it, an NPC lost it)
               nothing else ever moves it - the host never resyncs its own
               PC's inventory to us - so the host's drop stayed invisible.
               Our own PC's items are settled by our INVEN_SYNC (a pickup
               of ours may still be in flight). */
            int64_t parent = OBJ_HANDLE_NULL;

            if (item_parent(obj, &parent) && parent != OBJ_HANDLE_NULL
                && parent != player_get_local_pc_obj()) {
                /* object_drop, not sub_466E50: on a client that one only
                   asks the host to drop it (Packet27). */
                item_remove(obj);
                object_drop(obj, e->loc);
                coop_world_stats[e->kind]++;
            }
            return;
        }
        if (!coop_lite_client_ground_loc_live(obj_field_int64_get(obj, OBJ_F_LOCATION))) {
            /* PT27 item 3: a copy loaded earlier into a sector we never had
               (or one since unloaded) - in no sector list, drawn nowhere.
               Drop it and fetch the host's object afresh where it lies. */
            object_destroy(obj);
            if (coop_lite_client_reclaim_destroyed(obj) != OBJ_HANDLE_NULL) {
                return; /* a script kept it */
            }
            coop_world_ghosts++;
            if (coop_world_want_cnt < COOP_WORLD_CHUNK) {
                coop_world_want[coop_world_want_cnt++] = e;
            }
            return;
        }
        if (obj_field_int64_get(obj, OBJ_F_LOCATION) != e->loc) {
            sub_43E770(obj, e->loc, 0, 0);
            coop_world_stats[e->kind]++;
        }
        break;
    }
}

/* PT51: full snapshot = only sectors the host listed something in (it had
   them loaded); radius snapshot = PRUNE_RADIUS around the request point. */
static bool coop_world_prune_in_scope(int64_t loc, int64_t center)
{
    if (coop_world_rx_full) {
        return coop_lite_sector_list_has(coop_world_rx_sectors, coop_world_rx_sector_cnt, sector_id_from_loc(loc));
    }
    return location_dist(loc, center) <= COOP_WORLD_PRUNE_RADIUS;
}

/* LAST chunk: ground items near our PC the host did not list are gone. */
static int coop_lite_world_prune(void)
{
    int64_t pc = player_get_local_pc_obj();
    int64_t center;
    int64_t doomed[2048];
    int num = 0;
    int total = 0;
    int64_t obj;
    int iter;
    int i;

    if (pc == OBJ_HANDLE_NULL || coop_world_seen->overflow || !coop_world_req_center_valid) {
        return 0;
    }
    center = coop_world_req_center; /* PT32: not the current position */
    if (coop_world_rx_full && (coop_world_rx_sector_overflow || coop_world_rx_trunc)) {
        return 0;
    }
again:
    num = 0;
    if (obj_inst_first(&obj, &iter)) {
        do {
            unsigned int flags = obj_field_int32_get(obj, OBJ_F_FLAGS);
            ObjectID oid;

            if ((flags & (OF_DESTROYED | OF_INVENTORY)) != 0) {
                continue;
            }
            if (obj_field_int32_get(obj, OBJ_F_TYPE) == OBJ_TYPE_NPC) {
                /* PT31: an NPC in reach the host did not list is gone there
                   (corpse decayed, destroyed): a stale copy from our save. */
                if (!coop_world_npc_seen->overflow
                    && coop_world_prune_in_scope(obj_field_int64_get(obj, OBJ_F_LOCATION), center)) {
                    oid = obj_get_id(obj);
                    if (!coop_oid_set_has(coop_world_npc_seen, (const uint8_t*)&oid) && num < 2048) {
                        doomed[num++] = obj;
                    }
                }
                continue;
            }
            /* PT26: a junk pile the host destroyed when it was emptied
               stayed here with stale contents ("transfer refused").
               PT56: any container - one the host broke (object_bust ->
               object_destroy) stayed here at negative HP. */
            if (!item_is_item(obj)
                && obj_field_int32_get(obj, OBJ_F_TYPE) != OBJ_TYPE_TRAP /* PT42 */
                && obj_field_int32_get(obj, OBJ_F_TYPE) != OBJ_TYPE_CONTAINER
                && obj_field_int32_get(obj, OBJ_F_TYPE) != OBJ_TYPE_PORTAL /* PT60 */
                && !(obj_field_int32_get(obj, OBJ_F_TYPE) == OBJ_TYPE_SCENERY /* PT60b */
                    && (flags & OF_INVULNERABLE) == 0)) {
                continue;
            }
            if (!coop_world_prune_in_scope(obj_field_int64_get(obj, OBJ_F_LOCATION), center)) {
                continue;
            }
            oid = obj_get_id(obj);
            if (!coop_oid_set_has(coop_world_seen, (const uint8_t*)&oid) && num < 2048) {
                doomed[num++] = obj;
            }
        } while (obj_inst_next(&obj, &iter));
    }
    for (i = 0; i < num; i++) {
        if (coop_lite_env("ARCANUM_COOP_WORLD_TRACE") != NULL) {
            char oid_str[80];
            int64_t l = obj_field_int64_get(doomed[i], OBJ_F_LOCATION);

            objid_id_to_str(oid_str, obj_get_id(doomed[i]));
            tig_debug_printf("CoopLite: world trace: prune %s at %d,%d\n", oid_str,
                (int)LOCATION_GET_X(l), (int)LOCATION_GET_Y(l));
        }
        if (obj_field_int32_get(doomed[i], OBJ_F_TYPE) == OBJ_TYPE_CONTAINER) {
            tig_debug_printf("CoopLite: world: container gone on the host (broken/destroyed) - removed here (hp dmg %d)\n",
                object_hp_damage_get(doomed[i]));
        } else if (obj_field_int32_get(doomed[i], OBJ_F_TYPE) == OBJ_TYPE_PORTAL) {
            tig_debug_printf("CoopLite: world: door gone on the host (bashed/destroyed) - removed here\n");
        } else if (obj_field_int32_get(doomed[i], OBJ_F_TYPE) == OBJ_TYPE_SCENERY) {
            tig_debug_printf("CoopLite: world: scenery gone on the host (broken/destroyed) - removed here\n");
        }
        object_destroy(doomed[i]);
    }
    total += num;
    if (num == 2048) {
        goto again; /* more than one batch of leftovers */
    }
    return total;
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
    /* PT52e: a stray radius page must not mix into a running full stream */
    if (coop_world_rx_active && coop_world_rx_full
        && (pkt->flags & (COOP_WORLD_F_FULL | COOP_WORLD_F_FIRST)) == 0) {
        return;
    }
    if ((pkt->flags & COOP_WORLD_F_FIRST) != 0) {
        coop_oid_set_reset(coop_world_seen);
        coop_oid_set_reset(coop_world_npc_seen);
        memset(coop_world_stats, 0, sizeof(coop_world_stats));
        coop_world_fetched = 0;
        coop_world_rx_active = true;
        coop_world_rx_map = pkt->map;
        coop_world_rx_full = (pkt->flags & COOP_WORLD_F_FULL) != 0;
        coop_world_rx_trunc = false;
        coop_world_rx_sector_cnt = 0;
        coop_world_rx_sector_overflow = false;
    }
    if ((pkt->flags & COOP_WORLD_F_TRUNCATED) != 0) {
        coop_world_rx_trunc = true; /* PT79 */
    }
    if (coop_world_rx_full) {
        for (i = 0; i < pkt->count; i++) {
            coop_world_rx_sector_overflow |= !coop_lite_sector_list_add(coop_world_rx_sectors,
                &coop_world_rx_sector_cnt, COOP_WORLD_FULL_SECTORS, sector_id_from_loc(pkt->entries[i].loc));
        }
    }
    if (!coop_world_rx_active || coop_world_rx_map != pkt->map) {
        return;
    }

    multiplayer_lock();
    coop_lite_apply_begin();
    coop_world_want_cnt = 0;
    for (i = 0; i < pkt->count; i++) {
        coop_lite_world_apply(&pkt->entries[i]);
    }
    if ((pkt->flags & COOP_WORLD_F_LAST) != 0) {
        int pruned = coop_lite_world_prune();

        if (coop_lite_env("ARCANUM_COOP_WORLD_TRACE") != NULL && player_get_local_pc_obj() != OBJ_HANDLE_NULL) {
            int64_t l = obj_field_int64_get(player_get_local_pc_obj(), OBJ_F_LOCATION);

            tig_debug_printf("CoopLite: world trace: local PC at %d,%d\n",
                (int)LOCATION_GET_X(l), (int)LOCATION_GET_Y(l));
        }

        if (coop_lite_env("ARCANUM_COOP_WORLD_TRACE") != NULL) {
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
        coop_world_log_pending = true;
        coop_world_pruned = pruned;
    }
    coop_lite_apply_end();
    multiplayer_unlock();
    for (i = 0; i < coop_world_want_cnt; i++) {
        if (coop_lite_client_want_ground_blob(coop_world_want[i]->oid, coop_world_want[i]->loc,
                coop_world_want[i]->proto)) {
            coop_world_fetched++;
        }
    }
    coop_world_want_cnt = 0;
    coop_lite_obj_req_flush();
    if (coop_world_log_pending) {
        /* Counts are changes: an item already where the host has it counts
           as listed, not changed. */
        coop_world_log_pending = false;
        if (coop_world_reclaimed > 0) {
            tig_debug_printf("CoopLite: world/inven: %d items destroyed here earlier are listed by the host again - refetched\n",
                coop_world_reclaimed);
            coop_world_reclaimed = 0;
        }
        if (coop_world_ghosts > 0) {
            tig_debug_printf("CoopLite: world state: %d ground items were known here but in no loaded sector - refetched\n",
                coop_world_ghosts);
            coop_world_ghosts = 0;
        }
        coop_lite_inv_selfcheck(player_get_local_pc_obj(), "world_state_prune"); /* PT54d */
        tig_debug_printf("CoopLite: world state applied: changed portals %d, containers %d, npcs %d, ground items %d; fetched %d; %d ground items listed; %d pruned\n",
            coop_world_stats[COOP_WORLD_PORTAL], coop_world_stats[COOP_WORLD_CONTAINER],
            coop_world_stats[COOP_WORLD_NPC], coop_world_stats[COOP_WORLD_GROUND_ITEM],
            coop_world_fetched, coop_world_seen->count, coop_world_pruned);
    }
#else
    (void)pkt;
#endif
}

#ifndef COOP_LITE_TEST_STUBS
static void coop_lite_world_state_reset(void)
{
    coop_lite_client_pickup_reset();
    memset(coop_world_req_host_seen, 0, sizeof(coop_world_req_host_seen));
    {
        int i;

        for (i = 0; i < 8; i++) {
            coop_world_full_key[i] = -1; /* PT51: every session starts with a full snapshot */
            coop_world_full_active[i] = false;
            coop_world_full_need_walk[i] = false; /* PT52k */
            coop_world_full_pc[i] = OBJ_HANDLE_NULL;
            coop_world_full_free(i);
        }
    }
    coop_world_rx_active = false;
    coop_world_req_key = -1;
    coop_world_req_sector = -1;
    coop_world_was_ready = false;
    coop_world_ready_key = -1;
}

/* Client tick (world ready): ask after a map load and every 30 s. */
/* PT25: the host dropped something - ask for the world state in ~3.5 s
   (the host answers a slot at most every 3 s), not at the 30 s refresh. */
static void coop_lite_client_world_refresh_soon(void)
{
    unsigned int now_ms = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1;

    coop_world_req_ms = now_ms - 30000u + 3500u;
}

static void coop_lite_client_world_req_tick(unsigned int now_ms)
{
    PacketCoopWorldReq req;
    int key;

    if (coop_lite_env("ARCANUM_COOP_NO_WORLD_STATE") != NULL) {
        return;
    }
    key = coop_lite_pose_map_key(map_current_map(), coop_map_gen);
    if (!coop_world_was_ready || key != coop_world_ready_key) {
        coop_world_was_ready = true;
        coop_world_ready_key = key;
        coop_world_ready_ms = now_ms;
    }
    {
        /* PT77ad (review 3c): a new sector of the same map asks at once
           (radius reply: dead NPCs within COOP_WORLD_DEAD_RADIUS). */
        int64_t lpc = player_get_local_pc_obj();
        int64_t sec = lpc != OBJ_HANDLE_NULL ? sector_id_from_loc(obj_field_int64_get(lpc, OBJ_F_LOCATION)) : -1;

        if (key == coop_world_req_key && sec != -1
            && coop_lite_world_req_sector_due(coop_world_req_sector, sec, coop_world_req_ms, now_ms)) {
            if (coop_world_req_sector != -1) {
                coop_world_req_ms = now_ms - 30000u; /* due below */
            }
            coop_world_req_sector = sec;
        } else if (key != coop_world_req_key) {
            coop_world_req_sector = sec;
        }
    }
    if (!coop_lite_world_req_due(coop_world_req_key, key, coop_world_ready_ms, coop_world_req_ms, now_ms)) {
        return;
    }
    coop_world_req_key = key;
    coop_world_req_ms = now_ms;
    {
        int64_t lpc = player_get_local_pc_obj();

        coop_world_req_center_valid = lpc != OBJ_HANDLE_NULL;
        if (coop_world_req_center_valid) {
            coop_world_req_center = obj_field_int64_get(lpc, OBJ_F_LOCATION);
        }
    }
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
    const char* env = coop_lite_env("ARCANUM_DEV_WORLD_MUTATE");
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

/* ---- ground pickup (PT16) ------------------------------------------------ */

/*
 * The pickup goal runs inside our locally simulated goal (packets
 * suspended), so it only queues; the coop tick then sends a fresh pose and
 * the Packet28 requests on the reliable channel (in order, so the host's
 * reach check sees where we are now). Review P0-2: up to
 * COOP_PICKUP_QUEUE_MAX requests (fast clicks no longer overwrite each
 * other), each tagged with the map key; the queue is dropped on session /
 * world reset and stale entries (other map) are discarded.
 */
#ifndef COOP_LITE_TEST_STUBS
static CoopPickupQueue coop_pickup_q;
static CoopPickupQueue coop_door_q; /* PT37 */
#endif

void coop_pickup_queue_reset(CoopPickupQueue* q)
{
    q->count = 0;
}

bool coop_pickup_queue_push(CoopPickupQueue* q, const uint8_t oid[24], int map_key)
{
    int i;

    for (i = 0; i < q->count; i++) {
        if (coop_lite_oid_raw_equal(q->oid[i], oid)) {
            q->map_key[i] = map_key;
            return true; /* same item clicked twice: one request */
        }
    }
    if (q->count >= COOP_PICKUP_QUEUE_MAX) {
        return false;
    }
    memcpy(q->oid[q->count], oid, 24);
    q->map_key[q->count] = map_key;
    q->count++;
    return true;
}

int coop_pickup_queue_take(CoopPickupQueue* q, int map_key, uint8_t out[][24], int max)
{
    int n = 0;
    int i;

    for (i = 0; i < q->count; i++) {
        if (q->map_key[i] == map_key && n < max) {
            memcpy(out[n++], q->oid[i], 24);
        }
    }
    q->count = 0;
    return n;
}

void coop_lite_client_queue_pickup(int64_t item)
{
#ifndef COOP_LITE_TEST_STUBS
    char oid_str[80];
    ObjectID oid;

    if (item == OBJ_HANDLE_NULL) {
        return;
    }
    oid = obj_get_id(item);
    objid_id_to_str(oid_str, oid);
    if (!coop_pickup_queue_push(&coop_pickup_q, (const uint8_t*)&oid,
            coop_lite_pose_map_key(map_current_map(), coop_map_gen))) {
        tig_debug_printf("CoopLite: pickup queue full, %s dropped\n", oid_str);
        return;
    }
    tig_debug_printf("CoopLite: pickup queued %s\n", oid_str);
#else
    (void)item;
#endif
}

#ifndef COOP_LITE_TEST_STUBS
static void coop_lite_client_pickup_reset(void)
{
    coop_pickup_queue_reset(&coop_pickup_q);
    coop_pickup_queue_reset(&coop_door_q);
}

static void coop_lite_client_pickup_tick(unsigned int now_ms)
{
    int64_t pc = player_get_local_pc_obj();
    uint8_t oids[COOP_PICKUP_QUEUE_MAX][24];
    PacketCoopPcPos pos;
    Packet28 pkt;
    int n;
    int i;

    (void)now_ms;
    if (coop_pickup_q.count == 0) {
        return;
    }
    n = coop_pickup_queue_take(&coop_pickup_q, coop_lite_pose_map_key(map_current_map(), coop_map_gen),
        oids, COOP_PICKUP_QUEUE_MAX);
    if (pc == OBJ_HANDLE_NULL || n == 0 || critter_is_dead(pc) || coop_rejoin_pc_wanted) {
        return;
    }
    /* Where we stand now, ahead of the requests on the same channel. */
    coop_lite_fill_pc_pos(&pos, sub_529520(), pc);
    pos.epoch = coop_own_pose_epoch;
    pos.seq = ++coop_own_pose_seq;
    coop_client_pos = pos;
    coop_client_pos_valid = true;
    tig_net_send_app_all(&pos, sizeof(pos));
    for (i = 0; i < n; i++) {
        int64_t item = coop_lite_get_oid_obj(oids[i]);

        if (item == OBJ_HANDLE_NULL) {
            continue;
        }
        memset(&pkt, 0, sizeof(pkt));
        pkt.type = 28;
        sub_4F0640(item, &(pkt.item_oid));
        sub_4F0640(pc, &(pkt.critter_oid));
        pkt.inventory_location = -1;
        tig_net_send_app_all(&pkt, sizeof(pkt));
    }
    tig_debug_printf("CoopLite: pickup request sent to host (%d)\n", n);
}
#endif

bool coop_lite_door_use_valid(const PacketCoopDoorUse* pkt, int size)
{
    return pkt != NULL && pkt->type == PACKET_COOP_DOOR_USE && size >= (int)sizeof(PacketCoopDoorUse);
}

bool coop_lite_door_state_valid(const PacketCoopDoorState* pkt, int size)
{
    return pkt != NULL && pkt->type == PACKET_COOP_DOOR_STATE && size >= (int)sizeof(PacketCoopDoorState)
        && coop_lite_object_state_kind_ok(pkt->e.kind) && pkt->slot >= -1 && pkt->slot < 8
        && (pkt->e.kind == COOP_WORLD_PORTAL || pkt->e.kind == COOP_WORLD_SCENERY
            || pkt->e.kind == COOP_WORLD_WALL
            || (pkt->e.reserved[1] & COOP_WORLD_OBJ_GONE) != 0);
}

/* PT60b: DOOR_STATE carries doors, breakable scenery and (gone only)
   containers. Never NPCs, ground items or traps (own paths). */
bool coop_lite_object_state_kind_ok(int kind)
{
    return kind == COOP_WORLD_PORTAL || kind == COOP_WORLD_SCENERY || kind == COOP_WORLD_CONTAINER
        || kind == COOP_WORLD_WALL; /* PT61 */
}

/* PT63: client fallback when a host world-object id does not resolve:
   first live object of the type with the same art, else the first live one. */
int coop_lite_world_pick_candidate(const int* types, const bool* dead, const unsigned int* aids, int n,
    int want_type, unsigned int aid)
{
    int i;
    int any = -1;

    if (types == NULL || dead == NULL || aids == NULL) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (types[i] != want_type || dead[i]) {
            continue;
        }
        if (aid != 0 && aids[i] == aid) {
            return i;
        }
        if (any < 0) {
            any = i;
        }
    }
    return any;
}

bool coop_lite_door_pending_add(uint8_t (*set)[24], int* count, int max, const uint8_t* oid)
{
    int i;

    if (set == NULL || count == NULL || oid == NULL || *count < 0) {
        return false;
    }
    for (i = 0; i < *count && i < max; i++) {
        if (coop_lite_oid_raw_equal(set[i], oid)) {
            return true; /* already pending: sent with its newest state */
        }
    }
    if (*count >= max) {
        return false;
    }
    memcpy(set[*count], oid, 24);
    (*count)++;
    return true;
}

bool coop_lite_base_aid_accept(unsigned int aid, unsigned int have, bool aid_is_critter)
{
    return aid != 0 && aid != 0xFFFFFFFFu && aid_is_critter && aid != have;
}

bool coop_lite_portrait_use_number(bool coop, int num, bool alt_file_found)
{
    if (coop && num >= 1000) {
        return true; /* engine portrait: the same file on every machine */
    }
    return !alt_file_found;
}

bool coop_lite_skill_use_valid(const PacketCoopSkillUse* pkt, int size)
{
    return pkt != NULL && pkt->type == PACKET_COOP_SKILL_USE && size >= (int)sizeof(PacketCoopSkillUse)
        && pkt->flags == 0 && pkt->modifier >= -100 && pkt->modifier <= 100;
}

bool coop_lite_craft_valid(const PacketCoopCraft* pkt, int size)
{
    return pkt != NULL && pkt->type == PACKET_COOP_CRAFT && size >= (int)sizeof(PacketCoopCraft)
        && pkt->schematic >= 0 && pkt->schematic < 100000;
}

/* PT44: death writes hp damage 32000 (critter_kill / death path); floats
   showed that marker as "-31988"-style hits. Clamp to the hp pool. */
/* PT79 (pure; tests): exact size, sane level and pools. */
bool coop_lite_npc_stats_valid(const PacketCoopNpcStats* pkt, int size)
{
    return pkt != NULL && size == (int)sizeof(PacketCoopNpcStats) && pkt->type == PACKET_COOP_NPC_STATS
        && pkt->stats[17 /* STAT_LEVEL */] >= 1 && pkt->stats[17] <= 100
        && pkt->hp_pts >= 0 && pkt->hp_pts <= 100000 && pkt->fatigue_pts >= 0 && pkt->fatigue_pts <= 100000
        && pkt->hp_adj >= -100000 && pkt->hp_adj <= 100000
        && pkt->fatigue_adj >= -100000 && pkt->fatigue_adj <= 100000;
}

bool coop_lite_area_known_valid(const PacketCoopAreaKnown* pkt, int size)
{
    return pkt != NULL && size == (int)sizeof(PacketCoopAreaKnown) && pkt->type == PACKET_COOP_AREA_KNOWN
        && pkt->count >= 0 && pkt->count <= COOP_AREA_KNOWN_MAX;
}

/* PT78 (pure; tests): ORs the first count bits of src into dst; returns
   how many were new. */
int coop_lite_area_bits_merge(uint8_t* dst, const uint8_t* src, int count)
{
    int added = 0;
    int i;

    if (dst == NULL || src == NULL) {
        return 0;
    }
    if (count > COOP_AREA_KNOWN_MAX) {
        count = COOP_AREA_KNOWN_MAX;
    }
    for (i = 0; i < count; i++) {
        uint8_t m = (uint8_t)(1u << (i & 7));

        if ((src[i >> 3] & m) != 0 && (dst[i >> 3] & m) == 0) {
            dst[i >> 3] |= m;
            added++;
        }
    }
    return added;
}

int coop_lite_npc_rx_hp_damage(int host_damage, bool host_dead, int local_max)
{
    if (!host_dead && local_max > 0 && host_damage >= local_max) {
        return local_max - 1;
    }
    return host_damage;
}

int coop_lite_float_delta(int prev, int now, int hp_max)
{
    if (hp_max <= 0 || prev < 0 || prev >= hp_max || now <= prev) {
        return 0;
    }
    if (now > hp_max) {
        now = hp_max;
    }
    return now - prev;
}

int coop_lite_inv_compact_handles(int64_t* h, int n)
{
    int i;
    int j;
    int k = 0;

    for (i = 0; i < n; i++) {
        bool dup = false;

        if (h[i] == 0) {
            continue;
        }
        for (j = 0; j < k; j++) {
            if (h[j] == h[i]) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            h[k++] = h[i];
        }
    }
    return k;
}

/* PT54d: by_key[i] = handle stored at key i (0 = absent/null), for
   i < scanned. A healthy list has keys 0..count-1 non-null, unique,
   nothing at/after count, and NUM == count. */
int coop_lite_inv_keys_check(const int64_t* by_key, int scanned, int count, int num)
{
    int i;
    int j;

    if (num != count) {
        return COOP_INV_KEYS_NUM_MISMATCH;
    }
    for (i = 0; i < scanned; i++) {
        if (i < count && by_key[i] == 0) {
            return COOP_INV_KEYS_HOLE;
        }
        if (i >= count && by_key[i] != 0) {
            return COOP_INV_KEYS_STALE_TAIL;
        }
    }
    for (i = 0; i < count && i < scanned; i++) {
        for (j = 0; j < i; j++) {
            if (by_key[j] == by_key[i]) {
                return COOP_INV_KEYS_DUPLICATE;
            }
        }
    }
    return COOP_INV_KEYS_OK;
}

const char* coop_lite_inv_keys_check_name(int code)
{
    switch (code) {
    case COOP_INV_KEYS_OK: return "ok";
    case COOP_INV_KEYS_NUM_MISMATCH: return "NUM != list length";
    case COOP_INV_KEYS_HOLE: return "null entry (hole)";
    case COOP_INV_KEYS_STALE_TAIL: return "stale key past list length";
    case COOP_INV_KEYS_DUPLICATE: return "duplicate entry";
    }
    return "?";
}

int coop_lite_skill_merge_training(int local_value, int host_value)
{
    return (local_value & 0x3F) | (host_value & ~0x3F);
}

unsigned int coop_lite_pcs_portrait_pack(unsigned int flags, int portrait)
{
    flags &= 0xFFFFu;
    if (portrait > 0 && portrait < 0x8000) {
        flags |= (unsigned int)portrait << 16;
    }
    return flags;
}

int coop_lite_pcs_portrait_unpack(unsigned int flags)
{
    return (int)((flags >> 16) & 0x7FFFu);
}

bool coop_lite_loot_retry_auto_slot(bool moved, bool stackable, bool still_at_source)
{
    return !moved && !stackable && still_at_source;
}

unsigned int coop_lite_inven_state_flags(bool stackable, bool broken)
{
    if (stackable) {
        return COOP_INVEN_E_STACK;
    }
    return COOP_INVEN_E_STATE | (broken ? COOP_INVEN_E_BROKEN : 0u);
}

int coop_lite_inven_broken_fix(unsigned int state_flags, bool locally_broken)
{
    bool host_broken;

    if ((state_flags & COOP_INVEN_E_STATE) == 0) {
        return -1;
    }
    host_broken = (state_flags & COOP_INVEN_E_BROKEN) != 0;
    if (host_broken == locally_broken) {
        return -1;
    }
    return host_broken ? 1 : 0;
}

/*
 * PT23/PT77y: NPC_POS looks 40 tiles out, so a PC in the middle of a 64-
 * tile sector still needs the eight neighbours. Keep the 3x3 around every
 * active PC (skipping negative ids). The cache is 64 sectors in co-op.
 */
int coop_lite_keep_ids(int64_t loc, int64_t* ids, int max)
{
    int64_t sx = LOCATION_GET_X(loc) >> 6;
    int64_t sy = LOCATION_GET_Y(loc) >> 6;
    int dx;
    int dy;
    int n = 0;

    if (ids == NULL || max <= 0) {
        return 0;
    }
    for (dy = -1; dy <= 1; dy++) {
        for (dx = -1; dx <= 1; dx++) {
            int64_t x = sx + dx;
            int64_t y = sy + dy;

            if (x < 0 || y < 0) {
                continue;
            }
            if (n >= max) {
                return n;
            }
            ids[n++] = SECTOR_MAKE(x, y);
        }
    }
    return n;
}

/* Unchanged idle NPCs are keyframed every 5 s. A slot that just entered
   range has not seen that last state even if the global cache matches. */
bool coop_lite_npc_pos_force(unsigned int seen_mask, unsigned int in_range, bool unchanged)
{
    return !unchanged || ((seen_mask & in_range) != in_range);
}

int coop_lite_critter_effects_size(int count)
{
    if (count < 0 || count > COOP_CRITTER_EFFECTS_MAX) {
        return 0;
    }
    return (int)(offsetof(PacketCoopCritterEffects, data) + (size_t)count * 2u * sizeof(unsigned int));
}

bool coop_lite_critter_effects_valid(const PacketCoopCritterEffects* pkt, int size)
{
    if (pkt == NULL || pkt->type != PACKET_COOP_CRITTER_EFFECTS) {
        return false;
    }
    if (pkt->count < 0 || pkt->count > COOP_CRITTER_EFFECTS_MAX) {
        return false;
    }
    return size == coop_lite_critter_effects_size(pkt->count);
}

unsigned int coop_lite_critter_effects_sig(int count, const unsigned int* effects, const unsigned int* causes,
    unsigned int light_flags, unsigned int light_aid, unsigned int light_color)
{
    unsigned int h = 2166136261u;
    int i;

    if (count < 0 || (count > 0 && (effects == NULL || causes == NULL))) {
        return 1;
    }
    h ^= (unsigned int)count;
    h *= 16777619u;
    for (i = 0; i < count; i++) {
        h ^= effects[i];
        h *= 16777619u;
        h ^= causes[i];
        h *= 16777619u;
    }
    h ^= light_flags;
    h *= 16777619u;
    h ^= light_aid;
    h *= 16777619u;
    h ^= light_color;
    h *= 16777619u;
    if (h == 0) {
        h = 1;
    }
    return h;
}

bool coop_lite_throw_intent_valid(const PacketCoopThrowIntent* pkt, int size)
{
    int i;
    bool any = false;

    if (pkt == NULL || pkt->type != PACKET_COOP_THROW_INTENT || size != (int)sizeof(PacketCoopThrowIntent)
        || pkt->target_loc == 0) {
        return false;
    }
    for (i = 0; i < 24; i++) {
        if (pkt->item_oid[i] != 0) {
            any = true;
            break;
        }
    }
    return any;
}

bool coop_lite_throw_in_range(int dist, int throw_range)
{
    if (dist < 0 || throw_range < 1) {
        return false;
    }
    return dist <= throw_range + COOP_THROW_RANGE_SLACK;
}

bool coop_lite_projectile_fx_valid(const PacketCoopProjectileFx* pkt, int size)
{
    return pkt != NULL && pkt->type == PACKET_COOP_PROJECTILE_FX
        && size == (int)sizeof(PacketCoopProjectileFx)
        && pkt->src_loc != 0 && pkt->dst_loc != 0
        && pkt->missile_art_id != 0;
}

bool coop_lite_party_gift(bool source_is_pc, bool target_is_pc, bool from_target)
{
    return source_is_pc && target_is_pc && !from_target;
}

bool coop_lite_oid_unmap_ok(int64_t mapped, int64_t obj)
{
    return mapped == obj || mapped == OBJ_HANDLE_NULL;
}

bool coop_lite_worldmap_open_allowed(bool can_use_worldmap, bool selecting, int townmap)
{
    return can_use_worldmap || (!selecting && townmap != 0);
}

bool coop_lite_merchant_drop_blocked(bool coop_active, bool parent_is_store, bool parent_is_live_merchant)
{
    return coop_active && (parent_is_store || parent_is_live_merchant);
}

/* PT77g: what a client's item may target on the host: critters (potions,
   healing kits, scrolls); trap devices also doors, containers and walls
   (trap_use_on_obj decides the rest, e.g. scripted doors). */
bool coop_lite_use_item_target_ok(bool trap_device, int target_type)
{
    /* Raw obj.h values (this builds in the policy test too):
       0 wall, 1 portal, 2 container, 15 PC, 16 NPC. */
    if (target_type == 15 || target_type == 16) {
        return true;
    }
    return trap_device && target_type >= 0 && target_type <= 2;
}

bool coop_lite_body_drag_valid(const PacketCoopBodyDrag* pkt, int size)
{
    return pkt != NULL && pkt->type == PACKET_COOP_BODY_DRAG && size >= (int)sizeof(PacketCoopBodyDrag)
        && pkt->reserved == 0 && pkt->oid[0] != 0;
}

bool coop_lite_use_item_valid(const PacketCoopSkillUse* pkt, int size)
{
    return coop_lite_skill_use_valid(pkt, size) && pkt->skill == COOP_SKILL_USE_ITEM
        && pkt->item_oid[0] != 0;
}

bool coop_lite_fate_party_target(bool pc_present, bool slot_active, bool is_earner, int slot)
{
    return pc_present && !is_earner && slot >= 0 && slot < 8 && (slot == 0 || slot_active);
}

void coop_lite_host_portal_changed(void)
{
    if (tig_net_is_active() && tig_net_is_host() && tig_net_is_coop_enabled()) {
        coop_portal_changed = true;
        coop_portal_changed_ms = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1;
    }
}

#ifndef COOP_LITE_TEST_STUBS
/* ---- PT78: known world-map areas, shared by everybody (union) ---- */
static uint8_t coop_area_sent[COOP_AREA_KNOWN_MAX / 8];
static int coop_area_sent_count = -1;
static unsigned int coop_area_tick_ms;
static unsigned int coop_area_key_ms;
/* PT79 (review 1.2): a client sends nothing until the host's set of this
   session / load arrived; that first set REPLACES the client's (its old
   bits may be from a later save or another game and would leak into an
   earlier host save). */
static bool coop_area_baseline;

void coop_lite_area_session_reset(void)
{
    coop_area_baseline = false;
    coop_area_sent_count = -1;
    coop_area_key_ms = 0;
}

/* area.c: the engine keeps one known-area set (the local PC's). In a
   session every co-op PC uses it, so a script asking about (or teaching)
   a client's PC on the host reaches the shared set. */
bool coop_lite_area_pc_shared(int64_t pc_obj)
{
    return pc_obj != OBJ_HANDLE_NULL && tig_net_is_active() && tig_net_is_coop_enabled()
        && coop_lite_slot_of_obj(pc_obj) >= 0;
}

static void coop_lite_area_tick(unsigned int now_ms)
{
    PacketCoopAreaKnown pkt;

    if (!coop_lite_interval_elapsed(coop_area_tick_ms, now_ms, 1000u)) {
        return;
    }
    coop_area_tick_ms = now_ms != 0 ? now_ms : 1;
    if (!tig_net_is_host() && !coop_area_baseline) {
        return; /* PT79: wait for the host's set */
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_AREA_KNOWN;
    pkt.count = area_known_bits_get(pkt.bits, COOP_AREA_KNOWN_MAX);
    /* On change at once; otherwise every 10 s (a joiner, a host load or a
       client reset gets the full set back). */
    if (pkt.count == coop_area_sent_count && memcmp(pkt.bits, coop_area_sent, sizeof(pkt.bits)) == 0
        && !coop_lite_interval_elapsed(coop_area_key_ms, now_ms, 10000u)) {
        return;
    }
    coop_area_key_ms = coop_area_tick_ms;
    coop_area_sent_count = pkt.count;
    memcpy(coop_area_sent, pkt.bits, sizeof(pkt.bits));
    tig_net_send_app_all(&pkt, sizeof(pkt));
}

/* PT79 (review 1.4): NPC followers level on the host only (xp from the
   party's kills); clients kept the join-time build, so their portrait hp
   bar, carry limit (transfers refused as too heavy) and hp clamps were
   off. Sent per follower on change, re-sent every 10 s. */
#define COOP_NPC_STATS_SIGS 32
static struct {
    ObjectID oid;
    unsigned int sig;
    unsigned int ms;
    bool used;
} coop_npc_stats_sig[COOP_NPC_STATS_SIGS];
static int coop_npc_stats_sig_next;

static void coop_lite_npc_stats_fill(PacketCoopNpcStats* pkt, int64_t npc)
{
    ObjectID oid = obj_get_id(npc);
    int i;

    memset(pkt, 0, sizeof(*pkt));
    pkt->type = PACKET_COOP_NPC_STATS;
    memcpy(pkt->oid, &oid, sizeof(oid));
    for (i = 0; i < COOP_NPC_STATS_COUNT; i++) {
        pkt->stats[i] = obj_arrayfield_int32_get(npc, OBJ_F_CRITTER_STAT_BASE_IDX, i);
    }
    pkt->hp_pts = obj_field_int32_get(npc, OBJ_F_HP_PTS);
    pkt->hp_adj = obj_field_int32_get(npc, OBJ_F_HP_ADJ);
    pkt->fatigue_pts = obj_field_int32_get(npc, OBJ_F_CRITTER_FATIGUE_PTS);
    pkt->fatigue_adj = obj_field_int32_get(npc, OBJ_F_CRITTER_FATIGUE_ADJ);
}

static void coop_lite_host_npc_stats(int64_t npc, unsigned int now_ms)
{
    PacketCoopNpcStats pkt;
    unsigned int sig = 2166136261u;
    const unsigned char* b;
    size_t i;
    int k;
    int hit = -1;

    if (npc == OBJ_HANDLE_NULL || obj_field_int32_get(npc, OBJ_F_TYPE) != OBJ_TYPE_NPC) {
        return;
    }
    coop_lite_npc_stats_fill(&pkt, npc);
    b = (const unsigned char*)&pkt;
    for (i = 0; i < sizeof(pkt); i++) {
        sig = (sig ^ b[i]) * 16777619u;
    }
    for (k = 0; k < COOP_NPC_STATS_SIGS; k++) {
        if (coop_npc_stats_sig[k].used && memcmp(&coop_npc_stats_sig[k].oid, pkt.oid, sizeof(ObjectID)) == 0) {
            hit = k;
            break;
        }
    }
    if (hit >= 0 && coop_npc_stats_sig[hit].sig == sig
        && !coop_lite_interval_elapsed(coop_npc_stats_sig[hit].ms, now_ms, 10000u)) {
        return;
    }
    if (hit < 0) {
        hit = coop_npc_stats_sig_next;
        coop_npc_stats_sig_next = (coop_npc_stats_sig_next + 1) % COOP_NPC_STATS_SIGS;
        memcpy(&coop_npc_stats_sig[hit].oid, pkt.oid, sizeof(ObjectID));
        coop_npc_stats_sig[hit].used = true;
    }
    coop_npc_stats_sig[hit].sig = sig;
    coop_npc_stats_sig[hit].ms = now_ms != 0 ? now_ms : 1;
    tig_net_send_app_all(&pkt, sizeof(pkt));
}

void coop_lite_handle_npc_stats(PacketCoopNpcStats* pkt)
{
    ObjectID oid;
    int64_t npc;
    int64_t leader;
    int i;
    int old_max;
    bool changed = false;

    if (!tig_net_is_active() || tig_net_is_host()
        || !coop_lite_npc_stats_valid(pkt, tig_net_last_message_size())
        || !coop_lite_client_world_ready()) {
        return;
    }
    memcpy(&oid, pkt->oid, sizeof(oid));
    npc = obj_pool_perm_lookup(oid);
    if (npc == OBJ_HANDLE_NULL || obj_field_int32_get(npc, OBJ_F_TYPE) != OBJ_TYPE_NPC) {
        return;
    }
    leader = critter_leader_get(npc);
    if (leader == OBJ_HANDLE_NULL || coop_lite_slot_of_obj(leader) < 0) {
        return; /* only party followers (the FOLLOWERS table binds them first) */
    }
    old_max = object_hp_max(npc);
    multiplayer_lock();
    tig_net_local_suspend_begin();
    for (i = 0; i < COOP_NPC_STATS_COUNT; i++) {
        if (obj_arrayfield_int32_get(npc, OBJ_F_CRITTER_STAT_BASE_IDX, i) != pkt->stats[i]) {
            obj_arrayfield_int32_set(npc, OBJ_F_CRITTER_STAT_BASE_IDX, i, pkt->stats[i]);
            changed = true;
        }
    }
    if (obj_field_int32_get(npc, OBJ_F_HP_PTS) != pkt->hp_pts) {
        obj_field_int32_set(npc, OBJ_F_HP_PTS, pkt->hp_pts);
        changed = true;
    }
    if (obj_field_int32_get(npc, OBJ_F_HP_ADJ) != pkt->hp_adj) {
        obj_field_int32_set(npc, OBJ_F_HP_ADJ, pkt->hp_adj);
        changed = true;
    }
    if (obj_field_int32_get(npc, OBJ_F_CRITTER_FATIGUE_PTS) != pkt->fatigue_pts) {
        obj_field_int32_set(npc, OBJ_F_CRITTER_FATIGUE_PTS, pkt->fatigue_pts);
        changed = true;
    }
    if (obj_field_int32_get(npc, OBJ_F_CRITTER_FATIGUE_ADJ) != pkt->fatigue_adj) {
        obj_field_int32_set(npc, OBJ_F_CRITTER_FATIGUE_ADJ, pkt->fatigue_adj);
        changed = true;
    }
    tig_net_local_suspend_end();
    multiplayer_unlock();
    if (changed) {
        tig_debug_printf("CoopLite: follower build synced: level %d, max hp %d -> %d\n",
            pkt->stats[17], old_max, object_hp_max(npc));
    }
}

void coop_lite_handle_area_known(PacketCoopAreaKnown* pkt)
{
    int added;

    if (!tig_net_is_active() || !tig_net_is_coop_enabled()
        || !coop_lite_area_known_valid(pkt, tig_net_last_message_size())) {
        return;
    }
    if (!tig_net_is_host() && !coop_lite_client_in_session) {
        return;
    }
    if (!tig_net_is_host() && !coop_area_baseline) {
        area_known_clear_all(); /* PT79: the host's set is the baseline */
        coop_area_baseline = true;
        coop_area_sent_count = -1;
        tig_debug_printf("CoopLite: areas: host's known locations adopted as the baseline\n");
    }
    added = area_known_bits_merge(pkt->bits, pkt->count);
    if (added > 0) {
        if (tig_net_is_host()) {
            tig_debug_printf("CoopLite: areas: %d new known location(s) from slot %d - shared with everybody\n",
                added, sub_52A530());
            coop_area_tick_ms = 0; /* broadcast the union on the next tick */
        } else {
            tig_debug_printf("CoopLite: areas: %d new known location(s) from the party\n", added);
        }
    }
}

/* Client, own PC (packets are suspended inside its local goal): the walk to
   the door ran here; the use itself is the host's. */
void coop_lite_client_queue_door_use(int64_t portal)
{
    ObjectID oid;

    if (portal == OBJ_HANDLE_NULL) {
        return;
    }
    oid = obj_get_id(portal);
    if (coop_pickup_queue_push(&coop_door_q, (const uint8_t*)&oid,
            coop_lite_pose_map_key(map_current_map(), coop_map_gen))) {
        tig_debug_printf("CoopLite: door use queued\n");
    }
}

static void coop_lite_client_door_tick(void)
{
    int64_t pc = player_get_local_pc_obj();
    uint8_t oids[COOP_PICKUP_QUEUE_MAX][24];
    PacketCoopPcPos pos;
    PacketCoopDoorUse pkt;
    int n;
    int i;

    if (coop_door_q.count == 0) {
        return;
    }
    n = coop_pickup_queue_take(&coop_door_q, coop_lite_pose_map_key(map_current_map(), coop_map_gen),
        oids, COOP_PICKUP_QUEUE_MAX);
    if (pc == OBJ_HANDLE_NULL || n == 0 || critter_is_dead(pc) || coop_rejoin_pc_wanted) {
        return;
    }
    coop_lite_fill_pc_pos(&pos, sub_529520(), pc);
    pos.epoch = coop_own_pose_epoch;
    pos.seq = ++coop_own_pose_seq;
    coop_client_pos = pos;
    coop_client_pos_valid = true;
    tig_net_send_app_all(&pos, sizeof(pos));
    for (i = 0; i < n; i++) {
        memset(&pkt, 0, sizeof(pkt));
        pkt.type = PACKET_COOP_DOOR_USE;
        memcpy(pkt.oid, oids[i], 24);
        tig_net_send_app_all(&pkt, sizeof(pkt));
    }
    tig_debug_printf("CoopLite: door use sent to host (%d)\n", n);
}

/* PT53e: the door changes the host makes are what every client sees. */
static uint8_t coop_door_pend[COOP_DOOR_PENDING_MAX][24];
static int coop_door_pend_cnt;
static int coop_door_actor_slot = -1; /* set while a client's door intent runs */
static int coop_door_pend_slot[COOP_DOOR_PENDING_MAX];
static unsigned int coop_door_flush_ms;
/* last logged state per door (log once per event, not per frame) */
static uint8_t coop_door_log_oid[COOP_DOOR_PENDING_MAX][24];
static unsigned int coop_door_log_state[COOP_DOOR_PENDING_MAX];
static int coop_door_log_next;

static int coop_door_pend_kind[COOP_DOOR_PENDING_MAX]; /* PT60b */

/* PT61: walls a script changed this session, per map (never all walls).
   gone is recorded at destroy time: a wall not found later may only sit in
   an unloaded sector, so a failed lookup never means "destroyed". */
static struct {
    uint8_t oid[24];
    int map;
    bool gone;
    int64_t loc;
} coop_wall_changed[COOP_WALL_CHANGED_MAX];
static int coop_wall_changed_cnt;

static int coop_lite_wall_find(const uint8_t* oid, int map)
{
    int i;

    for (i = 0; i < coop_wall_changed_cnt; i++) {
        if (coop_wall_changed[i].map == map && coop_lite_oid_raw_equal(coop_wall_changed[i].oid, oid)) {
            return i;
        }
    }
    return -1;
}

/* Host: one changed wall's state -> slot (to_all: every client). Only
   reads the host's wall, never changes it. */
static void coop_lite_wall_send(const uint8_t* oid, int slot, bool to_all)
{
    PacketCoopDoorState pkt;
    int idx = coop_lite_wall_find(oid, map_current_map());
    int64_t wall;

    if (idx < 0) {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_DOOR_STATE;
    pkt.map = map_current_map();
    pkt.slot = -1;
    memcpy(pkt.e.oid, oid, 24);
    pkt.e.kind = COOP_WORLD_WALL;
    pkt.e.hp_damage = -1;
    pkt.e.loc = coop_wall_changed[idx].loc;
    if (coop_wall_changed[idx].gone) {
        pkt.e.reserved[1] = COOP_WORLD_OBJ_GONE;
    } else {
        wall = coop_lite_get_oid_obj(oid);
        if (wall == OBJ_HANDLE_NULL || obj_field_int32_get(wall, OBJ_F_TYPE) != OBJ_TYPE_WALL
            || (obj_field_int32_get(wall, OBJ_F_FLAGS) & OF_DESTROYED) != 0) {
            return;
        }
        pkt.e.aid = (unsigned int)obj_field_int32_get(wall, OBJ_F_CURRENT_AID);
        pkt.e.of_flags = (unsigned int)obj_field_int32_get(wall, OBJ_F_FLAGS) & COOP_WORLD_WALL_OF_MASK;
    }
    if (to_all) {
        tig_net_send_app_all(&pkt, sizeof(pkt));
    } else {
        tig_net_send_app(&pkt, sizeof(pkt), slot);
    }
}

void coop_lite_walls_reset(void)
{
    coop_wall_changed_cnt = 0;
    coop_door_pend_cnt = 0;
    memset(coop_fx_cache, 0, sizeof(coop_fx_cache));
}

void coop_lite_host_wall_touch(int64_t obj, bool destroyed)
{
    int depth;
    bool host;
    ObjectID oid;
    int idx;
    int map;
    int before;

    if (obj == OBJ_HANDLE_NULL) {
        return;
    }
    depth = tig_net_local_suspend_pause();
    host = tig_net_is_active() && tig_net_is_host() && tig_net_is_coop_enabled();
    tig_net_local_suspend_resume(depth);
    if (!host || !script_is_running() || !map_is_valid() || map_is_clearing_objects()
        || obj_field_int32_get(obj, OBJ_F_TYPE) != OBJ_TYPE_WALL) {
        return;
    }
    oid = obj_get_id(obj);
    if (oid.type == OID_TYPE_NULL) {
        return;
    }
    map = map_current_map();
    idx = coop_lite_wall_find((const uint8_t*)&oid, map);
    if (idx < 0) {
        if (coop_wall_changed_cnt >= COOP_WALL_CHANGED_MAX) {
            static bool logged;

            if (!logged) {
                logged = true;
                tig_debug_printf("CoopLite: PT61 changed-wall list full (%d) - later walls sync live only\n",
                    COOP_WALL_CHANGED_MAX);
            }
        } else {
            idx = coop_wall_changed_cnt++;
            memcpy(coop_wall_changed[idx].oid, &oid, 24);
            coop_wall_changed[idx].map = map;
            coop_wall_changed[idx].gone = false;
        }
    }
    if (idx >= 0) {
        coop_wall_changed[idx].loc = obj_field_int64_get(obj, OBJ_F_LOCATION);
        if (destroyed) {
            coop_wall_changed[idx].gone = true;
        }
    }
    if (idx < 0) {
        return;
    }
    before = coop_door_pend_cnt;
    if (coop_lite_door_pending_add(coop_door_pend, &coop_door_pend_cnt, COOP_DOOR_PENDING_MAX,
            (const uint8_t*)&oid)
        && coop_door_pend_cnt > before) {
        coop_door_pend_slot[coop_door_pend_cnt - 1] = -1;
        coop_door_pend_kind[coop_door_pend_cnt - 1] = COOP_WORLD_WALL;
    }
    tig_debug_printf("CoopLite: PT61 wall changed by a script (%s) - queued for clients\n",
        destroyed ? "destroyed" : "flags/art");
}

/* Host: on a client's world request, the changed walls of this map only. */
static void coop_lite_host_walls_to_slot(int slot)
{
    int map = map_current_map();
    int depth;
    int i;
    int n = 0;

    if (coop_wall_changed_cnt == 0) {
        return;
    }
    depth = tig_net_local_suspend_pause();
    for (i = 0; i < coop_wall_changed_cnt; i++) {
        if (coop_wall_changed[i].map == map) {
            coop_lite_wall_send(coop_wall_changed[i].oid, slot, false);
            n++;
        }
    }
    tig_net_local_suspend_resume(depth);
    if (n > 0) {
        tig_debug_printf("CoopLite: PT61 %d changed wall(s) -> slot %d\n", n, slot);
    }
}

void coop_lite_host_portal_touch(int64_t portal)
{
    coop_lite_host_object_touch(portal);
}

void coop_lite_host_object_touch(int64_t portal)
{
    int type;
    int kind;
    int depth;
    bool host;
    ObjectID oid;
    int before;

    if (portal == OBJ_HANDLE_NULL) {
        return;
    }
    depth = tig_net_local_suspend_pause();
    host = tig_net_is_active() && tig_net_is_host() && tig_net_is_coop_enabled();
    tig_net_local_suspend_resume(depth);
    if (!host) {
        return;
    }
    type = obj_field_int32_get(portal, OBJ_F_TYPE);
    kind = type == OBJ_TYPE_PORTAL ? COOP_WORLD_PORTAL
        : type == OBJ_TYPE_SCENERY ? COOP_WORLD_SCENERY
        : type == OBJ_TYPE_CONTAINER ? COOP_WORLD_CONTAINER
                                     : 0;
    if (kind == 0 || (obj_field_int32_get(portal, OBJ_F_FLAGS) & OF_INVENTORY) != 0) {
        return;
    }
    oid = obj_get_id(portal);
    if (oid.type == OID_TYPE_NULL) {
        return; /* PT60b: no stable id (temporary object): nothing to sync */
    }
    before = coop_door_pend_cnt;
    if (coop_lite_door_pending_add(coop_door_pend, &coop_door_pend_cnt, COOP_DOOR_PENDING_MAX,
            (const uint8_t*)&oid)
        && coop_door_pend_cnt > before) {
        coop_door_pend_slot[coop_door_pend_cnt - 1] = coop_door_actor_slot;
        coop_door_pend_kind[coop_door_pend_cnt - 1] = kind;
    }
}

/* PT63: object_destroy on the host. PT60 only queued the oid and let the
   100 ms flush send GONE; the Bates tunnel log shows no send at all for a
   bashed "Heavy Wood Door" (the flush's host check ran under a local
   suspend and dropped the whole queue). Send at once, with the location,
   so a client can match the door even if its id differs. */
void coop_lite_host_object_destroyed(int64_t obj)
{
    PacketCoopDoorState pkt;
    ObjectID oid;
    int type;
    int kind;
    int depth;
    char oid_str[80];

    if (obj == OBJ_HANDLE_NULL || !coop_lite_world_hooks_on) {
        return;
    }
    type = obj_field_int32_get(obj, OBJ_F_TYPE);
    kind = type == OBJ_TYPE_PORTAL ? COOP_WORLD_PORTAL
        : type == OBJ_TYPE_SCENERY ? COOP_WORLD_SCENERY
        : type == OBJ_TYPE_CONTAINER ? COOP_WORLD_CONTAINER
                                     : 0;
    if (kind == 0 || (obj_field_int32_get(obj, OBJ_F_FLAGS) & (OF_INVENTORY | OF_DESTROYED)) != 0
        || !map_is_valid() || map_is_clearing_objects()) {
        return;
    }
    oid = obj_get_id(obj);
    if (oid.type == OID_TYPE_NULL) {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_DOOR_STATE;
    pkt.map = map_current_map();
    pkt.slot = coop_door_actor_slot;
    memcpy(pkt.e.oid, &oid, 24);
    pkt.e.kind = kind;
    pkt.e.aid = (unsigned int)obj_field_int32_get(obj, OBJ_F_CURRENT_AID);
    pkt.e.loc = obj_field_int64_get(obj, OBJ_F_LOCATION);
    pkt.e.reserved[1] = COOP_WORLD_OBJ_GONE;
    depth = tig_net_local_suspend_pause();
    tig_net_send_app_all(&pkt, sizeof(pkt));
    tig_net_local_suspend_resume(depth);
    objid_id_to_str(oid_str, oid);
    tig_debug_printf("CoopLite: object: %s (%s) destroyed on the host at %d,%d - sent to clients now (slot %d)\n",
        oid_str, kind == COOP_WORLD_PORTAL ? "door" : kind == COOP_WORLD_SCENERY ? "scenery" : "container",
        (int)location_get_x(pkt.e.loc), (int)location_get_y(pkt.e.loc), pkt.slot);
}

static void coop_lite_door_log(const uint8_t* oid, int64_t portal, int slot, const char* side)
{
    unsigned int flags = (unsigned int)obj_field_int32_get(portal, OBJ_F_PORTAL_FLAGS);
    unsigned int state = (portal_is_open(portal) ? 1u : 0u)
        | ((flags & OPF_LOCKED) != 0 ? 2u : 0u)
        | ((flags & OPF_JAMMED) != 0 ? 4u : 0u)
        | ((flags & OPF_BUSTED) != 0 ? 8u : 0u);
    char oid_str[80];
    ObjectID id;
    int i;

    for (i = 0; i < COOP_DOOR_PENDING_MAX; i++) {
        if (coop_lite_oid_raw_equal(coop_door_log_oid[i], oid)) {
            if (coop_door_log_state[i] == state) {
                return;
            }
            break;
        }
    }
    if (i == COOP_DOOR_PENDING_MAX) {
        i = coop_door_log_next;
        coop_door_log_next = (coop_door_log_next + 1) % COOP_DOOR_PENDING_MAX;
        memcpy(coop_door_log_oid[i], oid, 24);
    }
    coop_door_log_state[i] = state;
    memcpy(&id, oid, sizeof(id));
    objid_id_to_str(oid_str, id);
    tig_debug_printf("CoopLite: door: %s open=%d locked=%d jammed=%d busted=%d from slot %d (%s)\n", oid_str,
        (state & 1u) != 0, (state & 2u) != 0, (state & 4u) != 0, (state & 8u) != 0, slot, side);
}

/* Host tick: every 100 ms, send each changed door's current state to all. */
static void coop_lite_host_door_flush_tick(unsigned int now_ms)
{
    PacketCoopDoorState pkt;
    int depth;
    int i;

    if (coop_door_pend_cnt == 0 || !coop_lite_interval_elapsed(coop_door_flush_ms, now_ms, 100u)) {
        return;
    }
    coop_door_flush_ms = now_ms;
    depth = tig_net_local_suspend_pause(); /* PT63: was after the host check, so a suspended tick dropped the queue */
    if (!tig_net_is_active() || !tig_net_is_host() || !map_is_valid() || map_is_clearing_objects()) {
        tig_net_local_suspend_resume(depth);
        tig_debug_printf("CoopLite: PT63 %d pending world change(s) dropped (no map / not host)\n", coop_door_pend_cnt);
        coop_door_pend_cnt = 0;
        return;
    }
    for (i = 0; i < coop_door_pend_cnt; i++) {
        int64_t portal;
        unsigned int flags;

        if (coop_door_pend_kind[i] == COOP_WORLD_WALL) {
            coop_lite_wall_send(coop_door_pend[i], -1, true);
            continue;
        }
        portal = coop_lite_get_oid_obj(coop_door_pend[i]);

        flags = portal != OBJ_HANDLE_NULL ? (unsigned int)obj_field_int32_get(portal, OBJ_F_FLAGS) : OF_DESTROYED;
        if (portal == OBJ_HANDLE_NULL || (flags & OF_DESTROYED) != 0) {
            /* PT60: bashed / blown apart (portal_bust -> object_destroy).
               PT53e skipped it, so the door stayed whole on every client. */
            char oid_str[80];
            ObjectID id;

            memset(&pkt, 0, sizeof(pkt));
            pkt.type = PACKET_COOP_DOOR_STATE;
            pkt.map = map_current_map();
            pkt.slot = coop_door_pend_slot[i];
            memcpy(pkt.e.oid, coop_door_pend[i], 24);
            pkt.e.kind = coop_door_pend_kind[i] != 0 ? coop_door_pend_kind[i] : COOP_WORLD_PORTAL;
            pkt.e.reserved[1] = COOP_WORLD_OBJ_GONE;
            if (portal != OBJ_HANDLE_NULL) {
                pkt.e.loc = obj_field_int64_get(portal, OBJ_F_LOCATION);
            }
            tig_net_send_app_all(&pkt, sizeof(pkt));
            memcpy(&id, coop_door_pend[i], sizeof(id));
            objid_id_to_str(oid_str, id);
            tig_debug_printf("CoopLite: object: %s (%s) destroyed on the host - sent to clients (slot %d)\n", oid_str,
                pkt.e.kind == COOP_WORLD_PORTAL ? "door" : pkt.e.kind == COOP_WORLD_SCENERY ? "scenery" : "container",
                pkt.slot);
            continue;
        }
        if (obj_field_int32_get(portal, OBJ_F_TYPE) == OBJ_TYPE_SCENERY) {
            /* PT60b: busted / respawning / damaged scenery */
            char oid_str[80];
            ObjectID id;

            memset(&pkt, 0, sizeof(pkt));
            pkt.type = PACKET_COOP_DOOR_STATE;
            pkt.map = map_current_map();
            pkt.slot = coop_door_pend_slot[i];
            memcpy(pkt.e.oid, coop_door_pend[i], 24);
            pkt.e.kind = COOP_WORLD_SCENERY;
            pkt.e.aid = (unsigned int)obj_field_int32_get(portal, OBJ_F_CURRENT_AID);
            pkt.e.of_flags = flags & COOP_WORLD_SCENERY_OF_MASK;
            pkt.e.kind_flags = (unsigned int)obj_field_int32_get(portal, OBJ_F_SCENERY_FLAGS);
            pkt.e.hp_damage = obj_field_int32_get(portal, OBJ_F_HP_DAMAGE);
            pkt.e.loc = obj_field_int64_get(portal, OBJ_F_LOCATION);
            tig_net_send_app_all(&pkt, sizeof(pkt));
            if ((pkt.e.kind_flags & OSCF_BUSTED) != 0 || (flags & OF_OFF) != 0) {
                memcpy(&id, coop_door_pend[i], sizeof(id));
                objid_id_to_str(oid_str, id);
                tig_debug_printf("CoopLite: object: scenery %s busted=%d off=%d hp dmg %d - sent to clients\n", oid_str,
                    (pkt.e.kind_flags & OSCF_BUSTED) != 0, (flags & OF_OFF) != 0, pkt.e.hp_damage);
            }
            continue;
        }
        if (obj_field_int32_get(portal, OBJ_F_TYPE) != OBJ_TYPE_PORTAL) {
            continue; /* a live container: the world snapshot carries it */
        }
        memset(&pkt, 0, sizeof(pkt));
        pkt.type = PACKET_COOP_DOOR_STATE;
        pkt.map = map_current_map();
        pkt.slot = coop_door_pend_slot[i];
        memcpy(pkt.e.oid, coop_door_pend[i], 24);
        pkt.e.kind = COOP_WORLD_PORTAL;
        pkt.e.aid = (unsigned int)obj_field_int32_get(portal, OBJ_F_CURRENT_AID);
        pkt.e.of_flags = flags & COOP_WORLD_PORTAL_OF_MASK;
        pkt.e.kind_flags = (unsigned int)obj_field_int32_get(portal, OBJ_F_PORTAL_FLAGS);
        pkt.e.hp_damage = obj_field_int32_get(portal, OBJ_F_HP_DAMAGE); /* PT60 */
        pkt.e.loc = obj_field_int64_get(portal, OBJ_F_LOCATION);
        pkt.e.reserved[0] = trap_type(portal) != TRAP_TYPE_INVALID ? COOP_WORLD_TRAP_ARMED : 0u;
        tig_net_send_app_all(&pkt, sizeof(pkt));
        coop_lite_door_log(coop_door_pend[i], portal, pkt.slot, "host sent");
    }
    tig_net_local_suspend_resume(depth);
    coop_door_pend_cnt = 0;
}

/* Client: the host's door state, applied at once (any distance). */
static int coop_lite_world_kind_obj_type(int kind)
{
    return kind == COOP_WORLD_SCENERY ? OBJ_TYPE_SCENERY
        : kind == COOP_WORLD_CONTAINER ? OBJ_TYPE_CONTAINER
        : kind == COOP_WORLD_WALL      ? OBJ_TYPE_WALL
                                       : OBJ_TYPE_PORTAL;
}

static int64_t coop_lite_find_world_obj_at(int64_t loc, int type, unsigned int aid)
{
    ObjectList objects;
    ObjectNode* node;
    int64_t objs[16];
    int types[16];
    bool dead[16];
    unsigned int aids[16];
    int n = 0;
    int pick;
    unsigned int mask = type == OBJ_TYPE_SCENERY ? OBJ_TM_SCENERY
        : type == OBJ_TYPE_CONTAINER ? OBJ_TM_CONTAINER
        : type == OBJ_TYPE_WALL ? OBJ_TM_WALL
                                : OBJ_TM_PORTAL;

    object_list_location(loc, mask, &objects);
    for (node = objects.head; node != NULL && n < 16; node = node->next) {
        objs[n] = node->obj;
        types[n] = obj_field_int32_get(node->obj, OBJ_F_TYPE);
        dead[n] = (obj_field_int32_get(node->obj, OBJ_F_FLAGS) & OF_DESTROYED) != 0;
        aids[n] = (unsigned int)obj_field_int32_get(node->obj, OBJ_F_CURRENT_AID);
        n++;
    }
    object_list_destroy(&objects);
    pick = coop_lite_world_pick_candidate(types, dead, aids, n, type, aid);
    return pick >= 0 ? objs[pick] : OBJ_HANDLE_NULL;
}

void coop_lite_handle_door_state(PacketCoopDoorState* pkt)
{
    int64_t portal;
    int want_type;

    if (!tig_net_is_active() || tig_net_is_host()
        || !coop_lite_door_state_valid(pkt, tig_net_last_message_size())) {
        return;
    }
    if (!coop_lite_client_world_ready() || pkt->map != map_current_map() || map_is_clearing_objects()) {
        return; /* the map's full snapshot carries it */
    }
    want_type = coop_lite_world_kind_obj_type(pkt->e.kind);
    portal = coop_lite_get_oid_obj(pkt->e.oid);
    if (portal != OBJ_HANDLE_NULL && obj_field_int32_get(portal, OBJ_F_TYPE) != want_type) {
        portal = OBJ_HANDLE_NULL;
    }
    if (portal == OBJ_HANDLE_NULL && pkt->e.loc != 0) {
        /* PT63: the id did not resolve here (positional P_ ids can differ
           from the host's); match by tile + type (+ art when known). */
        portal = coop_lite_find_world_obj_at(pkt->e.loc, want_type, pkt->e.aid);
        if (portal != OBJ_HANDLE_NULL) {
            tig_debug_printf("CoopLite: PT63 world object matched by location %d,%d (id did not resolve)\n",
                (int)location_get_x(pkt->e.loc), (int)location_get_y(pkt->e.loc));
        }
    }
    if (portal == OBJ_HANDLE_NULL) {
        if ((pkt->e.reserved[1] & COOP_WORLD_OBJ_GONE) != 0) {
            tig_debug_printf("CoopLite: PT63 host destroyed an object at %d,%d - not found here (already gone?)\n",
                (int)location_get_x(pkt->e.loc), (int)location_get_y(pkt->e.loc));
        }
        return;
    }
    if ((pkt->e.reserved[1] & COOP_WORLD_OBJ_GONE) != 0) {
        char oid_str[80];
        ObjectID id;

        if ((obj_field_int32_get(portal, OBJ_F_FLAGS) & OF_DESTROYED) != 0) {
            return;
        }
        memcpy(&id, pkt->e.oid, sizeof(id));
        objid_id_to_str(oid_str, id);
        multiplayer_lock();
        coop_lite_apply_begin();
        gsound_play_sfx_on_obj(sfx_misc_sound(portal, MISC_SOUND_DESTROYING), 1, portal);
        object_destroy(portal);
        coop_lite_apply_end();
        multiplayer_unlock();
        tig_debug_printf("CoopLite: object: %s (%s) destroyed on the host - removed here (slot %d)\n", oid_str,
            pkt->e.kind == COOP_WORLD_PORTAL ? "door" : pkt->e.kind == COOP_WORLD_SCENERY ? "scenery"
                : pkt->e.kind == COOP_WORLD_WALL                                         ? "wall"
                                                                                         : "container",
            pkt->slot);
        return;
    }
    if (pkt->e.kind == COOP_WORLD_SCENERY || pkt->e.kind == COOP_WORLD_WALL) {
        multiplayer_lock();
        coop_lite_apply_begin();
        coop_lite_world_apply(&pkt->e);
        coop_lite_apply_end();
        multiplayer_unlock();
        return;
    }
    multiplayer_lock();
    coop_lite_apply_begin();
    coop_lite_world_apply(&pkt->e);
    coop_lite_apply_end();
    multiplayer_unlock();
    coop_lite_door_log(pkt->e.oid, portal, pkt->slot, "client applied");
}

/* Host: a client used a door. Same steps as a local AG_ATTEMPT_OPEN_DOOR /
   AG_ATTEMPT_CLOSE_DOOR: can open, lock/key check, the door's SAP_USE
   script, then the toggle. PT37 only ran the script, so the host's door
   never moved ("open now 0") and the client's local open was reverted by
   the next world state. */
void coop_lite_handle_door_use(PacketCoopDoorUse* pkt)
{
    int slot;
    int64_t pc;
    int64_t portal;
    char oid_str[80];
    ObjectID oid;

    if (!tig_net_is_active() || !tig_net_is_host()
        || !coop_lite_door_use_valid(pkt, tig_net_last_message_size())) {
        return;
    }
    slot = sub_52A530();
    if (slot <= 0 || slot >= 8 || !map_is_valid() || map_is_clearing_objects()) {
        return;
    }
    if (!coop_lite_host_rate_ok(2, slot, 1)) {
        return; /* PT52k */
    }
    pc = sub_4A2B60(slot);
    portal = coop_lite_get_oid_obj(pkt->oid);
    if (pc == OBJ_HANDLE_NULL || critter_is_dead(pc) || portal == OBJ_HANDLE_NULL
        || (obj_field_int32_get(portal, OBJ_F_TYPE) != OBJ_TYPE_PORTAL
            && obj_field_int32_get(portal, OBJ_F_TYPE) != OBJ_TYPE_SCENERY)
        || (obj_field_int32_get(portal, OBJ_F_FLAGS) & (OF_DESTROYED | OF_OFF)) != 0) {
        return;
    }
    memcpy(&oid, pkt->oid, sizeof(oid));
    objid_id_to_str(oid_str, oid);
    if (object_dist(pc, portal) > 8) {
        tig_debug_printf("CoopLite: door: %s use from slot %d refused: %d tiles away\n", oid_str, slot,
            (int)object_dist(pc, portal));
        return;
    }
    if (obj_field_int32_get(portal, OBJ_F_TYPE) == OBJ_TYPE_SCENERY) {
        /* PT77af: scripted scenery (an entrance that teleports): its use
           script runs here with the client's PC; a teleport of that PC goes
           through the PT64 remote travel path. */
        coop_door_actor_slot = slot;
        (void)object_script_execute(pc, portal, pc, SAP_USE, 0);
        coop_door_actor_slot = -1;
        tig_debug_printf("CoopLite: slot %d used scenery %s\n", slot, oid_str);
        return;
    }
    coop_door_actor_slot = slot;
    if (portal_is_open(portal)) {
        if ((obj_field_int32_get(portal, OBJ_F_PORTAL_FLAGS) & OPF_MAGICALLY_HELD) != 0) {
            tig_debug_printf("CoopLite: door: %s close from slot %d refused: held open\n", oid_str, slot);
        } else {
            portal_toggle(portal);
        }
    } else if (!critter_can_open_portals(pc)
        || ai_attempt_open_portal(pc, portal, object_rot(pc, portal)) != AI_ATTEMPT_OPEN_PORTAL_OK) {
        tig_debug_printf("CoopLite: door: %s open from slot %d refused: locked or jammed\n", oid_str, slot);
        gsound_play_sfx_on_obj(sfx_portal_sound(portal, PORTAL_SOUND_LOCKED), 1, portal);
    } else if (object_script_execute(pc, portal, pc, SAP_USE, 0)) {
        portal_toggle(portal);
    } else {
        tig_debug_printf("CoopLite: door: %s open from slot %d stopped by its script\n", oid_str, slot);
    }
    coop_lite_host_portal_touch(portal); /* state goes back even when refused */
    coop_door_actor_slot = -1;
    tig_debug_printf("CoopLite: slot %d used a door (open now %d)\n", slot, portal_is_open(portal) ? 1 : 0);
}

/* ---- PT40: pick lock / disarm trap, crafting, fate, pre-session shop ---- */

void coop_lite_client_preshop_begin(void)
{
    if (coop_preshop || !tig_net_is_coop_client() || coop_lite_client_in_session) {
        return;
    }
    coop_preshop = true;
    tig_net_local_suspend_begin();
    tig_debug_printf("CoopLite: new-game shop runs locally (kit goes in the character bundle)\n");
}

void coop_lite_client_preshop_end(void)
{
    if (!coop_preshop) {
        return;
    }
    coop_preshop = false;
    tig_net_local_suspend_end();
}

bool coop_lite_client_skill_intent(struct SkillInvocation* si)
{
    PacketCoopSkillUse pkt;
    int64_t pc = player_get_local_pc_obj();
    int depth;

    if (si == NULL || coop_preshop || !tig_net_is_coop_client() || !coop_lite_client_in_session
        || (si->skill != SKILL_PICK_LOCKS && si->skill != SKILL_DISARM_TRAPS)
        || pc == OBJ_HANDLE_NULL || si->source.obj != pc || si->target.obj == OBJ_HANDLE_NULL) {
        return false;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_SKILL_USE;
    pkt.skill = si->skill;
    pkt.modifier = si->modifier < -100 ? -100 : (si->modifier > 100 ? 100 : si->modifier);
    coop_lite_put_oid(pkt.target_oid, si->target.obj);
    if (si->item.obj != OBJ_HANDLE_NULL) {
        coop_lite_put_oid(pkt.item_oid, si->item.obj);
    }
    pkt.target_loc = si->target_loc;
    depth = tig_net_local_suspend_pause();
    tig_net_send_app_all(&pkt, sizeof(pkt));
    tig_net_local_suspend_resume(depth);
    tig_debug_printf("CoopLite: skill %d on target sent to host\n", si->skill);
    return true;
}

/* PT44: potions / food / healing kits used by a client PC. Locally the
   item spell and the destroy were undone by the host's inventory and vitals
   sync, so the client never healed. The host uses the item instead. */
static bool coop_lite_item_host_usable(int64_t item)
{
    int type = obj_field_int32_get(item, OBJ_F_TYPE);

    Script scr;

    /* PT52: scrolls and script-driven items too; written notes/books
       (OBJ_TYPE_WRITTEN) stay local (they only open a UI). */
    if (type == OBJ_TYPE_FOOD || type == OBJ_TYPE_SCROLL) {
        return true;
    }
    /* PT77g: trap devices (Magickal Trap on a door, dynamite...) too: run
       locally they only trapped the client's copy and the host's inventory
       sync gave the item back. */
    if (trap_is_trap_device(item)) {
        return true;
    }
    if (type == OBJ_TYPE_WRITTEN || type == OBJ_TYPE_KEY || type == OBJ_TYPE_KEY_RING) {
        return false;
    }
    obj_arrayfield_script_get(item, OBJ_F_SCRIPTS_IDX, SAP_USE, &scr);
    if (scr.num != 0) {
        return true;
    }
    return type == OBJ_TYPE_GENERIC
        && (obj_field_int32_get(item, OBJ_F_GENERIC_FLAGS) & OGF_IS_HEALING_ITEM) != 0;
}

bool coop_lite_client_use_item_intent(int64_t source_obj, int64_t item_obj, int64_t target_obj)
{
    PacketCoopSkillUse pkt;
    int64_t pc = player_get_local_pc_obj();
    int64_t parent = OBJ_HANDLE_NULL;
    int depth;

    if (coop_preshop || !tig_net_is_coop_client() || !coop_lite_client_in_session
        || pc == OBJ_HANDLE_NULL || source_obj != pc || item_obj == OBJ_HANDLE_NULL
        || !item_parent(item_obj, &parent) || parent != pc || !coop_lite_item_host_usable(item_obj)
        || (target_obj == OBJ_HANDLE_NULL
            && (obj_field_int32_get(item_obj, OBJ_F_ITEM_FLAGS) & OIF_NEEDS_TARGET) != 0)) {
        return false;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_SKILL_USE;
    pkt.skill = COOP_SKILL_USE_ITEM;
    coop_lite_put_oid(pkt.item_oid, item_obj);
    if (target_obj != OBJ_HANDLE_NULL) {
        coop_lite_put_oid(pkt.target_oid, target_obj);
    }
    depth = tig_net_local_suspend_pause();
    tig_net_send_app_all(&pkt, sizeof(pkt));
    tig_net_local_suspend_resume(depth);
    tig_debug_printf("CoopLite: item use sent to host\n");
    return true;
}

/* PT77h: Alt-click on a corpse drags it one step to the PC (vanilla
   sub_4445A0). On a client that was a local move the host's NPC pose sync
   undid: the host does the move and its sync carries it to everyone. */
bool coop_lite_client_body_drag_intent(int64_t pc_obj, int64_t target_obj)
{
    PacketCoopBodyDrag pkt;
    int depth;

    if (!tig_net_is_coop_client() || !coop_lite_client_in_session
        || pc_obj == OBJ_HANDLE_NULL || pc_obj != player_get_local_pc_obj() || target_obj == OBJ_HANDLE_NULL) {
        return false;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_BODY_DRAG;
    coop_lite_put_oid(pkt.oid, target_obj);
    depth = tig_net_local_suspend_pause();
    tig_net_send_app_all(&pkt, sizeof(pkt));
    tig_net_local_suspend_resume(depth);
    tig_debug_printf("CoopLite: body drag sent to host\n");
    return true;
}

void coop_lite_handle_body_drag(PacketCoopBodyDrag* pkt)
{
    int slot;
    int64_t pc;
    int64_t target;
    int type;

    if (!tig_net_is_active() || !tig_net_is_host()
        || !coop_lite_body_drag_valid(pkt, tig_net_last_message_size())
        || !map_is_valid() || map_is_clearing_objects()) {
        return;
    }
    slot = sub_52A530();
    if (slot <= 0 || slot >= 8) {
        return;
    }
    pc = sub_4A2B60(slot);
    target = coop_lite_get_oid_obj(pkt->oid);
    if (pc == OBJ_HANDLE_NULL || critter_is_dead(pc) || target == OBJ_HANDLE_NULL || target == pc) {
        return;
    }
    type = obj_field_int32_get(target, OBJ_F_TYPE);
    /* Only corpses (never a living critter or another player's PC). */
    if (!obj_type_is_critter(type) || !critter_is_dead(target) || type == OBJ_TYPE_PC) {
        tig_debug_printf("CoopLite: slot %d body drag refused (not a corpse)\n", slot);
        return;
    }
    sub_4445A0(pc, target); /* range check (adjacent) is vanilla's */
    tig_debug_printf("CoopLite: slot %d dragged a corpse (host)\n", slot);
}

/* PT77z: client throws an item — host performs it authoritatively. */
bool coop_lite_client_throw_intent(int64_t item_obj, int64_t target_loc)
{
    PacketCoopThrowIntent pkt;
    int64_t pc = player_get_local_pc_obj();
    int64_t parent = OBJ_HANDLE_NULL;
    int depth;
    static unsigned int last_ms;

    if (!tig_net_is_coop_client() || !coop_lite_client_in_session
        || pc == OBJ_HANDLE_NULL || item_obj == OBJ_HANDLE_NULL || target_loc == 0
        || !item_parent(item_obj, &parent) || parent != pc) {
        return false;
    }
    if (last_ms != 0 && !coop_lite_interval_elapsed(last_ms, coop_obj_now_ms, 400u)) {
        return false;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_THROW_INTENT;
    coop_lite_put_oid(pkt.item_oid, item_obj);
    pkt.target_loc = target_loc;
    depth = tig_net_local_suspend_pause();
    tig_net_send_app_all(&pkt, sizeof(pkt));
    tig_net_local_suspend_resume(depth);
    last_ms = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1;
    tig_debug_printf("CoopLite: throw intent sent to host\n");
    return true;
}

void coop_lite_handle_throw_intent(PacketCoopThrowIntent* pkt)
{
    int slot;
    int64_t pc;
    int64_t item;
    int64_t parent = OBJ_HANDLE_NULL;
    int dist;
    int range;

    if (!tig_net_is_active() || !tig_net_is_host()
        || !coop_lite_throw_intent_valid(pkt, tig_net_last_message_size())
        || !map_is_valid() || map_is_clearing_objects()) {
        return;
    }
    slot = sub_52A530();
    if (slot <= 0 || slot >= 8) {
        return;
    }
    pc = sub_4A2B60(slot);
    item = coop_lite_get_oid_obj(pkt->item_oid);
    if (pc == OBJ_HANDLE_NULL || critter_is_dead(pc) || item == OBJ_HANDLE_NULL
        || !item_parent(item, &parent) || parent != pc) {
        tig_debug_printf("CoopLite: slot %d throw refused (ownership)\n", slot);
        return;
    }
    dist = (int)location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION), pkt->target_loc);
    range = item_throwing_distance(item, pc);
    if (!coop_lite_throw_in_range(dist, range)) {
        tig_debug_printf("CoopLite: slot %d throw refused: %d tiles (range %d)\n", slot, dist, range);
        return;
    }
    if (!anim_goal_throw_item(pc, item, pkt->target_loc)) {
        tig_debug_printf("CoopLite: slot %d throw: host anim_goal_throw_item failed\n", slot);
        return;
    }
    tig_debug_printf("CoopLite: slot %d throw started on host\n", slot);
}

void coop_lite_host_projectile_fx(int64_t src_loc, int64_t dst_loc, unsigned int missile_art_id, int speed)
{
    PacketCoopProjectileFx pkt;

    if (!tig_net_is_active() || !tig_net_is_host() || !tig_net_is_coop_enabled()
        || src_loc == 0 || dst_loc == 0 || missile_art_id == 0) {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_PROJECTILE_FX;
    pkt.speed = speed;
    pkt.missile_art_id = missile_art_id;
    pkt.src_loc = src_loc;
    pkt.dst_loc = dst_loc;
    tig_net_send_app_all(&pkt, sizeof(pkt));
}

void coop_lite_handle_projectile_fx(PacketCoopProjectileFx* pkt)
{
    int64_t proto;
    int64_t missile;

    if (!tig_net_is_active() || tig_net_is_host()
        || !coop_lite_projectile_fx_valid(pkt, tig_net_last_message_size())
        || !map_is_valid() || map_is_clearing_objects()) {
        return;
    }
    proto = sub_4685A0(BP_PROJECTILE);
    if (proto == OBJ_HANDLE_NULL || !object_create(proto, pkt->src_loc, &missile)) {
        return;
    }
    obj_field_int32_set(missile, OBJ_F_PROJECTILE_FLAGS_COMBAT, (int)COOP_PROJECTILE_VISUAL);
    if (!anim_goal_projectile_visual(missile, pkt->src_loc, pkt->dst_loc, (tig_art_id_t)pkt->missile_art_id, pkt->speed)) {
        object_destroy(missile);
    }
}

/* PT77g: client trap device on a tile (item_use_on_loc). */
bool coop_lite_client_use_item_loc_intent(int64_t source_obj, int64_t item_obj, int64_t target_loc)
{
    PacketCoopSkillUse pkt;
    int64_t pc = player_get_local_pc_obj();
    int64_t parent = OBJ_HANDLE_NULL;
    int depth;

    if (coop_preshop || !tig_net_is_coop_client() || !coop_lite_client_in_session
        || pc == OBJ_HANDLE_NULL || source_obj != pc || item_obj == OBJ_HANDLE_NULL || target_loc == 0
        || !item_parent(item_obj, &parent) || parent != pc || !trap_is_trap_device(item_obj)) {
        return false;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_SKILL_USE;
    pkt.skill = COOP_SKILL_USE_ITEM;
    coop_lite_put_oid(pkt.item_oid, item_obj);
    pkt.target_loc = target_loc;
    depth = tig_net_local_suspend_pause();
    tig_net_send_app_all(&pkt, sizeof(pkt));
    tig_net_local_suspend_resume(depth);
    tig_debug_printf("CoopLite: trap placement sent to host\n");
    return true;
}

static void coop_lite_host_use_item(PacketCoopSkillUse* pkt)
{
    int slot = sub_52A530();
    int64_t pc;
    int64_t item;
    int64_t target = OBJ_HANDLE_NULL;
    int64_t parent = OBJ_HANDLE_NULL;

    if (slot <= 0 || slot >= 8 || !map_is_valid() || map_is_clearing_objects()) {
        return;
    }
    pc = sub_4A2B60(slot);
    item = coop_lite_get_oid_obj(pkt->item_oid);
    if (pc == OBJ_HANDLE_NULL || critter_is_dead(pc) || item == OBJ_HANDLE_NULL
        || !item_parent(item, &parent) || parent != pc || !coop_lite_item_host_usable(item)) {
        tig_debug_printf("CoopLite: slot %d item use refused\n", slot);
        coop_lite_host_sync_slot_inven(slot);
        return;
    }
    if (pkt->target_oid[0] != 0) {
        target = coop_lite_get_oid_obj(pkt->target_oid);
        if (target == OBJ_HANDLE_NULL
            || !coop_lite_use_item_target_ok(trap_is_trap_device(item), obj_field_int32_get(target, OBJ_F_TYPE))
            || object_dist(pc, target) > COOP_USE_ITEM_TARGET_RANGE) {
            /* PT52: never redirect a targeted item onto the user */
            tig_debug_printf("CoopLite: slot %d item use: bad target (%s)\n", slot,
                target == OBJ_HANDLE_NULL ? "unknown here"
                    : (!coop_lite_use_item_target_ok(trap_is_trap_device(item), obj_field_int32_get(target, OBJ_F_TYPE))
                            ? "wrong target type" : "too far"));
            coop_lite_host_sync_slot_inven(slot);
            return;
        }
    } else if (pkt->target_loc != 0 && trap_is_trap_device(item)) {
        /* PT77g: a trap device placed on a tile. */
        if (location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION), pkt->target_loc) > COOP_USE_ITEM_TARGET_RANGE) {
            tig_debug_printf("CoopLite: slot %d trap placement too far\n", slot);
            coop_lite_host_sync_slot_inven(slot);
            return;
        }
        item_use_on_loc(pc, item, pkt->target_loc);
        tig_debug_printf("CoopLite: slot %d placed a trap device (host)\n", slot);
        coop_lite_host_sync_slot_inven(slot);
        return;
    } else if ((obj_field_int32_get(item, OBJ_F_ITEM_FLAGS) & OIF_NEEDS_TARGET) == 0) {
        target = OBJ_HANDLE_NULL;
    } else {
        target = pc;
    }
    item_use_on_obj(pc, item, target);
    tig_debug_printf("CoopLite: slot %d used an item (host)%s\n", slot,
        target != OBJ_HANDLE_NULL && target != pc ? " on another critter" : "");
    coop_lite_host_sync_slot_inven(slot);
}

void coop_lite_handle_skill_use(PacketCoopSkillUse* pkt)
{
    SkillInvocation si;
    int slot;
    int64_t pc;
    int64_t target;
    int64_t item = OBJ_HANDLE_NULL;
    int64_t parent = OBJ_HANDLE_NULL;
    bool ok;

    if (tig_net_is_active() && tig_net_is_host()
        && coop_lite_use_item_valid(pkt, tig_net_last_message_size())) {
        coop_lite_host_use_item(pkt);
        return;
    }
    if (!tig_net_is_active() || !tig_net_is_host()
        || !coop_lite_skill_use_valid(pkt, tig_net_last_message_size())
        || (pkt->skill != SKILL_PICK_LOCKS && pkt->skill != SKILL_DISARM_TRAPS)) {
        return;
    }
    slot = sub_52A530();
    if (slot <= 0 || slot >= 8 || !map_is_valid() || map_is_clearing_objects()) {
        return;
    }
    if (!coop_lite_host_rate_ok(2, slot, 1)) {
        return; /* PT52k */
    }
    pc = sub_4A2B60(slot);
    target = coop_lite_get_oid_obj(pkt->target_oid);
    if (pkt->item_oid[0] != 0) {
        item = coop_lite_get_oid_obj(pkt->item_oid);
        if (item == OBJ_HANDLE_NULL || !item_parent(item, &parent) || parent != pc) {
            item = OBJ_HANDLE_NULL; /* not ours: roll without the tool */
        }
    }
    if (pc == OBJ_HANDLE_NULL || critter_is_dead(pc) || target == OBJ_HANDLE_NULL
        || (obj_field_int32_get(target, OBJ_F_FLAGS) & (OF_DESTROYED | OF_OFF)) != 0) {
        return;
    }
    if (object_dist(pc, target) > 4) {
        tig_debug_printf("CoopLite: slot %d skill %d refused: %d tiles away\n", slot, pkt->skill,
            (int)object_dist(pc, target));
        return;
    }
    if (pkt->skill == SKILL_DISARM_TRAPS && trap_type(target) == TRAP_TYPE_INVALID) {
        tig_debug_printf("CoopLite: slot %d disarm refused: no armed trap\n", slot);
        coop_lite_host_slot_notice(slot, COOP_PARTY_SKILL_FAILED);
        coop_lite_host_world_dirty();
        return;
    }
    if (pkt->skill == SKILL_DISARM_TRAPS && !trap_is_spotted(pc, target)) {
        /* PT42: the client spotted it locally; the host's copy only knows
           shared spots. Its own Spot Trap roll for that PC decides. */
        SkillInvocation spot;

        skill_invocation_init(&spot);
        sub_4440E0(pc, &(spot.source));
        sub_4440E0(target, &(spot.target));
        spot.skill = SKILL_SPOT_TRAP;
        if (!magictech_check_env_sf(OSF_DETECTING_TRAPS)
            && (!skill_invocation_run(&spot) || (spot.flags & SKILL_INVOCATION_SUCCESS) == 0)) {
            tig_debug_printf("CoopLite: slot %d disarm refused: trap not spotted (host roll)\n", slot);
            coop_lite_host_slot_notice(slot, COOP_PARTY_SKILL_FAILED);
            return;
        }
        object_flags_set(target, OF_TRAP_SPOTTED);
        if (obj_field_int32_get(target, OBJ_F_TYPE) == OBJ_TYPE_TRAP) {
            object_flags_unset(target, OF_DONTDRAW);
        }
    }
    skill_invocation_init(&si);
    sub_4440E0(pc, &(si.source));
    sub_4440E0(target, &(si.target));
    sub_4440E0(item, &(si.item));
    si.skill = pkt->skill;
    si.modifier = pkt->modifier;
    si.target_loc = obj_field_int64_get(target, OBJ_F_LOCATION);
    ok = skill_invocation_run(&si) && (si.flags & SKILL_INVOCATION_SUCCESS) != 0;
    tig_debug_printf("CoopLite: slot %d %s: %s\n", slot,
        pkt->skill == SKILL_PICK_LOCKS ? "pick lock" : "disarm trap", ok ? "success" : "failed");
    coop_lite_host_slot_notice(slot, ok ? COOP_PARTY_STEAL_OK : COOP_PARTY_SKILL_FAILED);
    coop_lite_host_portal_touch(target); /* PT53e: unlocked / jammed door */
    coop_portal_changed = false;
    coop_lite_host_world_dirty();
    coop_lite_host_sync_slot_inven(slot);
}

bool coop_lite_client_craft_intent(int schematic)
{
    PacketCoopCraft pkt;
    int depth;

    if (coop_preshop || !tig_net_is_coop_client() || !coop_lite_client_in_session || schematic < 0) {
        return false;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_CRAFT;
    pkt.schematic = schematic;
    depth = tig_net_local_suspend_pause();
    tig_net_send_app_all(&pkt, sizeof(pkt));
    tig_net_local_suspend_resume(depth);
    tig_debug_printf("CoopLite: schematic %d combine sent to host\n", schematic);
    return true;
}

void coop_lite_handle_craft(PacketCoopCraft* pkt)
{
    int slot;
    int64_t pc;

    if (!tig_net_is_active() || !tig_net_is_host()
        || !coop_lite_craft_valid(pkt, tig_net_last_message_size())) {
        return;
    }
    slot = sub_52A530();
    if (slot <= 0 || slot >= 8) {
        return;
    }
    if (!coop_lite_host_rate_ok(2, slot, 1)) {
        return; /* PT52k */
    }
    pc = sub_4A2B60(slot);
    if (pc == OBJ_HANDLE_NULL || critter_is_dead(pc)) {
        return;
    }
    /* schematic_ui_process finds both ingredients in pc's pack and (PT40)
       checks the expertise; the product gets a host OID. */
    ui_schematic_process(pkt->schematic, pc, pc);
    tig_debug_printf("CoopLite: slot %d combined schematic %d\n", slot, pkt->schematic);
    coop_lite_host_sync_slot_inven(slot);
}

void coop_lite_host_party_fate_point(int64_t earner)
{
    int slot;

    if (!tig_net_is_active() || !tig_net_is_host() || earner == OBJ_HANDLE_NULL
        || coop_lite_slot_of_obj(earner) < 0) {
        return;
    }
    for (slot = 0; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);

        if (!coop_lite_fate_party_target(pc != OBJ_HANDLE_NULL,
                slot > 0 && tig_net_client_is_active(slot), pc == earner, slot)) {
            continue;
        }
        stat_base_set(pc, STAT_FATE_POINTS, stat_base_get(pc, STAT_FATE_POINTS) + 1);
        tig_debug_printf("CoopLite: party fate point to slot %d\n", slot);
    }
}
/* PT45: host — a slot PC changed outside a client request (host PC's gift,
   trainer dialog): push its inventory, and with stats its build (forced). */
void coop_lite_host_pc_changed(int64_t pc, bool stats)
{
    int slot;

    if (!tig_net_is_active() || !tig_net_is_host() || pc == OBJ_HANDLE_NULL) {
        return;
    }
    slot = coop_lite_slot_of_obj(pc);
    if (slot <= 0 || slot >= 8) {
        return;
    }
    if (stats) {
        coop_lite_pc_stats_send(slot, pc, COOP_PC_STATS_F_FORCE, slot);
        tig_debug_printf("CoopLite: slot %d trained (host), build forced to owner\n", slot);
    }
    coop_lite_host_sync_slot_inven(slot);
}
#else
void coop_lite_client_queue_door_use(int64_t portal) { (void)portal; }
void coop_lite_handle_door_use(PacketCoopDoorUse* pkt) { (void)pkt; }
bool coop_lite_area_pc_shared(int64_t pc_obj) { (void)pc_obj; return false; }
void coop_lite_handle_area_known(PacketCoopAreaKnown* pkt) { (void)pkt; }
void coop_lite_handle_npc_stats(PacketCoopNpcStats* pkt) { (void)pkt; }
static void coop_lite_host_npc_stats(int64_t npc, unsigned int now_ms) { (void)npc; (void)now_ms; }
static void coop_lite_area_tick(unsigned int now_ms) { (void)now_ms; }
void coop_lite_area_session_reset(void) { }
void coop_lite_host_portal_touch(int64_t portal) { (void)portal; }
void coop_lite_host_object_touch(int64_t obj) { (void)obj; }
void coop_lite_host_object_destroyed(int64_t obj) { (void)obj; }
void coop_lite_host_wall_touch(int64_t obj, bool destroyed) { (void)obj; (void)destroyed; }
void coop_lite_walls_reset(void) { }
void coop_lite_handle_door_state(PacketCoopDoorState* pkt) { (void)pkt; }
void coop_lite_client_preshop_begin(void) { }
void coop_lite_client_preshop_end(void) { }
bool coop_lite_client_skill_intent(struct SkillInvocation* si) { (void)si; return false; }
void coop_lite_handle_skill_use(PacketCoopSkillUse* pkt) { (void)pkt; }
bool coop_lite_client_use_item_intent(int64_t source_obj, int64_t item_obj, int64_t target_obj) { (void)source_obj; (void)item_obj; (void)target_obj; return false; }
bool coop_lite_client_use_item_loc_intent(int64_t source_obj, int64_t item_obj, int64_t target_loc) { (void)source_obj; (void)item_obj; (void)target_loc; return false; }
bool coop_lite_client_body_drag_intent(int64_t pc_obj, int64_t target_obj) { (void)pc_obj; (void)target_obj; return false; }
void coop_lite_handle_body_drag(PacketCoopBodyDrag* pkt) { (void)pkt; }
bool coop_lite_client_throw_intent(int64_t item_obj, int64_t target_loc) { (void)item_obj; (void)target_loc; return false; }
void coop_lite_handle_throw_intent(PacketCoopThrowIntent* pkt) { (void)pkt; }
void coop_lite_host_projectile_fx(int64_t src_loc, int64_t dst_loc, unsigned int missile_art_id, int speed)
{
    (void)src_loc; (void)dst_loc; (void)missile_art_id; (void)speed;
}
void coop_lite_handle_projectile_fx(PacketCoopProjectileFx* pkt) { (void)pkt; }
bool coop_lite_client_craft_intent(int schematic) { (void)schematic; return false; }
void coop_lite_handle_craft(PacketCoopCraft* pkt) { (void)pkt; }
void coop_lite_host_party_fate_point(int64_t earner) { (void)earner; }
void coop_lite_host_pc_changed(int64_t pc, bool stats) { (void)pc; (void)stats; }
#endif

bool coop_lite_client_local_goal_obj(int64_t obj)
{
    int depth = tig_net_local_suspend_pause();
    bool r = coop_lite_client_drives_obj(obj);

    tig_net_local_suspend_resume(depth);
    return r;
}

void coop_lite_host_item_gained(int64_t pc)
{
#ifndef COOP_LITE_TEST_STUBS
    int slot;

    if (!tig_net_is_active() || !tig_net_is_host() || pc == OBJ_HANDLE_NULL) {
        return;
    }
    slot = coop_lite_slot_of_obj(pc);
    if (slot <= 0) {
        return;
    }
    tig_debug_printf("CoopLite: slot %d PC picked up an item host-side — resyncing it\n", slot);
    coop_lite_host_sync_slot_and(slot, OBJ_HANDLE_NULL);
#else
    (void)pc;
#endif
}

#ifndef COOP_LITE_TEST_STUBS
/*
 * Dev (ARCANUM_DEV_KILL_CLIENT=N on the host, default 15): N s after a
 * client PC is first bound, the host kills it once (corpse carry and
 * Resurrect tests without a fight). The normal death path replicates it.
 */
static unsigned int coop_dev_kill_ms[8];
static bool coop_dev_kill_done[8];

static void coop_lite_host_dev_kill_client_tick(unsigned int now_ms)
{
    const char* env = coop_lite_env("ARCANUM_DEV_KILL_CLIENT");
    int secs;
    int slot;

    if (env == NULL || env[0] == '\0') {
        return;
    }
    secs = atoi(env);
    if (secs <= 0) {
        secs = 15;
    }
    for (slot = 1; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);
        char oid_str[80];

        if (pc == OBJ_HANDLE_NULL || coop_dev_kill_done[slot]) {
            continue;
        }
        if (coop_dev_kill_ms[slot] == 0) {
            coop_dev_kill_ms[slot] = now_ms != 0 ? now_ms : 1;
            continue;
        }
        if (!coop_lite_interval_elapsed(coop_dev_kill_ms[slot], now_ms, (unsigned int)secs * 1000u)) {
            continue;
        }
        coop_dev_kill_done[slot] = true;
        if (critter_is_dead(pc)) {
            continue;
        }
        objid_id_to_str(oid_str, obj_get_id(pc));
        tig_debug_printf("CoopLite: dev: killing slot %d PC %s\n", slot, oid_str);
        critter_kill(pc);
    }
}

/*
 * Review #2 pt 5 dev: ARCANUM_DEV_GOLD_PILE=N. Host: N s after a client PC
 * binds, a 37-gold pile appears on its tile. Client (=1): when it sees a
 * gold pile within 3 tiles, +3 s pick it up like a click, +4 s log its gold
 * and whether a pile is still here, then drop 10 gold through the
 * inventory window's Packet81 (flag 0x02), +4 s log again.
 */
/*
 * Dev (ARCANUM_DEV_CLIENT_LOOT, client, PT18): 25 s in, open a loot /
 * pickpocket window the way the UI does (inven_ui_open), then Take All
 * (loot) or one pickpocket drag, then close; inventory dumps between.
 *   c = nearest container   h = the host's PC (loot if dead, else pickpocket)
 *   n = nearest living NPC (pickpocket)
 */
static int coop_dev_loot_stage;
static unsigned int coop_dev_loot_ms;
static ObjectID coop_dev_loot_target;

static int64_t coop_lite_dev_nearest(int64_t pc, unsigned int tm, bool living)
{
    ObjectList objects;
    ObjectNode* node;
    int64_t best = OBJ_HANDLE_NULL;
    int64_t best_d = 0;

    object_list_vicinity(pc, tm, &objects);
    for (node = objects.head; node != NULL; node = node->next) {
        int64_t d;

        if (node->obj == pc || (obj_field_int32_get(node->obj, OBJ_F_FLAGS) & (OF_DESTROYED | OF_OFF)) != 0) {
            continue;
        }
        if (living && critter_is_dead(node->obj)) {
            continue;
        }
        d = location_dist(obj_field_int64_get(node->obj, OBJ_F_LOCATION), obj_field_int64_get(pc, OBJ_F_LOCATION));
        if (best == OBJ_HANDLE_NULL || d < best_d) {
            best = node->obj;
            best_d = d;
        }
    }
    object_list_destroy(&objects);
    return best;
}

/*
 * PT18 GUI dev: ARCANUM_DEV_LIST_NEAR=N (host or client): N s (default 20)
 * after the session starts, and on a "list" trigger, log every container
 * and critter within 15 tiles of the local PC: name, tile, distance,
 * lock / dead state, item count, and the game-window spot where a click
 * picks it (add the window's screen origin for desktop coordinates).
 */
static void coop_lite_dev_list_near(const char* why)
{
    int64_t pc = player_get_local_pc_obj();
    ObjectList objects;
    ObjectNode* node;
    int64_t pc_loc;

    if (pc == OBJ_HANDLE_NULL) {
        return;
    }
    pc_loc = obj_field_int64_get(pc, OBJ_F_LOCATION);
    tig_debug_printf("CoopLite: dev near (%s): my PC at %d,%d\n", why,
        (int)LOCATION_GET_X(pc_loc), (int)LOCATION_GET_Y(pc_loc));
    object_list_vicinity(pc, OBJ_TM_CONTAINER | OBJ_TM_CRITTER, &objects);
    for (node = objects.head; node != NULL; node = node->next) {
        int64_t loc;
        int64_t d;
        int type;
        int count;
        int x = -1;
        int y = -1;
        const char* spot;
        char name[128];
        char state[48];

        if ((obj_field_int32_get(node->obj, OBJ_F_FLAGS) & (OF_DESTROYED | OF_OFF)) != 0) {
            continue;
        }
        loc = obj_field_int64_get(node->obj, OBJ_F_LOCATION);
        d = location_dist(loc, pc_loc);
        if (d > 15) {
            continue;
        }
        type = obj_field_int32_get(node->obj, OBJ_F_TYPE);
        name[0] = '\0';
        object_examine(node->obj, pc, name);
        if (type == OBJ_TYPE_CONTAINER) {
            count = obj_field_int32_get(node->obj, OBJ_F_CONTAINER_INVENTORY_NUM);
            snprintf(state, sizeof(state), "container%s", object_locked_get(node->obj) ? " LOCKED" : "");
        } else {
            count = obj_field_int32_get(node->obj, OBJ_F_CRITTER_INVENTORY_NUM);
            snprintf(state, sizeof(state), "%s%s%s", type == OBJ_TYPE_PC ? "PC" : "NPC",
                node->obj == pc ? " (me)" : "", critter_is_dead(node->obj) ? " dead" : "");
        }
        if (coop_dev_probe_obj_hook == NULL) {
            spot = "no probe";
        } else if (coop_dev_probe_obj_hook(node->obj, &x, &y)) {
            spot = "click";
        } else {
            spot = x >= 0 ? "covered/off-screen, centre" : "off-screen";
        }
        tig_debug_printf("CoopLite: dev near:   %-10s '%s' @%d,%d dist %d items %d — %s (%d,%d)\n",
            state, name, (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc), (int)d, count, spot, x, y);
    }
    object_list_destroy(&objects);
}

/* Dev trigger "items": every item on the ground within 15 tiles, with its
   object flags, art and screen spot (PT18: a drop the host did not draw). */
static void coop_lite_dev_list_ground_items(void)
{
    int64_t pc = player_get_local_pc_obj();
    ObjectList objects;
    ObjectNode* node;
    int64_t pc_loc;

    if (pc == OBJ_HANDLE_NULL) {
        return;
    }
    pc_loc = obj_field_int64_get(pc, OBJ_F_LOCATION);
    tig_debug_printf("CoopLite: dev items: my PC at %d,%d\n",
        (int)LOCATION_GET_X(pc_loc), (int)LOCATION_GET_Y(pc_loc));
    object_list_vicinity(pc, OBJ_TM_ITEM, &objects);
    for (node = objects.head; node != NULL; node = node->next) {
        int64_t loc = obj_field_int64_get(node->obj, OBJ_F_LOCATION);
        int x = -1;
        int y = -1;
        bool hit;
        char name[128];
        char oid_str[80];

        if (location_dist(loc, pc_loc) > 15) {
            continue;
        }
        name[0] = '\0';
        object_examine(node->obj, pc, name);
        objid_id_to_str(oid_str, obj_get_id(node->obj));
        hit = coop_dev_probe_obj_hook != NULL && coop_dev_probe_obj_hook(node->obj, &x, &y);
        tig_debug_printf("CoopLite: dev items:   '%s' %s @%d,%d off %d,%d flags 0x%08x item flags 0x%08x aid 0x%08x %s (%d,%d)\n",
            name, oid_str, (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc),
            obj_field_int32_get(node->obj, OBJ_F_OFFSET_X), obj_field_int32_get(node->obj, OBJ_F_OFFSET_Y),
            (unsigned int)obj_field_int32_get(node->obj, OBJ_F_FLAGS),
            (unsigned int)obj_field_int32_get(node->obj, OBJ_F_ITEM_FLAGS),
            (unsigned int)obj_field_int32_get(node->obj, OBJ_F_CURRENT_AID),
            hit ? "click" : "no click spot", x, y);
    }
    object_list_destroy(&objects);
}

static const char* coop_lite_dev_ui_click(int64_t obj);
/*
 * PT18 GUI dev trigger: ARCANUM_DEV_TRIGGER=<file> (host or client). Polled
 * twice a second; when the file exists its first word is run and the file
 * is deleted (echo kill-host > file). Commands:
 *   list         log what is near (see ARCANUM_DEV_LIST_NEAR)
 *   kill-host    host only: the host PC dies (no wipe while a client
 *                lives; a client can loot the corpse and Resurrect it)
 *   kill-client  host only: every living client PC dies
 * Also ARCANUM_DEV_LIST_NEAR=N runs "list" once, N s in.
 */
static unsigned int coop_dev_trigger_ms;
static unsigned int coop_dev_list_ms;
static bool coop_dev_list_done;

/* ---- Dev trigger: core-systems test (combat, corpses, map change) ---- */

static void coop_lite_dev_hp_line(const char* tag, int64_t obj)
{
    char name[128];
    int64_t loc;
    int64_t focus = OBJ_HANDLE_NULL;
    char fname[128];

    if (obj == OBJ_HANDLE_NULL) {
        return;
    }
    name[0] = fname[0] = '\0';
    object_examine(obj, obj, name);
    loc = obj_field_int64_get(obj, OBJ_F_LOCATION);
    if (obj_field_int32_get(obj, OBJ_F_TYPE) == OBJ_TYPE_NPC) {
        focus = obj_field_handle_get(obj, OBJ_F_NPC_COMBAT_FOCUS);
        if (focus != OBJ_HANDLE_NULL) {
            object_examine(focus, focus, fname);
        }
    }
    tig_debug_printf("CoopLite: dev hp: %s '%s' @%d,%d STR %d lvl %d xp %d unspent %d hp %d/%d%s%s%s%s\n", tag, name,
        (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc), stat_base_get(obj, STAT_STRENGTH),
        stat_base_get(obj, STAT_LEVEL), stat_base_get(obj, STAT_EXPERIENCE_POINTS),
        stat_base_get(obj, STAT_UNSPENT_POINTS),
        object_hp_current(obj), object_hp_max(obj),
        critter_is_dead(obj) ? " DEAD" : "",
        focus != OBJ_HANDLE_NULL ? " fighting '" : "", fname, focus != OBJ_HANDLE_NULL ? "'" : "");
}

static int64_t coop_lite_dev_nearest_critter(bool hostile_only, bool dead, const char* name_part);

/* Nearest living non-PC critter within 12 tiles; hostile_only: only one
   that is fighting someone (combat focus set). */
static int64_t coop_lite_dev_nearest_npc(bool hostile_only, const char* name_part)
{
    return coop_lite_dev_nearest_critter(hostile_only, false, name_part);
}

static int64_t coop_lite_dev_nearest_critter(bool hostile_only, bool dead, const char* name_part)
{
    int64_t pc = player_get_local_pc_obj();
    ObjectList objects;
    ObjectNode* node;
    int64_t best = OBJ_HANDLE_NULL;
    int64_t best_d = 13;

    if (pc == OBJ_HANDLE_NULL) {
        return OBJ_HANDLE_NULL;
    }
    object_list_vicinity(pc, dead ? (OBJ_TM_NPC | OBJ_TM_PC) : OBJ_TM_NPC, &objects);
    for (node = objects.head; node != NULL; node = node->next) {
        int64_t d;

        if (critter_is_dead(node->obj) != dead) {
            continue;
        }
        if (hostile_only && obj_field_handle_get(node->obj, OBJ_F_NPC_COMBAT_FOCUS) == OBJ_HANDLE_NULL) {
            continue;
        }
        if (name_part != NULL && name_part[0] != '\0') {
            char name[128];

            name[0] = '\0';
            object_examine(node->obj, pc, name);
            if (strstr(name, name_part) == NULL) {
                continue;
            }
        }
        d = location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION), obj_field_int64_get(node->obj, OBJ_F_LOCATION));
        if (d < best_d) {
            best_d = d;
            best = node->obj;
        }
    }
    object_list_destroy(&objects);
    return best;
}

static bool coop_lite_dev_trigger_core(const char* cmd, const char* arg)
{
    int slot;

    if (strcmp(cmd, "hp") == 0) {
        ObjectList objects;
        ObjectNode* node;
        int64_t pc = player_get_local_pc_obj();

        for (slot = 0; slot < 8; slot++) {
            char tag[24];

            snprintf(tag, sizeof(tag), "slot %d PC", slot);
            coop_lite_dev_hp_line(tag, sub_4A2B60(slot));
        }
        if (pc != OBJ_HANDLE_NULL) {
            object_list_vicinity(pc, OBJ_TM_NPC, &objects);
            for (node = objects.head; node != NULL; node = node->next) {
                if (location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION), obj_field_int64_get(node->obj, OBJ_F_LOCATION)) <= 15) {
                    coop_lite_dev_hp_line("NPC", node->obj);
                }
            }
            object_list_destroy(&objects);
        }
        return true;
    }
    if (strcmp(cmd, "rest") == 0) {
        /* PT25: the real sleep button (refused on a client). */
        sleep_ui_toggle(OBJ_HANDLE_NULL);
        tig_debug_printf("CoopLite: dev rest: sleep UI %s\n", sleep_ui_is_active() ? "open" : "not open");
        return true;
    }
    if (strcmp(cmd, "rest-healed") == 0 && tig_net_is_host()) {
        /* PT27 item 5: open the sleep window and press "Until Healed". */
        if (!sleep_ui_is_active()) {
            sleep_ui_toggle(OBJ_HANDLE_NULL);
        }
        tig_debug_printf("CoopLite: dev rest-healed: day %d %02d:%02d, host hp damage %d: %s\n",
            datetime_current_day(), datetime_current_hour(), datetime_current_minute(),
            object_hp_damage_get(player_get_local_pc_obj()),
            sleep_ui_dev_until_healed() ? "sleeping" : "not possible");
        return true;
    }
    if (strcmp(cmd, "rest-sim") == 0 && tig_net_is_host()) {
        /* PT25: what a finished host sleep does, N hours (default 8). */
        int hours = atoi(arg) > 0 ? atoi(arg) : 8;
        int h;
        DateTime dt;

        coop_lite_host_rest_begin();
        for (h = 0; h < hours; h++) {
            sub_45A950(&dt, 3600000);
            timeevent_inc_datetime(&dt);
            critter_resting_heal(player_get_local_pc_obj(), 1);
            coop_lite_host_rest_heal(1);
        }
        coop_lite_host_rest_end();
        tig_debug_printf("CoopLite: dev rest-sim: %d h, now day %d %02d:%02d\n", hours,
            datetime_current_day(), datetime_current_hour(), datetime_current_minute());
        return true;
    }
    if (strcmp(cmd, "time") == 0) {
        tig_debug_printf("CoopLite: dev time: day %d %02d:%02d\n",
            datetime_current_day(), datetime_current_hour(), datetime_current_minute());
        return true;
    }
    if (strcmp(cmd, "dmg") == 0 && tig_net_is_host()) {
        int64_t pc = sub_4A2B60(strstr(arg, "host") != NULL ? 0 : 1);
        int n = atoi(arg) > 0 ? atoi(arg) : 10;

        if (pc != OBJ_HANDLE_NULL) {
            bool quiet = strstr(arg, "quiet") != NULL; /* PT26: under the MP lock = no packet */

            if (quiet) {
                multiplayer_lock();
            }
            object_hp_damage_set(pc, object_hp_damage_get(pc) + n);
            critter_fatigue_damage_set(pc, critter_fatigue_damage_get(pc) + n);
            if (quiet) {
                multiplayer_unlock();
            }
            tig_debug_printf("CoopLite: dev dmg: slot 1 PC -%d HP/fatigue%s\n", n, quiet ? " (quiet)" : "");
        }
        return true;
    }
    if (strcmp(cmd, "invuln") == 0 && tig_net_is_host()) {
        /* PT25 test aid: PCs cannot die (long melee tests). */
        for (slot = 0; slot < 8; slot++) {
            if (sub_4A2B60(slot) != OBJ_HANDLE_NULL) {
                object_flags_set(sub_4A2B60(slot), OF_INVULNERABLE);
            }
        }
        tig_debug_printf("CoopLite: dev invuln: PCs invulnerable\n");
        return true;
    }
    if (strcmp(cmd, "caught") == 0 && tig_net_is_host()) {
        /* PT25: what skill.c does to a caught thief (victim = slot 1 PC). */
        int64_t pc = sub_4A2B60(1);

        if (pc != OBJ_HANDLE_NULL) {
            ai_attack(player_get_local_pc_obj(), pc, LOUDNESS_NORMAL, 0);
            tig_debug_printf("CoopLite: dev caught: host thief vs slot 1 PC - done\n");
        }
        return true;
    }
    if (strcmp(cmd, "attack") == 0 || strcmp(cmd, "attack-npc") == 0) {
        /* The real UI click path (intgame), in combat mode, on the nearest
           hostile (attack) or the nearest NPC whose name contains arg. */
        int64_t pc = player_get_local_pc_obj();
        int64_t target = coop_lite_dev_nearest_npc(strcmp(cmd, "attack") == 0, arg);
        char name[128];

        name[0] = '\0';
        if (pc == OBJ_HANDLE_NULL || target == OBJ_HANDLE_NULL || critter_is_dead(pc)) {
            tig_debug_printf("CoopLite: dev attack: no target\n");
            return true;
        }
        object_examine(target, pc, name);
        if (!combat_critter_is_combat_mode_active(pc)) {
            combat_critter_activate_combat_mode(pc);
        }
        if (tig_net_is_host()) {
            /* Host: its own click path is vanilla; start the attack goal
               directly so an overlapping corpse can't catch the click. */
            tig_debug_printf("CoopLite: dev attack: host attacks '%s': %s\n", name,
                anim_goal_attack(pc, target) ? "goal started" : "goal refused");
            return true;
        }
        if (strncmp(arg, "direct:", 7) == 0 || coop_lite_env("ARCANUM_DEV_ATTACK_DIRECT") != NULL) {
            /* PT23: what intgame's coop branch does for an attack click. */
            tig_debug_printf("CoopLite: dev attack: direct on '%s' (dist %d)\n", name,
                (int)object_dist(pc, target));
            anim_goal_attack(pc, target);
            return true;
        }
        tig_debug_printf("CoopLite: dev attack: click on '%s' (combat mode %d): %s\n", name,
            combat_critter_is_combat_mode_active(pc) ? 1 : 0, coop_lite_dev_ui_click(target));
        return true;
    }
    if (!tig_net_is_host() && (strcmp(cmd, "spend") == 0 || strcmp(cmd, "inflate") == 0)) {
        /* PT19: what charedit does (spend: +1 STR, -1 point), or a cheat
           (inflate: +5 STR, no points) the host must refuse. */
        int64_t pc = player_get_local_pc_obj();
        int str;
        int pts;

        if (pc == OBJ_HANDLE_NULL) {
            return true;
        }
        str = stat_base_get(pc, STAT_STRENGTH);
        pts = stat_base_get(pc, STAT_UNSPENT_POINTS);
        if (strcmp(cmd, "spend") == 0) {
            if (pts <= 0) {
                tig_debug_printf("CoopLite: dev spend: no unspent points\n");
                return true;
            }
            obj_arrayfield_int32_set(pc, OBJ_F_CRITTER_STAT_BASE_IDX, STAT_STRENGTH, str + 1);
            obj_arrayfield_int32_set(pc, OBJ_F_CRITTER_STAT_BASE_IDX, STAT_UNSPENT_POINTS, pts - 1);
        } else {
            obj_arrayfield_int32_set(pc, OBJ_F_CRITTER_STAT_BASE_IDX, STAT_STRENGTH, str + 5);
        }
        tig_debug_printf("CoopLite: dev %s: STR %d -> %d, unspent %d -> %d\n", cmd, str,
            stat_base_get(pc, STAT_STRENGTH), pts, stat_base_get(pc, STAT_UNSPENT_POINTS));
        return true;
    }
    if (strcmp(cmd, "give-xp") == 0 && tig_net_is_host()) {
        /* PT20: vanilla XP award to the host PC (the party split applies). */
        int xp = atoi(arg) > 0 ? atoi(arg) : 100;

        tig_debug_printf("CoopLite: dev: critter_give_xp(host PC, %d)\n", xp);
        critter_give_xp(player_get_local_pc_obj(), xp);
        return true;
    }
    if (strcmp(cmd, "give-points") == 0 && tig_net_is_host()) {
        for (slot = 0; slot < 8; slot++) {
            int64_t pc = sub_4A2B60(slot);

            if (pc != OBJ_HANDLE_NULL) {
                obj_arrayfield_int32_set(pc, OBJ_F_CRITTER_STAT_BASE_IDX, STAT_UNSPENT_POINTS,
                    stat_base_get(pc, STAT_UNSPENT_POINTS) + (atoi(arg) > 0 ? atoi(arg) : 1));
                tig_debug_printf("CoopLite: dev: slot %d unspent points now %d\n", slot,
                    stat_base_get(pc, STAT_UNSPENT_POINTS));
            }
        }
        return true;
    }
    if (strcmp(cmd, "loot") == 0) {
        /* The real UI click on the nearest corpse whose name contains arg. */
        int64_t corpse = coop_lite_dev_nearest_critter(false, true, arg);
        char name[128];

        name[0] = '\0';
        if (corpse != OBJ_HANDLE_NULL) {
            object_examine(corpse, player_get_local_pc_obj(), name);
        }
        if (player_get_local_pc_obj() != OBJ_HANDLE_NULL
            && combat_critter_is_combat_mode_active(player_get_local_pc_obj())) {
            combat_critter_deactivate_combat_mode(player_get_local_pc_obj()); /* else the click attacks */
        }
        if (tig_net_is_host() && corpse != OBJ_HANDLE_NULL) {
            tig_debug_printf("CoopLite: dev loot: host opens loot on '%s'\n", name);
            ui_show_inven_loot(player_get_local_pc_obj(), corpse);
            return true;
        }
        tig_debug_printf("CoopLite: dev loot: click on corpse '%s': %s\n", name,
            coop_lite_dev_ui_click(corpse));
        return true;
    }
    if (strcmp(cmd, "npc-see") == 0) {
        /* PT26: is there a cow near our PC, and is it shown? */
        int64_t pc = player_get_local_pc_obj();
        int n = 0;
        int64_t obj;
        int iter;

        if (pc != OBJ_HANDLE_NULL && obj_inst_first(&obj, &iter)) {
            do {
                char oid_str[40];

                if (obj_field_int32_get(obj, OBJ_F_TYPE) != OBJ_TYPE_NPC || sub_49B290(obj) != BP_COW
                    || (obj_field_int32_get(obj, OBJ_F_FLAGS) & OF_DESTROYED) != 0
                    || location_dist(obj_field_int64_get(obj, OBJ_F_LOCATION), obj_field_int64_get(pc, OBJ_F_LOCATION)) > 20) {
                    continue;
                }
                if (strcmp(arg, "hide") == 0) {
                    /* our copy switched off while the host shows it */
                    tig_net_local_suspend_begin();
                    object_flags_set(obj, OF_OFF);
                    tig_net_local_suspend_end();
                    tig_debug_printf("CoopLite: dev npc-see: hid our copy\n");
                }
                objid_id_to_str(oid_str, obj_get_id(obj));
                tig_debug_printf("CoopLite: dev npc-see: cow %s %d tiles, %s\n", oid_str,
                    (int)location_dist(obj_field_int64_get(obj, OBJ_F_LOCATION), obj_field_int64_get(pc, OBJ_F_LOCATION)),
                    (obj_field_int32_get(obj, OBJ_F_FLAGS) & OF_OFF) != 0 ? "OFF" : "shown");
                n++;
            } while (obj_inst_next(&obj, &iter));
        }
        tig_debug_printf("CoopLite: dev npc-see: %d cows\n", n);
        return true;
    }
    if (strcmp(cmd, "oid") == 0) {
        /* PT27 item 3: where is this object here? */
        ObjectID oid;
        int64_t obj;

        if (!objid_id_from_str(&oid, arg) || (obj = obj_pool_perm_lookup(oid)) == OBJ_HANDLE_NULL) {
            tig_debug_printf("CoopLite: dev oid %s: unknown here\n", arg);
        } else {
            int64_t loc = obj_field_int64_get(obj, OBJ_F_LOCATION);

            tig_debug_printf("CoopLite: dev oid %s: @%d,%d flags 0x%08x sector %s\n", arg,
                (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc), obj_field_int32_get(obj, OBJ_F_FLAGS),
                sector_loaded(sector_id_from_loc(loc)) ? "loaded" : "NOT loaded");
        }
        return true;
    }
    if (strcmp(cmd, "box-click") == 0) {
        /* PT27 item 4: a player's click on the nearest container. */
        int64_t pc = player_get_local_pc_obj();
        int64_t box = pc != OBJ_HANDLE_NULL ? coop_lite_dev_nearest(pc, OBJ_TM_CONTAINER, false) : OBJ_HANDLE_NULL;

        tig_debug_printf("CoopLite: dev box-click: container %d tiles away: %s\n",
            box != OBJ_HANDLE_NULL ? (int)object_dist(pc, box) : -1, coop_lite_dev_ui_click(box));
        return true;
    }
    if (strcmp(cmd, "box-open") == 0 || strcmp(cmd, "box-dump") == 0 || strcmp(cmd, "take-all") == 0
        || strcmp(cmd, "box-put") == 0 || strcmp(cmd, "loot-close") == 0) {
        /* PT26: container stacks. Open/dump the nearest container, the
           window's Take All, put our first ammo stack in, close. */
        int64_t pc = player_get_local_pc_obj();
        int64_t box = pc != OBJ_HANDLE_NULL ? coop_lite_dev_nearest(pc, OBJ_TM_CONTAINER, false) : OBJ_HANDLE_NULL;
        bool ok = false;

        if (strcmp(cmd, "box-open") == 0) {
            ok = box != OBJ_HANDLE_NULL && coop_inven_open_hook != NULL
                && coop_inven_open_hook(pc, box, 2 /* INVEN_UI_MODE_LOOT */);
        } else if (strcmp(cmd, "box-dump") == 0) {
            coop_lite_dev_inven_dump("box", box);
            coop_lite_dev_inven_dump("my PC", pc);
            ok = box != OBJ_HANDLE_NULL;
        } else {
            ok = coop_dev_loot_hook != NULL
                && coop_dev_loot_hook(strcmp(cmd, "take-all") == 0 ? 0 : strcmp(cmd, "box-put") == 0 ? 3 : 1);
        }
        tig_debug_printf("CoopLite: dev %s: %s\n", cmd, ok ? "done" : "not possible");
        return true;
    }
    if (strcmp(cmd, "box") == 0 && tig_net_is_host()) {
        /* PT26: a chest next to slot 1's PC with gold, bullets and a
           dagger; slot 1 also gets 15 bullets of its own. */
        int64_t cpc = sub_4A2B60(1);
        int64_t loc;
        int64_t chest = OBJ_HANDLE_NULL;
        int64_t obj = OBJ_HANDLE_NULL;

        if (cpc == OBJ_HANDLE_NULL) {
            return true;
        }
        loc = obj_field_int64_get(cpc, OBJ_F_LOCATION);
        /* PT27 item 4: "box N" puts it N tiles away (default 1). */
        loc = location_make(LOCATION_GET_X(loc) + (atoi(arg) > 0 ? atoi(arg) : 1), LOCATION_GET_Y(loc));
        if (mp_object_create(BP_CHEST_1, loc, &chest) && chest != OBJ_HANDLE_NULL) {
            obj = item_gold_create(25, loc);
            if (obj != OBJ_HANDLE_NULL) {
                item_transfer(obj, chest);
            }
            obj = item_ammo_create(15, 1 /* bullets */, loc);
            if (obj != OBJ_HANDLE_NULL) {
                item_transfer(obj, chest);
            }
            obj = OBJ_HANDLE_NULL;
            mp_object_create(BP_DAGGER, loc, &obj);
            if (obj != OBJ_HANDLE_NULL) {
                item_transfer(obj, chest);
            }
            obj = item_ammo_create(15, 1, obj_field_int64_get(cpc, OBJ_F_LOCATION));
            if (obj != OBJ_HANDLE_NULL) {
                item_transfer(obj, cpc);
            }
            coop_lite_dev_inven_dump("host box", chest);
        }
        tig_debug_printf("CoopLite: dev box: %s\n", chest != OBJ_HANDLE_NULL ? "created" : "failed");
        return true;
    }
    if (strcmp(cmd, "medic") == 0 && tig_net_is_host()) {
        /* PT27 item 6: the nearest NPC named arg follows the host and gets
           the host's bandages (what a healer companion needs). */
        int64_t hpc = player_get_local_pc_obj();
        int64_t npc = OBJ_HANDLE_NULL;
        int64_t* items;
        int cnt;
        int i;
        int given = 0;
        ObjectList near;
        ObjectNode* nn;

        /* by its own name (the host may not have been introduced yet) */
        object_list_vicinity(hpc, OBJ_TM_NPC, &near);
        for (nn = near.head; nn != NULL; nn = nn->next) {
            char nname[128];

            nname[0] = '\0';
            object_examine(nn->obj, nn->obj, nname);
            if (!critter_is_dead(nn->obj) && strstr(nname, arg) != NULL) {
                npc = nn->obj;
                break;
            }
        }
        object_list_destroy(&near);

        if (npc == OBJ_HANDLE_NULL) {
            tig_debug_printf("CoopLite: dev medic: no NPC '%s'\n", arg);
            return true;
        }
        {
            int64_t talk = sub_4C1110(npc);
            int ts = talk != OBJ_HANDLE_NULL ? coop_lite_slot_of_obj(talk) : -2;

            tig_debug_printf("CoopLite: dev medic: '%s' in dialog with %s (slot %d)\n", arg,
                talk == OBJ_HANDLE_NULL ? "nobody" : "a PC", ts);
        }
        if (sub_4C1110(npc) == hpc) {
            ui_end_dialog(hpc, 0); /* the opening talk of the dev start */
        }
        if (critter_leader_get(npc) != hpc) {
            critter_follow(npc, hpc, true);
        }
        cnt = sub_4A2B60(1) != OBJ_HANDLE_NULL ? item_list_get(sub_4A2B60(1), &items) : item_list_get(hpc, &items);
        for (i = 0; i < cnt; i++) {
            char iname[128];

            iname[0] = '\0';
            object_examine(items[i], hpc, iname);
            if (strstr(iname, "Bandage") != NULL && item_transfer(items[i], npc)) {
                given++;
            }
        }
        item_list_free(items);
        tig_debug_printf("CoopLite: dev medic: '%s' follows the host: %s, bandages given %d\n", arg,
            critter_leader_get(npc) == hpc ? "yes" : "no", given);
        return true;
    }
    if (strcmp(cmd, "kill-npc") == 0 && tig_net_is_host()) {
        int64_t npc = coop_lite_dev_nearest_npc(false, arg);

        if (npc != OBJ_HANDLE_NULL) {
            tig_debug_printf("CoopLite: dev: killing NPC matching '%s'\n", arg);
            critter_kill(npc);
        }
        return true;
    }
    if (!tig_net_is_host()) {
        return false;
    }
    if (strcmp(cmd, "far") == 0) {
        /* PT23: host moves every client PC N tiles east (default 60). */
        int tiles = atoi(arg) != 0 ? atoi(arg) : 60;

        for (slot = 1; slot < 8; slot++) {
            int64_t pc = sub_4A2B60(slot);
            int64_t loc;
            TeleportData td;

            if (pc == OBJ_HANDLE_NULL) {
                continue;
            }
            loc = obj_field_int64_get(pc, OBJ_F_LOCATION);
            memset(&td, 0, sizeof(td));
            td.obj = pc;
            td.map = map_current_map();
            td.loc = coop_lite_follow_loc_for_slot(LOCATION_MAKE(LOCATION_GET_X(loc) + tiles, LOCATION_GET_Y(loc)), 0);
            tig_debug_printf("CoopLite: dev far: slot %d PC %d,%d -> %d,%d\n", slot,
                (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc),
                (int)LOCATION_GET_X(td.loc), (int)LOCATION_GET_Y(td.loc));
            coop_lite_auth_teleport(&td);
        }
        return true;
    }
    if (strcmp(cmd, "host-far") == 0 || strcmp(cmd, "host-back") == 0 || strcmp(cmd, "grab") == 0
        || strcmp(cmd, "drop-grab") == 0 || strcmp(cmd, "put-grab") == 0) {
        /* PT27 item 3: the host fetches an item far from the client (an
           unloaded sector there) and drops it / puts it in a box near the
           client. host-far N: host N tiles east; host-back: back next to
           slot 1; grab: nearest ground item (or a quiet dagger); drop-grab /
           put-grab: onto the ground / into the box nearest slot 1. */
        static ObjectID grab_oid;
        int64_t hpc = player_get_local_pc_obj();
        int64_t cpc = sub_4A2B60(1);
        int64_t item = OBJ_HANDLE_NULL;
        char oid_str[40];
        bool ok = false;

        if (strcmp(cmd, "host-far") == 0 || strcmp(cmd, "host-back") == 0) {
            TeleportData td;
            int64_t loc = obj_field_int64_get(strcmp(cmd, "host-far") == 0 || cpc == OBJ_HANDLE_NULL ? hpc : cpc,
                OBJ_F_LOCATION);
            int tiles = strcmp(cmd, "host-far") == 0 ? (atoi(arg) != 0 ? atoi(arg) : 120) : 2;

            memset(&td, 0, sizeof(td));
            td.obj = hpc;
            td.map = map_current_map();
            td.loc = coop_lite_follow_loc_for_slot(LOCATION_MAKE(LOCATION_GET_X(loc) + tiles, LOCATION_GET_Y(loc)), 0);
            ok = coop_lite_auth_teleport(&td);
        } else if (strcmp(cmd, "grab") == 0) {
            if (strcmp(arg, "mp") == 0 || strcmp(arg, "ammo") == 0) {
                /* announced with Packet70 while the client is far away (loot,
                   a split stack, a script's item) and lying there first */
                if (strcmp(arg, "ammo") == 0) {
                    item = item_ammo_create(12, 1, obj_field_int64_get(hpc, OBJ_F_LOCATION));
                } else {
                    mp_object_create(BP_DAGGER, obj_field_int64_get(hpc, OBJ_F_LOCATION), &item);
                }
                if (item != OBJ_HANDLE_NULL) {
                    grab_oid = obj_get_id(item);
                }
                objid_id_to_str(oid_str, grab_oid);
                tig_debug_printf("CoopLite: dev grab %s: %s on the ground at the host\n", arg, oid_str);
                return true;
            }
            if (strcmp(arg, "take") == 0) {
                item = obj_pool_perm_lookup(grab_oid);
            } else {
                item = strcmp(arg, "new") == 0 ? OBJ_HANDLE_NULL : coop_lite_dev_nearest(hpc, OBJ_TM_ITEM, false);
            }
            if (item == OBJ_HANDLE_NULL || location_dist(obj_field_int64_get(item, OBJ_F_LOCATION),
                                               obj_field_int64_get(hpc, OBJ_F_LOCATION))
                    > 40) {
                item = OBJ_HANDLE_NULL;
                object_create(sub_4685A0(BP_DAGGER), obj_field_int64_get(hpc, OBJ_F_LOCATION), &item);
                tig_debug_printf("CoopLite: dev grab: no ground item near, made a quiet dagger\n");
            }
            if (item != OBJ_HANDLE_NULL) {
                char iname[128];

                iname[0] = '\0';
                object_examine(item, hpc, iname);
                grab_oid = obj_get_id(item);
                tig_debug_printf("CoopLite: dev grab: '%s' %d tiles from the host\n", iname,
                    (int)location_dist(obj_field_int64_get(item, OBJ_F_LOCATION), obj_field_int64_get(hpc, OBJ_F_LOCATION)));
                ok = item_transfer(item, hpc);
            }
        } else {
            item = obj_pool_perm_lookup(grab_oid);
            if (item != OBJ_HANDLE_NULL && strcmp(cmd, "drop-grab") == 0) {
                ok = item_drop(item);
            } else if (item != OBJ_HANDLE_NULL && cpc != OBJ_HANDLE_NULL) {
                int64_t box = coop_lite_dev_nearest(cpc, OBJ_TM_CONTAINER, false);

                ok = box != OBJ_HANDLE_NULL && item_transfer(item, box);
            }
        }
        objid_id_to_str(oid_str, grab_oid);
        tig_debug_printf("CoopLite: dev %s: %s (item %s, oid type %d)\n", cmd, ok ? "done" : "failed",
            oid_str, grab_oid.type);
        return true;
    }
    if (strcmp(cmd, "npc-quiet") == 0 || strcmp(cmd, "npc-off") == 0 || strcmp(cmd, "npc-on") == 0) {
        /* PT26 missing NPCs. npc-quiet: a cow next to slot 1's PC made
           with object_create, so no Packet70 goes out. npc-off: switch it
           off (world state tells the client); npc-on: back on, locally,
           as a script would (only NPC_POS can tell). */
        static ObjectID quiet_oid;
        int64_t cpc = sub_4A2B60(1);
        int64_t npc = OBJ_HANDLE_NULL;

        if (cpc == OBJ_HANDLE_NULL) {
            return true;
        }
        if (strcmp(cmd, "npc-quiet") == 0) {
            int64_t loc = obj_field_int64_get(cpc, OBJ_F_LOCATION);

            loc = coop_lite_follow_loc_for_slot(LOCATION_MAKE(LOCATION_GET_X(loc) + 3, LOCATION_GET_Y(loc)), 0);
            if (object_create(sub_4685A0(BP_COW), loc, &npc) && npc != OBJ_HANDLE_NULL) {
                quiet_oid = obj_get_id(npc);
            }
        } else {
            npc = obj_pool_perm_lookup(quiet_oid);
            if (npc != OBJ_HANDLE_NULL) {
                tig_net_local_suspend_begin();
                if (strcmp(cmd, "npc-off") == 0) {
                    object_flags_set(npc, OF_OFF);
                    coop_lite_host_world_dirty();
                } else {
                    object_flags_unset(npc, OF_OFF);
                }
                tig_net_local_suspend_end();
            }
        }
        {
            char oid_str[40];

            objid_id_to_str(oid_str, quiet_oid);
            tig_debug_printf("CoopLite: dev %s: %s %s\n", cmd, oid_str, npc != OBJ_HANDLE_NULL ? "done" : "failed");
        }
        return true;
    }
    if (strcmp(cmd, "kos") == 0) {
        /* PT26 item 2: party-shared hostility. The NPC nearest slot 1's PC
           (name filter arg): KOS against host / client before and after the
           NPC is made to hate the host PC. */
        int64_t cpc = sub_4A2B60(1);
        int64_t hpc = sub_4A2B60(0);
        int64_t npc = OBJ_HANDLE_NULL;
        int64_t obj;
        int iter;
        int best = 1000;
        char name[128];

        if (cpc == OBJ_HANDLE_NULL || hpc == OBJ_HANDLE_NULL) {
            return true;
        }
        if (obj_inst_first(&obj, &iter)) {
            do {
                int d;

                if (obj_field_int32_get(obj, OBJ_F_TYPE) != OBJ_TYPE_NPC || critter_is_dead(obj)
                    || (obj_field_int32_get(obj, OBJ_F_FLAGS) & (OF_OFF | OF_DESTROYED)) != 0
                    || critter_pc_leader_get(obj) != OBJ_HANDLE_NULL) {
                    continue;
                }
                name[0] = '\0';
                object_examine(obj, hpc, name);
                if (arg[0] != '\0' && strstr(name, arg) == NULL) {
                    continue;
                }
                d = (int)object_dist(obj, cpc);
                if (d < best) {
                    best = d;
                    npc = obj;
                }
            } while (obj_inst_next(&obj, &iter));
        }
        if (npc == OBJ_HANDLE_NULL) {
            tig_debug_printf("CoopLite: dev kos: no NPC\n");
            return true;
        }
        name[0] = '\0';
        object_examine(npc, hpc, name);
        tig_debug_printf("CoopLite: dev kos: '%s' %d tiles: host %d client %d party-client %d (reaction host %d client %d)\n",
            name, best, ai_check_kos(npc, hpc), ai_check_kos(npc, cpc), ai_check_kos_party_dev(npc, cpc),
            reaction_get(npc, hpc), reaction_get(npc, cpc));
        reaction_adj(npc, hpc, -200);
        tig_debug_printf("CoopLite: dev kos: after hating the host: host %d client %d party-client %d (reaction host %d client %d)\n",
            ai_check_kos(npc, hpc), ai_check_kos(npc, cpc), ai_check_kos_party_dev(npc, cpc),
            reaction_get(npc, hpc), reaction_get(npc, cpc));
        return true;
    }
    if (strcmp(cmd, "spawn-idle") == 0) {
        /* PT23: a wolf near each client PC that is NOT told to attack — the
           host AI must notice the (far) client PC on its own. */
        for (slot = 1; slot < 8; slot++) {
            int64_t pc = sub_4A2B60(slot);
            int64_t loc;
            int64_t npc;

            if (pc == OBJ_HANDLE_NULL || critter_is_dead(pc)) {
                continue;
            }
            loc = obj_field_int64_get(pc, OBJ_F_LOCATION);
            loc = coop_lite_follow_loc_for_slot(LOCATION_MAKE(LOCATION_GET_X(loc) + (atoi(arg) > 0 ? atoi(arg) : 5),
                                                    LOCATION_GET_Y(loc) + 1), 0);
            if (mp_object_create(strstr(arg, "rat") != NULL ? BP_SEWER_RAT : BP_WOLF, loc, &npc) && npc != OBJ_HANDLE_NULL) {
                tig_debug_printf("CoopLite: dev: idle wolf near slot %d PC at %d,%d (host PC %d tiles away)\n", slot,
                    (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc),
                    (int)object_dist(player_get_local_pc_obj(), pc));
            }
        }
        return true;
    }
    if (strcmp(cmd, "spawn-rat") == 0 || strcmp(cmd, "spawn-wolf") == 0) {
        /* arg "host" = at the host PC, otherwise at each client PC. */
        bool wolf = strcmp(cmd, "spawn-wolf") == 0;

        for (slot = 0; slot < 8; slot++) {
            int64_t pc = sub_4A2B60(slot);
            int64_t loc;
            int64_t npc;

            if (pc == OBJ_HANDLE_NULL || critter_is_dead(pc)
                || ((strcmp(arg, "host") == 0) != (slot == 0))) {
                continue;
            }
            loc = obj_field_int64_get(pc, OBJ_F_LOCATION);
            {
                static int spawn_n;

                /* a fresh tile each time, so a new creature never stands on an old corpse */
                spawn_n++;
                loc = coop_lite_follow_loc_for_slot(LOCATION_MAKE(LOCATION_GET_X(loc) + 2 + (spawn_n % 3),
                                                        LOCATION_GET_Y(loc) - 1 + (spawn_n % 4)), 0);
            }
            if (!mp_object_create(wolf ? BP_WOLF : BP_SEWER_RAT, loc, &npc) || npc == OBJ_HANDLE_NULL) {
                tig_debug_printf("CoopLite: dev: spawn failed\n");
                continue;
            }
            item_gold_transfer(OBJ_HANDLE_NULL, npc, 25, OBJ_HANDLE_NULL); /* corpse loot test */
            ai_attack(pc, npc, LOUDNESS_NORMAL, 0);
            tig_debug_printf("CoopLite: dev: %s (25 gold) spawned to attack slot %d PC at %d,%d\n",
                wolf ? "wolf" : "sewer rat", slot, (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc));
        }
        return true;
    }
    if (strcmp(cmd, "plant") == 0 || strcmp(cmd, "drop-item") == 0) {
        /* PT25: host->client item visibility. arg "new" = a fresh dagger
           (mp_object_create), else the host PC's first non-stack item. */
        int64_t hpc = player_get_local_pc_obj();
        int64_t item = OBJ_HANDLE_NULL;
        int64_t target = sub_4A2B60(1);
        char iname[128];
        bool ok;

        if (strcmp(arg, "new") == 0) {
            mp_object_create(BP_DAGGER, obj_field_int64_get(hpc, OBJ_F_LOCATION), &item);
            if (item != OBJ_HANDLE_NULL) {
                item_transfer(item, hpc);
            }
        } else {
            int64_t* items;
            int cnt = item_list_get(hpc, &items);
            int i;

            for (i = 0; i < cnt; i++) {
                if (!coop_lite_is_stackable(items[i])) {
                    item = items[i];
                    break;
                }
            }
            item_list_free(items);
        }
        iname[0] = '\0';
        if (item == OBJ_HANDLE_NULL) {
            tig_debug_printf("CoopLite: dev %s: no item\n", cmd);
            return true;
        }
        object_examine(item, hpc, iname);
        if (strcmp(cmd, "plant") == 0) {
            ok = target != OBJ_HANDLE_NULL && item_transfer(item, target);
        } else {
            ok = item_drop(item);
        }
        {
            char oid_str[40];

            objid_id_to_str(oid_str, obj_get_id(item));
            tig_debug_printf("CoopLite: dev %s: '%s' %s: %s\n", cmd, iname, oid_str, ok ? "done" : "failed");
        }
        return true;
    }
    if (strcmp(cmd, "wmap") == 0 || strcmp(cmd, "wmap-close") == 0) {
        /* PT25: the host's "exit to world map" click (wmap_ui_open). */
        if (strcmp(cmd, "wmap") == 0) {
            wmap_ui_open();
        } else {
            wmap_ui_close();
        }
        tig_debug_printf("CoopLite: dev %s: world map %s\n", cmd, wmap_ui_is_created() ? "open" : "closed");
        return true;
    }
    if (strcmp(cmd, "wmap-travel") == 0) {
        /* PT25: the end of a world-map trip (wmap_ui_teleport), N tiles east. */
        int64_t loc = obj_field_int64_get(player_get_local_pc_obj(), OBJ_F_LOCATION);
        int tiles = atoi(arg) != 0 ? atoi(arg) : 200;

        loc = LOCATION_MAKE(LOCATION_GET_X(loc) + tiles, LOCATION_GET_Y(loc));
        {
            /* what wmap_ui_teleport (static) does at the end of a trip */
            TeleportData td;

            sector_flush(0);
            memset(&td, 0, sizeof(td));
            td.flags = TELEPORT_RENDER_LOCK;
            td.obj = player_get_local_pc_obj();
            td.loc = loc;
            td.map = map_by_type(MAP_TYPE_START_MAP);
            tig_debug_printf("CoopLite: dev wmap-travel: host arrives %d tiles east: %s\n", tiles,
                coop_lite_auth_teleport(&td) ? "ok" : "failed");
        }
        if (wmap_ui_is_created()) {
            wmap_ui_close();
        }
        return true;
    }
    if (strcmp(cmd, "maps") == 0) {
        int m;

        for (m = 0; m < 200; m++) {
            char* name = NULL;
            int64_t x;
            int64_t y;

            if (!map_get_name(m, &name) || name == NULL) {
                continue;
            }
            if (!map_get_starting_location(m, &x, &y)) {
                x = y = -1;
            }
            tig_debug_printf("CoopLite: dev map %d '%s' start %d,%d%s\n", m, name, (int)x, (int)y,
                m == map_current_map() ? " (current)" : "");
        }
        return true;
    }
    if (strcmp(cmd, "map") == 0) {
        /* Host map change through teleport_do (what a stair/door jump
           point does), to map arg at its start location. */
        TeleportData td;
        int m = atoi(arg);
        int64_t x;
        int64_t y;

        if (!map_get_starting_location(m, &x, &y)) {
            tig_debug_printf("CoopLite: dev map: no map %d\n", m);
            return true;
        }
        memset(&td, 0, sizeof(td));
        td.obj = player_get_local_pc_obj();
        td.loc = LOCATION_MAKE(x, y);
        td.map = m;
        tig_debug_printf("CoopLite: dev map: host teleports to map %d at %d,%d\n", m, (int)x, (int)y);
        coop_lite_auth_teleport(&td);
        return true;
    }
    return false;
}

static int coop_dev_autostart_rearm;

int coop_lite_take_dev_autostart_rearm(void)
{
    int v = coop_dev_autostart_rearm;

    coop_dev_autostart_rearm = 0;
    return v;
}

static void coop_lite_dev_trigger_tick(unsigned int now_ms)
{
    const char* env;
    FILE* f;
    char cmd[64];
    char arg[64];
    int slot;

    env = coop_lite_env("ARCANUM_DEV_LIST_NEAR");
    if (!coop_dev_list_done && env != NULL && env[0] != '\0') {
        if (coop_dev_list_ms == 0) {
            coop_dev_list_ms = now_ms != 0 ? now_ms : 1;
        } else if (coop_lite_interval_elapsed(coop_dev_list_ms, now_ms,
                       (unsigned int)(atoi(env) > 0 ? atoi(env) : 20) * 1000u)) {
            coop_dev_list_done = true;
            coop_lite_dev_list_near("timer");
        }
    }

    env = coop_lite_env("ARCANUM_DEV_TRIGGER");
    if (env == NULL || env[0] == '\0' || !coop_lite_interval_elapsed(coop_dev_trigger_ms, now_ms, 500u)) {
        return;
    }
    coop_dev_trigger_ms = now_ms;
    f = fopen(env, "r");
    if (f == NULL) {
        return;
    }
    cmd[0] = '\0';
    arg[0] = '\0';
    if (fscanf(f, "%63s %63s", cmd, arg) < 1) {
        cmd[0] = '\0';
    }
    fclose(f);
    remove(env);
    tig_debug_printf("CoopLite: dev trigger: '%s'%s%s\n", cmd, arg[0] != '\0' ? " " : "", arg);
    if (coop_lite_dev_trigger_core(cmd, arg)) {
        /* core-systems test commands (spawn, attack, hp, map) */
    } else if (strcmp(cmd, "menu") == 0) {
        /* PT23: back to the main menu (no restart); the menu's dev
           autostart then starts a New Game with pregen <arg> (0 = none). */
        coop_dev_autostart_rearm = atoi(arg);
        if (coop_return_to_menu_hook != NULL) {
            tig_debug_printf("CoopLite: dev: back to the main menu (then pregen %d)\n", coop_dev_autostart_rearm);
            coop_return_to_menu_hook();
        }
    } else if (strcmp(cmd, "list") == 0) {
        coop_lite_dev_list_near("trigger");
    } else if (strcmp(cmd, "items") == 0) {
        coop_lite_dev_list_ground_items();
    } else if (strcmp(cmd, "inven") == 0) {
        for (slot = 0; slot < 8; slot++) {
            char tag[48];

            snprintf(tag, sizeof(tag), "slot %d PC%s", slot,
                sub_4A2B60(slot) == player_get_local_pc_obj() ? " (mine)" : "");
            coop_lite_dev_inven_dump(tag, sub_4A2B60(slot));
        }
    } else if (strcmp(cmd, "kill-host") == 0 && tig_net_is_host()) {
        int64_t pc = player_get_local_pc_obj();

        if (pc != OBJ_HANDLE_NULL && !critter_is_dead(pc)) {
            tig_debug_printf("CoopLite: dev: killing the host PC\n");
            critter_kill(pc);
        }
    } else if (strcmp(cmd, "kill-client") == 0 && tig_net_is_host()) {
        for (slot = 1; slot < 8; slot++) {
            int64_t pc = sub_4A2B60(slot);

            if (pc != OBJ_HANDLE_NULL && !critter_is_dead(pc)) {
                tig_debug_printf("CoopLite: dev: killing slot %d PC\n", slot);
                critter_kill(pc);
            }
        }
    } else {
        tig_debug_printf("CoopLite: dev trigger: unknown or not for this side\n");
    }
}

void coop_lite_client_dev_loot_tick(unsigned int now_ms)
{
    const char* env = coop_lite_env("ARCANUM_DEV_CLIENT_LOOT");
    int64_t pc = player_get_local_pc_obj();
    int64_t target;
    char name[128];

    if (env == NULL || env[0] == '\0' || pc == OBJ_HANDLE_NULL || coop_dev_loot_stage > 5) {
        return;
    }
    if (coop_dev_loot_ms == 0) {
        coop_dev_loot_ms = now_ms != 0 ? now_ms : 1;
        return;
    }
    if (!coop_lite_interval_elapsed(coop_dev_loot_ms, now_ms, coop_dev_loot_stage == 0 ? 25000u : 4000u)) {
        return;
    }
    coop_dev_loot_ms = now_ms != 0 ? now_ms : 1;
    name[0] = '\0';
    switch (coop_dev_loot_stage++) {
    case 0:
        target = env[0] == 'c' ? coop_lite_dev_nearest(pc, OBJ_TM_CONTAINER, false)
            : env[0] == 'n' ? coop_lite_dev_nearest(pc, OBJ_TM_NPC, true)
            : sub_4A2B60(0);
        if (target == OBJ_HANDLE_NULL) {
            tig_debug_printf("CoopLite: dev loot: no target\n");
            coop_dev_loot_stage = 99;
            return;
        }
        coop_dev_loot_target = obj_get_id(target);
        object_examine(target, pc, name);
        tig_debug_printf("CoopLite: dev loot: target '%s' %d tiles away; walking next to it\n", name,
            (int)location_dist(obj_field_int64_get(target, OBJ_F_LOCATION), obj_field_int64_get(pc, OBJ_F_LOCATION)));
        coop_lite_dev_inven_dump("my PC", pc);
        if (location_dist(obj_field_int64_get(target, OBJ_F_LOCATION), obj_field_int64_get(pc, OBJ_F_LOCATION)) > 1) {
            anim_goal_move_to_tile(pc, obj_field_int64_get(target, OBJ_F_LOCATION));
        }
        return;
    case 1: {
        int mode;

        target = obj_pool_perm_lookup(coop_dev_loot_target);
        if (target == OBJ_HANDLE_NULL || coop_inven_open_hook == NULL) {
            coop_dev_loot_stage = 99;
            return;
        }
        mode = obj_type_is_critter(obj_field_int32_get(target, OBJ_F_TYPE)) && !critter_is_dead(target)
            ? 3 /* INVEN_UI_MODE_STEAL */
            : 2 /* INVEN_UI_MODE_LOOT */;
        tig_debug_printf("CoopLite: dev loot: opening the %s window (%d tiles)\n", mode == 3 ? "pickpocket" : "loot",
            (int)location_dist(obj_field_int64_get(target, OBJ_F_LOCATION), obj_field_int64_get(pc, OBJ_F_LOCATION)));
        coop_inven_open_hook(pc, target, mode);
        return;
    }
    case 2:
        coop_lite_dev_inven_dump("loot target", obj_pool_perm_lookup(coop_dev_loot_target));
        tig_debug_printf("CoopLite: dev loot: %s\n",
            coop_dev_loot_hook == NULL ? "no hook"
                : coop_dev_loot_hook(0) ? "take all"
                : coop_dev_loot_hook(2) ? "pickpocket drag sent"
                : "nothing to take");
        return;
    case 3:
        coop_lite_dev_inven_dump("my PC", pc);
        tig_debug_printf("CoopLite: dev loot: close %s\n", coop_dev_loot_hook != NULL && coop_dev_loot_hook(1) ? "done" : "not possible");
        return;
    default:
        coop_lite_dev_inven_dump("my PC", pc);
        return;
    }
}

static int64_t coop_lite_dev_gold_piles(int64_t pc, int* count)
{
    ObjectList objects;
    ObjectNode* node;
    int64_t first = OBJ_HANDLE_NULL;
    int n = 0;

    object_list_vicinity(pc, OBJ_TM_GOLD, &objects);
    for (node = objects.head; node != NULL; node = node->next) {
        if ((obj_field_int32_get(node->obj, OBJ_F_FLAGS) & (OF_INVENTORY | OF_DESTROYED | OF_OFF)) != 0
            || location_dist(obj_field_int64_get(node->obj, OBJ_F_LOCATION),
                   obj_field_int64_get(pc, OBJ_F_LOCATION)) > 3) {
            continue;
        }
        if (first == OBJ_HANDLE_NULL) {
            first = node->obj;
        }
        n++;
    }
    object_list_destroy(&objects);
    if (count != NULL) {
        *count = n;
    }
    return first;
}

static int64_t coop_lite_dev_find_gold_pile(int64_t pc)
{
    return coop_lite_dev_gold_piles(pc, NULL);
}

static int coop_lite_dev_count_gold_piles(int64_t pc)
{
    int n = 0;

    coop_lite_dev_gold_piles(pc, &n);
    return n;
}

static unsigned int coop_dev_gold_ms[8];
static bool coop_dev_gold_done[8];

static void coop_lite_host_dev_gold_pile_tick(unsigned int now_ms)
{
    const char* env = coop_lite_env("ARCANUM_DEV_GOLD_PILE");
    int slot;

    if (env == NULL || atoi(env) <= 0) {
        return;
    }
    for (slot = 1; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);
        int64_t pile;
        char oid_str[80];

        if (pc == OBJ_HANDLE_NULL || coop_dev_gold_done[slot]) {
            continue;
        }
        if (coop_dev_gold_ms[slot] == 0) {
            coop_dev_gold_ms[slot] = now_ms != 0 ? now_ms : 1;
            continue;
        }
        if (!coop_lite_interval_elapsed(coop_dev_gold_ms[slot], now_ms, (unsigned int)atoi(env) * 1000u)) {
            continue;
        }
        coop_dev_gold_done[slot] = true;
        {
            int64_t loc = obj_field_int64_get(pc, OBJ_F_LOCATION);

            /* "15o": two tiles off the PC, so a click picks the pile and
               not the PC standing on it; "15oi": a dagger too (not a stack). */
            if (strchr(env, 'o') != NULL) {
                loc = location_make(LOCATION_GET_X(loc) + 2, LOCATION_GET_Y(loc));
            }
            pile = item_gold_create(37, loc);
            if (pile != OBJ_HANDLE_NULL) {
                objid_id_to_str(oid_str, obj_get_id(pile));
                tig_debug_printf("CoopLite: dev: gold pile of 37 %s at slot %d PC%s (host gold piles near it: %d)\n",
                    oid_str, slot, strchr(env, 'o') != NULL ? " +2 tiles" : "", coop_lite_dev_count_gold_piles(pc));
            }
            if (strchr(env, 'i') != NULL) {
                int64_t dagger;

                multiplayer_lock();
                if (object_create(sub_4685A0(BP_DAGGER),
                        location_make(LOCATION_GET_X(loc), LOCATION_GET_Y(loc) + 2), &dagger)) {
                    multiplayer_unlock();
                    coop_lite_host_announce_ground_item(dagger);
                    tig_debug_printf("CoopLite: dev: dagger on the ground near slot %d PC\n", slot);
                } else {
                    multiplayer_unlock();
                }
            }
        }
    }
}

static int coop_dev_cgold_stage;
static unsigned int coop_dev_cgold_ms;
static ObjectID coop_dev_cgold_weapon; /* the ground weapon the UI click took */

/* Dev: that weapon, when it is in our PC's inventory. */
static int64_t coop_lite_dev_carried_weapon(int64_t pc)
{
    int64_t obj;
    int64_t parent = OBJ_HANDLE_NULL;

    if (coop_dev_cgold_weapon.type == OID_TYPE_NULL) {
        return OBJ_HANDLE_NULL;
    }
    obj = obj_pool_perm_lookup(coop_dev_cgold_weapon);
    if (obj == OBJ_HANDLE_NULL || !item_parent(obj, &parent) || parent != pc) {
        return OBJ_HANDLE_NULL;
    }
    return obj;
}

/* Dev: nearest ground object of a type mask within `radius` tiles. */
static int64_t coop_lite_dev_ground_near(int64_t pc, unsigned int tm, int radius, int* count)
{
    ObjectList objects;
    ObjectNode* node;
    int64_t best = OBJ_HANDLE_NULL;
    int64_t best_d = 0;
    int n = 0;

    object_list_vicinity(pc, tm, &objects);
    for (node = objects.head; node != NULL; node = node->next) {
        int64_t d;

        if ((obj_field_int32_get(node->obj, OBJ_F_FLAGS) & (OF_INVENTORY | OF_DESTROYED | OF_OFF)) != 0) {
            continue;
        }
        d = location_dist(obj_field_int64_get(node->obj, OBJ_F_LOCATION), obj_field_int64_get(pc, OBJ_F_LOCATION));
        if (d > radius) {
            continue;
        }
        n++;
        if (best == OBJ_HANDLE_NULL || d < best_d) {
            best = node->obj;
            best_d = d;
        }
    }
    object_list_destroy(&objects);
    if (count != NULL) {
        *count = n;
    }
    return best;
}

/* Dev: the Packet81 the inventory drop box sends (inven_ui sub_578B80). */
static void coop_lite_dev_send_drop(int64_t pc, int64_t item, int qty)
{
    Packet81 pkt;

    memset(&pkt, 0, sizeof(pkt));
    pkt.type = 81;
    pkt.field_8 = 0x02;
    pkt.field_10 = obj_get_id(item);
    coop_lite_client_map_stack_oid(&pkt.field_10);
    pkt.field_28 = obj_get_id(pc);
    pkt.field_40.type = OID_TYPE_NULL;
    pkt.field_58 = -1;
    pkt.field_5C = -1;
    pkt.field_60 = qty;
    pkt.field_68 = sub_529520();
    pkt.field_70.type = OID_TYPE_NULL;
    pkt.field_88.type = OID_TYPE_NULL;
    pkt.field_A0 = obj_get_id(pc);
    tig_net_send_app_all(&pkt, sizeof(pkt));
}

static const char* coop_lite_dev_ui_click(int64_t obj)
{
    return obj != OBJ_HANDLE_NULL && coop_dev_click_obj_hook != NULL && coop_dev_click_obj_hook(obj, 0)
        ? "queued"
        : "not possible";
}

/*
 * ARCANUM_DEV_GOLD_PILE on a client:
 *   =1  pickup goal + drop-box gold drop (PT17).
 *   =2  the same through real UI clicks (queued mouse messages), plus a
 *       weapon: click it up, drop it, walk off 6 tiles, click both back.
 */
static void coop_lite_client_dev_gold_pile_tick(unsigned int now_ms)
{
    const char* env = coop_lite_env("ARCANUM_DEV_GOLD_PILE");
    int64_t pc = player_get_local_pc_obj();
    int64_t pile;
    int64_t weapon;
    int depth;
    int n;
    int n2;
    bool ui;

    if (env == NULL || (env[0] != '1' && env[0] != '2') || pc == OBJ_HANDLE_NULL) {
        return;
    }
    ui = env[0] == '2';
    if (coop_dev_cgold_stage >= (ui ? 9 : 4)) {
        return;
    }
    if (coop_dev_cgold_stage == 0) {
        pile = coop_lite_dev_find_gold_pile(pc);
        if (pile == OBJ_HANDLE_NULL) {
            return;
        }
        tig_debug_printf("CoopLite: dev gold: pile of %d seen, my gold %d\n",
            obj_field_int32_get(pile, OBJ_F_GOLD_QUANTITY), item_gold_get(pc));
        coop_dev_cgold_ms = now_ms != 0 ? now_ms : 1;
        coop_dev_cgold_stage = 1;
        return;
    }
    if (!coop_lite_interval_elapsed(coop_dev_cgold_ms, now_ms,
            coop_dev_cgold_stage == 1 ? 3000u : (coop_dev_cgold_stage >= 6 ? 7000u : 4000u))) {
        return;
    }
    coop_dev_cgold_ms = now_ms != 0 ? now_ms : 1;
    depth = tig_net_local_suspend_pause();
    switch (coop_dev_cgold_stage) {
    case 1:
        pile = coop_lite_dev_find_gold_pile(pc);
        if (ui) {
            tig_debug_printf("CoopLite: dev gold: UI click on the pile %s\n", coop_lite_dev_ui_click(pile));
        } else {
            tig_debug_printf("CoopLite: dev gold: pickup goal (as a click) %s\n",
                pile != OBJ_HANDLE_NULL && anim_goal_pickup_item_as_click(pc, pile) ? "started" : "refused");
        }
        break;
    case 2: {
        int64_t gold = obj_field_handle_get(pc, OBJ_F_CRITTER_GOLD);

        tig_debug_printf("CoopLite: dev gold: after pickup my gold %d, piles here %d\n",
            item_gold_get(pc), coop_lite_dev_count_gold_piles(pc));
        if (gold != OBJ_HANDLE_NULL && item_gold_get(pc) >= 10) {
            coop_lite_dev_send_drop(pc, gold, 10);
            tig_debug_printf("CoopLite: dev gold: dropping 10 gold (drop box)\n");
        }
        break;
    }
    case 3:
        tig_debug_printf("CoopLite: dev gold: after drop my gold %d, piles here %d\n",
            item_gold_get(pc), coop_lite_dev_count_gold_piles(pc));
        if (ui) {
            weapon = coop_lite_dev_ground_near(pc, OBJ_TM_WEAPON, 4, NULL);
            if (weapon != OBJ_HANDLE_NULL) {
                coop_dev_cgold_weapon = obj_get_id(weapon);
                tig_debug_printf("CoopLite: dev gold: UI click on the ground weapon %s\n", coop_lite_dev_ui_click(weapon));
            }
        }
        break;
    case 4:
        weapon = coop_lite_dev_carried_weapon(pc);
        tig_debug_printf("CoopLite: dev gold: picked-up weapon %s; dropping it (drop box)\n",
            weapon != OBJ_HANDLE_NULL ? "in my inventory" : "NOT in my inventory");
        if (weapon == OBJ_HANDLE_NULL) {
            /* An NPC got the ground one first: drop one of our own. */
            weapon = item_find_first_of_type(pc, OBJ_TYPE_WEAPON);
            if (weapon != OBJ_HANDLE_NULL) {
                coop_dev_cgold_weapon = obj_get_id(weapon);
                tig_debug_printf("CoopLite: dev gold: dropping my own weapon instead\n");
            }
        }
        if (weapon != OBJ_HANDLE_NULL) {
            coop_lite_dev_send_drop(pc, weapon, 1);
        }
        break;
    case 5: {
        int64_t l = obj_field_int64_get(pc, OBJ_F_LOCATION);

        weapon = coop_lite_dev_ground_near(pc, OBJ_TM_WEAPON, 0, &n);
        if (weapon != OBJ_HANDLE_NULL && coop_dev_click_obj_hook != NULL) {
            tig_debug_printf("CoopLite: dev gold: the dropped weapon under my feet is %s\n",
                coop_dev_click_obj_hook(weapon, 2) ? "clickable" : "NOT clickable (the PC covers it)");
        }
        tig_debug_printf("CoopLite: dev gold: weapons at my feet %d, dagger in inventory %d; walking 6 tiles away (%s)\n",
            n, coop_lite_dev_carried_weapon(pc) != OBJ_HANDLE_NULL,
            anim_goal_move_to_tile(pc, location_make(LOCATION_GET_X(l) - 6, LOCATION_GET_Y(l))) ? "ok" : "refused");
        break;
    }
    case 6:
        weapon = coop_lite_dev_ground_near(pc, OBJ_TM_WEAPON, 12, &n);
        coop_lite_dev_ground_near(pc, OBJ_TM_GOLD, 12, &n2);
        tig_debug_printf("CoopLite: dev gold: after walking: weapons within 12 tiles %d, piles %d\n", n, n2);
        tig_debug_printf("CoopLite: dev gold: UI click on the far weapon %s\n", coop_lite_dev_ui_click(weapon));
        break;
    case 7:
        tig_debug_printf("CoopLite: dev gold: dagger in inventory %d\n", coop_lite_dev_carried_weapon(pc) != OBJ_HANDLE_NULL);
        pile = coop_lite_dev_ground_near(pc, OBJ_TM_GOLD, 12, NULL);
        tig_debug_printf("CoopLite: dev gold: UI click on the far pile %s\n", coop_lite_dev_ui_click(pile));
        break;
    default:
        tig_debug_printf("CoopLite: dev gold: end: my gold %d, dagger in inventory %d, piles here %d\n",
            item_gold_get(pc), coop_lite_dev_carried_weapon(pc) != OBJ_HANDLE_NULL,
            coop_lite_dev_count_gold_piles(pc));
        break;
    }
    tig_net_local_suspend_resume(depth);
    coop_dev_cgold_stage++;
}

/*
 * PT17 dev: ARCANUM_DEV_AUTO_RESURRECT=N (host) — N s after a client PC is
 * seen dead, the host PC casts Resurrect on it the way the spell UI does
 * (spell_ui_apply: invocation from the caster, target the corpse).
 */
static unsigned int coop_dev_res_ms[8];
static bool coop_dev_res_done[8];
static int coop_dev_res_ui_stage[8];

static void coop_lite_host_dev_auto_resurrect_tick(unsigned int now_ms)
{
    const char* env = coop_lite_env("ARCANUM_DEV_AUTO_RESURRECT");
    int64_t caster = player_get_local_pc_obj();
    int secs;
    int slot;

    if (env == NULL || env[0] == '\0' || caster == OBJ_HANDLE_NULL || critter_is_dead(caster)) {
        return;
    }
    secs = atoi(env);
    if (secs <= 0) {
        secs = 5;
    }
    for (slot = 1; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);
        MagicTechInvocation mt_invocation;

        if (pc == OBJ_HANDLE_NULL || coop_dev_res_done[slot] || !critter_is_dead(pc)) {
            coop_dev_res_ms[slot] = 0;
            coop_dev_res_ui_stage[slot] = 0;
            continue;
        }
        if (coop_dev_res_ms[slot] == 0) {
            coop_dev_res_ms[slot] = now_ms != 0 ? now_ms : 1;
            continue;
        }
        if (!coop_lite_interval_elapsed(coop_dev_res_ms[slot], now_ms, (unsigned int)secs * 1000u)) {
            continue;
        }
        if (strchr(env, 'l') != NULL && coop_dev_res_ui_stage[slot] < 3) {
            /* "5lh": first loot the corpse the GUI way (click it, take all,
               close the window), then go on. */
            if (coop_dev_res_ui_stage[slot] == 0) {
                tig_debug_printf("CoopLite: dev: UI click on slot %d's corpse to loot it %s\n", slot,
                    coop_dev_click_obj_hook != NULL && coop_dev_click_obj_hook(pc, 0) ? "queued" : "not possible");
            } else if (coop_dev_res_ui_stage[slot] == 1) {
                tig_debug_printf("CoopLite: dev: loot window: take all %s\n",
                    coop_dev_loot_hook != NULL && coop_dev_loot_hook(0) ? "done" : "not possible (window not open)");
            } else {
                tig_debug_printf("CoopLite: dev: loot window: close %s\n",
                    coop_dev_loot_hook != NULL && coop_dev_loot_hook(1) ? "done" : "not possible");
            }
            coop_dev_res_ui_stage[slot]++;
            coop_dev_res_ms[slot] = now_ms != 0 ? now_ms : 1;
            continue;
        }
        if (strchr(env, 'h') != NULL) {
            /* "5h": the GUI way — a real click on the Resurrect hotbar slot,
               then (another N s later) a real click on the corpse. */
            if (coop_dev_res_ui_stage[slot] == (strchr(env, 'l') != NULL ? 3 : 0)) {
                int idx = coop_hotkey_spell_hook != NULL ? coop_hotkey_spell_hook(SPELL_RESURRECT) : -1;

                tig_debug_printf("CoopLite: dev: UI click on hotbar slot %d (Resurrect) %s\n", idx,
                    idx >= 0 && coop_dev_click_hotkey_hook != NULL && coop_dev_click_hotkey_hook(idx)
                        ? "queued"
                        : "not possible");
                coop_dev_res_ui_stage[slot]++;
                coop_dev_res_ms[slot] = now_ms != 0 ? now_ms : 1;
                continue;
            }
            coop_dev_res_done[slot] = true;
            tig_debug_printf("CoopLite: dev: UI click on slot %d's corpse %s\n", slot,
                coop_dev_click_obj_hook != NULL && coop_dev_click_obj_hook(pc, 0) ? "queued" : "not possible");
            continue;
        }
        coop_dev_res_done[slot] = true;
        magictech_invocation_init(&mt_invocation, caster, SPELL_RESURRECT);
        sub_4440E0(pc, &(mt_invocation.target_obj));
        if (strchr(env, 'f') != NULL) {
            mt_invocation.flags |= MAGICTECH_INVOCATION_FREE; /* "5f": no fatigue cost */
        }
        tig_debug_printf("CoopLite: dev: host casts Resurrect on slot %d PC\n", slot);
        magictech_invocation_run(&mt_invocation);
        if (strchr(env, 'd') != NULL) {
            /* "5d": a double click — a second cast right behind the first. */
            MagicTechInvocation again;

            magictech_invocation_init(&again, caster, SPELL_RESURRECT);
            sub_4440E0(pc, &(again.target_obj));
            again.flags = mt_invocation.flags;
            tig_debug_printf("CoopLite: dev: host casts Resurrect again (double cast)\n");
            magictech_invocation_run(&again);
        }
    }
}
#endif

#ifndef COOP_LITE_TEST_STUBS
/*
 * PT23 (revisits review M1): state the original session reset did not
 * cover. Not every item was shown to misbehave, but all of it describes the
 * previous session's objects or a half-received batch, and main menu ->
 * New Game without a restart kept it: half-assembled inventory batches,
 * stack OID maps, PC build / inventory signatures, fallen flags, sector
 * keep-alive locks, ground-item reports.
 */
static void coop_lite_session_reset_extra(void)
{
    coop_lite_area_session_reset(); /* PT79 */
    coop_inv_owner_cnt = 0;
    coop_inv_entry_cnt = 0;
    coop_inv_overflow = false;
    memset(coop_stack_map, 0, sizeof(coop_stack_map));
    coop_stack_map_next = 0;
    memset(coop_host_gold_oid, 0, sizeof(coop_host_gold_oid));
    memset(coop_host_ammo_oid, 0, sizeof(coop_host_ammo_oid));
    memset(coop_slot_inven_sig_valid, 0, sizeof(coop_slot_inven_sig_valid));
    memset(coop_pcs_sig, 0, sizeof(coop_pcs_sig));
    memset(coop_pcs_obj, 0, sizeof(coop_pcs_obj));
    coop_client_pcs_sig = 0;
    coop_client_pcs_resend = 4; /* PT54a */
    coop_client_pcs_resend_ms = 0;
    coop_lite_story_reset();
    coop_lite_dialog_reset();
    memset(coop_remote_fallen, 0, sizeof(coop_remote_fallen));
    memset(coop_slot_hp_damage, 0, sizeof(coop_slot_hp_damage));
    memset(coop_world_prev_ground_cnt, 0, sizeof(coop_world_prev_ground_cnt));
    memset(coop_rejoin_pc_oid, 0, sizeof(coop_rejoin_pc_oid));
    coop_party_inven_dirty = false;
    coop_follow_carry_pending = false;
    coop_follow_host_loc = 0;
    coop_follow_on = false; /* PT52 */
    coop_lite_travel_session_reset(); /* PT52 */
    if (coop_client_wmap_dimmed) {
        coop_lite_client_wmap_dim(false); /* PT26: never stuck dimmed in the menu */
    }
    coop_host_wmap_open = false;
    memset(coop_vitals_sent_hp, 0, sizeof(coop_vitals_sent_hp));
    memset(coop_vitals_sent_fat, 0, sizeof(coop_vitals_sent_fat));
}
#endif

/* ===== PT50: travel tether / ready check, client spell-end intent ===== */

bool coop_lite_travel_slot_ready(int state, int64_t dist)
{
    if (state == COOP_TRAVEL_READY) {
        return true;
    }
    return state == COOP_TRAVEL_IDLE && dist >= 0 && dist <= COOP_TRAVEL_RANGE;
}

/* PT52k: pure tally of client slots 1..7 (soak-tested). A dead PC's READY
   is dropped (state -> -1); absent / dead slots never hold the party. */
int coop_lite_travel_tally(const CoopTravelSlot ts[8], int state[8], int* ready_ptr, int* far_mask_ptr)
{
    int slot;
    int living = 0;
    int ready = 0;
    int far_mask = 0;

    for (slot = 1; slot < 8; slot++) {
        if (!ts[slot].present || ts[slot].dead) {
            if (ts[slot].present) {
                state[slot] = -1; /* PT52e: READY dies with the PC */
            }
            continue;
        }
        living++;
        if (ts[slot].dist > COOP_TRAVEL_RANGE) {
            far_mask |= 1 << slot;
        }
        if (coop_lite_travel_slot_ready(state[slot], ts[slot].dist)) {
            ready++;
        }
    }
    if (ready_ptr != NULL) {
        *ready_ptr = ready;
    }
    if (far_mask_ptr != NULL) {
        *far_mask_ptr = far_mask;
    }
    return living;
}

/* PT52k: connected client slots not yet told about the running check. */
int coop_lite_travel_untold(int told_mask, int present_mask)
{
    return present_mask & ~told_mask & 0xFE;
}

bool coop_lite_party_ctrl_valid(const PacketCoopPartyCtrl* pkt, int size)
{
    return pkt != NULL && pkt->type == PACKET_COOP_PARTY_CTRL && size >= (int)sizeof(PacketCoopPartyCtrl)
        && pkt->op >= COOP_CTRL_TRAVEL_START && pkt->op <= COOP_CTRL_SPELL_END;
}

/* PT57 */
bool coop_lite_travel_req_valid(const PacketCoopTravelReq* pkt, int size)
{
    return pkt != NULL && pkt->type == PACKET_COOP_TRAVEL_REQ && size >= (int)sizeof(PacketCoopTravelReq)
        && pkt->kind >= COOP_TREQ_ASK && pkt->kind <= COOP_TREQ_ACCEPTED && pkt->map > 0 && pkt->map < 100000;
}

static bool (*coop_travel_busy_hook)(void);
static void (*coop_travel_show_hook)(const char* text);
static void (*coop_travel_hide_hook)(void);
#define coop_travel_show_text(t) coop_travel_show_hook(coop_lite_lang(t)) /* PT53 */

void coop_lite_set_travel_ui_hooks(bool (*busy)(void), void (*show)(const char* text), void (*hide)(void))
{
    coop_travel_busy_hook = busy;
    coop_travel_show_hook = show;
    coop_travel_hide_hook = hide;
}

#ifndef COOP_LITE_TEST_STUBS
static bool coop_travel_active; /* host: ready check running; client: notice shown */
static bool coop_travel_bypass;
static TeleportData coop_travel_td;
static unsigned int coop_travel_draw_ms;
static int coop_travel_state[8];
static int coop_travel_client_sent; /* client: last state sent */
static bool coop_travel_far; /* client: host said we are out of range */
static bool coop_travel_pressed;
static bool coop_client_busy[8]; /* PT52 host: last BUSY/IDLE a client reported */
static int64_t coop_travel_start_loc; /* PT52 host: where the check started */
static int coop_travel_told_mask; /* PT52d host: slots that got TRAVEL_START */

static void coop_travel_send(int op, int a, int b, int slot)
{
    PacketCoopPartyCtrl pkt;

    pkt.type = PACKET_COOP_PARTY_CTRL;
    pkt.op = op;
    pkt.a = a;
    pkt.b = b;
    if (slot < 0) {
        int depth = tig_net_local_suspend_pause();

        tig_net_send_app_all(&pkt, sizeof(pkt));
        tig_net_local_suspend_resume(depth);
    } else {
        tig_net_send_app(&pkt, sizeof(pkt), slot);
    }
}

static void coop_travel_hide(void)
{
    if (coop_travel_hide_hook != NULL) {
        coop_travel_hide_hook();
    }
}

/* Host: living clients and how many are ready; mask of far ones. */
static int coop_travel_count(int* ready_ptr, int* far_mask_ptr, char* names, size_t size)
{
    int64_t host_pc = player_get_local_pc_obj();
    int64_t host_loc;
    int slot;
    int living = 0;
    int ready = 0;
    int far_mask = 0;

    if (names != NULL && size > 0) {
        names[0] = '\0';
    }
    if (host_pc == OBJ_HANDLE_NULL) {
        return 0;
    }
    host_loc = obj_field_int64_get(host_pc, OBJ_F_LOCATION);
    {
        CoopTravelSlot ts[8];

        memset(ts, 0, sizeof(ts));
        for (slot = 1; slot < 8; slot++) {
            int64_t pc = sub_4A2B60(slot);

            ts[slot].present = pc != OBJ_HANDLE_NULL && pc != host_pc;
            ts[slot].dead = ts[slot].present && critter_is_dead(pc);
            ts[slot].dist = ts[slot].present ? location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION), host_loc) : -1;
        }
        living = coop_lite_travel_tally(ts, coop_travel_state, &ready, &far_mask);
    }
    for (slot = 1; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);

        if (pc == OBJ_HANDLE_NULL || pc == host_pc || critter_is_dead(pc)) {
            continue;
        }
        if (coop_lite_travel_slot_ready(coop_travel_state[slot],
                location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION), host_loc))) {
            continue;
        } else if (names != NULL && strlen(names) + 16 < size) {
            char name[128];
            size_t cut;

            object_examine(pc, pc, name);
            cut = strlen(name) > 15 ? 15 : strlen(name); /* PT52k: no read past the NUL */
            /* PT52j: cut on a UTF-8 boundary (Cyrillic names are 2 bytes/char) */
            while (cut > 0 && ((unsigned char)name[cut] & 0xC0) == 0x80) {
                cut--;
            }
            name[cut] = '\0';
            strncat(names, " ", size - strlen(names) - 1);
            strncat(names, name, size - strlen(names) - 1);
        }
    }
    if (ready_ptr != NULL) {
        *ready_ptr = ready;
    }
    if (far_mask_ptr != NULL) {
        *far_mask_ptr = far_mask;
    }
    return living;
}

/* PT52: drop a running check (session reset, save load, host death /
   combat / walked off). The host tells the clients. */
void coop_lite_travel_reset(void)
{
    if (coop_travel_active && tig_net_is_active() && tig_net_is_host()) {
        coop_travel_send(COOP_CTRL_TRAVEL_END, 0, 0, -1);
    }
    if (coop_travel_active) {
        coop_travel_hide();
    }
    coop_travel_active = false;
    coop_travel_pressed = false;
    coop_travel_client_sent = -1;
}

/* PT52c host: a disconnected slot's busy flag and snapshot list go */
static bool coop_treq_active; /* PT57 */
static int coop_treq_slot;
static bool coop_treq_accept(void);

void coop_lite_pt52_slot_gone(int slot)
{
    if (slot > 0 && slot < 8) {
        coop_lite_pt61_slot_gone(slot);
        if (coop_treq_active && coop_treq_slot == slot) {
            coop_treq_active = false; /* PT57: the asker left */
            coop_travel_hide();
        }
        coop_client_busy[slot] = false;
        coop_travel_state[slot] = -1; /* PT52d: a rejoin is not READY */
        coop_travel_told_mask &= ~(1 << slot);
        coop_world_full_active[slot] = false;
        coop_world_full_need_walk[slot] = false; /* PT52k */
        coop_world_full_key[slot] = -1;
        coop_world_full_pc[slot] = OBJ_HANDLE_NULL;
        coop_world_full_cursor[slot] = 0;
        coop_world_full_first[slot] = false;
        coop_world_full_free(slot);
    }
}

void coop_lite_travel_session_reset(void)
{
    int slot;

    coop_treq_active = false; /* PT57 */

    coop_lite_travel_reset();
    memset(coop_client_busy, 0, sizeof(coop_client_busy));
    for (slot = 0; slot < 8; slot++) {
        coop_travel_state[slot] = -1; /* PT52g: 0 is COOP_TRAVEL_BUSY */
    }
}

static void coop_travel_go(const char* why)
{
    TeleportData td = coop_travel_td;

    coop_travel_active = false;
    coop_travel_send(COOP_CTRL_TRAVEL_END, 0, 0, -1);
    coop_travel_hide();
    tig_debug_printf("CoopLite: PT50 travel check done (%s), host leaves\n", why);
    coop_travel_bypass = true;
    if (!coop_lite_auth_teleport(&td)) {
        tig_debug_printf("CoopLite: PT52e travel: teleport_do failed\n");
    }
    coop_travel_bypass = false;
}

bool coop_lite_host_travel_gate(const TeleportData* td)
{
    int living;
    int far_mask;
    int slot;

    if (td == NULL || coop_travel_bypass || !tig_net_is_coop_enabled() || !tig_net_is_active()
        || !tig_net_is_host() || !coop_lite_follow_armed || coop_lite_applying_follow
        || !player_is_local_pc_obj(td->obj) || td->map <= 0) {
        return false;
    }
    if (coop_travel_active) {
        /* the host repeated the action: go now with the newest target */
        coop_travel_td = *td;
        coop_travel_go("forced by host");
        return true;
    }
    if (critter_is_dead(td->obj) || combat_critter_is_combat_mode_active(td->obj)) {
        return false;
    }
    /* PT77q: a world-map stop / arrival / random encounter (wmap_ui_teleport)
       must not start a ready check: that used the old-map coords and left
       the host stuck. The party already travels with the host on the map.
       Town-exit teleports that only open the world map afterwards still
       go through the gate (the map UI is not open yet). The town map is
       not a world-map trip. */
    if (wmap_ui_is_created() && !wmap_ui_is_town_mode()) {
        return false;
    }
    /* PT52: start from what the clients reported (BUSY/IDLE); everyone near
       and free -> no panel, travel right away. */
    for (slot = 0; slot < 8; slot++) {
        coop_travel_state[slot] = coop_client_busy[slot] ? COOP_TRAVEL_BUSY : COOP_TRAVEL_IDLE;
    }
    {
        int ready;

        living = coop_travel_count(&ready, &far_mask, NULL, 0);
        if (living == 0 || ready >= living) {
            return false;
        }
    }
    coop_travel_start_loc = obj_field_int64_get(td->obj, OBJ_F_LOCATION);
    coop_travel_td = *td;
    coop_travel_active = true;
    coop_travel_draw_ms = 0;
    coop_travel_send(COOP_CTRL_TRAVEL_START, far_mask, 0, -1);
    /* PT52i: only slots connected now got it; a late joiner is told by the tick */
    coop_travel_told_mask = 0;
    for (slot = 1; slot < 8; slot++) {
        if (sub_4A2B60(slot) != OBJ_HANDLE_NULL) {
            coop_travel_told_mask |= 1 << slot;
        }
    }
    tig_debug_printf("CoopLite: PT50 host travel deferred, ready check (living=%d far=0x%x)\n", living, far_mask);
    return true;
}

bool coop_lite_travel_key(void)
{
    if (tig_net_is_active() && tig_net_is_host() && coop_treq_active && !coop_travel_active) {
        return coop_treq_accept(); /* PT57: Y = go */
    }
    if (!tig_net_is_active() || !coop_travel_active) {
        return false;
    }
    if (tig_net_is_host()) {
        coop_travel_go("host pressed Y");
    } else {
        coop_travel_pressed = true;
        coop_travel_client_sent = COOP_TRAVEL_READY;
        coop_travel_send(COOP_CTRL_TRAVEL_STATE, COOP_TRAVEL_READY, 0, -1);
        if (coop_travel_show_hook != NULL) {
            coop_travel_show_text("Лидер группы собирается в путь. Вы готовы — ждём остальных.\nParty leader is setting off. You are ready - waiting for the others.");
        }
    }
    return true;
}

static void coop_travel_tick(unsigned int now_ms)
{
    char text[512];
    char names[128];
    int living;
    int ready;

    if (!tig_net_is_active()) {
        if (coop_travel_active) {
            coop_travel_active = false;
            coop_travel_hide();
        }
        return;
    }
    if (!tig_net_is_host()) {
        int st;

        /* PT52: always report BUSY/IDLE changes, not only during a check */
        if (!tig_net_is_coop_client() || !coop_lite_client_in_session || coop_travel_pressed) {
            return;
        }
        st = coop_travel_busy_hook != NULL && coop_travel_busy_hook() ? COOP_TRAVEL_BUSY : COOP_TRAVEL_IDLE;
        if (st != coop_travel_client_sent) {
            coop_travel_client_sent = st;
            coop_travel_send(COOP_CTRL_TRAVEL_STATE, st, 0, -1);
            /* PT52d: became busy during a check: say who is waiting */
            if (coop_travel_active && st == COOP_TRAVEL_BUSY && coop_travel_show_hook != NULL) {
                coop_travel_show_text("Лидер группы ждёт вас: закройте окно или нажмите Y.\nParty leader is waiting: close the window or press Y.");
            }
        }
        return;
    }
    if (!coop_travel_active) {
        return;
    }
    {
        int64_t host_pc = player_get_local_pc_obj();

        /* PT52: host died, entered combat or walked off (clicked elsewhere) */
        if (host_pc == OBJ_HANDLE_NULL || critter_is_dead(host_pc)
            || combat_critter_is_combat_mode_active(host_pc)
            || location_dist(obj_field_int64_get(host_pc, OBJ_F_LOCATION), coop_travel_start_loc) > 3) {
            tig_debug_printf("CoopLite: PT52 travel check cancelled\n");
            coop_lite_travel_reset();
            return;
        }
    }
    {
        int far_mask;
        int slot;

        /* PT52d: a client that joined during the check gets TRAVEL_START */
        coop_travel_count(NULL, &far_mask, NULL, 0);
        int present = 0;
        int untold;

        for (slot = 1; slot < 8; slot++) {
            if (sub_4A2B60(slot) != OBJ_HANDLE_NULL) {
                present |= 1 << slot;
            }
        }
        untold = coop_lite_travel_untold(coop_travel_told_mask, present);
        for (slot = 1; slot < 8; slot++) {
            if ((untold & (1 << slot)) != 0) {
                coop_travel_told_mask |= 1 << slot;
                coop_travel_state[slot] = -1;
                coop_travel_send(COOP_CTRL_TRAVEL_START, far_mask, 0, slot);
            }
        }
    }
    living = coop_travel_count(&ready, NULL, names, sizeof(names));
    if (living == 0 || ready >= living) {
        coop_travel_go("all ready");
        return;
    }
    if (coop_travel_draw_ms != 0 && !coop_lite_interval_elapsed(coop_travel_draw_ms, now_ms, 500u)) {
        return;
    }
    coop_travel_draw_ms = now_ms != 0 ? now_ms : 1;
    snprintf(text, sizeof(text), "Готовы %d/%d. Ждём:%s. Y — отправиться сразу.\nReady %d/%d. Waiting:%s. Y - go now.", ready, living, names, ready, living, names);
    if (coop_travel_show_hook != NULL) {
        coop_travel_show_text(text);
    }
}

static void coop_travel_host_spell_end(int slot, int mt_id)
{
    MagicTechRunInfo* run_info;
    int64_t pc = sub_4A2B60(slot);

    if (pc == OBJ_HANDLE_NULL || !magictech_id_to_run_info(mt_id, &run_info)
        || run_info->parent_obj.obj != pc || run_info->action != MAGICTECH_ACTION_MAINTAIN) {
        tig_debug_printf("CoopLite: PT50 slot %d spell end %d rejected\n", slot, mt_id);
        /* PT52e: tell the client, it drops its stale shadow icon */
        coop_travel_send(COOP_CTRL_SPELL_END, mt_id, 1, slot);
        return;
    }
    tig_debug_printf("CoopLite: PT50 slot %d ends maintained spell %d\n", slot, mt_id);
    magictech_interrupt(mt_id); /* END path sends Packet61 to the owner */
}

void coop_lite_handle_party_ctrl(PacketCoopPartyCtrl* pkt)
{
    int slot;

    if (!tig_net_is_active() || !coop_lite_party_ctrl_valid(pkt, tig_net_last_message_size())) {
        return;
    }
    if (tig_net_is_host()) {
        slot = sub_52A530();
        if (slot <= 0 || slot >= 8) {
            return;
        }
        if (pkt->op == COOP_CTRL_TRAVEL_STATE && pkt->a >= COOP_TRAVEL_BUSY && pkt->a <= COOP_TRAVEL_READY) {
            coop_client_busy[slot] = pkt->a == COOP_TRAVEL_BUSY;
            if (coop_travel_active) {
                coop_travel_state[slot] = pkt->a;
                coop_travel_draw_ms = 0;
            }
        } else if (pkt->op == COOP_CTRL_SPELL_END) {
            coop_travel_host_spell_end(slot, pkt->a);
        }
        return;
    }
    if (pkt->op == COOP_CTRL_TRAVEL_START) {
        int my = sub_529520();
        bool busy = coop_travel_busy_hook != NULL && coop_travel_busy_hook();

        coop_travel_active = true;
        coop_travel_pressed = false;
        coop_travel_far = my > 0 && my < 8 && (pkt->a & (1 << my)) != 0;
        coop_travel_client_sent = busy ? COOP_TRAVEL_BUSY : COOP_TRAVEL_IDLE;
        coop_travel_send(COOP_CTRL_TRAVEL_STATE, coop_travel_client_sent, 0, -1);
        if ((coop_travel_far || busy) && coop_travel_show_hook != NULL) {
            char text[512];

            snprintf(text, sizeof(text), "Лидер группы собирается в путь%s. Y — готов.\nParty leader is setting off%s. Y - ready.",
                coop_travel_far ? " (вы далеко)" : "", coop_travel_far ? " (you are far)" : "");
            coop_travel_show_text(text);
        }
    } else if (pkt->op == COOP_CTRL_SPELL_END) {
        /* PT52e: host has no such maintained spell; end the local shadow */
        tig_debug_printf("CoopLite: PT52e host rejected spell end %d, local end\n", pkt->a);
        magictech_interrupt(pkt->a);
    } else if (pkt->op == COOP_CTRL_TRAVEL_END) {
        coop_travel_active = false;
        coop_travel_pressed = false; /* PT52c: proactive BUSY/IDLE resumes */
        coop_travel_client_sent = -1;
        coop_travel_hide();
    }
}

/* ---- PT57: client travel requests (coop_treq_active: host, a request waits for Y / N) ---- */
static PacketCoopTravelReq coop_treq;
static unsigned int coop_treq_sent_ms; /* client: last ask (jump points re-fire) */

static void coop_treq_send(int kind, int map, int64_t loc, int slot)
{
    PacketCoopTravelReq pkt;

    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_TRAVEL_REQ;
    pkt.kind = kind;
    pkt.map = map;
    pkt.loc = loc;
    if (slot < 0) {
        int depth = tig_net_local_suspend_pause();

        tig_net_send_app_all(&pkt, sizeof(pkt));
        tig_net_local_suspend_resume(depth);
    } else {
        tig_net_send_app(&pkt, sizeof(pkt), slot);
    }
}

static const char* coop_treq_map_name(int map)
{
    char* name = NULL;

    if (map > 0 && map_get_name(map, &name) && name != NULL && name[0] != '\0') {
        return name;
    }
    return "another area";
}

bool coop_lite_client_travel_intercept(const TeleportData* td)
{
    unsigned int now;

    if (td == NULL || coop_client_tp_auth > 0 || !tig_net_is_coop_enabled() || !tig_net_is_active()
        || tig_net_is_host() || !coop_lite_client_in_session || !player_is_local_pc_obj(td->obj)
        || td->map <= 0 || !map_is_valid() || td->map == map_current_map()) {
        return false;
    }
    now = coop_obj_now_ms;
    if (coop_treq_sent_ms == 0 || coop_lite_interval_elapsed(coop_treq_sent_ms, now, 3000u)) {
        coop_treq_sent_ms = now != 0 ? now : 1u;
        coop_treq_send(COOP_TREQ_ASK, td->map, td->loc, -1);
        coop_lite_show_notice(coop_lite_lang_ru() ? "Запрос на переход отправлен хосту." : "Travel request sent to the host - waiting for Y / N.", TF_TYPE_WHITE);
        tig_debug_printf("CoopLite: PT57 travel request to map %d sent to the host\n", td->map);
    }
    return true; /* never alone */
}

bool coop_lite_client_may_open_worldmap(void)
{
    if (!tig_net_is_active() || !tig_net_is_coop_enabled() || tig_net_is_host()
        || !coop_lite_client_in_session) {
        return false;
    }
    /* Host already travelling: PT73 spectator view owns the map. */
    if (coop_client_wmap_dimmed || wmap_ui_is_spectating()) {
        return false;
    }
    return true;
}

bool coop_lite_client_worldmap_propose(int64_t loc)
{
    unsigned int now;
    int map;

    if (!tig_net_is_coop_enabled() || !tig_net_is_active() || tig_net_is_host()
        || !coop_lite_client_in_session || coop_client_tp_auth > 0) {
        return false;
    }
    map = map_by_type(MAP_TYPE_START_MAP);
    /* No destination / no overworld map: still do not walk the route alone. */
    if (loc == 0 || map <= 0) {
        return true;
    }
    now = coop_obj_now_ms;
    if (coop_treq_sent_ms == 0 || coop_lite_interval_elapsed(coop_treq_sent_ms, now, 3000u)) {
        coop_treq_sent_ms = now != 0 ? now : 1u;
        coop_treq_send(COOP_TREQ_ASK, map, loc, -1);
        coop_lite_show_notice(coop_lite_lang_ru()
            ? "Запрос на переход отправлен хосту."
            : "Travel request sent to the host - waiting for Y / N.", TF_TYPE_WHITE);
        tig_debug_printf("CoopLite: PT57 world-map travel request to %d,%d (map %d) sent to the host\n",
            (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc), map);
    }
    return true;
}

bool coop_lite_travel_refuse_key(void)
{
    if (!tig_net_is_active() || !tig_net_is_host() || !coop_treq_active) {
        return false;
    }
    coop_treq_active = false;
    coop_travel_hide();
    coop_treq_send(COOP_TREQ_REFUSED, coop_treq.map, coop_treq.loc, coop_treq_slot);
    tig_debug_printf("CoopLite: PT57 host refused slot %d travel to map %d\n", coop_treq_slot, coop_treq.map);
    return true;
}

static bool coop_treq_accept(void)
{
    TeleportData td;
    int64_t pc = player_get_local_pc_obj();

    if (!coop_treq_active) {
        return false;
    }
    coop_treq_active = false;
    coop_travel_hide();
    if (pc == OBJ_HANDLE_NULL || critter_is_dead(pc)) {
        coop_treq_send(COOP_TREQ_REFUSED, coop_treq.map, coop_treq.loc, coop_treq_slot);
        return true;
    }
    coop_treq_send(COOP_TREQ_ACCEPTED, coop_treq.map, coop_treq.loc, coop_treq_slot);
    memset(&td, 0, sizeof(td));
    td.obj = pc;
    td.map = coop_treq.map;
    td.loc = coop_treq.loc;
    tig_debug_printf("CoopLite: PT57 host accepted slot %d travel to map %d - party travels\n",
        coop_treq_slot, coop_treq.map);
    /* The host way: ready check (gate) and party carry, as if the host left. */
    teleport_do(&td);
    return true;
}

/* PT64: the host shows the visible Y / N travel prompt for slot's ask
   (from a PT57 packet or a transition the host itself ran for that PC). */
static void coop_treq_host_ask(int slot, int map, int64_t loc, const char* via)
{
    int64_t pc;
    char name[128];
    char text[512];

    pc = sub_4A2B60(slot);
    /* A missing map name still shows the Y/N prompt ("another area"); only a
       missing or dead asker is auto-refused. */
    if (pc == OBJ_HANDLE_NULL || critter_is_dead(pc)) {
        coop_treq_send(COOP_TREQ_REFUSED, map, loc, slot);
        return;
    }
    if (coop_treq_active && coop_treq_slot == slot && coop_treq.map == map) {
        return; /* the same ask is already on screen (jump points re-fire) */
    }
    memset(&coop_treq, 0, sizeof(coop_treq));
    coop_treq.type = PACKET_COOP_TRAVEL_REQ;
    coop_treq.kind = COOP_TREQ_ASK;
    coop_treq.map = map;
    coop_treq.loc = loc;
    coop_treq_active = true;
    coop_treq_slot = slot;
    object_examine(pc, pc, name);
    if (coop_lite_lang_ru()) {
        snprintf(text, sizeof(text), "%s хочет перейти в %s. Y - идти всей группой, N - отказать.",
            name, coop_treq_map_name(map));
    } else {
        snprintf(text, sizeof(text), "%s wants to travel to %s. Y = the party goes, N = refuse.",
            name, coop_treq_map_name(map));
    }
    if (coop_travel_show_hook != NULL) {
        coop_travel_show_text(text);
    }
    /* always in the message line too, so it is never only in the log */
    coop_lite_show_notice(text, TF_TYPE_WHITE);
    tig_debug_printf("CoopLite: PT64 Travel request: slot %d asks to travel to map %d (%s)\n", slot, map, via);
}

/* PT64: a client PC stepped on a jump point / ran a scripted exit / portal
   on the HOST (its moves and scripts run here): it must not leave alone. */
bool coop_lite_host_remote_travel_intercept(const TeleportData* td)
{
    int slot;

    if (td == NULL || coop_client_tp_auth > 0 || coop_travel_bypass || coop_lite_applying_follow
        || !tig_net_is_coop_enabled() || !tig_net_is_active() || !tig_net_is_host()
        || td->obj == OBJ_HANDLE_NULL || player_is_local_pc_obj(td->obj)
        || td->map <= 0 || !map_is_valid() || td->map == map_current_map()) {
        return false;
    }
    slot = coop_lite_slot_of_obj(td->obj);
    if (slot <= 0 || slot >= 8) {
        return false;
    }
    if (!coop_travel_active) {
        coop_treq_host_ask(slot, td->map, td->loc, "host-side transition");
    }
    return true; /* never alone */
}

void coop_lite_handle_travel_req(PacketCoopTravelReq* pkt)
{
    if (!tig_net_is_active() || !coop_lite_travel_req_valid(pkt, tig_net_last_message_size())) {
        return;
    }
    if (tig_net_is_host()) {
        int slot = sub_52A530();

        if (pkt->kind != COOP_TREQ_ASK || slot <= 0 || slot >= 8) {
            return;
        }
        coop_treq_host_ask(slot, pkt->map, pkt->loc, "client packet");
        return;
    }
    if (pkt->kind == COOP_TREQ_REFUSED) {
        coop_treq_sent_ms = 0;
        coop_lite_show_notice(coop_lite_lang_ru() ? "Хост отказал в переходе." : "The host refused the trip.", TF_TYPE_RED);
    } else if (pkt->kind == COOP_TREQ_ACCEPTED) {
        coop_treq_sent_ms = 0;
        coop_lite_show_notice(coop_lite_lang_ru() ? "Хост согласился: группа отправляется." : "The host agreed: the party is setting off.", TF_TYPE_WHITE);
    }
}

bool coop_lite_client_spell_end(int mt_id)
{
    if (!tig_net_is_coop_enabled() || !tig_net_is_active() || tig_net_is_host()) {
        return false;
    }
    coop_travel_send(COOP_CTRL_SPELL_END, mt_id, 0, -1);
    return true;
}
#else
bool coop_lite_host_travel_gate(const TeleportData* td) { (void)td; return false; }
void coop_lite_travel_reset(void) { }
void coop_lite_travel_session_reset(void) { }
void coop_lite_pt52_slot_gone(int slot) { (void)slot; }
bool coop_lite_travel_key(void) { return false; }
void coop_lite_handle_party_ctrl(PacketCoopPartyCtrl* pkt) { (void)pkt; }
bool coop_lite_client_spell_end(int mt_id) { (void)mt_id; return false; }
bool coop_lite_client_travel_intercept(const TeleportData* td) { (void)td; return false; }
bool coop_lite_host_remote_travel_intercept(const TeleportData* td) { (void)td; return false; }
bool coop_lite_client_may_open_worldmap(void) { return false; }
bool coop_lite_client_worldmap_propose(int64_t loc) { (void)loc; return false; }
bool coop_lite_travel_refuse_key(void) { return false; }
void coop_lite_handle_travel_req(PacketCoopTravelReq* pkt) { (void)pkt; }
static void coop_travel_tick(unsigned int now_ms) { (void)now_ms; }
#endif

/* PT66 (pure; tests): a dead critter's body is kept for good (never
   decays) when it is a player (any PC, co-op client PCs on the host too),
   a companion (its leader chain reaches a PC), or already marked. */
bool coop_lite_corpse_keep(bool is_pc, bool leads_to_pc, bool no_decay_flag)
{
    return is_pc || leads_to_pc || no_decay_flag;
}

/* PT73 (pure; tests) */
bool coop_lite_wmap_pos_valid(const PacketCoopWmapPos* pkt, int size)
{
    return pkt != NULL && pkt->type == PACKET_COOP_WMAP_POS && size >= (int)sizeof(PacketCoopWmapPos)
        && pkt->x >= 0 && pkt->y >= 0 && pkt->x < 100000 && pkt->y < 100000;
}

bool coop_lite_wmap_pos_should_send(bool have_last, unsigned int last_ms, unsigned int now_ms,
    int last_x, int last_y, int x, int y)
{
    unsigned int dt;

    if (!have_last) {
        return true;
    }
    dt = now_ms - last_ms;
    if (x != last_x || y != last_y) {
        return dt >= 150u;
    }
    return dt >= 1000u;
}

/* ---- PT77 chat (pure) --------------------------------------------------- */

bool coop_lite_chat_valid(const PacketCoopChat* pkt, int size)
{
    int i;

    if (pkt == NULL || pkt->type != PACKET_COOP_CHAT || size < (int)sizeof(PacketCoopChat)) {
        return false;
    }
    if (pkt->slot < 0 || pkt->slot >= 8) {
        return false;
    }
    if (pkt->name[COOP_CHAT_MAX_NAME - 1] != '\0' || pkt->text[COOP_CHAT_MAX_TEXT - 1] != '\0') {
        return false;
    }
    if (pkt->text[0] == '\0') {
        return false;
    }
    for (i = 0; i < COOP_CHAT_MAX_NAME; i++) {
        if (pkt->name[i] == '\0') {
            break;
        }
        if ((unsigned char)pkt->name[i] < 0x20 || pkt->name[i] == 0x7F) {
            return false;
        }
    }
    for (i = 0; i < COOP_CHAT_MAX_TEXT; i++) {
        if (pkt->text[i] == '\0') {
            return true;
        }
        if ((unsigned char)pkt->text[i] < 0x20 || pkt->text[i] == 0x7F) {
            return false;
        }
    }
    return false;
}

void coop_lite_chat_sanitize(char* text, size_t max_len)
{
    size_t i;
    size_t o;
    char* start;

    if (text == NULL || max_len == 0) {
        return;
    }
    for (i = 0; text[i] != '\0' && i + 1 < max_len; i++) {
        if ((unsigned char)text[i] < 0x20 || text[i] == 0x7F) {
            text[i] = ' ';
        }
    }
    text[max_len - 1] = '\0';
    /* trim leading spaces */
    start = text;
    while (*start == ' ') {
        start++;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }
    /* trim trailing spaces */
    o = strlen(text);
    while (o > 0 && text[o - 1] == ' ') {
        text[--o] = '\0';
    }
}

bool coop_lite_chat_prepare(const char* in, char* out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (in == NULL || out_size < 2) {
        return false;
    }
    strncpy(out, in, out_size - 1);
    out[out_size - 1] = '\0';
    coop_lite_chat_sanitize(out, out_size);
    return out[0] != '\0';
}

void coop_lite_chat_format_line(const char* name, const char* text, char* out, size_t out_size)
{
    const char* n = (name != NULL && name[0] != '\0') ? name : "Player";
    const char* t = (text != NULL) ? text : "";

    if (out == NULL || out_size == 0) {
        return;
    }
    snprintf(out, out_size, "[%s]: %s", n, t);
}

bool coop_lite_chat_log_visible(bool typing, unsigned int last_activity_ms, unsigned int now_ms, unsigned int fade_ms)
{
    if (typing) {
        return true;
    }
    if (last_activity_ms == 0 || fade_ms == 0) {
        return false;
    }
    return (unsigned int)(now_ms - last_activity_ms) < fade_ms;
}

void coop_lite_chat_format_full(const char* nick, int hh, int mm, const char* character, const char* text, char* out, size_t out_size)
{
    bool has_nick = nick != NULL && nick[0] != '\0';
    bool has_char = character != NULL && character[0] != '\0';
    const char* t = text != NULL ? text : "";

    if (out == NULL || out_size == 0) {
        return;
    }
    if (hh < 0 || hh > 23) {
        hh = 0;
    }
    if (mm < 0 || mm > 59) {
        mm = 0;
    }
    if (has_nick && has_char) {
        snprintf(out, out_size, "(%s) [%02d:%02d] %s: %s", nick, hh, mm, character, t);
    } else if (has_nick || has_char) {
        snprintf(out, out_size, "[%02d:%02d] %s: %s", hh, mm, has_nick ? nick : character, t);
    } else {
        snprintf(out, out_size, "[%02d:%02d] Player: %s", hh, mm, t);
    }
}

void coop_lite_chat_log_origin(int screen_w, int screen_h, int log_h, int* x, int* y)
{
    int hud_x = (screen_w - COOP_CHAT_HUD_W) / 2;
    int ox = hud_x + 8;
    int oy = screen_h - COOP_CHAT_HUD_H - log_h - 4;

    if (hud_x < 0) {
        ox = 8;
    }
    if (oy < 0) {
        oy = 0;
    }
    if (x != NULL) {
        *x = ox;
    }
    if (y != NULL) {
        *y = oy;
    }
}

void coop_lite_chat_text_area(int screen_w, int screen_h, int lines, int line_h, int* x, int* y, int* w, int* h)
{
    int ah = lines * line_h;
    int ax;
    int ay;
    int aw;

    if (ah < 0) {
        ah = 0;
    }
    if (ah > screen_h) {
        ah = screen_h;
    }
    coop_lite_chat_log_origin(screen_w, screen_h, ah - 2, &ax, &ay);
    aw = screen_w - ax - 8;
    if (aw > 560) {
        aw = 560;
    }
    if (aw < 0) {
        aw = 0;
    }
    if (ay + ah > screen_h) {
        ay = screen_h - ah;
    }
    if (ay < 0) {
        ay = 0;
    }
    if (x != NULL) {
        *x = ax;
    }
    if (y != NULL) {
        *y = ay;
    }
    if (w != NULL) {
        *w = aw;
    }
    if (h != NULL) {
        *h = ah;
    }
}

int coop_lite_dialog_chat_route(bool coop_enabled, bool textedit_focused, bool is_keyboard, bool is_text_input, bool enter_pressed)
{
    if (!coop_enabled) {
        return 0;
    }
    if (textedit_focused && (is_keyboard || is_text_input)) {
        return 2;
    }
    if (is_keyboard && enter_pressed) {
        return 1;
    }
    return 0;
}

/* PT77aa (pure; tests): a post-close wipe of the destination map folder
   would delete the mobile.mdy teleport just wrote with the carried PC. */
bool coop_lite_carry_mdy_needs_side_file(const char* dst_map_dir, const char* wipe_dir)
{
    if (dst_map_dir == NULL || dst_map_dir[0] == '\0'
        || wipe_dir == NULL || wipe_dir[0] == '\0') {
        return false;
    }
#ifdef COOP_LITE_TEST_STUBS
    return strcasecmp(dst_map_dir, wipe_dir) == 0;
#else
    return SDL_strcasecmp(dst_map_dir, wipe_dir) == 0;
#endif
}

void coop_lite_carry_mdy_side_path(char* out, size_t out_sz)
{
    if (out == NULL || out_sz == 0) {
        return;
    }
    snprintf(out, out_sz, "%s", COOP_LITE_CARRY_MDY);
}

const char* coop_lite_teleport_mobile_mdy_path(const char* dst_map_dir, char* buf, size_t buf_sz)
{
    if (coop_lite_carry_mdy_needs_side_file(dst_map_dir, coop_map_wipe_path)) {
        coop_lite_carry_mdy_side_path(coop_carry_mdy_path, sizeof(coop_carry_mdy_path));
#ifndef COOP_LITE_TEST_STUBS
        if (!coop_carry_mdy_started) {
            tig_file_mkdir_ex("Save\\Current");
            tig_file_remove(coop_carry_mdy_path);
            coop_carry_mdy_started = true;
        }
#endif
#ifdef COOP_LITE_TEST_STUBS
        coop_carry_mdy_started = true;
#endif
        return coop_carry_mdy_path;
    }
    if (buf == NULL || buf_sz == 0) {
        return COOP_LITE_CARRY_MDY;
    }
    if (dst_map_dir == NULL || dst_map_dir[0] == '\0') {
        snprintf(buf, buf_sz, "mobile.mdy");
        return buf;
    }
    snprintf(buf, buf_sz, "%s\\mobile.mdy", dst_map_dir);
    return buf;
}

void coop_lite_test_set_map_wipe_path(const char* path)
{
    if (path == NULL || path[0] == '\0') {
        coop_map_wipe_path[0] = '\0';
    } else {
        snprintf(coop_map_wipe_path, sizeof(coop_map_wipe_path), "%s", path);
    }
    coop_carry_mdy_path[0] = '\0';
    coop_carry_mdy_started = false;
}

/* PT72 (pure; tests): a SESSION_SYNC that found no local PC is retried
   (about 10 s at the 250 ms retry pace) before the client gives up. */
bool coop_lite_sync_retry_allowed(int tries)
{
    return tries > 0 && tries <= 40;
}

/* PT77ae (pure; tests): from the 4th retry (~1 s) on, ask the host for our
   PC's blob, again every 4th retry; the menu rejoin stays the last resort
   after retry 40. */
bool coop_lite_sync_restore_due(int tries)
{
    return tries >= 4 && tries <= 40 && tries % 4 == 0;
}

/* PT67 (pure; tests): the dialog text window / input filter is still
   installed but nothing owns it any more (the shared dialog entry was
   already ended, or the mode stack is back in MAIN): it must be torn
   down, or every click goes to an option-less dialog forever. */
bool coop_lite_dialog_ui_orphaned(bool filter_installed, bool entry_active, bool mode_is_main)
{
    return filter_installed && (!entry_active || mode_is_main);
}

/* PT51: small sector-id set (pure; tests). add: false only when full. */
bool coop_lite_sector_list_add(int64_t* list, int* cnt, int max, int64_t id)
{
    if (coop_lite_sector_list_has(list, *cnt, id)) {
        return true;
    }
    if (*cnt >= max) {
        return false;
    }
    list[(*cnt)++] = id;
    return true;
}

bool coop_lite_sector_list_has(const int64_t* list, int cnt, int64_t id)
{
    int i;

    for (i = 0; i < cnt; i++) {
        if (list[i] == id) {
            return true;
        }
    }
    return false;
}

/* ---- PT53: client own-pack layout intent ---------------------------- */

bool coop_lite_inven_layout_loc_ok(int loc)
{
    return (loc >= 0 && loc < 120) || (loc >= 1000 && loc <= 1008) || (loc >= 2000 && loc <= 2009);
}

unsigned int coop_lite_inven_echo_flags(unsigned int seq)
{
    return (seq & 0xFFFFu) << COOP_INVEN_ECHO_SHIFT;
}

unsigned int coop_lite_inven_echo_seq(int flags)
{
    return ((unsigned int)flags >> COOP_INVEN_ECHO_SHIFT) & 0xFFFFu;
}

bool coop_lite_inven_echo_reached(unsigned int sent_seq, unsigned int echo_seq)
{
    if (sent_seq == 0) {
        return true;
    }
    return (int16_t)(uint16_t)((echo_seq - sent_seq) & 0xFFFFu) >= 0;
}

bool coop_lite_inven_layout_valid(const PacketCoopInvenLayout* pkt, int size)
{
    return pkt != NULL && pkt->type == PACKET_COOP_INVEN_LAYOUT
        && size >= (int)offsetof(PacketCoopInvenLayout, entries)
        && pkt->count >= 0 && pkt->count <= COOP_INVEN_LAYOUT_MAX
        && (size_t)size >= offsetof(PacketCoopInvenLayout, entries) + (size_t)pkt->count * sizeof(PacketCoopInvenLayoutEntry);
}

/* PT54: an OID's value bytes only (type + payload, padding zeroed), so
   two copies of one ID always canonicalise to the same 24 bytes. */
void coop_lite_oid_canon(uint8_t out[24], const void* oid24)
{
    const uint8_t* x = (const uint8_t*)oid24;
    int16_t t;
    size_t n;

    memset(out, 0, 24);
    memcpy(&t, x, sizeof(t));
    memcpy(out, &t, sizeof(t));
    switch (t) {
    case 0:
        return;
    case 1:
        n = 4;
        break;
    case 2:
    case 3:
        n = 16;
        break;
    default:
        n = 8;
        break;
    }
    memcpy(out + 8, x + 8, n);
}

unsigned int coop_lite_inven_layout_term(const uint8_t* oid24, int loc)
{
    unsigned int h = 2166136261u;
    uint8_t c[24];
    int k;

    /* PT54: by OID value (padding differed between copies), and grid
       cells count: backpack order is synced. */
    coop_lite_oid_canon(c, oid24);
    for (k = 0; k < 24; k++) {
        h = (h ^ c[k]) * 16777619u;
    }
    h = (h ^ (unsigned int)loc) * 16777619u;
    return h;
}

bool coop_lite_inven_layout_send_due(unsigned int local_hash, unsigned int host_hash, unsigned int acked_hash)
{
    return local_hash != host_hash && local_hash != acked_hash;
}

int coop_lite_inven_layout_plan(const uint8_t (*have_oid)[24], const int* have_loc, int nhave,
    const PacketCoopInvenLayoutEntry* want, int nwant, int* plan)
{
    int moves = 0;
    int i;
    int k;

    for (i = 0; i < nwant; i++) {
        int found = -1;
        bool bad = !coop_lite_inven_layout_loc_ok(want[i].inventory_location);

        for (k = 0; k < i && !bad; k++) {
            if (coop_lite_oid_raw_equal(want[k].oid, want[i].oid)) {
                bad = true; /* same item asked twice */
            }
        }
        for (k = 0; k < nhave && !bad; k++) {
            if (coop_lite_oid_raw_equal(have_oid[k], want[i].oid)) {
                if (found != -1) {
                    bad = true; /* two objects share the OID: ambiguous */
                }
                found = k;
            }
        }
        if (bad || found == -1) {
            plan[i] = COOP_LAYOUT_REFUSE;
        } else if (have_loc[found] == want[i].inventory_location) {
            /* PT54: grid order is synced too (PT53b kept it local). */
            plan[i] = COOP_LAYOUT_NOOP;
        } else {
            plan[i] = found;
            moves++;
        }
    }
    return moves;
}

/* PT54: squared distance between two backpack cells (10 columns). */
int coop_lite_grid_cell_dist(int a, int b)
{
    int dx = a % 10 - b % 10;
    int dy = a / 10 - b / 10;

    return dx * dx + dy * dy;
}

#define COOP_LAYOUT_HAVE_MAX 512
#ifndef COOP_LITE_TEST_STUBS

static unsigned int coop_lite_local_layout_hash(int64_t pc, PacketCoopInvenLayout* out)
{
    unsigned int h = 0;
    int cnt;
    int i;

    if (out != NULL) {
        out->count = 0;
    }
    cnt = obj_field_int32_get(pc, OBJ_F_CRITTER_INVENTORY_NUM);
    for (i = 0; i < cnt; i++) {
        int64_t item = obj_arrayfield_handle_get(pc, OBJ_F_CRITTER_INVENTORY_LIST_IDX, i);
        ObjectID oid;
        int loc;

        if (item == OBJ_HANDLE_NULL) {
            continue;
        }
        oid = obj_get_id(item);
        if (coop_lite_is_stackable(item)) {
            /* PT54b: gold / ammo stacks are local objects on a client (the
               host's have other OIDs and travel as totals); name the host's
               stack so it can be placed. Not in the host's echo either, so
               the client's cell for a stack is the truth on both sides. */
            coop_lite_client_map_stack_oid(&oid);
        }
        loc = item_inventory_location_get(item);
        h += coop_lite_inven_layout_term((const uint8_t*)&oid, loc);
        if (out != NULL && out->count < COOP_INVEN_LAYOUT_MAX) {
            memcpy(out->entries[out->count].oid, &oid, sizeof(out->entries[0].oid));
            out->entries[out->count].inventory_location = loc;
            out->entries[out->count].reserved = 0;
            out->count++;
        } else if (out != NULL) {
            static bool warned;

            if (!warned) {
                warned = true;
                tig_debug_printf("CoopLite: inven: layout over %d items - the rest is not sent\n", COOP_INVEN_LAYOUT_MAX);
            }
        }
    }
    return h;
}

/* Called at the end of an INVEN_SYNC apply: the host's view of our pack. */
static void coop_lite_client_layout_note_host(int64_t local_pc)
{
    int o;
    int i;
    unsigned int h = 0;

    if (coop_lite_client_layout_hold()) {
        return; /* PT54: an older sync; the echo of our newest layout follows */
    }
    if (coop_layout_sent_seq != 0) {
        coop_layout_sent_seq = 0; /* echo in: the host's layout is the truth */
    }
    for (o = 0; o < coop_inv_owner_cnt; o++) {
        int64_t owner = coop_lite_get_oid_obj(coop_inv_owner[o].head.owner_oid);

        if (owner != OBJ_HANDLE_NULL && owner != local_pc) {
            /* PT57: another player's pack we may have open: its host
               layout, so our own rearranging of it is sent as a request. */
            int k;

            for (k = 0; k < 8; k++) {
                if (sub_4A2B60(k) == owner) {
                    h = 0;
                    for (i = coop_inv_owner[o].first; i < coop_inv_owner[o].first + coop_inv_owner[o].count; i++) {
                        h += coop_lite_inven_layout_term(coop_inv_entry[i].oid, coop_inv_entry[i].inventory_location);
                    }
                    coop_other_host_hash[k] = h;
                    coop_other_acked_hash[k] = coop_lite_local_layout_hash(owner, NULL);
                    coop_other_valid[k] = true;
                    h = 0;
                    break;
                }
            }
            continue;
        }
        if (local_pc == OBJ_HANDLE_NULL || owner != local_pc) {
            continue;
        }
        for (i = coop_inv_owner[o].first; i < coop_inv_owner[o].first + coop_inv_owner[o].count; i++) {
            /* PT57: stacks are listed with the host's OIDs, the same names
               our layout uses for them (coop_lite_client_map_stack_oid). */
            h += coop_lite_inven_layout_term(coop_inv_entry[i].oid, coop_inv_entry[i].inventory_location);
        }
        coop_layout_host_hash = h;
        coop_layout_host_valid = true;
        /* PT53a: the host's echo is the truth; whatever differs locally now
           (a refused move, the client's own grid order) is not resent. */
        coop_layout_acked_hash = coop_lite_local_layout_hash(local_pc, NULL);
    }
}

void coop_lite_client_layout_tick(unsigned int now_ms)
{
    static PacketCoopInvenLayout pkt;
    int64_t pc;
    unsigned int h;
    ObjectID oid;
    int depth;

    if (!tig_net_is_active() || tig_net_is_host() || !coop_layout_host_valid) {
        return;
    }
    pc = player_get_local_pc_obj();
    if (pc == OBJ_HANDLE_NULL || critter_is_dead(pc) || inven_ui_drag_item_obj_get() != OBJ_HANDLE_NULL) {
        return; /* not mid-drag: a held item has no location yet */
    }
    h = coop_lite_local_layout_hash(pc, NULL);
    (void)now_ms;
    if (!coop_lite_inven_layout_send_due(h, coop_layout_host_hash, coop_layout_acked_hash)) {
        /* PT57: our edits to another player's pack go to the host as a
           request (seq 0: not an echo of our own layout). */
        int k;
        int me = sub_529520();

        for (k = 0; k < 8; k++) {
            int64_t other = sub_4A2B60(k);
            unsigned int oh;

            if (k == me || !coop_other_valid[k] || other == OBJ_HANDLE_NULL || other == pc || critter_is_dead(other)) {
                continue;
            }
            oh = coop_lite_local_layout_hash(other, NULL);
            if (!coop_lite_inven_layout_send_due(oh, coop_other_host_hash[k], coop_other_acked_hash[k])) {
                continue;
            }
            memset(&pkt, 0, sizeof(pkt));
            pkt.type = PACKET_COOP_INVEN_LAYOUT;
            oid = obj_get_id(other);
            memcpy(pkt.pc_oid, &oid, sizeof(pkt.pc_oid));
            coop_lite_local_layout_hash(other, &pkt);
            depth = tig_net_local_suspend_pause();
            tig_net_send_app_all(&pkt, (int)(offsetof(PacketCoopInvenLayout, entries) + (size_t)pkt.count * sizeof(pkt.entries[0])));
            tig_net_local_suspend_resume(depth);
            coop_other_acked_hash[k] = oh;
            tig_debug_printf("CoopLite: inven: PT57 layout request for slot %d's pack sent (%d items)\n", k, pkt.count);
            break;
        }
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_INVEN_LAYOUT;
    coop_layout_next_seq = (coop_layout_next_seq + 1) & 0xFFFFu;
    if (coop_layout_next_seq == 0) {
        coop_layout_next_seq = 1;
    }
    pkt.seq = coop_layout_next_seq;
    coop_layout_sent_seq = pkt.seq;
    coop_layout_sent_ms = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1u;
    oid = obj_get_id(pc);
    memcpy(pkt.pc_oid, &oid, sizeof(pkt.pc_oid));
    coop_lite_local_layout_hash(pc, &pkt);
    depth = tig_net_local_suspend_pause();
    tig_net_send_app_all(&pkt, (int)(offsetof(PacketCoopInvenLayout, entries) + (size_t)pkt.count * sizeof(pkt.entries[0])));
    tig_net_local_suspend_resume(depth);
    coop_layout_acked_hash = h;
    tig_debug_printf("CoopLite: inven: layout sent (%d items)\n", pkt.count);
}

static bool coop_lite_layout_place_ok(int64_t pc, int64_t item, int loc)
{
    int slots[960];

    if (IS_WEAR_INV_LOC(loc)) {
        return sub_465690(pc, loc) == OBJ_HANDLE_NULL && sub_464D20(item, loc, pc) == ITEM_CANNOT_OK;
    }
    if (IS_HOTKEY_INV_LOC(loc)) {
        return sub_465690(pc, loc) == OBJ_HANDLE_NULL;
    }
    item_inventory_slots_get(pc, slots);
    return item_inventory_slots_has_room_for(item, pc, loc, slots);
}

/* PT54: the legal backpack cell (10 x 12 grid) closest to `want`. */
static int coop_lite_layout_nearest_cell(int64_t pc, int64_t item, int want)
{
    int slots[960];
    int best = -1;
    int best_d = 1 << 30;
    int c;

    item_inventory_slots_get(pc, slots);
    for (c = 0; c < 120; c++) {
        int d = coop_lite_grid_cell_dist(want, c);

        if (d < best_d && item_inventory_slots_has_room_for(item, pc, c, slots)) {
            best = c;
            best_d = d;
        }
    }
    return best;
}

/* PT53b: each distinct (OID, loc, reason) refusal is logged once. */
static void coop_lite_layout_refusal_log(const uint8_t* oid24, int loc, const char* why)
{
    static unsigned int seen[128];
    static int nseen;
    unsigned int key = coop_lite_inven_layout_term(oid24, loc) ^ (unsigned int)(size_t)why;
    char oid_str[40];
    ObjectID oid;
    int i;

    for (i = 0; i < nseen; i++) {
        if (seen[i] == key) {
            return;
        }
    }
    seen[nseen++ % 128] = key;
    if (nseen > 128) {
        nseen = 128;
    }
    memcpy(&oid, oid24, sizeof(oid));
    objid_id_to_str(oid_str, oid);
    tig_debug_printf("CoopLite: inven: layout refused %s loc %d: %s\n", oid_str, loc, why);
}

void coop_lite_host_handle_inven_layout(PacketCoopInvenLayout* pkt)
{
    static uint8_t have_oid[COOP_LAYOUT_HAVE_MAX][24];
    static int64_t have_obj[COOP_LAYOUT_HAVE_MAX];
    static int have_loc[COOP_LAYOUT_HAVE_MAX];
    int plan[COOP_INVEN_LAYOUT_MAX];
    int64_t moved_item[COOP_INVEN_LAYOUT_MAX];
    int old_loc[COOP_INVEN_LAYOUT_MAX];
    int want_loc[COOP_INVEN_LAYOUT_MAX];
    int nhave = 0;
    int n = 0;
    int refused = 0;
    int slot;
    int target_slot = -1; /* PT57: >= 0 = another player's pack */
    int64_t pc;
    int cnt;
    int i;

    if (!tig_net_is_host() || !coop_lite_inven_layout_valid(pkt, tig_net_last_message_size())) {
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
    if (coop_lite_get_oid_obj(pkt->pc_oid) != pc) {
        /* PT57: a client rearranged ANOTHER player's pack it has open (trade
           / give / view). A request: the target must be a living party PC
           the sender has open or stands next to; applied here, then echoed
           to the owner and to everyone who has that pack open. */
        int64_t t = coop_lite_get_oid_obj(pkt->pc_oid);
        int k;

        for (k = 0; k < 8; k++) {
            if (k != slot && t != OBJ_HANDLE_NULL && sub_4A2B60(k) == t) {
                target_slot = k;
                break;
            }
        }
        if (target_slot < 0 || critter_is_dead(t)
            || (coop_lite_target_obj(slot) != t
                && location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION), obj_field_int64_get(t, OBJ_F_LOCATION)) > 12)) {
            tig_debug_printf("CoopLite: inven: slot %d layout for a PC it does not own / reach\n", slot);
            coop_lite_host_sync_slot_inven(slot);
            return;
        }
        pc = t;
    } else {
        coop_layout_seq_applied[slot] = pkt->seq & 0xFFFFu; /* PT54: the echo below answers this one */
    }
    /* PT53a: the PC's real list is the only source of handles (an OID
       lookup can name a stale object whose parent field says this PC but
       which the list does not hold: item_force_remove then failed and the
       re-insert duplicated it). Null / duplicate handles are repaired first. */
    coop_lite_inv_list_repair(pc);
    coop_lite_fix_inven_num(pc);
    cnt = obj_field_int32_get(pc, OBJ_F_CRITTER_INVENTORY_NUM);
    for (i = 0; i < cnt && nhave < COOP_LAYOUT_HAVE_MAX; i++) {
        int64_t item = obj_arrayfield_handle_get(pc, OBJ_F_CRITTER_INVENTORY_LIST_IDX, i);
        int64_t parent = OBJ_HANDLE_NULL;
        ObjectID oid;

        if (item == OBJ_HANDLE_NULL || !item_parent(item, &parent) || parent != pc) {
            continue; /* PT54b: gold / ammo stacks are placed too */
        }
        oid = obj_get_id(item);
        memcpy(have_oid[nhave], &oid, 24);
        have_obj[nhave] = item;
        have_loc[nhave] = item_inventory_location_get(item);
        nhave++;
    }
    coop_lite_inven_layout_plan((const uint8_t(*)[24])have_oid, have_loc, nhave, pkt->entries, pkt->count, plan);
    for (i = 0; i < pkt->count; i++) {
        int64_t item;

        if (plan[i] == COOP_LAYOUT_REFUSE) {
            refused++;
            coop_lite_layout_refusal_log(pkt->entries[i].oid, pkt->entries[i].inventory_location,
                "item not (uniquely) in host list or bad loc");
            continue;
        }
        if (plan[i] == COOP_LAYOUT_NOOP) {
            continue;
        }
        item = have_obj[plan[i]];
        if (IS_WEAR_INV_LOC(have_loc[plan[i]]) && item_check_remove(item) != ITEM_CANNOT_OK) {
            refused++; /* e.g. cursed: stays worn */
            coop_lite_layout_refusal_log(pkt->entries[i].oid, pkt->entries[i].inventory_location,
                "worn item cannot be removed");
            continue;
        }
        moved_item[n] = item;
        old_loc[n] = have_loc[plan[i]];
        want_loc[n] = pkt->entries[i].inventory_location;
        n++;
    }
    if (n > 0) {
        multiplayer_lock();
        for (i = 0; i < n; i++) {
            item_remove(moved_item[i]); /* in the list exactly once: verified above */
        }
        /* Requested place if legal, else the old one, else any room. */
        for (i = 0; i < n; i++) {
            int loc = want_loc[i];

            if (!coop_lite_layout_place_ok(pc, moved_item[i], loc) && loc >= 0 && loc < 120) {
                /* PT54: the nearest free grid cell; the echo tells the client. */
                int near_loc = coop_lite_layout_nearest_cell(pc, moved_item[i], loc);

                if (near_loc >= 0) {
                    loc = near_loc;
                }
            }
            if (!coop_lite_layout_place_ok(pc, moved_item[i], loc)) {
                ObjectID roid = obj_get_id(moved_item[i]);

                refused++;
                coop_lite_layout_refusal_log((const uint8_t*)&roid, loc, "slot not legal on host");
                loc = old_loc[i];
                if (!coop_lite_layout_place_ok(pc, moved_item[i], loc)) {
                    int free_loc = item_find_free_inv_loc_for_insertion(moved_item[i], pc);

                    if (free_loc != -1) {
                        loc = free_loc;
                    }
                }
            }
            item_insert(moved_item[i], pc, loc);
        }
        coop_lite_inv_list_repair(pc);
        coop_lite_fix_inven_num(pc);
        multiplayer_unlock();
    }
    if (n > 0 || refused > 0) {
        tig_debug_printf("CoopLite: inven: slot %d layout applied (%d moved, %d refused)\n", slot, n, refused);
        coop_lite_inv_selfcheck(pc, "layout_apply"); /* PT54d */
    }
    if (target_slot >= 0) {
        int k;

        tig_debug_printf("CoopLite: inven: PT57 slot %d rearranged slot %d's pack (%d moved, %d refused)\n",
            slot, target_slot, n, refused);
        /* Echo to the requester (with the target pack), the owner, and
           anyone else who has that pack open. */
        coop_lite_host_sync_slot_and(slot, pc);
        for (k = 1; k < 8; k++) {
            if (k != slot && sub_4A2B60(k) != OBJ_HANDLE_NULL
                && (k == target_slot || coop_lite_target_obj(k) == pc)) {
                coop_lite_host_sync_slot_inven(k);
            }
        }
        if (target_slot > 0) {
            coop_slot_inven_sig[target_slot] = coop_lite_pc_inven_sig(pc);
            coop_slot_inven_sig_valid[target_slot] = true;
        }
        if (target_slot == 0) {
            ui_update_inven(OBJ_HANDLE_NULL); /* the host's own window, if open */
        }
        return;
    }
    /* Always echo: the client takes the echo as truth and stops sending. */
    coop_lite_host_sync_slot_inven(slot);
    coop_slot_inven_sig[slot] = coop_lite_pc_inven_sig(pc);
    coop_slot_inven_sig_valid[slot] = true;
}
#else
void coop_lite_host_handle_inven_layout(PacketCoopInvenLayout* pkt)
{
    (void)pkt;
}
void coop_lite_client_layout_tick(unsigned int now_ms)
{
    (void)now_ms;
}
#endif

/* ---- PT53: language pick for bilingual co-op strings ----------------- */

const char* coop_lite_lang_pick(const char* s, bool ru)
{
    const char* sep;
    const char* p;
    bool cyr = false;

    if (s == NULL || ru) {
        return s;
    }
    sep = strchr(s, '\n');
    if (sep == NULL) {
        sep = strstr(s, " / ");
    }
    if (sep == NULL) {
        return s;
    }
    /* Only a Cyrillic first half is dropped: plain English stays whole. */
    for (p = s; p < sep; p++) {
        if ((unsigned char)*p == 0xD0 || (unsigned char)*p == 0xD1) {
            cyr = true;
            break;
        }
    }
    if (!cyr) {
        return s;
    }
    return *sep == '\n' ? sep + 1 : sep + 3;
}

bool coop_lite_lang_ru(void)
{
    static int cached = -1;

    if (cached < 0) {
        const char* v = getenv("ARCANUM_COOP_LANG");

        cached = v != NULL && (v[0] == 'r' || v[0] == 'R') && (v[1] == 'u' || v[1] == 'U') ? 1 : 0;
    }
    return cached == 1;
}

const char* coop_lite_lang(const char* s)
{
    return coop_lite_lang_pick(s, coop_lite_lang_ru());
}

void coop_lite_center_rect(int sw, int sh, int w, int h, int* x, int* y)
{
    *x = sw > w ? (sw - w) / 2 : 0;
    *y = sh > h ? (sh - h) / 2 : 0;
}

/* ---- PT61: client save / load requests, host trap visuals ---- */

bool coop_lite_save_req_valid(const PacketCoopSaveReq* pkt, int size)
{
    return pkt != NULL && pkt->type == PACKET_COOP_SAVE_REQ && size >= (int)sizeof(PacketCoopSaveReq)
        && pkt->kind >= COOP_SREQ_SAVE && pkt->kind <= COOP_SREQ_BUSY;
}

bool coop_lite_trap_fx_valid(const PacketCoopTrapFx* pkt, int size)
{
    return pkt != NULL && pkt->type == PACKET_COOP_TRAP_FX && size >= (int)sizeof(PacketCoopTrapFx)
        && pkt->count >= 0 && pkt->count <= COOP_TRAP_FX_VICTIMS && pkt->base >= 0 && pkt->base < 1000;
}

/* ---- PT63: hotbar (quick-access slots) per player ---- */
static struct {
    bool used;
    uint64_t hash;
    CoopHotbarEntry e[COOP_HOTBAR_SLOTS];
} coop_hotbar_kept[COOP_HOTBAR_KEPT_MAX];
static bool (*coop_hotbar_get_hook)(int index, CoopHotbarEntry* e);
static bool (*coop_hotbar_set_hook)(int index, const CoopHotbarEntry* e);

void coop_lite_set_hotbar_hooks(bool (*get)(int index, CoopHotbarEntry* e), bool (*set)(int index, const CoopHotbarEntry* e))
{
    coop_hotbar_get_hook = get;
    coop_hotbar_set_hook = set;
}

static bool coop_lite_hotbar_entries_ok(const CoopHotbarEntry* e)
{
    int i;

    for (i = 0; i < COOP_HOTBAR_SLOTS; i++) {
        if (e[i].type < 0 || e[i].type > 4 || e[i].data < -1 || e[i].data >= 1000) {
            return false;
        }
    }
    return true;
}

bool coop_lite_hotbar_valid(const PacketCoopHotbar* pkt, int size)
{
    return pkt != NULL && pkt->type == PACKET_COOP_HOTBAR && size >= (int)sizeof(PacketCoopHotbar)
        && (pkt->flags & ~COOP_HOTBAR_HAVE) == 0 && coop_lite_hotbar_entries_ok(pkt->e);
}

void coop_lite_hotbar_kept_reset(void)
{
    memset(coop_hotbar_kept, 0, sizeof(coop_hotbar_kept));
}

bool coop_lite_hotbar_kept_put(uint64_t hash, const CoopHotbarEntry* e)
{
    int i;
    int free_i = -1;

    if (hash == 0 || e == NULL || !coop_lite_hotbar_entries_ok(e)) {
        return false;
    }
    for (i = 0; i < COOP_HOTBAR_KEPT_MAX; i++) {
        if (coop_hotbar_kept[i].used && coop_hotbar_kept[i].hash == hash) {
            break;
        }
        if (!coop_hotbar_kept[i].used && free_i < 0) {
            free_i = i;
        }
    }
    if (i == COOP_HOTBAR_KEPT_MAX) {
        if (free_i < 0) {
            return false;
        }
        i = free_i;
    }
    coop_hotbar_kept[i].used = true;
    coop_hotbar_kept[i].hash = hash;
    memcpy(coop_hotbar_kept[i].e, e, sizeof(coop_hotbar_kept[i].e));
    return true;
}

bool coop_lite_hotbar_kept_get(uint64_t hash, CoopHotbarEntry* out)
{
    int i;

    for (i = 0; hash != 0 && i < COOP_HOTBAR_KEPT_MAX; i++) {
        if (coop_hotbar_kept[i].used && coop_hotbar_kept[i].hash == hash) {
            if (out != NULL) {
                memcpy(out, coop_hotbar_kept[i].e, sizeof(coop_hotbar_kept[i].e));
            }
            return true;
        }
    }
    return false;
}

int coop_lite_hotbar_kept_count(void)
{
    int i;
    int n = 0;

    for (i = 0; i < COOP_HOTBAR_KEPT_MAX; i++) {
        n += coop_hotbar_kept[i].used ? 1 : 0;
    }
    return n;
}

/* "CHB1", count, then count x (token hash, 10 entries). */
int coop_lite_hotbar_serialize(uint8_t* out, int cap)
{
    uint32_t magic = COOP_HOTBAR_SAVE_MAGIC;
    uint32_t count = 0;
    int off = 8;
    int i;

    if (out == NULL || cap < 8 + COOP_HOTBAR_KEPT_MAX * COOP_HOTBAR_SAVE_ENTRY) {
        return -1;
    }
    for (i = 0; i < COOP_HOTBAR_KEPT_MAX; i++) {
        if (!coop_hotbar_kept[i].used) {
            continue;
        }
        memcpy(out + off, &coop_hotbar_kept[i].hash, 8);
        memcpy(out + off + 8, coop_hotbar_kept[i].e, sizeof(coop_hotbar_kept[i].e));
        off += COOP_HOTBAR_SAVE_ENTRY;
        count++;
    }
    memcpy(out, &magic, 4);
    memcpy(out + 4, &count, 4);
    return off;
}

bool coop_lite_hotbar_deserialize(const uint8_t* in, int len)
{
    uint32_t magic;
    uint32_t count;
    uint32_t i;

    coop_lite_hotbar_kept_reset();
    if (in == NULL || len < 8) {
        return false;
    }
    memcpy(&magic, in, 4);
    memcpy(&count, in + 4, 4);
    if (magic != COOP_HOTBAR_SAVE_MAGIC || count > COOP_HOTBAR_KEPT_MAX
        || len != 8 + (int)count * COOP_HOTBAR_SAVE_ENTRY) {
        return false;
    }
    for (i = 0; i < count; i++) {
        uint64_t hash;
        CoopHotbarEntry e[COOP_HOTBAR_SLOTS];

        memcpy(&hash, in + 8 + i * COOP_HOTBAR_SAVE_ENTRY, 8);
        memcpy(e, in + 8 + i * COOP_HOTBAR_SAVE_ENTRY + 8, sizeof(e));
        if (!coop_lite_hotbar_kept_put(hash, e)) {
            coop_lite_hotbar_kept_reset();
            return false;
        }
    }
    return true;
}

void coop_lite_coop_save_desc(const char* player_name, char* out, size_t size)
{
    size_t n;
    const char* p;

    if (out == NULL || size == 0) {
        return;
    }
    snprintf(out, size, "coop-");
    n = strlen(out);
    for (p = player_name != NULL ? player_name : ""; *p != '\0' && n + 1 < size && n < 5 + 24; p++) {
        char c = *p;

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out[n++] = c;
        } else if (c == ' ' && n > 5 && out[n - 1] != '_') {
            out[n++] = '_';
        }
    }
    if (n == 5 && n + 6 < size) {
        memcpy(out + n, "player", 6);
        n += 6;
    }
    out[n] = '\0';
}

#ifndef COOP_LITE_TEST_STUBS
static unsigned int coop_sreq_client_ms; /* client: last ask */
static unsigned int coop_sreq_host_ms[8]; /* host: last accepted ask per slot */
static bool coop_sreq_save_pending; /* host: a save waits for the main loop */
static int coop_sreq_save_slot;
static char coop_sreq_save_name[16];
static char coop_sreq_save_desc[48];
static bool coop_sreq_load_active; /* host: Y / N prompt up */
static int coop_sreq_load_slot;
static char coop_sreq_load_file[TIG_MAX_PATH]; /* full save list name */
static char coop_sreq_load_label[64];

static void coop_sreq_send(int kind, const char* text, int slot)
{
    PacketCoopSaveReq pkt;
    int depth;

    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_SAVE_REQ;
    pkt.kind = kind;
    if (text != NULL) {
        snprintf(pkt.text, sizeof(pkt.text), "%s", text);
    }
    depth = tig_net_local_suspend_pause();
    if (slot < 0) {
        tig_net_send_app_all(&pkt, sizeof(pkt));
    } else {
        tig_net_send_app(&pkt, sizeof(pkt), slot);
    }
    tig_net_local_suspend_resume(depth);
}

static void coop_sreq_player_name(int slot, char* out, size_t size)
{
    const char* nm = tig_net_client_info_get_name(slot);
    int64_t pc = sub_4A2B60(slot);

    if (nm != NULL && nm[0] != '\0') {
        snprintf(out, size, "%s", nm);
    } else if (pc != OBJ_HANDLE_NULL) {
        char buf[128];

        object_examine(pc, pc, buf);
        snprintf(out, size, "%s", buf);
    } else {
        snprintf(out, size, "player%d", slot);
    }
}

/* Host: the asker's slot. Found by description; else the first free
   Slot9700..Slot9789. full (optional) gets the save list name. */
static bool coop_sreq_find_slot(const char* desc, char* slot, size_t slot_size, char* full, size_t full_size,
    bool create)
{
    GameSaveList list;
    bool used[COOP_SREQ_SLOT_LAST - COOP_SREQ_SLOT_FIRST + 1];
    unsigned int i;
    int n;
    bool found = false;

    memset(used, 0, sizeof(used));
    gamelib_savelist_create(&list);
    for (i = 0; i < list.count; i++) {
        const char* name = list.names[i];
        int num;

        if (strlen(name) < 8 || SDL_strncasecmp(name, "Slot", 4) != 0 || sscanf(name + 4, "%4d", &num) != 1
            || num < COOP_SREQ_SLOT_FIRST || num > COOP_SREQ_SLOT_LAST) {
            continue;
        }
        used[num - COOP_SREQ_SLOT_FIRST] = true;
        if (!found && SDL_strcasecmp(name + 8, desc) == 0) {
            found = true;
            snprintf(slot, slot_size, "Slot%04d", num);
            if (full != NULL) {
                snprintf(full, full_size, "%s", name);
            }
        }
    }
    gamelib_savelist_destroy(&list);
    if (found || !create) {
        return found;
    }
    for (n = 0; n <= COOP_SREQ_SLOT_LAST - COOP_SREQ_SLOT_FIRST; n++) {
        if (!used[n]) {
            snprintf(slot, slot_size, "Slot%04d", COOP_SREQ_SLOT_FIRST + n);
            return true;
        }
    }
    return false;
}

/* Host: the save list name that starts with prefix (SlotAuto). */
static bool coop_sreq_find_prefix(const char* prefix, char* full, size_t full_size)
{
    GameSaveList list;
    unsigned int i;
    bool found = false;

    gamelib_savelist_create(&list);
    for (i = 0; i < list.count; i++) {
        if (SDL_strncasecmp(list.names[i], prefix, strlen(prefix)) == 0) {
            snprintf(full, full_size, "%s", list.names[i]);
            found = true;
            break;
        }
    }
    gamelib_savelist_destroy(&list);
    return found;
}

bool coop_lite_client_load_blocked(void)
{
    return (tig_net_is_active() && !tig_net_is_host()) || coop_lite_client_in_session;
}

static bool coop_sreq_client_send(int kind)
{
    unsigned int now = coop_obj_now_ms;

    if (!tig_net_is_active() || tig_net_is_host()) {
        return false;
    }
    if (coop_sreq_client_ms != 0 && !coop_lite_interval_elapsed(coop_sreq_client_ms, now, 3000u)) {
        coop_lite_show_notice("Request already sent - wait a moment.", TF_TYPE_RED);
        return true;
    }
    coop_sreq_client_ms = now != 0 ? now : 1u;
    coop_sreq_send(kind, NULL, -1);
    tig_debug_printf("CoopLite: PT61 %s request sent to the host\n", kind == COOP_SREQ_SAVE ? "save" : "load");
    return true;
}

bool coop_lite_client_request_save(void)
{
    if (!coop_sreq_client_send(COOP_SREQ_SAVE)) {
        return false;
    }
    coop_lite_show_notice("Save request sent to the host.", TF_TYPE_WHITE);
    return true;
}

bool coop_lite_client_request_load(void)
{
    if (!tig_net_is_active() || tig_net_is_host()) {
        if (coop_lite_client_load_blocked()) {
            coop_lite_show_notice("Not allowed in co-op: loading is done by the host.", TF_TYPE_RED);
            tig_debug_printf("CoopLite: PT61 local load blocked (coop client, not connected)\n");
            return true;
        }
        return false;
    }
    coop_sreq_client_send(COOP_SREQ_LOAD);
    coop_lite_show_notice("Loading is done by the host. Request sent.", TF_TYPE_WHITE);
    tig_debug_printf("CoopLite: PT61 local load blocked (coop client) - asked the host\n");
    return true;
}

bool coop_lite_take_client_save_request(char* slot, size_t slot_size, char* desc, size_t desc_size)
{
    if (!coop_sreq_save_pending) {
        return false;
    }
    coop_sreq_save_pending = false;
    if (!tig_net_is_active() || !tig_net_is_host()) {
        return false;
    }
    snprintf(slot, slot_size, "%s", coop_sreq_save_name);
    snprintf(desc, desc_size, "%s", coop_sreq_save_desc);
    return true;
}

void coop_lite_client_save_done(bool ok)
{
    char text[160];

    tig_debug_printf("CoopLite: PT61 saved slot %d's request as %s '%s': %s\n", coop_sreq_save_slot,
        coop_sreq_save_name, coop_sreq_save_desc, ok ? "ok" : "FAILED");
    coop_sreq_send(ok ? COOP_SREQ_SAVED : COOP_SREQ_SAVE_FAILED, coop_sreq_save_desc, coop_sreq_save_slot);
    snprintf(text, sizeof(text), ok ? "Saved for a player: %s" : "Save for a player failed: %s", coop_sreq_save_desc);
    coop_lite_show_notice(text, ok ? TF_TYPE_WHITE : TF_TYPE_RED);
}

bool coop_lite_save_req_key(bool yes)
{
    int slot;

    if (!tig_net_is_active() || !tig_net_is_host() || !coop_sreq_load_active) {
        return false;
    }
    coop_sreq_load_active = false;
    coop_travel_hide();
    if (!yes) {
        coop_sreq_send(COOP_SREQ_LOAD_REFUSED, coop_sreq_load_label, coop_sreq_load_slot);
        tig_debug_printf("CoopLite: PT61 host refused slot %d load of %s\n", coop_sreq_load_slot, coop_sreq_load_file);
        return true;
    }
    coop_sreq_send(COOP_SREQ_LOAD_ACCEPTED, coop_sreq_load_label, coop_sreq_load_slot);
    tig_debug_printf("CoopLite: PT61 host accepted slot %d load of %s - party reload\n", coop_sreq_load_slot,
        coop_sreq_load_file);
    /* The party-reload path: main loop loads, then the clients resync. */
    snprintf(coop_reload_request_name, sizeof(coop_reload_request_name), "%s", coop_sreq_load_file);
    coop_reload_request_valid = true;
    coop_reload_resync_pending = true;
    for (slot = 1; slot < 8; slot++) {
        coop_reload_raise[slot] = tig_net_client_is_active(slot);
    }
    return true;
}

void coop_lite_handle_save_req(PacketCoopSaveReq* pkt)
{
    char text[256];

    if (!tig_net_is_active() || !coop_lite_save_req_valid(pkt, tig_net_last_message_size())) {
        return;
    }
    pkt->text[sizeof(pkt->text) - 1] = '\0';
    if (tig_net_is_host()) {
        int slot = sub_52A530();
        char player[64];
        char desc[48];
        char name[16];

        if ((pkt->kind != COOP_SREQ_SAVE && pkt->kind != COOP_SREQ_LOAD) || slot <= 0 || slot >= 8) {
            return;
        }
        if (coop_sreq_host_ms[slot] != 0 && !coop_lite_interval_elapsed(coop_sreq_host_ms[slot], coop_obj_now_ms, 10000u)) {
            coop_sreq_send(COOP_SREQ_BUSY, NULL, slot);
            tig_debug_printf("CoopLite: PT61 slot %d request rate-limited\n", slot);
            return;
        }
        coop_sreq_player_name(slot, player, sizeof(player));
        coop_lite_coop_save_desc(player, desc, sizeof(desc));
        if (pkt->kind == COOP_SREQ_SAVE) {
            if (coop_sreq_save_pending || coop_sreq_load_active || !map_is_valid()
                || !coop_sreq_find_slot(desc, name, sizeof(name), NULL, 0, true)) {
                coop_sreq_send(COOP_SREQ_BUSY, NULL, slot);
                return;
            }
            coop_sreq_host_ms[slot] = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1u;
            coop_sreq_save_pending = true;
            coop_sreq_save_slot = slot;
            snprintf(coop_sreq_save_name, sizeof(coop_sreq_save_name), "%s", name);
            snprintf(coop_sreq_save_desc, sizeof(coop_sreq_save_desc), "%s", desc);
            tig_debug_printf("CoopLite: PT61 slot %d (%s) asks to save -> %s '%s'\n", slot, player, name, desc);
            return;
        }
        /* load: the asker's own coop slot, else the host's quicksave */
        if (coop_sreq_load_active) {
            coop_sreq_send(COOP_SREQ_BUSY, NULL, slot);
            return;
        }
        if (coop_sreq_find_slot(desc, name, sizeof(name), coop_sreq_load_file, sizeof(coop_sreq_load_file), false)) {
            snprintf(coop_sreq_load_label, sizeof(coop_sreq_load_label), "%s", desc);
        } else if (coop_sreq_find_prefix("SlotAuto", coop_sreq_load_file, sizeof(coop_sreq_load_file))) {
            snprintf(coop_sreq_load_label, sizeof(coop_sreq_load_label), "%s", "host quicksave");
        } else {
            coop_sreq_send(COOP_SREQ_LOAD_REFUSED, "no save", slot);
            tig_debug_printf("CoopLite: PT61 slot %d load: no coop slot and no quicksave\n", slot);
            return;
        }
        coop_sreq_host_ms[slot] = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1u;
        coop_sreq_load_active = true;
        coop_sreq_load_slot = slot;
        snprintf(text, sizeof(text), "%s asks to load %s. Y = load, N = refuse.", player, coop_sreq_load_label);
        if (coop_travel_show_hook != NULL) {
            coop_travel_show_hook(text);
        } else {
            coop_lite_show_notice(text, TF_TYPE_WHITE);
        }
        tig_debug_printf("CoopLite: PT61 slot %d (%s) asks to load %s\n", slot, player, coop_sreq_load_file);
        return;
    }
    switch (pkt->kind) {
    case COOP_SREQ_SAVED:
        snprintf(text, sizeof(text), "Saved on the host as %s.", pkt->text);
        coop_lite_show_notice(text, TF_TYPE_GREEN);
        break;
    case COOP_SREQ_SAVE_FAILED:
        coop_lite_show_notice("The host could not save.", TF_TYPE_RED);
        break;
    case COOP_SREQ_LOAD_REFUSED:
        snprintf(text, sizeof(text), "The host refused the load (%s).", pkt->text[0] ? pkt->text : "refused");
        coop_lite_show_notice(text, TF_TYPE_RED);
        break;
    case COOP_SREQ_LOAD_ACCEPTED:
        snprintf(text, sizeof(text), "The host is loading %s.", pkt->text);
        coop_lite_show_notice(text, TF_TYPE_WHITE);
        break;
    case COOP_SREQ_BUSY:
        coop_lite_show_notice("The host is busy - try again in a few seconds.", TF_TYPE_RED);
        break;
    default:
        return;
    }
    coop_sreq_client_ms = 0;
    tig_debug_printf("CoopLite: PT61 host answered %d (%s)\n", pkt->kind, pkt->text);
}

void coop_lite_pt61_slot_gone(int slot)
{
    if (coop_sreq_load_active && coop_sreq_load_slot == slot) {
        coop_sreq_load_active = false;
        coop_travel_hide();
    }
    if (slot > 0 && slot < 8) {
        coop_sreq_host_ms[slot] = 0;
    }
}

void coop_lite_host_trap_fx(int64_t trap, int64_t triggerer, int base, const int64_t* victims, int count)
{
    PacketCoopTrapFx pkt;
    ObjectID oid;
    int depth;
    int i;
    bool host;

    depth = tig_net_local_suspend_pause();
    host = tig_net_is_active() && tig_net_is_host() && tig_net_is_coop_enabled();
    tig_net_local_suspend_resume(depth);
    if (!host || base < 0) {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_TRAP_FX;
    pkt.map = map_current_map();
    pkt.base = base;
    if (trap != OBJ_HANDLE_NULL) {
        oid = obj_get_id(trap);
        memcpy(pkt.trap, &oid, sizeof(oid));
    }
    if (triggerer != OBJ_HANDLE_NULL) {
        oid = obj_get_id(triggerer);
        memcpy(pkt.triggerer, &oid, sizeof(oid));
    }
    for (i = 0; i < count && pkt.count < COOP_TRAP_FX_VICTIMS; i++) {
        if (victims[i] != OBJ_HANDLE_NULL) {
            oid = obj_get_id(victims[i]);
            memcpy(pkt.victims[pkt.count++], &oid, sizeof(oid));
        }
    }
    depth = tig_net_local_suspend_pause();
    tig_net_send_app_all(&pkt, sizeof(pkt));
    tig_net_local_suspend_resume(depth);
    tig_debug_printf("CoopLite: PT61 trap fired (fx base %d, %d victims) - visuals sent to clients\n", base, pkt.count);
}

void coop_lite_handle_trap_fx(PacketCoopTrapFx* pkt)
{
    int64_t victims[COOP_TRAP_FX_VICTIMS];
    int64_t trap;
    int64_t triggerer;
    int i;

    if (!tig_net_is_active() || tig_net_is_host() || !coop_lite_trap_fx_valid(pkt, tig_net_last_message_size())) {
        return;
    }
    if (!coop_lite_client_world_ready() || pkt->map != map_current_map() || map_is_clearing_objects()) {
        return;
    }
    trap = coop_lite_get_oid_obj(pkt->trap);
    triggerer = coop_lite_get_oid_obj(pkt->triggerer);
    for (i = 0; i < pkt->count; i++) {
        victims[i] = coop_lite_get_oid_obj(pkt->victims[i]);
    }
    multiplayer_lock();
    coop_lite_apply_begin();
    trap_coop_play_fx(trap, triggerer, pkt->base, victims, pkt->count);
    coop_lite_apply_end();
    multiplayer_unlock();
    tig_debug_printf("CoopLite: PT61 trap visuals played (fx base %d, %d victims)\n", pkt->base, pkt->count);
}
/* ---- PT63: hotbar runtime ---- */
static PacketCoopHotbar coop_hotbar_last;
static bool coop_hotbar_last_valid;
static unsigned int coop_hotbar_poll_ms;
static unsigned int coop_hotbar_ready_ms;
static bool coop_hotbar_got_host;
static PacketCoopHotbar coop_hotbar_restore;
static bool coop_hotbar_restore_pending;
static unsigned int coop_hotbar_restore_ms;
static uint64_t coop_hotbar_sent_hash[8];

static int coop_lite_hotbar_used(const CoopHotbarEntry* e)
{
    int i;
    int n = 0;

    for (i = 0; i < COOP_HOTBAR_SLOTS; i++) {
        n += e[i].type != 0 ? 1 : 0;
    }
    return n;
}

static void coop_lite_hotbar_read(PacketCoopHotbar* pkt)
{
    int i;

    memset(pkt, 0, sizeof(*pkt));
    pkt->type = PACKET_COOP_HOTBAR;
    for (i = 0; i < COOP_HOTBAR_SLOTS; i++) {
        if (!coop_hotbar_get_hook(i, &pkt->e[i]) || pkt->e[i].type < 0 || pkt->e[i].type > 4) {
            memset(&pkt->e[i], 0, sizeof(pkt->e[i]));
        }
        if (pkt->e[i].data < -1 || pkt->e[i].data >= 1000) {
            pkt->e[i].data = -1;
        }
    }
}

/* Client: restore what the host kept, then send every change. */
static void coop_lite_client_hotbar_tick(unsigned int now_ms)
{
    PacketCoopHotbar cur;
    int i;

    if (coop_hotbar_get_hook == NULL || coop_hotbar_set_hook == NULL) {
        return;
    }
    if (!coop_lite_client_world_ready()) {
        coop_hotbar_ready_ms = 0;
        return;
    }
    if (coop_hotbar_ready_ms == 0) {
        coop_hotbar_ready_ms = now_ms != 0 ? now_ms : 1u;
    }
    if (coop_hotbar_restore_pending) {
        int missing = 0;

        for (i = 0; i < COOP_HOTBAR_SLOTS; i++) {
            if (!coop_hotbar_set_hook(i, &coop_hotbar_restore.e[i])) {
                missing++;
            }
        }
        if (missing == 0 || coop_lite_interval_elapsed(coop_hotbar_restore_ms, now_ms, 30000u)) {
            coop_hotbar_restore_pending = false;
            tig_debug_printf("CoopLite: PT63 hotbar restored from the host (%d slot(s) used, %d item(s) not found)\n",
                coop_lite_hotbar_used(coop_hotbar_restore.e), missing);
            coop_lite_hotbar_read(&coop_hotbar_last);
            coop_hotbar_last_valid = true;
        }
        return; /* items may still arrive with the inventory sync */
    }
    if (!coop_lite_interval_elapsed(coop_hotbar_poll_ms, now_ms, 500u)) {
        return;
    }
    coop_hotbar_poll_ms = now_ms;
    if (!coop_hotbar_got_host && !coop_lite_interval_elapsed(coop_hotbar_ready_ms, now_ms, 10000u)) {
        return; /* wait for the host's copy before overwriting it */
    }
    coop_lite_hotbar_read(&cur);
    if (coop_hotbar_last_valid && memcmp(&cur, &coop_hotbar_last, sizeof(cur)) == 0) {
        return;
    }
    coop_hotbar_last = cur;
    coop_hotbar_last_valid = true;
    tig_net_send_app(&cur, sizeof(cur), 0);
    tig_debug_printf("CoopLite: PT63 hotbar changed - sent to the host (%d slot(s) used)\n", coop_lite_hotbar_used(cur.e));
}

/* Host: each (re)joined player gets their kept hotbar once per join. */
static void coop_lite_host_hotbar_tick(void)
{
    PacketCoopHotbar pkt;
    int slot;

    for (slot = 1; slot < 8; slot++) {
        uint64_t hash = coop_slot_token_hash[slot];

        if (hash == 0) {
            coop_hotbar_sent_hash[slot] = 0;
            continue;
        }
        if (coop_hotbar_sent_hash[slot] == hash || sub_4A2B60(slot) == OBJ_HANDLE_NULL) {
            continue;
        }
        memset(&pkt, 0, sizeof(pkt));
        pkt.type = PACKET_COOP_HOTBAR;
        if (coop_lite_hotbar_kept_get(hash, pkt.e)) {
            pkt.flags = COOP_HOTBAR_HAVE;
        }
        tig_net_send_app(&pkt, sizeof(pkt), slot);
        coop_hotbar_sent_hash[slot] = hash;
        tig_debug_printf("CoopLite: PT63 hotbar -> slot %d: %s (%d slot(s) used)\n", slot,
            pkt.flags != 0 ? "kept layout" : "none kept yet", coop_lite_hotbar_used(pkt.e));
    }
}

void coop_lite_handle_hotbar(PacketCoopHotbar* pkt)
{
    if (!tig_net_is_active() || !coop_lite_hotbar_valid(pkt, tig_net_last_message_size())) {
        return;
    }
    if (tig_net_is_host()) {
        int slot = sub_52A530();

        if (slot <= 0 || slot >= 8 || !coop_lite_host_rate_ok(2, slot, 1)) {
            return;
        }
        if (coop_slot_token_hash[slot] == 0 || !coop_lite_hotbar_kept_put(coop_slot_token_hash[slot], pkt->e)) {
            tig_debug_printf("CoopLite: PT63 slot %d hotbar not kept (no player token / table full)\n", slot);
            return;
        }
        coop_hotbar_sent_hash[slot] = coop_slot_token_hash[slot]; /* theirs is newer than any kept copy */
        tig_debug_printf("CoopLite: PT63 slot %d hotbar kept (%d slot(s) used)\n", slot, coop_lite_hotbar_used(pkt->e));
        return;
    }
    coop_hotbar_got_host = true;
    if ((pkt->flags & COOP_HOTBAR_HAVE) == 0) {
        tig_debug_printf("CoopLite: PT63 host has no hotbar kept for us - ours will be sent\n");
        coop_hotbar_last_valid = false;
        return;
    }
    coop_hotbar_restore = *pkt;
    coop_hotbar_restore_pending = true;
    coop_hotbar_restore_ms = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1u;
}

/* Host load: every player gets the loaded layout again. */
static void coop_lite_hotbar_after_load(void)
{
    memset(coop_hotbar_sent_hash, 0, sizeof(coop_hotbar_sent_hash));
}

#else
void coop_lite_handle_hotbar(PacketCoopHotbar* pkt) { (void)pkt; }
bool coop_lite_client_load_blocked(void) { return false; }
bool coop_lite_client_request_save(void) { return false; }
bool coop_lite_client_request_load(void) { return false; }
bool coop_lite_take_client_save_request(char* slot, size_t slot_size, char* desc, size_t desc_size)
{
    (void)slot; (void)slot_size; (void)desc; (void)desc_size;
    return false;
}
void coop_lite_client_save_done(bool ok) { (void)ok; }
bool coop_lite_save_req_key(bool yes) { (void)yes; return false; }
void coop_lite_handle_save_req(PacketCoopSaveReq* pkt) { (void)pkt; }
void coop_lite_pt61_slot_gone(int slot) { (void)slot; }
void coop_lite_host_trap_fx(int64_t trap, int64_t triggerer, int base, const int64_t* victims, int count)
{
    (void)trap; (void)triggerer; (void)base; (void)victims; (void)count;
}
void coop_lite_handle_trap_fx(PacketCoopTrapFx* pkt) { (void)pkt; }
#endif


/* ---- PT77: chat + launcher lobby --------------------------------------- */

#ifndef COOP_LITE_TEST_STUBS

static void (*coop_chat_on_line_hook)(const char* line);
static void (*coop_chat_set_visible_hook)(bool on);
static bool coop_chat_typing;
static unsigned int coop_chat_last_ms;
static unsigned int coop_chat_seq;
static unsigned int coop_chat_last_send_ms[8];
static unsigned int coop_chat_last_visible_ms;
static bool coop_chat_log_shown;

void coop_lite_set_chat_ui_hooks(void (*on_line)(const char* line), void (*set_visible)(bool on))
{
    coop_chat_on_line_hook = on_line;
    coop_chat_set_visible_hook = set_visible;
}

void coop_lite_chat_set_typing(bool typing)
{
    coop_chat_typing = typing;
    if (typing) {
        unsigned int now = coop_obj_now_ms != 0 ? coop_obj_now_ms : 1u;

        coop_chat_last_ms = now;
        if (coop_chat_set_visible_hook != NULL) {
            coop_chat_set_visible_hook(true);
            coop_chat_log_shown = true;
            coop_chat_last_visible_ms = now;
        }
    }
}

static void coop_chat_log_activity(unsigned int now_ms)
{
    bool show;

    if (now_ms == 0) {
        now_ms = 1u;
    }
    coop_chat_last_ms = now_ms;
    show = coop_lite_chat_log_visible(coop_chat_typing, coop_chat_last_ms, now_ms, COOP_CHAT_FADE_MS);
    if (show != coop_chat_log_shown && coop_chat_set_visible_hook != NULL) {
        coop_chat_set_visible_hook(show);
        coop_chat_log_shown = show;
    }
    if (show) {
        coop_chat_last_visible_ms = now_ms;
    }
}

static void coop_chat_present(int slot, const char* name, const char* text, bool bubble)
{
    char line[COOP_CHAT_MAX_NAME + COOP_CHAT_MAX_TEXT + 96];
    char character[64];
    unsigned int now = coop_obj_now_ms != 0 ? coop_obj_now_ms : (unsigned int)(SDL_GetTicksNS() / 1000000u);
    int64_t pc;
    time_t tnow = time(NULL);
    struct tm* lt = localtime(&tnow);

    /* PT77e: "(Nick) [HH:MM] Character: text" (local time of receipt). */
    character[0] = '\0';
    pc = (slot >= 0 && slot < 8) ? sub_4A2B60(slot) : OBJ_HANDLE_NULL;
    if (pc != OBJ_HANDLE_NULL && obj_handle_is_valid(pc)) {
        char* pname = NULL;

        obj_field_string_get(pc, OBJ_F_PC_PLAYER_NAME, &pname);
        if (pname != NULL) {
            strncpy(character, pname, sizeof(character) - 1);
            character[sizeof(character) - 1] = '\0';
            FREE(pname);
            coop_lite_chat_sanitize(character, sizeof(character));
        }
    }
    coop_lite_chat_format_full(name, lt != NULL ? lt->tm_hour : 0, lt != NULL ? lt->tm_min : 0,
        character, text, line, sizeof(line));
    tig_debug_printf("[CHAT] %s\n", line);
    if (coop_chat_on_line_hook != NULL) {
        coop_chat_on_line_hook(line);
    }
    coop_chat_log_activity(now);

    if (!bubble) {
        return;
    }
    if (pc == OBJ_HANDLE_NULL || !obj_handle_is_valid(pc)) {
        return;
    }
    if ((obj_field_int32_get(pc, OBJ_F_FLAGS) & (OF_DESTROYED | OF_OFF)) != 0) {
        return;
    }
    dialog_ui_float_line(pc, OBJ_HANDLE_NULL, text, -1);
}

static void coop_chat_fill_pkt(PacketCoopChat* pkt, int slot, const char* name, const char* text)
{
    memset(pkt, 0, sizeof(*pkt));
    pkt->type = PACKET_COOP_CHAT;
    pkt->slot = slot;
    pkt->seq = ++coop_chat_seq;
    if (name != NULL) {
        strncpy(pkt->name, name, COOP_CHAT_MAX_NAME - 1);
    }
    if (text != NULL) {
        strncpy(pkt->text, text, COOP_CHAT_MAX_TEXT - 1);
    }
    pkt->name[COOP_CHAT_MAX_NAME - 1] = '\0';
    pkt->text[COOP_CHAT_MAX_TEXT - 1] = '\0';
    coop_lite_chat_sanitize(pkt->name, sizeof(pkt->name));
    coop_lite_chat_sanitize(pkt->text, sizeof(pkt->text));
}

static const char* coop_chat_local_name(void)
{
    const char* n = tig_net_client_info_get_name(sub_529520());

    if (n == NULL || n[0] == '\0') {
        n = getenv("ARCANUM_COOP_NAME");
    }
    if (n == NULL || n[0] == '\0') {
        n = "Player";
    }
    return n;
}

static void coop_chat_net_send(const char* text, bool present_local, bool bubble_local)
{
    PacketCoopChat pkt;
    char clean[COOP_CHAT_MAX_TEXT];
    int local_slot;
    unsigned int now;

    if (!coop_lite_chat_prepare(text, clean, sizeof(clean))) {
        return;
    }
    if (!tig_net_is_active() || !tig_net_is_coop_enabled()) {
        if (present_local) {
            coop_chat_present(sub_529520(), coop_chat_local_name(), clean, bubble_local);
        }
        return;
    }
    local_slot = sub_529520();
    now = coop_obj_now_ms != 0 ? coop_obj_now_ms : (unsigned int)(SDL_GetTicksNS() / 1000000u);
    if (local_slot >= 0 && local_slot < 8) {
        if (coop_chat_last_send_ms[local_slot] != 0
            && !coop_lite_interval_elapsed(coop_chat_last_send_ms[local_slot], now, COOP_CHAT_RATE_MS)) {
            return;
        }
        coop_chat_last_send_ms[local_slot] = now != 0 ? now : 1u;
    }
    coop_chat_fill_pkt(&pkt, local_slot, coop_chat_local_name(), clean);
    if (present_local) {
        coop_chat_present(local_slot, pkt.name, pkt.text, bubble_local);
    }
    if (tig_net_is_host()) {
        tig_net_send_app_all(&pkt, sizeof(pkt));
    } else {
        tig_net_send_app(&pkt, sizeof(pkt), 0);
    }
}

void coop_lite_chat_on_local_broadcast(const char* text)
{
    /* Bubble already shown by broadcast_msg_client; only log + relay. */
    coop_chat_net_send(text, true, false);
}

void coop_lite_chat_submit(const char* text)
{
    coop_chat_net_send(text, true, true);
}

void coop_lite_handle_chat(PacketCoopChat* pkt)
{
    PacketCoopChat out;
    int sender;
    unsigned int now;
    const char* name;
    char clean[COOP_CHAT_MAX_TEXT];

    if (!tig_net_is_active() || pkt == NULL || pkt->type != PACKET_COOP_CHAT) {
        return;
    }
    now = coop_obj_now_ms != 0 ? coop_obj_now_ms : (unsigned int)(SDL_GetTicksNS() / 1000000u);

    if (tig_net_is_host()) {
        sender = sub_52A530();
        if (sender <= 0 || sender >= 8) {
            return; /* host chat is sent locally in coop_chat_net_send */
        }
        if (tig_net_last_message_size() < (int)sizeof(PacketCoopChat)) {
            return;
        }
        if (!coop_lite_chat_prepare(pkt->text, clean, sizeof(clean))) {
            return;
        }
        if (coop_chat_last_send_ms[sender] != 0
            && !coop_lite_interval_elapsed(coop_chat_last_send_ms[sender], now, COOP_CHAT_RATE_MS)) {
            return;
        }
        coop_chat_last_send_ms[sender] = now != 0 ? now : 1u;
        name = tig_net_client_info_get_name(sender);
        if (name == NULL || name[0] == '\0') {
            name = pkt->name[0] != '\0' ? pkt->name : "Player";
        }
        coop_chat_fill_pkt(&out, sender, name, clean);
        coop_chat_present(out.slot, out.name, out.text, true);
        tig_net_send_app_except(&out, sizeof(out), sender);
        return;
    }

    /* Client receives host relay. */
    if (!coop_lite_chat_valid(pkt, tig_net_last_message_size())) {
        return;
    }
    if (pkt->slot == sub_529520()) {
        coop_chat_log_activity(now);
        return;
    }
    coop_chat_present(pkt->slot, pkt->name, pkt->text, true);
}

void coop_lite_lobby_tick(unsigned int now_ms)
{
    bool show;
    char path[260];
    FILE* f;
    static unsigned int lobby_players_ms;
    static unsigned int lobby_in_ms;
    int slot;

    coop_paths_ensure_dir();
    show = coop_lite_chat_log_visible(coop_chat_typing, coop_chat_last_ms, now_ms, COOP_CHAT_FADE_MS);
    if (show != coop_chat_log_shown && coop_chat_set_visible_hook != NULL) {
        coop_chat_set_visible_hook(show);
        coop_chat_log_shown = show;
    }

    if (!coop_paths_join(path, sizeof(path), "lobby_chat_in.txt")) {
        return;
    }
    if (coop_lite_interval_elapsed(lobby_in_ms, now_ms, 250u)) {
        lobby_in_ms = now_ms != 0 ? now_ms : 1u;
        f = fopen(path, "rb");
        if (f != NULL) {
            char buf[512];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);

            fclose(f);
            remove(path);
            if (n > 0) {
                char* line = buf;
                char* nl;

                buf[n] = '\0';
                while (line != NULL && *line != '\0') {
                    nl = strchr(line, '\n');
                    if (nl != NULL) {
                        *nl = '\0';
                        if (nl > line && nl[-1] == '\r') {
                            nl[-1] = '\0';
                        }
                    }
                    while (*line == ' ' || *line == '\t') {
                        line++;
                    }
                    if (strncmp(line, "CHAT\t", 5) == 0) {
                        coop_lite_chat_submit(line + 5);
                    } else if (line[0] != '\0') {
                        coop_lite_chat_submit(line);
                    }
                    line = nl != NULL ? nl + 1 : NULL;
                }
            }
        }
    }

    if (!coop_lite_interval_elapsed(lobby_players_ms, now_ms, 1000u)) {
        return;
    }
    lobby_players_ms = now_ms != 0 ? now_ms : 1u;
    if (!coop_paths_join(path, sizeof(path), "lobby_players.txt")) {
        return;
    }
    f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    for (slot = 0; slot < 8; slot++) {
        const char* name;
        const char* role = (slot == 0) ? "host" : "client";

        if (!tig_net_client_is_active(slot)) {
            continue;
        }
        name = tig_net_client_info_get_name(slot);
        if (name == NULL || name[0] == '\0') {
            name = (slot == sub_529520()) ? coop_chat_local_name() : "Player";
        }
        fprintf(f, "%d\t%s\t%s\t1\n", slot, name, role);
    }
    fclose(f);
}

#else /* COOP_LITE_TEST_STUBS */

void coop_lite_set_chat_ui_hooks(void (*on_line)(const char* line), void (*set_visible)(bool on))
{
    (void)on_line;
    (void)set_visible;
}
void coop_lite_chat_set_typing(bool typing) { (void)typing; }
void coop_lite_chat_on_local_broadcast(const char* text) { (void)text; }
void coop_lite_chat_submit(const char* text) { (void)text; }
void coop_lite_handle_chat(PacketCoopChat* pkt) { (void)pkt; }
void coop_lite_lobby_tick(unsigned int now_ms) { (void)now_ms; }

#endif /* COOP_LITE_TEST_STUBS */
