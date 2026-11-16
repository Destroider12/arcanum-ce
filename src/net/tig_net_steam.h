#ifndef ARCANUM_NET_TIG_NET_STEAM_H_
#define ARCANUM_NET_TIG_NET_STEAM_H_

/*
 * PT58: the handful of Steamworks flat-API entry points the Steam transport
 * uses, declared here (no Steamworks SDK headers in the repo). Layouts follow
 * the public flat API (steam_api_flat.h / steamnetworkingtypes.h, pack 8,
 * 64-bit); the static asserts in tig_net_steam.c pin them. On Windows they
 * come from steam_api64.dll at runtime; tests inject a fake table.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TIG_STEAM_IDENTITY_STEAMID 16
#define TIG_STEAM_SEND_UNRELIABLE 0
#define TIG_STEAM_SEND_NO_NAGLE 1
#define TIG_STEAM_SEND_RELIABLE 8
#define TIG_STEAM_SEND_AUTO_RESTART 32
#define TIG_STEAM_CB_SESSION_REQUEST 1251
#define TIG_STEAM_CB_SESSION_FAILED 1252
#define TIG_STEAM_DEFAULT_APPID "500810" /* Arcanum on Steam */

typedef struct TigSteamIdentity {
    int type; /* ESteamNetworkingIdentityType */
    int cb_size;
    union {
        uint64_t steam_id64;
        char raw[128];
    } u;
} TigSteamIdentity;

typedef struct TigSteamMessage {
    void* data;
    int size;
    uint32_t conn;
    TigSteamIdentity peer;
    int64_t conn_user_data;
    int64_t usec_received;
    int64_t message_number;
    void (*free_data)(struct TigSteamMessage* msg);
    void (*release)(struct TigSteamMessage* msg);
    int channel;
    int flags;
    int64_t user_data;
    uint16_t lane;
    uint16_t pad;
} TigSteamMessage;

typedef struct TigSteamCallbackMsg {
    int32_t user;
    int callback;
    uint8_t* param;
    int param_size;
} TigSteamCallbackMsg;

typedef struct TigSteamApi {
    /* Returns true and fills err ("" on success) - wraps InitFlat / Init. */
    bool (*init)(char* err, int err_size);
    void (*shutdown)(void);
    void (*manual_dispatch_init)(void);
    int32_t (*get_pipe)(void);
    void (*run_frame)(int32_t pipe);
    bool (*next_callback)(int32_t pipe, TigSteamCallbackMsg* out);
    void (*free_last_callback)(int32_t pipe);
    void* (*messages)(void);
    int (*send_to_user)(void* self, const TigSteamIdentity* id, const void* data, uint32_t size, int flags, int channel);
    int (*receive_on_channel)(void* self, int channel, TigSteamMessage** out, int max);
    bool (*accept_session)(void* self, const TigSteamIdentity* id);
    bool (*close_session)(void* self, const TigSteamIdentity* id);
    void (*release_message)(TigSteamMessage* msg);
    uint64_t (*local_steam_id)(void);
    void (*init_relay)(void); /* optional */
    unsigned int (*now_ms)(void); /* optional (tests) */
    void (*sleep_ms)(unsigned int ms); /* optional */
} TigSteamApi;

/* Tests: use this table instead of loading steam_api64.dll (NULL = load). */
void tig_net_steam_set_api(const TigSteamApi* api);
/* Tests: forget all state (as if the process restarted). */
void tig_net_steam_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif
