#ifndef ARCANUM_GAME_COOP_LITE_H_
#define ARCANUM_GAME_COOP_LITE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Packet type for host→client forced follow teleport (within 1..130). */
#define PACKET_COOP_ATTACK_INTENT 128
#define PACKET_COOP_FOLLOW_HOST 129
#define PACKET_COOP_CHAR_OFFER 130
#define PACKET_COOP_SESSION_SYNC 131
#define PACKET_COOP_PC_POS 132

/* multiplayer_validate_message must allow up to this (exclusive). */
#define PACKET_COOP_TYPE_LIMIT 133

/* PC position broadcast interval (ms). */
#define COOP_LITE_PC_POS_INTERVAL_MS 50u

/* PacketCoopPcPos.flags */
#define COOP_PC_POS_FORCE 0x1 /* host moved an owner-driven PC; owner must apply */

/* Upper bound for a plausible basic prototype number on the wire. */
#define COOP_LITE_MAX_PROTO 100000


typedef struct PacketCoopAttackIntent {
    /* 0000 */ int type;
    /* 0004 */ int attack_kind; /* 0 = default melee/ranged AG_ATTACK */
    /* 0008 */ unsigned char attacker_oid[24]; /* ObjectID */
    /* 0020 */ unsigned char target_oid[24]; /* ObjectID */
} PacketCoopAttackIntent;

typedef struct PacketCoopFollowHost {
    /* 0000 */ int type;
    /* 0004 */ int map;
    /* 0008 */ int64_t loc;
} PacketCoopFollowHost;

/* Client → host: character chosen in menu (do not start an independent world). */
typedef struct PacketCoopCharOffer {
    /* 0000 */ int type;
    /* 0004 */ int slot;
    /* 0008 */ int basic_prototype;
    /* 000C */ char name[64];
    /* 004C */ int has_oid;
    /* 0050 */ unsigned char pc_oid[24]; /* client's own PC ObjectID — host adopts it */
} PacketCoopCharOffer;

/*
 * PC position/art (rate limited, only on change).
 *  client → host: the client's own PC (client drives its own movement).
 *  host → clients: every slot PC; a slot's own PC is not echoed back to its
 *  owner unless the host moved it (COOP_PC_POS_FORCE).
 */
typedef struct PacketCoopPcPos {
    /* 0000 */ int type;
    /* 0004 */ int slot;
    /* 0008 */ unsigned char oid[24];
    /* 0020 */ int64_t loc;
    /* 0028 */ int offset_x;
    /* 002C */ int offset_y;
    /* 0030 */ unsigned int art_id;
    /* 0034 */ int flags;
} PacketCoopPcPos;

/*
 * Host → client: join the host's session. Client loads map, creates both PCs
 * with matching OIDs (host PC as NETWORK proxy, local PC as player_pc_obj).
 */
typedef struct PacketCoopSessionSync {
    /* 0000 */ int type;
    /* 0004 */ int map;
    /* 0008 */ int64_t host_loc;
    /* 0010 */ int64_t client_loc;
    /* 0018 */ unsigned char host_pc_oid[24];
    /* 0030 */ unsigned char client_pc_oid[24];
    /* 0048 */ int client_slot;
    /* 004C */ int client_proto;
    /* 0050 */ int host_proto;
    /* 0054 */ char client_name[64];
    /* 0094 */ char host_name[64];
} PacketCoopSessionSync;

/* Wire layout must match across compilers (MSVC vs gcc/clang). */
#if defined(__cplusplus)
#define COOP_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define COOP_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
COOP_STATIC_ASSERT(sizeof(PacketCoopAttackIntent) == 0x38, "PacketCoopAttackIntent size");
COOP_STATIC_ASSERT(offsetof(PacketCoopAttackIntent, target_oid) == 0x20, "PacketCoopAttackIntent layout");
COOP_STATIC_ASSERT(sizeof(PacketCoopFollowHost) == 0x10, "PacketCoopFollowHost size");
COOP_STATIC_ASSERT(offsetof(PacketCoopFollowHost, loc) == 0x08, "PacketCoopFollowHost layout");
COOP_STATIC_ASSERT(sizeof(PacketCoopCharOffer) == 0x68, "PacketCoopCharOffer size");
COOP_STATIC_ASSERT(offsetof(PacketCoopCharOffer, has_oid) == 0x4C, "PacketCoopCharOffer layout");
COOP_STATIC_ASSERT(offsetof(PacketCoopCharOffer, pc_oid) == 0x50, "PacketCoopCharOffer layout");
COOP_STATIC_ASSERT(sizeof(PacketCoopPcPos) == 0x38, "PacketCoopPcPos size");
COOP_STATIC_ASSERT(offsetof(PacketCoopPcPos, loc) == 0x20, "PacketCoopPcPos layout");
COOP_STATIC_ASSERT(offsetof(PacketCoopPcPos, flags) == 0x34, "PacketCoopPcPos layout");
COOP_STATIC_ASSERT(sizeof(PacketCoopSessionSync) == 0xD8, "PacketCoopSessionSync size");
COOP_STATIC_ASSERT(offsetof(PacketCoopSessionSync, host_loc) == 0x08, "PacketCoopSessionSync layout");
COOP_STATIC_ASSERT(offsetof(PacketCoopSessionSync, client_slot) == 0x48, "PacketCoopSessionSync layout");
COOP_STATIC_ASSERT(offsetof(PacketCoopSessionSync, host_name) == 0x94, "PacketCoopSessionSync layout");

