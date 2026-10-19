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

/* True when world map may be opened/used: always in SP; host-only in coop. */
bool coop_lite_can_use_worldmap(void);

/* True when the local peer may drive game-time (SP or coop host). */
bool coop_lite_can_advance_game_time(void);

/*
 * Host: after the local PC finishes a teleport/area change, broadcast follow
 * so every remote player is relocated to the same map/location.
 * No-op on clients, when coop is off, or while applying a follow locally.
 */
void coop_lite_notify_host_teleported(int map, int64_t loc);

/* Dispatch helper for PACKET_COOP_FOLLOW_HOST (called from multiplayer_handle_message). */
void coop_lite_handle_follow_packet(PacketCoopFollowHost* pkt);

#ifdef __cplusplus
}
#endif

#endif /* ARCANUM_GAME_COOP_LITE_H_ */
