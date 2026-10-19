#ifndef ARCANUM_GAME_COOP_LITE_H_
#define ARCANUM_GAME_COOP_LITE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Packet type for host→client forced follow teleport (within 1..130). */
#define PACKET_COOP_FOLLOW_HOST 129

typedef struct PacketCoopFollowHost {
    /* 0000 */ int type;
    /* 0004 */ int map;
    /* 0008 */ int64_t loc;
} PacketCoopFollowHost;

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
 * Merchants and NPCs whose dialog script has no global/quest/story side
 * effects are ALLOW; story/quest-givers are INFO_ONLY (float, no flag changes).
 */
CoopTalkPolicy coop_lite_can_talk(int64_t npc_obj, int64_t pc_obj);

/*
 * Pure classifier used by coop_lite_can_talk and unit tests.
 * is_host / !coop → ALLOW; merchant → ALLOW; story script → INFO_ONLY; else ALLOW.
 */
CoopTalkPolicy coop_lite_classify_talk(bool coop_active, bool is_host,
    bool is_merchant, bool dialog_has_story_effects);

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

/*
 * Export the local PC to Players/<oid>.mpc (client character file). No-op when
 * coop is off or there is no local PC. Does not write the campaign save.
 */
bool coop_lite_export_local_character(void);

#ifdef __cplusplus
}
#endif

#endif /* ARCANUM_GAME_COOP_LITE_H_ */
