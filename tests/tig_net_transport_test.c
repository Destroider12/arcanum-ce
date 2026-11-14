/*
 * PT58: tig_net transport abstraction.
 *  T1 backend selection by ARCANUM_COOP_TRANSPORT value.
 *  T2 tig_net host over an in-process loopback fake transport: a fake remote
 *     peer connects, gets WELCOME, sends HELLO + app packets (dispatched),
 *     receives app traffic, disconnects (DISCONNECT event to the game).
 *  T3 Steam backend against a fake Steamworks flat API: host accepts a
 *     CONNECT, delivers app data, sends, refuses when full, times a silent
 *     peer out; client knocks until ACCEPT; a missing API fails cleanly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "net/tig_net.h"
#include "net/tig_net_steam.h"
#include "net/tig_net_transport.h"

static int failures;
#define CHECK(c, m)                                                  \
    do {                                                             \
        if (!(c)) {                                                  \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, m);      \
            failures++;                                              \
        }                                                            \
    } while (0)

/* ---------------- loopback fake transport ---------------- */
#define FK_PEERS 4
#define FK_Q 64

struct TigNetTPeer {
    bool used;
    void* data;
    bool connected;
};

typedef struct FkMsg {
    int type;
    int peer;
    int channel;
    bool reliable;
    size_t size;
    unsigned char bytes[512];
} FkMsg;

static struct TigNetTPeer fk_peers[FK_PEERS];
static FkMsg fk_in[FK_Q]; /* to tig_net */
static int fk_in_n;
static FkMsg fk_out[FK_Q]; /* from tig_net */
static int fk_out_n;
static bool fk_open;
static unsigned int fk_clock = 1000;
static int fk_flushes;

static void fk_push_in(int type, int peer, int channel, const void* data, size_t size)
{
    FkMsg* m = &fk_in[fk_in_n++];

    memset(m, 0, sizeof(*m));
    m->type = type;
    m->peer = peer;
    m->channel = channel;
    m->size = size;
    if (data != NULL) {
        memcpy(m->bytes, data, size);
    }
}

static bool fk_init(void) { return true; }
static bool fk_listen(int port, int max_peers, int channels)
{
    (void)port;
    (void)max_peers;
    CHECK(channels == 3, "three channels");
    fk_open = true;
    return true;
}
static TigNetTPeer* fk_connect(const char* t, int port, int ch)
{
    (void)t;
    (void)port;
    (void)ch;
    return NULL;
}
static int fk_service(TigNetTEvent* ev, unsigned int timeout)
{
    FkMsg* m;

    (void)timeout;
    memset(ev, 0, sizeof(*ev));
    if (fk_in_n == 0) {
        return 0;
    }
    m = &fk_in[0];
    ev->type = m->type;
    ev->peer = &fk_peers[m->peer];
    ev->channel = m->channel;
    if (m->type == TIG_NET_TEV_CONNECT) {
        fk_peers[m->peer].used = true;
        fk_peers[m->peer].connected = true;
    }
    if (m->type == TIG_NET_TEV_RECEIVE) {
        static unsigned char hold[512];

        memcpy(hold, m->bytes, m->size);
        ev->data = hold;
        ev->size = m->size;
        ev->internal = hold;
    }
    memmove(&fk_in[0], &fk_in[1], sizeof(FkMsg) * (size_t)(fk_in_n - 1));
    fk_in_n--;
    return 1;
}
static void fk_event_done(TigNetTEvent* ev) { ev->internal = NULL; }
static int fk_send(TigNetTPeer* const* peers, int count, int channel, const void* data, size_t size, bool reliable)
{
    int i;

    for (i = 0; i < count; i++) {
        FkMsg* m = &fk_out[fk_out_n++];

        m->type = TIG_NET_TEV_RECEIVE;
        m->peer = (int)(peers[i] - fk_peers);
        m->channel = channel;
        m->reliable = reliable;
        m->size = size;
        memcpy(m->bytes, data, size);
    }
    return count;
}
static void fk_flush(void) { fk_flushes++; }
static void fk_disconnect(TigNetTPeer* p, bool now)
{
    p->connected = false;
    if (!now) {
        fk_push_in(TIG_NET_TEV_DISCONNECT, (int)(p - fk_peers), 0, NULL, 0);
    }
}
static void fk_reset(TigNetTPeer* p) { p->connected = false; }
static void fk_timeout(TigNetTPeer* p, unsigned int a, unsigned int b)
{
    (void)p;
    (void)a;
    (void)b;
}
static void fk_close(void) { fk_open = false; }
static bool fk_is_open(void) { return fk_open; }
static void** fk_data(TigNetTPeer* p) { return &p->data; }
static unsigned int fk_now(void) { return fk_clock; }
static void fk_desc(TigNetTPeer* p, char* b, size_t n)
{
    (void)p;
    snprintf(b, n, "fake");
}
static bool fk_id(char* b, size_t n)
{
    snprintf(b, n, "fake-host");
    return true;
}

