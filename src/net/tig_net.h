#ifndef ARCANUM_NET_TIG_NET_H_
#define ARCANUM_NET_TIG_NET_H_

#include <stddef.h>
#include <stdbool.h>

#ifdef TIG_NET_TEST_STUB_DEBUG
/* Headless test binary: avoid pulling SDL via tig/types.h. */
#ifndef TIG_OK
#define TIG_OK 0
#define TIG_ERR_GENERIC 16
#endif
#else
#include "tig/types.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define TIG_NET_SERVER_PLAYER_KILLING 0x0001
#define TIG_NET_SERVER_FRIENDLY_FIRE 0x0002
#define TIG_NET_SERVER_AUTO_EQUIP 0x0020
#define TIG_NET_SERVER_KEY_SHARING 0x0040

#define TIG_NET_MAX_PLAYERS 8
#define TIG_NET_DEFAULT_PORT 7777
#define TIG_NET_MAX_NAME 32
/* PT20 (review): a full world-state chunk is 0x10 + 64 * 0x40 = 4112 bytes and
   an object blob carries up to 8 KB; ENet fragments larger packets. */
#define TIG_NET_MAX_PACKET 16384
/* Co-op wire protocol revision, exchanged in WELCOME / NAME. Bump it when a
   packet layout changes: mismatched builds refuse each other cleanly. */
#define TIG_NET_PROTOCOL 29

typedef enum TigNetEvent {
    TIG_NET_EVENT_CONNECT = 1,
    TIG_NET_EVENT_DISCONNECT = 2,
} TigNetEvent;

typedef void (*TigNetMessageFunc)(void* msg);
typedef bool (*TigNetMessageValidationFunc)(void* msg);
typedef bool (*TigNetNetworkEventFunc)(int type, int client_id, void* data, int size);

/* --- lifecycle / mode (coop-gated; inactive unless coop enabled) --- */
void tig_net_set_coop_enabled(bool enabled);
bool tig_net_is_coop_enabled(void);
bool tig_net_describe_coop_mode(char* buf, size_t size); /* PT37: main menu status line */
/* -join host[:port] was given (client can reconnect). */
bool tig_net_join_configured(void);

void tig_net_configure_host(int port);
void tig_net_configure_join(const char* host, int port);
/* Client: the host was refused for a protocol mismatch (no rejoin). */
bool tig_net_host_refused(void);

int tig_net_start_client(void);
void tig_net_start_server(void);
int tig_net_connect_configured(void);
void tig_net_reset_connection(void);
int sub_5280F0(void);

void tig_net_pump(void);

bool tig_net_is_active(void);
bool tig_net_is_host(void);
/* PT33: a coop client, whether or not its calls are locally suspended. */
bool tig_net_is_coop_client(void);

void tig_net_on_message(TigNetMessageFunc func);
void tig_net_on_message_validation(TigNetMessageValidationFunc func);
void tig_net_on_network_event(TigNetNetworkEventFunc func);

void tig_net_send_app_all(void* data, int size);
void tig_net_send_app(void* data, int size, int client_id);
void tig_net_send_app_except(void* data, int size, int client_id);

int tig_net_local_client_set_name(const char* name);
int tig_net_local_server_set_max_players(int max_players);
int tig_net_local_server_set_description(const char* description);
void tig_net_local_server_set_name(const char* name);
int tig_net_local_server_get_max_players(void);
unsigned int tig_net_local_server_get_options(void);
void tig_net_local_server_set_options(unsigned int options);

bool tig_net_client_is_active(int client_id);
/* Unreliable+sequenced pose traffic on its own channel (review M7). */
void tig_net_send_pose_all(void* data, int size);
void tig_net_send_pose_except(void* data, int size, int client_id);
/* Client: mark another client's slot as present (host roster, review H3). */
void tig_net_set_remote_slot_present(int slot, bool present);
bool tig_net_client_is_waiting(int client_id);
bool tig_net_client_is_loading(int client_id);
const char* tig_net_client_info_get_name(int client_id);

/* File xfer not implemented yet — safe no-ops for CE call sites. */
int tig_net_xfer_count(int client_id);
void tig_net_xfer_send_as(const char* src, const char* dst, int client_id, void* unused);
void tig_net_xfer_send(const char* path, int client_id, void* unused);

/* Opaque Troika helpers used by multiplayer.c / timeevent.c */
int sub_529520(void); /* local client slot */
int sub_52A530(void); /* last message sender slot (local slot outside dispatch) */
int tig_net_last_message_size(void); /* size of the app message being dispatched */
bool tig_net_welcome_slot_valid(int slot);

/*
 * Coop: run a block of code as if single-player (tig_net_is_active() and
 * tig_net_is_host() return false, sends are suppressed). Used for the
 * client driving its own PC's movement/talk locally. Nestable.
 */
void tig_net_local_suspend_begin(void);
void tig_net_local_suspend_end(void);
bool tig_net_is_locally_suspended(void);
/* Leave local suspension for one network request (returns the depth). */
int tig_net_local_suspend_pause(void);
void tig_net_local_suspend_resume(int depth);
int sub_52A900(void); /* non-zero blocks time advance */
void sub_52A940(void);
void sub_52A950(void);
void sub_52B210(void);
void sub_5286E0(void);
void sub_52A9E0(int client_id);

#ifdef __cplusplus
}
#endif

#endif /* ARCANUM_NET_TIG_NET_H_ */
