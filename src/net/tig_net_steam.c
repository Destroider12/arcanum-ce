/*
 * PT58: Steam transport for tig_net over ISteamNetworkingMessages (P2P with
 * Valve's SDR relays, NAT traversal by Steam). Peers are SteamID64s.
 *
 * Loaded at runtime: steam_api64.dll is never linked, so GOG / non-Steam
 * players are unaffected; without the DLL or a running Steam client the
 * backend's init fails with one log line and tig_net reports the error.
 *
 * Connection model (ISteamNetworkingMessages is connectionless): a small
 * control protocol on channel 7 - CONNECT (client, resent every 500 ms),
 * ACCEPT / FULL (host), BYE, PING (1/s). A peer silent for its timeout
 * (default 10 s) is disconnected. App channels 0..2 map 1:1; reliable sends
 * are ordered per channel like ENet's.
 */
#include "net/tig_net_steam.h"
#include "net/tig_net_transport.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#ifdef TIG_NET_TEST_STUB_DEBUG
void tig_debug_printf(const char* fmt, ...);
#else
#include "tig/debug.h"
#endif

#if UINTPTR_MAX > 0xFFFFFFFFu
/* Flat-API layouts (pack 8, 64-bit). */
_Static_assert(sizeof(TigSteamIdentity) == 136, "SteamNetworkingIdentity layout");
_Static_assert(offsetof(TigSteamMessage, peer) == 16, "SteamNetworkingMessage_t.m_identityPeer");
_Static_assert(offsetof(TigSteamMessage, channel) == 192, "SteamNetworkingMessage_t.m_nChannel");
_Static_assert(offsetof(TigSteamCallbackMsg, param) == 8, "CallbackMsg_t.m_pubParam");
#endif

#define TNS_CTRL_CHANNEL 7
#define TNS_MAGIC 0x4E435241u /* "ARCN" */
#define TNS_K_CONNECT 1
#define TNS_K_ACCEPT 2
#define TNS_K_BYE 3
#define TNS_K_PING 4
#define TNS_K_FULL 5
#define TNS_MAX_PEERS 16
#define TNS_MAX_EVENTS 64
#define TNS_APP_CHANNELS 3

typedef struct TnsCtrl {
    uint32_t magic;
    uint32_t kind;
} TnsCtrl;

struct TigNetTPeer {
    bool used;
    bool connected;
    bool free_after_event;
    uint64_t id;
    void* data;
    unsigned int last_rx;
    unsigned int last_tx;
    unsigned int timeout_ms;
};

static const TigSteamApi* tns_api;
static const TigSteamApi* tns_injected;
static bool tns_ready;
static void* tns_msgs;
static int32_t tns_pipe;
static uint64_t tns_self;
static bool tns_listening;
static bool tns_client;
static int tns_max_peers;
static struct TigNetTPeer tns_peers[TNS_MAX_PEERS];
static TigNetTEvent tns_events[TNS_MAX_EVENTS];
static int tns_ev_head;
static int tns_ev_count;

void tig_net_steam_set_api(const TigSteamApi* api)
{
    tns_injected = api;
}

void tig_net_steam_reset_for_test(void)
{
    tns_api = NULL;
    tns_ready = false;
    tns_msgs = NULL;
    tns_self = 0;
    tns_listening = false;
    tns_client = false;
    memset(tns_peers, 0, sizeof(tns_peers));
    tns_ev_head = 0;
    tns_ev_count = 0;
}

static unsigned int tns_now(void)
{
    if (tns_api != NULL && tns_api->now_ms != NULL) {
        return tns_api->now_ms();
    }
#ifdef _WIN32
    return (unsigned int)GetTickCount();
#else
    {
        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (unsigned int)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000);
    }
#endif
}

static void tns_sleep(unsigned int ms)
{
    if (tns_api != NULL && tns_api->sleep_ms != NULL) {
        tns_api->sleep_ms(ms);
        return;
    }
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000u);
#endif
}