static const TigNetTransport fk_transport = {
    "fake", fk_init, fk_listen, fk_connect, fk_service, fk_event_done, fk_send, fk_flush, fk_disconnect, fk_reset,
    fk_timeout, fk_close, fk_is_open, fk_data, fk_now, fk_desc, fk_id,
};

/* tig_net wire bits the fake remote speaks (mirrors tig_net.c). */
typedef struct {
    int magic;
    int slot;
    int max_players;
    int protocol;
} Welcome;
typedef struct {
    int magic;
    int slot;
    char name[TIG_NET_MAX_NAME];
    int protocol;
} Hello;

static int g_events[16];
static int g_event_slot[16];
static int g_nevents;
static int g_msgs;
static int g_last_value;

static bool on_event(int type, int client, void* d, int s)
{
    (void)d;
    (void)s;
    g_events[g_nevents] = type;
    g_event_slot[g_nevents++] = client;
    return true;
}
static void on_msg(void* m)
{
    g_msgs++;
    g_last_value = ((int*)m)[1];
}
static bool on_valid(void* m)
{
    return *(int*)m > 0 && *(int*)m < 256;
}

static void test_fake_host(void)
{
    Hello hello;
    int pkt[2] = { 42, 1234 };
    Welcome w;
    char id[32];

    tig_net_set_transport(&fk_transport);
    tig_net_set_coop_enabled(true);
    tig_net_on_network_event(on_event);
    tig_net_on_message(on_msg);
    tig_net_on_message_validation(on_valid);
    tig_net_configure_host(7777);
    CHECK(tig_net_start_client() == TIG_OK, "host starts over the fake");
    CHECK(tig_net_is_host() && fk_open, "hosting");
    CHECK(g_nevents == 1 && g_events[0] == TIG_NET_EVENT_CONNECT && g_event_slot[0] == 0, "host slot 0 event");
    CHECK(tig_net_local_join_id(id, sizeof(id)) && strcmp(id, "fake-host") == 0, "local join id from backend");

    fk_push_in(TIG_NET_TEV_CONNECT, 1, 0, NULL, 0);
    tig_net_pump();
    CHECK(fk_out_n == 1 && fk_out[0].channel == 1 && fk_out[0].reliable, "WELCOME on the ctrl channel");
    memcpy(&w, fk_out[0].bytes, sizeof(w));
    CHECK(w.magic == 1 && w.slot == 1 && w.protocol == TIG_NET_PROTOCOL, "WELCOME slot 1, our protocol");
    CHECK(g_nevents == 1, "no game CONNECT before the handshake");

    /* App traffic before HELLO is dropped. */
    fk_push_in(TIG_NET_TEV_RECEIVE, 1, 0, pkt, sizeof(pkt));
    tig_net_pump();
    CHECK(g_msgs == 0, "pre-handshake app packet dropped");

    memset(&hello, 0, sizeof(hello));
    hello.magic = 0x454D414E;
    strcpy(hello.name, "Remote");
    hello.protocol = TIG_NET_PROTOCOL;
    fk_push_in(TIG_NET_TEV_RECEIVE, 1, 0, &hello, sizeof(hello));
    fk_push_in(TIG_NET_TEV_RECEIVE, 1, 0, pkt, sizeof(pkt));
    tig_net_pump();
    CHECK(g_nevents == 2 && g_events[1] == TIG_NET_EVENT_CONNECT && g_event_slot[1] == 1, "game CONNECT for slot 1");
    CHECK(strcmp(tig_net_client_info_get_name(1), "Remote") == 0, "name from HELLO");
    CHECK(g_msgs == 1 && g_last_value == 1234, "app packet dispatched");

    fk_out_n = 0;
    pkt[1] = 99;
    tig_net_send_app_all(pkt, sizeof(pkt));
    tig_net_send_pose_all(pkt, sizeof(pkt));
    tig_net_pump();
    CHECK(fk_out_n == 2 && fk_out[0].channel == 0 && fk_out[0].reliable && fk_out[0].peer == 1, "app send reliable ch 0");
    CHECK(fk_out[1].channel == 2 && !fk_out[1].reliable, "pose unreliable ch 2");
    CHECK(fk_flushes > 0, "batched flush in pump");

    fk_push_in(TIG_NET_TEV_DISCONNECT, 1, 0, NULL, 0);
    tig_net_pump();
    CHECK(g_nevents == 3 && g_events[2] == TIG_NET_EVENT_DISCONNECT && g_event_slot[2] == 1, "DISCONNECT to the game");
    CHECK(!tig_net_client_is_active(1), "slot freed");

    /* A peer that never says HELLO is dropped after the handshake timeout. */
    fk_push_in(TIG_NET_TEV_CONNECT, 2, 0, NULL, 0);
    tig_net_pump();
    fk_clock += 6000;
    tig_net_pump();
    tig_net_pump();
    CHECK(!fk_peers[2].connected, "silent peer disconnected after handshake timeout");

    tig_net_reset_connection();
    CHECK(!fk_open && !tig_net_is_active(), "reset closes the backend");
    tig_net_set_transport(NULL);
}

static void test_pick(void)
{
    CHECK(tig_net_transport_pick(NULL) == tig_net_transport_enet(), "default ENet");
    CHECK(tig_net_transport_pick("direct") == tig_net_transport_enet(), "direct = ENet");
    CHECK(tig_net_transport_pick("STEAM") == tig_net_transport_steam(), "steam");
    CHECK(tig_net_transport_pick("bogus") == tig_net_transport_enet(), "unknown -> ENet");
    CHECK(strcmp(tig_net_transport_steam()->name, "steam") == 0, "names");
}

/* ---------------- fake Steamworks flat API ---------------- */
#define FS_Q 64
typedef struct {
    TigSteamMessage m;
    unsigned char bytes[256];
} FsMsg;
static FsMsg fs_in[FS_Q];
static int fs_in_n;
typedef struct {
    uint64_t to;
    int channel;
    int flags;
    size_t size;
    unsigned char bytes[256];
} FsOut;
static FsOut fs_out[256];
static int fs_out_n;
static unsigned int fs_clock = 5000;
static bool fs_init_ok = true;
static int fs_accepts;
static int fs_released;
static TigSteamCallbackMsg fs_cb;
static bool fs_cb_pending;
static TigSteamIdentity fs_cb_id;

static void fs_inject(uint64_t from, int channel, const void* d, size_t n)
{
    FsMsg* m = &fs_in[fs_in_n++];

    memset(m, 0, sizeof(*m));
    memcpy(m->bytes, d, n);
    m->m.data = m->bytes;
    m->m.size = (int)n;
    m->m.peer.type = TIG_STEAM_IDENTITY_STEAMID;
    m->m.peer.cb_size = 8;
    m->m.peer.u.steam_id64 = from;
    m->m.channel = channel;
}
static void fs_ctrl(uint64_t from, uint32_t kind)
{
    uint32_t c[2] = { 0x4E435241u, kind };

    fs_inject(from, 7, c, sizeof(c));
}
static bool fs_init(char* e, int n)
{
    snprintf(e, (size_t)n, fs_init_ok ? "" : "Steam is not running");
    return fs_init_ok;
}
static void fs_shutdown(void) {}
static void fs_md(void) {}
static int32_t fs_pipe(void) { return 1; }
static void fs_frame(int32_t p) { (void)p; }
static bool fs_next(int32_t p, TigSteamCallbackMsg* out)
{
    (void)p;
    if (!fs_cb_pending) {
        return false;
    }
    fs_cb.callback = TIG_STEAM_CB_SESSION_REQUEST;
    fs_cb.param = (uint8_t*)&fs_cb_id;
    fs_cb.param_size = (int)sizeof(fs_cb_id);
    *out = fs_cb;
    return true;
}
static void fs_free(int32_t p)
{
    (void)p;
    fs_cb_pending = false;
}
static void* fs_msgs(void) { return (void*)&fs_in; }
static int fs_send(void* s, const TigSteamIdentity* id, const void* d, uint32_t n, int f, int ch)
{
    FsOut* o = &fs_out[fs_out_n++ % 256];

    (void)s;
    o->to = id->u.steam_id64;
    o->channel = ch;
    o->flags = f;
    o->size = n;
    memcpy(o->bytes, d, n < 256 ? n : 256);
    return 1;
}
static int fs_recv(void* s, int ch, TigSteamMessage** out, int max)
{
    int i;

    (void)s;
    (void)max;
    for (i = 0; i < fs_in_n; i++) {
        if (fs_in[i].m.channel == ch) {
            static FsMsg hold[8];
            static int hi;
            FsMsg* h = &hold[hi++ % 8];

            *h = fs_in[i];
            h->m.data = h->bytes;
            memmove(&fs_in[i], &fs_in[i + 1], sizeof(FsMsg) * (size_t)(fs_in_n - i - 1));
            fs_in_n--;
            *out = &h->m;
            return 1;
        }
    }
    return 0;
}
static bool fs_accept(void* s, const TigSteamIdentity* id)
{
    (void)s;
    (void)id;
    fs_accepts++;
    return true;
}
static bool fs_close(void* s, const TigSteamIdentity* id)
{
    (void)s;
    (void)id;
    return true;
}
static void fs_release(TigSteamMessage* m)
{
    (void)m;
    fs_released++;
}
static uint64_t fs_self(void) { return 76561190000000001ull; }
static unsigned int fs_now(void) { return fs_clock; }
static void fs_sleep(unsigned int ms) { fs_clock += ms; }