typedef enum CoopTalkPolicy {
    COOP_TALK_ALLOW = 0,
    COOP_TALK_INFO_ONLY = 1,
    COOP_TALK_BLOCK = 2,
} CoopTalkPolicy;

/* True when world map may be opened/used: always in SP; host-only in coop. */
bool coop_lite_can_use_worldmap(void);

/* True when the local peer may drive game-time (SP or coop host). */
bool coop_lite_can_advance_game_time(void);

/* True when the local peer may write the campaign/world save (SP or host). */
bool coop_lite_can_save_world(void);

/*
 * Dialog gate for coop companions. Host / SP always ALLOW.
 * Clients always ALLOW entry; response nodes that change quest/story/gvars/
 * reputation are filtered via coop_lite_dialog_option_allowed.
 */
CoopTalkPolicy coop_lite_can_talk(int64_t npc_obj, int64_t pc_obj);

/*
 * Pure classifier used by coop_lite_can_talk and unit tests.
 * Clients always ALLOW (per-node filter elsewhere). INFO_ONLY unused.
 */
CoopTalkPolicy coop_lite_classify_talk(bool coop_active, bool is_host,
    bool is_merchant, bool dialog_has_story_effects);

/* True if non-host may pick this response (barter / harmless actions). */
bool coop_lite_dialog_option_allowed(const char* actions, int response_code);
/* True on a coop client, including while running local SP paths under
   tig_net_local_suspend (net reads inactive there). */
bool coop_lite_is_client_role(void);
/* Dialog action code (2 chars) a client must not execute. */
bool coop_lite_dialog_action_blocked(const char* code);
/* Client may not open dialog with another player's follower. */
bool coop_lite_client_may_open_dialog(int64_t npc_obj, int64_t pc_obj);

/* Story scripts that resolve SFO_PLAYER / "the PC" use the host PC in coop. */
int64_t coop_lite_story_pc_obj(void);

/*
 * Preferred per-slot tile offset from host (dx, dy in tiles). Slot 0 is (0,0).
 * Exposed for tests; follow apply uses nearest free walkable near this bias.
 */
void coop_lite_slot_preferred_offset(int slot, int* dx_ptr, int* dy_ptr);

/* Nearest free walkable loc near host_loc for this client slot (host_loc if none). */
int64_t coop_lite_follow_loc_for_slot(int64_t host_loc, int slot);

/*
 * Host: after the local PC finishes a teleport/area change, broadcast follow
 * so every remote player is relocated near the host (per-slot walkable offset).
 */
void coop_lite_notify_host_teleported(int map, int64_t loc);

/* Dispatch helper for PACKET_COOP_FOLLOW_HOST (from multiplayer_handle_message). */
void coop_lite_handle_follow_packet(PacketCoopFollowHost* pkt);

void coop_lite_handle_attack_intent(PacketCoopAttackIntent* pkt);

/* Reset follow arming / queued follow (call on disconnect / return to main menu). */
void coop_lite_session_reset(void);

/* Flush a follow packet queued while the client was still in the main menu. */
void coop_lite_flush_pending_follow(void);

/* Coop always uses real-time combat — block turn-based toggle. */
bool coop_lite_can_use_turn_based(void);

/* Host: mirror companion aggression onto host PC (party). */
void coop_lite_propagate_party_aggression(int64_t attacker_obj, int64_t target_obj);

/*
 * Export the local PC to Players/<oid>.mpc (client character file). No-op when
 * coop is off or there is no local PC. Does not write the campaign save.
 */