/* ---- runtime loader (Windows): steam_api64.dll, flat C exports ---- */
#ifdef _WIN32
static HMODULE tns_dll;
static int(__cdecl* p_InitFlat)(char* err);
static bool(__cdecl* p_Init)(void);
static void(__cdecl* p_Shutdown)(void);
static void(__cdecl* p_MDInit)(void);
static int32_t(__cdecl* p_GetPipe)(void);
static void(__cdecl* p_RunFrame)(int32_t);
static bool(__cdecl* p_NextCb)(int32_t, TigSteamCallbackMsg*);
static void(__cdecl* p_FreeCb)(int32_t);
static void*(__cdecl* p_Msgs)(void);
static int(__cdecl* p_Send)(void*, const TigSteamIdentity*, const void*, uint32_t, int, int);
static int(__cdecl* p_Recv)(void*, int, TigSteamMessage**, int);
static bool(__cdecl* p_Accept)(void*, const TigSteamIdentity*);
static bool(__cdecl* p_Close)(void*, const TigSteamIdentity*);
static void(__cdecl* p_Release)(TigSteamMessage*);
static void*(__cdecl* p_User)(void);
static uint64_t(__cdecl* p_GetSteamID)(void*);
static void*(__cdecl* p_Utils)(void);
static void(__cdecl* p_InitRelay)(void*);

static bool w_init(char* err, int err_size)
{
    if (p_InitFlat != NULL) {
        char msg[1024] = { 0 };
        int rc = p_InitFlat(msg);

        if (rc != 0) {
            snprintf(err, (size_t)err_size, "SteamAPI_InitFlat failed (%d): %s", rc, msg);
            return false;
        }
    } else if (!p_Init()) {
        snprintf(err, (size_t)err_size, "SteamAPI_Init failed (is Steam running and logged in?)");
        return false;
    }
    err[0] = '\0';
    return true;
}
static void w_shutdown(void) { p_Shutdown(); }
static void w_mdinit(void) { p_MDInit(); }
static int32_t w_pipe(void) { return p_GetPipe(); }
static void w_frame(int32_t p) { p_RunFrame(p); }
static bool w_next(int32_t p, TigSteamCallbackMsg* m) { return p_NextCb(p, m); }
static void w_free(int32_t p) { p_FreeCb(p); }
static void* w_msgs(void) { return p_Msgs(); }
static int w_send(void* s, const TigSteamIdentity* id, const void* d, uint32_t n, int f, int c) { return p_Send(s, id, d, n, f, c); }
static int w_recv(void* s, int c, TigSteamMessage** o, int m) { return p_Recv(s, c, o, m); }
static bool w_accept(void* s, const TigSteamIdentity* id) { return p_Accept(s, id); }
static bool w_close(void* s, const TigSteamIdentity* id) { return p_Close(s, id); }
static void w_release(TigSteamMessage* m) { p_Release(m); }
static uint64_t w_self(void)
{
    void* user = p_User != NULL ? p_User() : NULL;

    return user != NULL && p_GetSteamID != NULL ? p_GetSteamID(user) : 0;
}
static void w_relay(void)
{
    void* utils = p_Utils != NULL ? p_Utils() : NULL;

    if (utils != NULL && p_InitRelay != NULL) {
        p_InitRelay(utils);
    }
}

static const TigSteamApi w_api = {
    w_init, w_shutdown, w_mdinit, w_pipe, w_frame, w_next, w_free, w_msgs, w_send, w_recv, w_accept, w_close,
    w_release, w_self, w_relay, NULL, NULL,
};

#define TNS_SYM(var, name) (*(FARPROC*)&(var) = GetProcAddress(tns_dll, name))