static const TigSteamApi fs_api = {
    fs_init, fs_shutdown, fs_md, fs_pipe, fs_frame, fs_next, fs_free, fs_msgs, fs_send, fs_recv, fs_accept, fs_close,
    fs_release, fs_self, NULL, fs_now, fs_sleep,
};

static int fs_count_kind(uint64_t to, uint32_t kind)
{
    int i;
    int n = 0;

    for (i = 0; i < fs_out_n && i < 256; i++) {
        uint32_t c[2];

        if (fs_out[i].to == to && fs_out[i].channel == 7 && fs_out[i].size == 8) {
            memcpy(c, fs_out[i].bytes, 8);
            n += c[1] == kind;
        }
    }
    return n;
}

static void test_steam(void)
{
    const TigNetTransport* t = tig_net_transport_steam();
    TigNetTEvent ev;
    TigNetTPeer* peer;
    TigNetTPeer* c;
    int data[2] = { 7, 77 };
    char id[32];
    const uint64_t A = 76561190000000002ull;
    const uint64_t B = 76561190000000003ull;
    const uint64_t H = 76561190000000009ull;

    /* No API (Linux: no DLL loader) -> clean failure. */
    tig_net_steam_reset_for_test();
    tig_net_steam_set_api(NULL);
#ifndef _WIN32
    CHECK(!t->init(), "no steam_api -> init fails cleanly");
#endif
    tig_net_steam_set_api(&fs_api);
    fs_init_ok = false;
    CHECK(!t->init(), "Steam not running -> init fails");
    CHECK(getenv("SteamAppId") != NULL && strcmp(getenv("SteamAppId"), TIG_STEAM_DEFAULT_APPID) == 0, "AppID 500810 exported");
    fs_init_ok = true;
    CHECK(t->init(), "fake Steam init");
    CHECK(t->local_id(id, sizeof(id)) && strcmp(id, "76561190000000001") == 0, "SteamID64 shown to the host");

    /* Host, room for one client. */
    CHECK(t->listen(7777, 2, 3) && t->is_open(), "listen");
    fs_cb_id.type = TIG_STEAM_IDENTITY_STEAMID;
    fs_cb_id.u.steam_id64 = A;
    fs_cb_pending = true;
    fs_ctrl(A, 1);
    CHECK(t->service(&ev, 0) == 1 && ev.type == TIG_NET_TEV_CONNECT, "CONNECT -> event");
    CHECK(fs_accepts == 1, "session request accepted by the host");
    CHECK(fs_count_kind(A, 2) == 1, "ACCEPT sent");
    peer = ev.peer;
    *t->peer_data(peer) = (void*)2;
    fs_inject(A, 0, data, sizeof(data));
    CHECK(t->service(&ev, 0) == 1 && ev.type == TIG_NET_TEV_RECEIVE && ev.peer == peer && ev.channel == 0
            && ev.size == sizeof(data) && ((const int*)ev.data)[1] == 77,
        "app data delivered");
    t->event_done(&ev);
    CHECK(fs_released >= 2, "messages released");
    fs_inject(B, 0, data, sizeof(data));
    CHECK(t->service(&ev, 0) == 0, "data from a non-member dropped");
    fs_ctrl(B, 1);
    CHECK(t->service(&ev, 0) == 0 && fs_count_kind(B, 5) == 1, "second client refused: FULL");
    fs_out_n = 0;
    CHECK(t->send(&peer, 1, 2, data, sizeof(data), false) == 1 && fs_out[0].channel == 2
            && (fs_out[0].flags & TIG_STEAM_SEND_RELIABLE) == 0,
        "unreliable send on ch 2");
    CHECK(t->send(&peer, 1, 0, data, sizeof(data), true) == 1 && (fs_out[1].flags & TIG_STEAM_SEND_RELIABLE) != 0,
        "reliable send");
    fs_clock += 1500;
    t->service(&ev, 0);
    CHECK(fs_count_kind(A, 4) >= 1, "keepalive PING");
    fs_clock += 11000;
    CHECK(t->service(&ev, 0) == 1 && ev.type == TIG_NET_TEV_DISCONNECT && ev.peer == peer, "silent peer times out");
    *t->peer_data(peer) = NULL;
    t->service(&ev, 0);
    fs_ctrl(A, 1);
    CHECK(t->service(&ev, 0) == 1 && ev.type == TIG_NET_TEV_CONNECT, "rejoin after timeout");
    fs_ctrl(A, 3);
    CHECK(t->service(&ev, 0) == 1 && ev.type == TIG_NET_TEV_DISCONNECT, "BYE -> DISCONNECT");
    t->close();
    CHECK(!t->is_open(), "closed");

    /* Client. */
    fs_out_n = 0;
    CHECK(t->connect("not-an-id", 0, 3) == NULL, "bad SteamID refused");
    c = t->connect("76561190000000009", 0, 3);
    CHECK(c != NULL && fs_count_kind(H, 1) == 1, "CONNECT knock sent");
    CHECK(t->service(&ev, 600) == 0 && fs_count_kind(H, 1) >= 2, "knocks again while waiting");
    fs_ctrl(H, 2);
    CHECK(t->service(&ev, 0) == 1 && ev.type == TIG_NET_TEV_CONNECT && ev.peer == c, "ACCEPT -> CONNECT");
    t->disconnect(c, false);
    CHECK(t->service(&ev, 0) == 1 && ev.type == TIG_NET_TEV_DISCONNECT && fs_count_kind(H, 3) == 1, "graceful BYE");
    t->close();
    tig_net_steam_reset_for_test();
}

int main(void)
{
    test_pick();
    test_fake_host();
    test_steam();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("tig_net_transport: ok\n");
    return 0;
}