bool coop_lite_export_local_character(void);

/* Packet routing roles for anim/combat/magictech receive (docs + tests). */
typedef enum CoopMpPacketRole {
    COOP_MP_PKT_UNKNOWN = 0,
    COOP_MP_PKT_HOST_RESOLVE = 1, /* client intent → host applies/resolves */
    COOP_MP_PKT_CLIENT_APPLY = 2, /* host broadcast → clients apply */
    COOP_MP_PKT_BOTH = 3,
} CoopMpPacketRole;

CoopMpPacketRole coop_lite_mp_packet_role(int type);

/*
 * Network hardening (pure; used by multiplayer_validate_message and tests).
 * coop_lite_packet_allowed: may a packet of this type be accepted by this
 * receiver from sender_slot? (Host drops host→client-only types from clients,
 * clients drop client→host-only types and anything not from the host.)
 */
bool coop_lite_packet_allowed(int type, bool receiver_is_host, int sender_slot);

/* Minimum wire size for coop packet types (0 if not a coop type). */
size_t coop_lite_coop_packet_min_size(int type);

/* Basic sanity for a basic_prototype number received from the network. */
bool coop_lite_pc_proto_plausible(int proto);

/* Force NUL termination of a fixed-size network string. */
void coop_lite_terminate_name(char* name, size_t size);

/*
 * Client-side: true when obj is our own PC and the session is synced, i.e.
 * movement/talk for it run locally (single-player code path) and the result
 * is replicated with PC_POS.
 */
bool coop_lite_client_drives_obj(int64_t obj);

/* Host: a client disconnected — clear its offer / replication state. */
void coop_lite_host_on_client_disconnect(int slot);

/* Host: is obj the PC bound to the sender of the message being handled? */
bool coop_lite_sender_owns_obj(int64_t obj);

/* True if two PC_POS samples differ (loc/offset/art). Pure; used by tests. */
bool coop_lite_pc_pos_changed(const PacketCoopPcPos* prev, const PacketCoopPcPos* cur);

/* True if at least interval_ms elapsed (wrap-safe). Pure; used by tests. */
bool coop_lite_interval_elapsed(unsigned int last_ms, unsigned int now_ms, unsigned int interval_ms);

/* Per-frame coop tick (host: PC_POS broadcast). now_ms = monotonic ms. */
void coop_lite_ping(unsigned int now_ms);

/* Client: apply host PC_POS. */
void coop_lite_handle_pc_pos(PacketCoopPcPos* pkt);

/* Client: local PC teleport finished — complete a pending SESSION_SYNC. */
void coop_lite_client_on_teleport_done(void);

/* Test helper: host has an unconsumed CHAR_OFFER for this slot. */
bool coop_lite_test_pending_offer(int slot);


/* Client: send CHAR_OFFER from local premade/created PC; return true if sent. */
bool coop_lite_client_offer_character(int basic_prototype, const char* name);

/* True while a coop client is waiting for SESSION_SYNC (no independent New Game). */
bool coop_lite_client_waiting_for_sync(void);

/* True after SESSION_SYNC applied — mainmenu may close into the shared world. */
bool coop_lite_client_session_ready(void);

/* Consume the one-shot "menu may close" signal after SESSION_SYNC. */
void coop_lite_acknowledge_session_ready(void);

/* Host: remember offer / spawn + SESSION_SYNC when in-world. */
void coop_lite_handle_char_offer(PacketCoopCharOffer* pkt);

/* Client: apply SESSION_SYNC (load host map, bind PCs). */
void coop_lite_handle_session_sync(PacketCoopSessionSync* pkt);

/* Host: after entering world, bind slot 0 and sync any pending client offers. */
void coop_lite_host_on_entered_world(void);

/* True if this peer should skip the single-player New Game start-map teleport. */
bool coop_lite_should_skip_independent_new_game(void);


#ifdef __cplusplus
}
#endif

/*
 * Camera dead zone follow (pure). dx/dy: pixels the view origin would have
 * to move to centre the PC (location_calc_dist_from_screen_center). Returns
 * true and the scroll needed to bring the PC back to the dead-zone edge when
 * it is outside the zone.
 */
#define COOP_LITE_FOLLOW_DEADZONE_X 200
#define COOP_LITE_FOLLOW_DEADZONE_Y 120
bool coop_lite_follow_delta(int64_t dx, int64_t dy, int64_t zone_x, int64_t zone_y,
    int64_t* out_x, int64_t* out_y);

#endif /* ARCANUM_GAME_COOP_LITE_H_ */