static const TigSteamApi* tns_load(char* err, size_t err_size)
{
    static const char* const user_ver[] = { "SteamAPI_SteamUser_v023", "SteamAPI_SteamUser_v022",
        "SteamAPI_SteamUser_v021", "SteamAPI_SteamUser_v020" };
    size_t i;
#if UINTPTR_MAX > 0xFFFFFFFFu
    const char* dll_name = "steam_api64.dll";
#else
    const char* dll_name = "steam_api.dll";
#endif

    if (tns_dll == NULL) {
        tns_dll = LoadLibraryA(dll_name);
    }
    if (tns_dll == NULL) {
        snprintf(err, err_size, "%s not found next to arcanum-ce.exe", dll_name);
        return NULL;
    }
    TNS_SYM(p_InitFlat, "SteamAPI_InitFlat");
    TNS_SYM(p_Init, "SteamAPI_Init");
    TNS_SYM(p_Shutdown, "SteamAPI_Shutdown");
    TNS_SYM(p_MDInit, "SteamAPI_ManualDispatch_Init");
    TNS_SYM(p_GetPipe, "SteamAPI_GetHSteamPipe");
    TNS_SYM(p_RunFrame, "SteamAPI_ManualDispatch_RunFrame");
    TNS_SYM(p_NextCb, "SteamAPI_ManualDispatch_GetNextCallback");
    TNS_SYM(p_FreeCb, "SteamAPI_ManualDispatch_FreeLastCallback");
    TNS_SYM(p_Msgs, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
    TNS_SYM(p_Send, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    TNS_SYM(p_Recv, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    TNS_SYM(p_Accept, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    TNS_SYM(p_Close, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");
    TNS_SYM(p_Release, "SteamAPI_SteamNetworkingMessage_t_Release");
    TNS_SYM(p_GetSteamID, "SteamAPI_ISteamUser_GetSteamID");
    TNS_SYM(p_Utils, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
    TNS_SYM(p_InitRelay, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
    for (i = 0; i < sizeof(user_ver) / sizeof(user_ver[0]) && p_User == NULL; i++) {
        TNS_SYM(p_User, user_ver[i]);
    }
    if ((p_InitFlat == NULL && p_Init == NULL) || p_Shutdown == NULL || p_MDInit == NULL || p_GetPipe == NULL
        || p_RunFrame == NULL || p_NextCb == NULL || p_FreeCb == NULL || p_Msgs == NULL || p_Send == NULL
        || p_Recv == NULL || p_Accept == NULL || p_Close == NULL || p_Release == NULL) {
        snprintf(err, err_size, "%s is too old (needs ISteamNetworkingMessages v002 flat API, Steamworks SDK 1.51+)",
            dll_name);
        return NULL;
    }
    return &w_api;
}
#else
static const TigSteamApi* tns_load(char* err, size_t err_size)
{
    snprintf(err, err_size, "the Steam transport is only built for Windows");
    return NULL;
}
#endif

/* ---- backend ---- */

static void tns_identity(TigSteamIdentity* id, uint64_t steam_id)
{
    memset(id, 0, sizeof(*id));
    id->type = TIG_STEAM_IDENTITY_STEAMID;
    id->cb_size = 8;
    id->u.steam_id64 = steam_id;
}

static bool tns_init(void)
{
    char err[1200];
    const char* appid;

    if (tns_ready) {
        return true;
    }
    err[0] = '\0';
    tns_api = tns_injected != NULL ? tns_injected : tns_load(err, sizeof(err));
    if (tns_api == NULL) {
        tig_debug_printf("tig_net: Steam transport unavailable: %s. Use the Direct transport, or see docs/coop-lite.md (PT58).\n", err);
        return false;
    }
    /* Steam reads the AppID from SteamAppId (or steam_appid.txt). */
    appid = getenv("ARCANUM_COOP_STEAM_APPID");
    if (getenv("SteamAppId") == NULL) {
        char buf[64];

        snprintf(buf, sizeof(buf), "SteamAppId=%s", appid != NULL && appid[0] != '\0' ? appid : TIG_STEAM_DEFAULT_APPID);
#ifdef _WIN32
        _putenv(buf);
#else
        putenv(strdup(buf));
#endif
    }
    if (!tns_api->init(err, (int)sizeof(err))) {
        tig_debug_printf("tig_net: Steam transport unavailable: %s. Start Steam (logged in), or use the Direct transport.\n", err);
        tns_api = NULL;
        return false;
    }
    tns_api->manual_dispatch_init();
    tns_pipe = tns_api->get_pipe();
    tns_msgs = tns_api->messages();
    if (tns_msgs == NULL) {
        tig_debug_printf("tig_net: Steam transport unavailable: no ISteamNetworkingMessages interface\n");
        tns_api->shutdown();
        tns_api = NULL;
        return false;
    }
    if (tns_api->init_relay != NULL) {
        tns_api->init_relay();
    }
    tns_self = tns_api->local_steam_id != NULL ? tns_api->local_steam_id() : 0;
    tns_ready = true;
    if (tns_injected == NULL) {
        atexit(tns_api->shutdown);
    }
    tig_debug_printf("tig_net: Steam transport ready, our SteamID64 %llu\n", (unsigned long long)tns_self);
    return true;
}

static void tns_push(int type, struct TigNetTPeer* peer)
{
    TigNetTEvent* ev;

    if (tns_ev_count >= TNS_MAX_EVENTS) {
        tig_debug_printf("tig_net: Steam event queue full, dropping event %d\n", type);
        return;
    }
    ev = &tns_events[(tns_ev_head + tns_ev_count) % TNS_MAX_EVENTS];
    memset(ev, 0, sizeof(*ev));
    ev->type = type;
    ev->peer = peer;
    tns_ev_count++;
}

static struct TigNetTPeer* tns_find(uint64_t id)
{
    int i;

    for (i = 0; i < TNS_MAX_PEERS; i++) {
        if (tns_peers[i].used && !tns_peers[i].free_after_event && tns_peers[i].id == id) {
            return &tns_peers[i];
        }
    }
    return NULL;
}

static struct TigNetTPeer* tns_alloc(uint64_t id)
{
    int i;

    for (i = 0; i < TNS_MAX_PEERS; i++) {
        if (!tns_peers[i].used) {
            memset(&tns_peers[i], 0, sizeof(tns_peers[i]));
            tns_peers[i].used = true;
            tns_peers[i].id = id;
            tns_peers[i].last_rx = tns_now();
            tns_peers[i].timeout_ms = 10000;
            return &tns_peers[i];
        }
    }
    return NULL;
}

static int tns_connected_count(void)
{
    int i;
    int n = 0;

    for (i = 0; i < TNS_MAX_PEERS; i++) {
        n += tns_peers[i].used && tns_peers[i].connected && !tns_peers[i].free_after_event;
    }
    return n;
}

static void tns_ctrl(uint64_t id, uint32_t kind)
{
    TigSteamIdentity ident;
    TnsCtrl c;

    tns_identity(&ident, id);
    c.magic = TNS_MAGIC;
    c.kind = kind;
    tns_api->send_to_user(tns_msgs, &ident, &c, sizeof(c),
        kind == TNS_K_PING ? TIG_STEAM_SEND_UNRELIABLE | TIG_STEAM_SEND_NO_NAGLE
                           : TIG_STEAM_SEND_RELIABLE | TIG_STEAM_SEND_NO_NAGLE | TIG_STEAM_SEND_AUTO_RESTART,
        TNS_CTRL_CHANNEL);
}

static void tns_drop(struct TigNetTPeer* p, bool event)
{
    TigSteamIdentity ident;

    tns_identity(&ident, p->id);
    tns_api->close_session(tns_msgs, &ident);
    p->connected = false;
    if (event) {
        p->free_after_event = true;
        tns_push(TIG_NET_TEV_DISCONNECT, p);
    } else {
        p->used = false;
    }
}

static void tns_callbacks(void)
{
    TigSteamCallbackMsg cb;

    tns_api->run_frame(tns_pipe);
    while (tns_api->next_callback(tns_pipe, &cb)) {
        if (cb.callback == TIG_STEAM_CB_SESSION_REQUEST && cb.param != NULL && cb.param_size >= (int)sizeof(TigSteamIdentity)) {
            TigSteamIdentity id;

            memcpy(&id, cb.param, sizeof(id));
            /* Host: anyone may knock (CONNECT decides). Client: only our host. */
            if (id.type == TIG_STEAM_IDENTITY_STEAMID && (tns_listening || tns_find(id.u.steam_id64) != NULL)) {
                tns_api->accept_session(tns_msgs, &id);
            }
        } else if (cb.callback == TIG_STEAM_CB_SESSION_FAILED && cb.param != NULL && cb.param_size >= (int)sizeof(TigSteamIdentity)) {
            TigSteamIdentity id;
            struct TigNetTPeer* p;

            memcpy(&id, cb.param, sizeof(id));
            p = tns_find(id.u.steam_id64);
            tig_debug_printf("tig_net: Steam session with %llu failed\n", (unsigned long long)id.u.steam_id64);
            if (p != NULL) {
                tns_drop(p, true);
            }
        }
        tns_api->free_last_callback(tns_pipe);
    }
}

static void tns_handle_ctrl(uint64_t from, const TnsCtrl* c)
{
    struct TigNetTPeer* p = tns_find(from);

    if (p != NULL) {
        p->last_rx = tns_now();
    }
    switch (c->kind) {
    case TNS_K_CONNECT:
        if (!tns_listening) {
            return;
        }
        if (p != NULL && p->connected) {
            tns_ctrl(from, TNS_K_ACCEPT); /* our ACCEPT was lost */
            return;
        }
        if (tns_connected_count() >= tns_max_peers || (p == NULL && (p = tns_alloc(from)) == NULL)) {
            tig_debug_printf("tig_net: Steam: refusing %llu (full)\n", (unsigned long long)from);
            tns_ctrl(from, TNS_K_FULL);
            return;
        }
        p->connected = true;
        tns_ctrl(from, TNS_K_ACCEPT);
        tns_push(TIG_NET_TEV_CONNECT, p);
        break;
    case TNS_K_ACCEPT:
        if (tns_client && p != NULL && !p->connected) {
            p->connected = true;
            tns_push(TIG_NET_TEV_CONNECT, p);
        }
        break;
    case TNS_K_FULL:
    case TNS_K_BYE:
        if (p != NULL) {
            tns_drop(p, true);
        }
        break;
    default:
        break;
    }
}

static void tns_poll(void)
{
    TigSteamMessage* msg;
    unsigned int now = tns_now();
    int i;

    tns_callbacks();
    while (tns_api->receive_on_channel(tns_msgs, TNS_CTRL_CHANNEL, &msg, 1) == 1) {
        if (msg->size >= (int)sizeof(TnsCtrl) && msg->peer.type == TIG_STEAM_IDENTITY_STEAMID) {
            TnsCtrl c;

            memcpy(&c, msg->data, sizeof(c));
            if (c.magic == TNS_MAGIC) {
                tns_handle_ctrl(msg->peer.u.steam_id64, &c);
            }
        }
        tns_api->release_message(msg);
    }
    for (i = 0; i < TNS_MAX_PEERS; i++) {
        struct TigNetTPeer* p = &tns_peers[i];

        if (!p->used || p->free_after_event) {
            continue;
        }
        if (!p->connected) {
            /* Client: knock until ACCEPT (Steam may need seconds for SDR). */
            if (tns_client && now - p->last_tx >= 500u) {
                p->last_tx = now;
                tns_ctrl(p->id, TNS_K_CONNECT);
            }
            continue;
        }
        if (now - p->last_rx > p->timeout_ms) {
            tig_debug_printf("tig_net: Steam peer %llu timed out (%u ms)\n", (unsigned long long)p->id, p->timeout_ms);
            tns_drop(p, true);
            continue;
        }
        if (now - p->last_tx >= 1000u) {
            p->last_tx = now;
            tns_ctrl(p->id, TNS_K_PING);
        }
    }
}

static int tns_next(TigNetTEvent* ev)
{
    int ch;
    int i;

    if (tns_ev_count > 0) {
        *ev = tns_events[tns_ev_head];
        tns_ev_head = (tns_ev_head + 1) % TNS_MAX_EVENTS;
        tns_ev_count--;
        return 1;
    }
    for (ch = 0; ch < TNS_APP_CHANNELS; ch++) {
        TigSteamMessage* msg;

        while (tns_api->receive_on_channel(tns_msgs, ch, &msg, 1) == 1) {
            struct TigNetTPeer* p = msg->peer.type == TIG_STEAM_IDENTITY_STEAMID ? tns_find(msg->peer.u.steam_id64) : NULL;

            if (p == NULL || !p->connected) {
                tns_api->release_message(msg); /* not a session member */
                continue;
            }
            p->last_rx = tns_now();
            memset(ev, 0, sizeof(*ev));
            ev->type = TIG_NET_TEV_RECEIVE;
            ev->peer = p;
            ev->channel = ch;
            ev->data = msg->data;
            ev->size = (size_t)msg->size;
            ev->internal = msg;
            return 1;
        }
    }
    (void)i;
    return 0;
}

static int tns_service(TigNetTEvent* ev, unsigned int timeout_ms)
{
    unsigned int start;
    int i;

    memset(ev, 0, sizeof(*ev));
    if (!tns_ready || (!tns_listening && !tns_client)) {
        return 0;
    }
    /* Peers whose DISCONNECT tig_net has seen are gone now. */
    for (i = 0; i < TNS_MAX_PEERS; i++) {
        if (tns_peers[i].used && tns_peers[i].free_after_event) {
            bool queued = false;
            int k;

            for (k = 0; k < tns_ev_count; k++) {
                queued |= tns_events[(tns_ev_head + k) % TNS_MAX_EVENTS].peer == &tns_peers[i];
            }
            if (!queued) {
                tns_peers[i].used = false;
            }
        }
    }
    start = tns_now();
    for (;;) {
        if (tns_ev_count == 0) {
            tns_poll();
        }
        if (tns_next(ev)) {
            return 1;
        }
        if (tns_now() - start >= timeout_ms) {
            return 0;
        }
        tns_sleep(5);
    }
}

static void tns_event_done(TigNetTEvent* ev)
{
    if (ev != NULL && ev->internal != NULL) {
        tns_api->release_message((TigSteamMessage*)ev->internal);
        ev->internal = NULL;
    }
}

static bool tns_listen(int port, int max_peers, int channels)
{
    (void)port;
    (void)channels;
    if (!tns_ready) {
        return false;
    }
    tns_listening = true;
    tns_client = false;
    tns_max_peers = max_peers - 1 > 0 ? max_peers - 1 : 1; /* slot 0 is us */
    if (tns_max_peers > TNS_MAX_PEERS) {
        tns_max_peers = TNS_MAX_PEERS;
    }
    tig_debug_printf("tig_net: Steam host - friends join with SteamID64 %llu\n", (unsigned long long)tns_self);
    return true;
}

static TigNetTPeer* tns_connect(const char* target, int port, int channels)
{
    struct TigNetTPeer* p;
    unsigned long long id;
    char* end = NULL;

    (void)port;
    (void)channels;
    if (!tns_ready) {
        return NULL;
    }
    id = target != NULL ? strtoull(target, &end, 10) : 0;
    if (id == 0 || end == NULL || (*end != '\0' && *end != ':')) {
        tig_debug_printf("tig_net: Steam join needs the host's SteamID64 (17 digits), got '%s'\n", target != NULL ? target : "");
        return NULL;
    }
    tns_client = true;
    tns_listening = false;
    p = tns_alloc((uint64_t)id);
    if (p == NULL) {
        return NULL;
    }
    p->last_tx = tns_now();
    tns_ctrl(p->id, TNS_K_CONNECT);
    tig_debug_printf("tig_net: Steam: connecting to %llu\n", id);
    return p;
}

static int tns_send(TigNetTPeer* const* peers, int count, int channel, const void* data, size_t size, bool reliable)
{
    int i;
    int sent = 0;
    int flags = reliable ? TIG_STEAM_SEND_RELIABLE | TIG_STEAM_SEND_AUTO_RESTART : TIG_STEAM_SEND_UNRELIABLE;

    if (!tns_ready) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        TigSteamIdentity ident;

        if (peers[i] == NULL || !peers[i]->used || !peers[i]->connected) {
            continue;
        }
        tns_identity(&ident, peers[i]->id);
        if (tns_api->send_to_user(tns_msgs, &ident, data, (uint32_t)size, flags, channel) == 1) {
            sent++;
            peers[i]->last_tx = tns_now();
        }
    }
    return sent;
}

static void tns_flush(void)
{
    /* Steam batches with Nagle (~5 ms); nothing to force. */
}

static void tns_disconnect(TigNetTPeer* peer, bool now)
{
    if (peer == NULL || !peer->used || peer->free_after_event) {
        return;
    }
    if (peer->connected) {
        tns_ctrl(peer->id, TNS_K_BYE);
    }
    tns_drop(peer, !now);
}

static void tns_reset_peer(TigNetTPeer* peer)
{
    if (peer != NULL && peer->used) {
        tns_drop(peer, false);
    }
}

static void tns_set_timeout(TigNetTPeer* peer, unsigned int min_ms, unsigned int max_ms)
{
    (void)min_ms;
    if (peer != NULL && max_ms > 0) {
        peer->timeout_ms = max_ms;
    }
}

static void tns_close(void)
{
    int i;

    for (i = 0; i < TNS_MAX_PEERS; i++) {
        if (tns_peers[i].used) {
            if (tns_peers[i].connected && tns_ready) {
                tns_ctrl(tns_peers[i].id, TNS_K_BYE);
            }
            if (tns_ready) {
                TigSteamIdentity ident;

                tns_identity(&ident, tns_peers[i].id);
                tns_api->close_session(tns_msgs, &ident);
            }
        }
    }
    memset(tns_peers, 0, sizeof(tns_peers));
    tns_ev_head = 0;
    tns_ev_count = 0;
    tns_listening = false;
    tns_client = false;
}

static bool tns_is_open(void)
{
    return tns_ready && (tns_listening || tns_client);
}

static void** tns_peer_data(TigNetTPeer* peer)
{
    return &peer->data;
}

static void tns_describe_peer(TigNetTPeer* peer, char* buf, size_t size)
{
    snprintf(buf, size, "steam %llu last_rx=%u ms ago", (unsigned long long)peer->id, tns_now() - peer->last_rx);
}

static bool tns_local_id(char* buf, size_t size)
{
    if (!tns_ready || tns_self == 0 || buf == NULL || size == 0) {
        return false;
    }
    snprintf(buf, size, "%llu", (unsigned long long)tns_self);
    return true;
}

static const TigNetTransport tns_transport = {
    "steam",
    tns_init,
    tns_listen,
    tns_connect,
    tns_service,
    tns_event_done,
    tns_send,
    tns_flush,
    tns_disconnect,
    tns_reset_peer,
    tns_set_timeout,
    tns_close,
    tns_is_open,
    tns_peer_data,
    tns_now,
    tns_describe_peer,
    tns_local_id,
};

const TigNetTransport* tig_net_transport_steam(void)
{
    return &tns_transport;
}
