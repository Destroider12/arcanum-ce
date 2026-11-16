#include "net/tig_net.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "net/tig_net_transport.h"
#ifdef ARCANUM_UPNP
#include "net/coop_upnp.h" /* PT51 */
#endif

#ifdef TIG_NET_TEST_STUB_DEBUG
#include <stdio.h>
#include <stdarg.h>
void tig_debug_printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
#else
#include "tig/debug.h"
#endif

#define TIG_NET_CHANNEL_APP 0
#define TIG_NET_CHANNEL_CTRL 1
/* Review M7: high-rate poses, unreliable+sequenced so a lost one never
   holds back events queued behind it on the reliable app channel. */
#define TIG_NET_CHANNEL_POSE 2
#define TIG_NET_NUM_CHANNELS 3

#define TIG_NET_CTRL_WELCOME 1
#define TIG_NET_CTRL_NAME 2
/* Review PT24 #1: the client's NAME/HELLO travels on the APP channel as its
   very first app packet ("NAME" magic, outside the 0..255 app type range),
   so ENet's per-channel ordering guarantees it precedes CHAR_OFFER etc.
   The host drops app packets from a slot until a valid HELLO. */
#define TIG_NET_APP_HELLO 0x454D414E
#define TIG_NET_HANDSHAKE_TIMEOUT_MS 5000u
#define TIG_NET_MAX_EVENTS_PER_PUMP 512

typedef struct TigNetCtrlWelcome {
    int magic; /* TIG_NET_CTRL_WELCOME */
    int slot;
    int max_players;
    int protocol; /* TIG_NET_PROTOCOL */
} TigNetCtrlWelcome;

typedef struct TigNetCtrlName {
    int magic; /* TIG_NET_CTRL_NAME */
    int slot;
    char name[TIG_NET_MAX_NAME];
    int protocol; /* TIG_NET_PROTOCOL */
} TigNetCtrlName;

_Static_assert(sizeof(TigNetCtrlWelcome) == 16, "WELCOME wire layout");
_Static_assert(sizeof(TigNetCtrlName) == 12 + TIG_NET_MAX_NAME, "NAME/HELLO wire layout");

typedef struct TigNetClient {
    TigNetTPeer* peer;
    bool active;
    bool waiting;
    bool loading;
    bool refused; /* host: protocol mismatch, disconnect pending */
    bool handshaken; /* host: valid HELLO (protocol) received */
    unsigned int connect_ms; /* host: for the handshake timeout */
    char name[TIG_NET_MAX_NAME];
} TigNetClient;

static bool coop_enabled;
static bool library_ready;
static bool active;
static bool is_host;
static int listen_port = TIG_NET_DEFAULT_PORT;
static char join_host[256];
static int join_port = TIG_NET_DEFAULT_PORT;
static bool want_host;
static bool want_join;

/* PT58: the transport backend (ENet unless ARCANUM_COOP_TRANSPORT says otherwise). */
static const TigNetTransport* tp;

static const TigNetTransport* tig_net_tp(void)
{
    if (tp == NULL) {
        tp = tig_net_transport_pick(getenv("ARCANUM_COOP_TRANSPORT"));
        tig_debug_printf("tig_net: transport %s\n", tp->name);
    }
    return tp;
}

void tig_net_set_transport(const TigNetTransport* transport)
{
    tp = transport;
    library_ready = false;
}

const TigNetTransport* tig_net_get_transport(void)
{
    return tig_net_tp();
}

bool tig_net_local_join_id(char* buf, size_t size)
{
    return tig_net_tp()->local_id(buf, size);
}

#define enet_host_open() (tp != NULL && tp->is_open())

/* Per-type app packet + byte counters (diagnose spam / lag). One compact
   [NETSTAT] line every ~10 s. ARCANUM_COOP_NETSTAT=0 disables the line. */
#define TIG_NET_STAT_TYPES 256
static unsigned int tig_net_stat_tx_pk[TIG_NET_STAT_TYPES];
static unsigned int tig_net_stat_rx_pk[TIG_NET_STAT_TYPES];
static uint64_t tig_net_stat_tx_bytes[TIG_NET_STAT_TYPES];
static uint64_t tig_net_stat_rx_bytes[TIG_NET_STAT_TYPES];
static unsigned int tig_net_stat_window_start;
static int tig_net_stat_enabled = -1; /* -1 unknown, 0 off, 1 on */
static bool tig_net_send_pending_flush;

static bool tig_net_stat_on(void)
{
    const char* v;

    if (tig_net_stat_enabled < 0) {
        v = getenv("ARCANUM_COOP_NETSTAT");
        tig_net_stat_enabled = (v != NULL && v[0] == '0' && v[1] == '\0') ? 0 : 1;
    }
    return tig_net_stat_enabled != 0;
}

static void tig_net_stat_add(unsigned int* pk, uint64_t* bytes, const void* data, size_t size, int copies)
{
    int type;

    if (!tig_net_stat_on() || data == NULL || size < sizeof(int) || copies <= 0) {
        return;
    }
    memcpy(&type, data, sizeof(type));
    type = (int)((unsigned int)type & 0xFFu);
    pk[type] += (unsigned int)copies;
    bytes[type] += (uint64_t)size * (uint64_t)copies;
}

/* Short names for coop app types 128..164 (PACKET_COOP_*). Vanilla stays "P<n>". */
static const char* const tig_net_stat_coop_names[] = {
    "ATTACK_INTENT",
    "FOLLOW_HOST",
    "CHAR_OFFER",
    "SESSION_SYNC",
    "PC_POS",
    "FOLLOWERS",
    "NPC_POS",
    "PARTY_STATE",
    "BARTER",
    "INVEN_SYNC",
    "ROSTER",
    "OBJ_REQ",
    "OBJ_BLOB",
    "CHAR_BUNDLE",
    "WORLD_REQ",
    "WORLD_STATE",
    "PC_STATS",
    "SWING",
    "NPC_REQ",
    "STORY_SYNC",
    "DIALOG_STATE",
    "DOOR_USE",
    "SKILL_USE",
    "CRAFT",
    "PARTY_CTRL",
    "INVEN_LAYOUT",
    "DOOR_STATE",
    "TRAVEL_REQ",
    "SAVE_REQ",
    "TRAP_FX",
    "HOTBAR",
    "WMAP_POS",
    "CHAT",
    "BODY_DRAG",
    "CRITTER_EFFECTS",
    "THROW_INTENT",
    "PROJECTILE_FX",
    "AREA_KNOWN",
    "NPC_STATS",
};

static const char* tig_net_stat_type_name(int type, char* scratch, size_t scratch_n)
{
    if (type >= 128 && type <= 166) {
        return tig_net_stat_coop_names[type - 128];
    }
    snprintf(scratch, scratch_n, "P%d", type);
    return scratch;
}

static int tig_net_stat_fmt_bytes(char* dst, size_t cap, uint64_t bytes)
{
    unsigned int tenths;

    if (bytes >= 1000000ull) {
        tenths = (unsigned int)((bytes * 10ull) / 1000000ull);
        return snprintf(dst, cap, "%u.%uMB", tenths / 10u, tenths % 10u);
    }
    if (bytes >= 1000ull) {
        tenths = (unsigned int)((bytes * 10ull) / 1000ull);
        return snprintf(dst, cap, "%u.%uKB", tenths / 10u, tenths % 10u);
    }
    return snprintf(dst, cap, "%uB", (unsigned int)bytes);
}

static size_t tig_net_stat_cat(char* line, size_t len, size_t cap, const char* fmt, ...)
{
    va_list ap;
    int n;

    if (len >= cap) {
        return cap - 1;
    }
    va_start(ap, fmt);
    n = vsnprintf(line + len, cap - len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return len;
    }
    len += (size_t)n;
    if (len >= cap) {
        len = cap - 1;
    }
    return len;
}

/* Indices of non-zero types, sorted by bytes descending. Returns count. */
static int tig_net_stat_pick_top(const uint64_t* bytes, int* out, int max)
{
    int n = 0;
    int i;
    int j;

    for (i = 0; i < TIG_NET_STAT_TYPES; i++) {
        if (bytes[i] == 0) {
            continue;
        }
        if (n < max) {
            j = n++;
        } else if (bytes[i] > bytes[out[n - 1]]) {
            j = n - 1;
        } else {
            continue;
        }
        while (j > 0 && bytes[i] > bytes[out[j - 1]]) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = i;
    }
    return n;
}

static size_t tig_net_stat_append_dir(char* line, size_t len, size_t cap,
    const char* tag, const unsigned int* pk, const uint64_t* bytes, unsigned int elapsed_ms)
{
    uint64_t total_b = 0;
    unsigned int total_pk = 0;
    unsigned int i;
    int top[TIG_NET_STAT_TYPES];
    int n;
    int t;
    char bbuf[16];
    char nbuf[16];
    const char* name;

    for (i = 0; i < TIG_NET_STAT_TYPES; i++) {
        total_b += bytes[i];
        total_pk += pk[i];
    }
    tig_net_stat_fmt_bytes(bbuf, sizeof(bbuf), total_b * 1000ull / elapsed_ms);
    len = tig_net_stat_cat(line, len, cap, "%s %s/s %upk/s",
        tag, bbuf, (unsigned int)((unsigned long long)total_pk * 1000ull / elapsed_ms));
    n = tig_net_stat_pick_top(bytes, top, TIG_NET_STAT_TYPES);
    for (t = 0; t < n && len + 40 < cap; t++) {
        i = (unsigned int)top[t];
        tig_net_stat_fmt_bytes(bbuf, sizeof(bbuf), bytes[i] * 1000ull / elapsed_ms);
        name = tig_net_stat_type_name((int)i, nbuf, sizeof(nbuf));
        len = tig_net_stat_cat(line, len, cap, "%s%s %s %upk",
            t == 0 ? ": " : ", ",
            name,
            bbuf,
            (unsigned int)((unsigned long long)pk[i] * 1000ull / elapsed_ms));
    }
    return len;
}

static bool tig_net_stat_logged;

static void tig_net_stat_maybe_log(void)
{
    unsigned int now;
    unsigned int elapsed;
    unsigned int total_pk = 0;
    int i;
    char line[1000];
    size_t len;

    now = tig_net_tp()->now_ms();
    if (tig_net_stat_window_start == 0) {
        tig_net_stat_window_start = now;
        return;
    }
    elapsed = now - tig_net_stat_window_start;
    if (elapsed < 10000u) {
        return;
    }

    if (tig_net_stat_on()) {
        for (i = 0; i < TIG_NET_STAT_TYPES; i++) {
            total_pk += tig_net_stat_tx_pk[i];
            total_pk += tig_net_stat_rx_pk[i];
        }
        if (total_pk > 0) {
            len = (size_t)snprintf(line, sizeof(line), "[NETSTAT %s] ",
                is_host ? "host" : "client");
            if (len >= sizeof(line)) {
                len = sizeof(line) - 1;
            }
            len = tig_net_stat_append_dir(line, len, sizeof(line),
                "tx", tig_net_stat_tx_pk, tig_net_stat_tx_bytes, elapsed);
            len = tig_net_stat_cat(line, len, sizeof(line), " | ");
            (void)tig_net_stat_append_dir(line, len, sizeof(line),
                "rx", tig_net_stat_rx_pk, tig_net_stat_rx_bytes, elapsed);
            tig_debug_printf("%s\n", line);
        }
    }
    tig_net_stat_logged = true;
    memset(tig_net_stat_tx_pk, 0, sizeof(tig_net_stat_tx_pk));
    memset(tig_net_stat_rx_pk, 0, sizeof(tig_net_stat_rx_pk));
    memset(tig_net_stat_tx_bytes, 0, sizeof(tig_net_stat_tx_bytes));
    memset(tig_net_stat_rx_bytes, 0, sizeof(tig_net_stat_rx_bytes));
    tig_net_stat_window_start = now;
}
static TigNetClient clients[TIG_NET_MAX_PLAYERS];
static int local_slot;
static int last_sender_slot;
static int max_players = TIG_NET_MAX_PLAYERS;
static unsigned int server_options;
static char server_name[64] = "Arcanum";
static char server_description[128] = "Description";
static char local_name[TIG_NET_MAX_NAME] = "Player"; /* PT58: ARCANUM_COOP_NAME via set_name */

static int last_message_size;
static int local_suspend_depth;
/* Client: host connection dropped during this pump; tear down after it. */
static bool lost_host;
/* Client: the host's protocol doesn't match ours. Sticky until the join
   target is configured again: the automatic rejoin (New Game) sends its
   CHAR_OFFER before WELCOME, so it must not reconnect at all. */
static bool host_refused;
static TigNetConnectWaitFn connect_wait_fn;

static TigNetMessageFunc message_func;
static TigNetMessageValidationFunc validation_func;
static TigNetNetworkEventFunc network_event_func;

static void tig_net_clear_clients(void)
{
    int i;

    for (i = 0; i < TIG_NET_MAX_PLAYERS; i++) {
        clients[i].peer = NULL;
        clients[i].active = false;
        clients[i].waiting = false;
        clients[i].loading = false;
        clients[i].refused = false;
        clients[i].handshaken = false;
        clients[i].connect_ms = 0;
        clients[i].name[0] = '\0';
    }
}

static int tig_net_alloc_slot(void)
{
    int i;

    /* Slot 0 is always the host. */
    for (i = 1; i < max_players; i++) {
        if (!clients[i].active) {
            return i;
        }
    }
    return -1;
}

/*
 * ENet peer->data holds (slot + 1) so that NULL (a rejected / not yet
 * assigned peer) decodes to -1 rather than to the host slot 0.
 */
static void tig_net_peer_set_slot(TigNetTPeer* peer, int slot)
{
    *tp->peer_data(peer) = (void*)(intptr_t)(slot + 1);
}

static int tig_net_peer_get_slot(TigNetTPeer* peer)
{
    int slot;

    if (peer == NULL) {
        return -1;
    }
    slot = (int)(intptr_t)*tp->peer_data(peer) - 1;
    if (slot < 0 || slot >= TIG_NET_MAX_PLAYERS) {
        return -1;
    }
    /* Must be the peer we actually registered for that slot. */
    if (clients[slot].peer != peer) {
        return -1;
    }
    return slot;
}

static void tig_net_fire_event(int type, int client_id, void* data, int size)
{
    if (network_event_func != NULL) {
        network_event_func(type, client_id, data, size);
    }
}

/* PT55: per-message log budget against a misbehaving peer filling the log:
   the first 20, then every 1000th (with the running total). */
bool tig_net_log_budget(unsigned int* counter)
{
    unsigned int n;

    if (counter == NULL) {
        return true;
    }
    n = ++*counter;
    return n <= 20u || n % 1000u == 0u;
}

static void tig_net_dispatch_app(const void* data, size_t size, int sender_slot)
{
    static unsigned int log_size_n;
    static unsigned int log_valid_n;

    /* Review PT24 #10: handlers cast packet structs onto this buffer, so
       give it the strictest scalar alignment. */
    union {
        char bytes[TIG_NET_MAX_PACKET];
        int64_t i64;
        double d;
        void* p;
    } copy_buf;
    char* copy = copy_buf.bytes;

    if (size < sizeof(int) || size > sizeof(copy_buf.bytes)) {
        if (tig_net_log_budget(&log_size_n)) {
            tig_debug_printf("tig_net: dropping oversized/undersized app packet (%zu; %u so far)\n", size, log_size_n);
        }
        return;
    }

    memcpy(copy, data, size);
    last_sender_slot = sender_slot;
    last_message_size = (int)size;

    if (validation_func != NULL && !validation_func(copy)) {
        if (tig_net_log_budget(&log_valid_n)) {
            tig_debug_printf("tig_net: validation failed for packet type %d (size %zu, from %d; %u so far)\n",
                *(int*)copy, size, sender_slot, log_valid_n);
        }
    } else if (message_func != NULL) {
        message_func(copy);
    }

    /* Outside dispatch, "sender" is the local peer. */
    last_sender_slot = local_slot;
    last_message_size = 0;
}

static void tig_net_send_ctrl_to_peer(TigNetTPeer* peer, const void* data, size_t size)
{
    if (peer == NULL || !enet_host_open()) {
        return;
    }
    tp->send(&peer, 1, TIG_NET_CHANNEL_CTRL, data, size, true);
}

static void tig_net_handle_ctrl(const void* data, size_t size, int from_slot)
{
    int magic;

    if (size < sizeof(int)) {
        return;
    }

    memcpy(&magic, data, sizeof(magic));
    if (magic == TIG_NET_CTRL_WELCOME) {
        TigNetCtrlWelcome welcome_copy;
        const TigNetCtrlWelcome* welcome = &welcome_copy;

        /* Only a client accepts WELCOME, only from the host, only once,
           and only with a sane client slot (never 0 = host). */
        if (is_host || from_slot != 0) {
            tig_debug_printf("tig_net: ignoring WELCOME (host=%d from=%d size=%zu)\n",
                is_host ? 1 : 0, from_slot, size);
            return;
        }
        memset(&welcome_copy, 0, sizeof(welcome_copy));
        memcpy(&welcome_copy, data, size < sizeof(welcome_copy) ? size : sizeof(welcome_copy));
        if (size < sizeof(TigNetCtrlWelcome) || welcome->protocol != TIG_NET_PROTOCOL) {
            if (size < sizeof(TigNetCtrlWelcome)) {
                /* An older host sends a shorter WELCOME. */
                tig_debug_printf("tig_net: refusing old host (WELCOME %zu bytes, no protocol field; ours is %d) - run the same build on both\n",
                    size, TIG_NET_PROTOCOL);
            } else {
                tig_debug_printf("tig_net: refusing host: protocol %d != ours %d - run the same build on both\n",
                    welcome->protocol, TIG_NET_PROTOCOL);
            }
            if (local_slot == 0) {
                host_refused = true; /* never welcomed: no app traffic either */
                if (clients[0].peer != NULL) {
                    tp->disconnect(clients[0].peer, false);
                }
            }
            return;
        }
        if (!tig_net_welcome_slot_valid(welcome->slot)) {
            tig_debug_printf("tig_net: ignoring WELCOME with bad slot %d\n", welcome->slot);
            return;
        }
        if (local_slot != 0) {
            tig_debug_printf("tig_net: ignoring duplicate WELCOME\n");
            return;
        }
        local_slot = welcome->slot;
        max_players = welcome->max_players;
        if (max_players < 1 || max_players > TIG_NET_MAX_PLAYERS) {
            max_players = TIG_NET_MAX_PLAYERS;
        }
        clients[local_slot].active = true;
        strncpy(clients[local_slot].name, local_name, TIG_NET_MAX_NAME - 1);
        tig_debug_printf("tig_net: welcomed as slot %d\n", local_slot);
        tig_net_fire_event(TIG_NET_EVENT_CONNECT, local_slot, NULL, 0);

        /* Our HELLO already went out on the app channel at CONNECT. */
        return;
    }

    if (magic == TIG_NET_CTRL_NAME || magic == TIG_NET_APP_HELLO) {
        TigNetCtrlName name_pkt;

        /* Host only; the slot is the authenticated sender, not the payload. */
        if (!is_host || from_slot <= 0 || from_slot >= TIG_NET_MAX_PLAYERS) {
            return;
        }
        memset(&name_pkt, 0, sizeof(name_pkt));
        memcpy(&name_pkt, data, size < sizeof(name_pkt) ? size : sizeof(name_pkt));
        if (size < sizeof(TigNetCtrlName) || name_pkt.protocol != TIG_NET_PROTOCOL) {
            if (size < sizeof(TigNetCtrlName)) {
                /* An older client sends a shorter NAME. */
                tig_debug_printf("tig_net: refusing old client in slot %d (NAME %zu bytes, no protocol field; ours is %d) - run the same build on both\n",
                    from_slot, size, TIG_NET_PROTOCOL);
            } else {
                tig_debug_printf("tig_net: refusing slot %d: protocol %d != ours %d - run the same build on both\n",
                    from_slot, name_pkt.protocol, TIG_NET_PROTOCOL);
            }
            /* Its CHAR_OFFER etc. may already be queued behind the NAME:
               drop everything from it until the disconnect lands. */
            clients[from_slot].refused = true;
            if (clients[from_slot].peer != NULL) {
                tp->disconnect(clients[from_slot].peer, false);
            }
            return;
        }
        if (clients[from_slot].handshaken) {
            return; /* duplicate */
        }
        name_pkt.name[TIG_NET_MAX_NAME - 1] = '\0';
        strncpy(clients[from_slot].name, name_pkt.name, TIG_NET_MAX_NAME - 1);
        clients[from_slot].name[TIG_NET_MAX_NAME - 1] = '\0';
        clients[from_slot].handshaken = true;
        tig_debug_printf("tig_net: slot %d handshake ok (protocol %d)\n", from_slot, TIG_NET_PROTOCOL);
        /* The game hears about the slot only now (player list, slot init). */
        tig_net_fire_event(TIG_NET_EVENT_CONNECT, from_slot, NULL, 0);
    }
}

/* Client: first packet on the APP channel, sent the moment ENet connects. */
static void tig_net_send_hello(TigNetTPeer* peer)
{
    TigNetCtrlName hello;

    memset(&hello, 0, sizeof(hello));
    hello.magic = TIG_NET_APP_HELLO;
    hello.slot = 0; /* the host uses the authenticated peer, not this */
    strncpy(hello.name, local_name, TIG_NET_MAX_NAME - 1);
    hello.protocol = TIG_NET_PROTOCOL;
    if (tp->send(&peer, 1, TIG_NET_CHANNEL_APP, &hello, sizeof(hello), true) != 1) {
        return;
    }
    tp->flush();
}

bool tig_net_welcome_slot_valid(int slot)
{
    return slot > 0 && slot < TIG_NET_MAX_PLAYERS;
}

void tig_net_set_coop_enabled(bool enabled)
{
    coop_enabled = enabled;
}

bool tig_net_join_configured(void)
{
    return want_join && join_host[0] != '\0';
}

bool tig_net_is_coop_enabled(void)
{
    return coop_enabled;
}

/* PT37: one line for the main menu when co-op was set up by command line flags. */
bool tig_net_describe_coop_mode(char* buf, size_t size)
{
    /* PT55: the menu line was always "RU / EN" (never went through the
       language pick); English unless ARCANUM_COOP_LANG=ru. */
    const char* lang = getenv("ARCANUM_COOP_LANG");
    bool ru = lang != NULL && (lang[0] == 'r' || lang[0] == 'R') && (lang[1] == 'u' || lang[1] == 'U');

    if (!coop_enabled || buf == NULL || size == 0) {
        return false;
    }
    if (want_host && tp != NULL && tp != tig_net_transport_enet()) {
        char id[32];

        /* PT58: friends join with this (shown on the main menu). */
        if (!tp->local_id(id, sizeof(id))) {
            snprintf(id, sizeof(id), "%s", "?");
        }
        snprintf(buf, size, ru ? "Кооп: хост Steam / Co-op: Steam host, ID %s" : "Co-op: Steam host, ID %s", id);
    } else if (want_host) {
#ifdef ARCANUM_UPNP
        char upnp[256];

        coop_upnp_state(); /* reaps a finished worker, logs once */
        coop_upnp_describe(upnp, sizeof(upnp));
        snprintf(buf, size, ru ? "Кооп: хост / Co-op: host, port %d - %s" : "Co-op: host, port %d - %s", listen_port, upnp);
#else
        snprintf(buf, size, ru ? "Кооп: хост / Co-op: host, port %d" : "Co-op: host, port %d", listen_port);
#endif
    } else if (want_join && active) {
        snprintf(buf, size, ru ? "Кооп: подключено / Co-op: connected, %s:%d" : "Co-op: connected, %s:%d", join_host, join_port);
    } else if (want_join) {
        snprintf(buf, size, ru ? "Кооп: подключение / Co-op: joining %s:%d" : "Co-op: joining %s:%d", join_host, join_port);
    } else {
        snprintf(buf, size, "%s", ru ? "Кооп / Co-op Mode" : "Co-op Mode");
    }
    return true;
}

void tig_net_configure_host(int port)
{
    want_host = true;
    want_join = false;
    if (port > 0 && port < 65536) {
        listen_port = port;
    }
}

void tig_net_configure_join(const char* host, int port)
{
    want_join = true;
    want_host = false;
    host_refused = false; /* a new join target gets a fresh check */
    if (host != NULL) {
        strncpy(join_host, host, sizeof(join_host) - 1);
        join_host[sizeof(join_host) - 1] = '\0';
    }
    if (port > 0 && port < 65536) {
        join_port = port;
    }
}

int tig_net_start_client(void)
{
    if (!coop_enabled) {
        return TIG_OK;
    }

    if (!library_ready) {
        if (!tig_net_tp()->init()) {
            return TIG_ERR_GENERIC;
        }
        library_ready = true;
    }

    tig_net_clear_clients();
    local_slot = 0;
    last_sender_slot = 0;
    active = false;
    is_host = false;

    if (want_host) {
        tig_net_start_server();
        return active ? TIG_OK : TIG_ERR_GENERIC;
    }

    if (want_join) {
        if (host_refused) {
            tig_debug_printf("tig_net: not rejoining %s:%d - its protocol doesn't match ours (%d); restart with the same build on both\n",
                join_host, join_port, TIG_NET_PROTOCOL);
            return TIG_ERR_GENERIC;
        }
        return tig_net_connect_configured();
    }

    /* Coop enabled but no host/join yet — subsystem ready. */
    return TIG_OK;
}

void tig_net_start_server(void)
{
    if (!coop_enabled) {
        return;
    }

    if (!library_ready && tig_net_start_client() != TIG_OK) {
        return;
    }

    if (enet_host_open()) {
        return;
    }

    if (!tp->listen(listen_port, max_players, TIG_NET_NUM_CHANNELS)) {
        tig_debug_printf("tig_net: failed to listen on port %d (%s)\n", listen_port, tp->name);
        return;
    }

    is_host = true;
    active = true;
    local_slot = 0;
    clients[0].active = true;
    strncpy(clients[0].name, local_name, TIG_NET_MAX_NAME - 1);
    tig_debug_printf("tig_net: hosting on port %d (max %d, %s)\n", listen_port, max_players, tp->name);
#ifdef ARCANUM_UPNP
    if (tp == tig_net_transport_enet()) {
        coop_upnp_start(listen_port); /* PT51: async, never blocks */
    }
#endif
    tig_net_fire_event(TIG_NET_EVENT_CONNECT, 0, NULL, 0);
}

void tig_net_set_connect_wait(TigNetConnectWaitFn fn)
{
    connect_wait_fn = fn;
}

int tig_net_connect_configured(void)
{
    TigNetTPeer* peer;
    TigNetTEvent event;
    const char* target;
    int waits;

    if (!coop_enabled) {
        return TIG_OK;
    }

    if (!library_ready && !tig_net_tp()->init()) {
        return TIG_ERR_GENERIC;
    }
    library_ready = true;

    if (enet_host_open()) {
        if (active) {
            return TIG_OK;
        }
        /* Review M1: the host dropped us earlier; that ENet host is dead. */
        tig_debug_printf("tig_net: discarding stale client connection before reconnect\n");
        tig_net_reset_connection();
    }

    /* PT58: Steam joins a SteamID64 (ARCANUM_COOP_STEAM_HOST wins over -join). */
    target = join_host;
    if (tp != tig_net_transport_enet() && getenv("ARCANUM_COOP_STEAM_HOST") != NULL
        && getenv("ARCANUM_COOP_STEAM_HOST")[0] != '\0') {
        target = getenv("ARCANUM_COOP_STEAM_HOST");
    }
    if (target[0] == '\0') {
        tig_debug_printf("tig_net: join host not configured\n");
        return TIG_ERR_GENERIC;
    }

    peer = tp->connect(target, join_port, TIG_NET_NUM_CHANNELS);
    if (peer == NULL) {
        return TIG_ERR_GENERIC;
    }

    is_host = false;
    active = false;

    /* PT58: Steam relays may need several seconds to route the first packet.
       PT77q: 20 ms slices (same 5 s / 20 s budget) so the game can pump
       SDL events and abort on Esc; a 100 ms block made the window
       "Not Responding". */
    {
        int max_waits = (tp == tig_net_transport_enet() ? 50 : 200) * 5;

        for (waits = 0; waits < max_waits; waits++) {
            if (connect_wait_fn != NULL && connect_wait_fn()) {
                tig_debug_printf("tig_net: connect cancelled\n");
                tp->reset_peer(peer);
                tp->close();
                return TIG_ERR_GENERIC;
            }
            if (tp->service(&event, 20) <= 0) {
                continue;
            }
            if (event.type == TIG_NET_TEV_RECEIVE) {
                /* Not expected before CONNECT; never leak the packet. */
                tp->event_done(&event);
                continue;
            }
            if (event.type == TIG_NET_TEV_DISCONNECT) {
                break; /* refused / unreachable */
            }
            if (event.type == TIG_NET_TEV_CONNECT) {
                clients[0].peer = peer;
                clients[0].active = true;
                tig_net_peer_set_slot(peer, 0);
                active = true;
                /* PT57: a crashed / frozen host is noticed in ~10 s (ENet's
                   default is up to 30 s of a frozen client screen). */
                tp->set_timeout(peer, 4000, 10000);
                tig_debug_printf("tig_net: connected to %s:%d (%s)\n", target, join_port, tp->name);
                tig_net_send_hello(peer);
                /* Slot assignment arrives via WELCOME ctrl packet. */
                return TIG_OK;
            }
        }
    }

    tig_debug_printf("tig_net: timed out connecting to %s:%d (%s)\n", target, join_port, tp->name);
    tp->reset_peer(peer);
    tp->close();
    return TIG_ERR_GENERIC;
}

void tig_net_reset_connection(void)
{
    int i;

#ifdef ARCANUM_UPNP
    if (is_host && tp == tig_net_transport_enet()) {
        coop_upnp_stop(); /* PT51: drop the router mapping */
    }
#endif

    if (enet_host_open()) {
        if (tig_net_send_pending_flush) {
            tp->flush();
            tig_net_send_pending_flush = false;
        }
        for (i = 0; i < TIG_NET_MAX_PLAYERS; i++) {
            if (clients[i].peer != NULL) {
                tp->disconnect(clients[i].peer, true);
                clients[i].peer = NULL;
            }
        }
        tp->close();
    }

    tig_net_clear_clients();
    active = false;
    is_host = false;
    local_slot = 0;
}

int sub_5280F0(void)
{
    bool was_active = active;
    tig_net_reset_connection();
    /* Original returns non-zero if shutdown did not complete cleanly.
       multiplayer_end does `return !v1`. */
    return was_active ? 0 : 1;
}

static void tig_net_peer_diag_log(void)
{
    int i;

    /* PT23: queue depth / RTT per peer — a growing outgoing queue means
       latency that grows over a session ("everything gets laggy"). */
    for (i = 0; i < max_players; i++) {
        TigNetTPeer* p = clients[i].peer;
        char desc[256];

        if (p == NULL || (is_host && i == 0) || (!is_host && i != 0)) {
            continue;
        }
        tp->describe_peer(p, desc, sizeof(desc));
        tig_debug_printf("tig_net: peer %d %s\n", i, desc);
    }
}

void tig_net_pump(void)
{
    TigNetTEvent event;

    if (!coop_enabled || !enet_host_open()) {
        return;
    }

    /* Review PT24 #4: bounded work per frame; the rest stays queued in
       ENet and is handled next frame. */
    int budget = TIG_NET_MAX_EVENTS_PER_PUMP;

    while (budget-- > 0 && enet_host_open() && tp->service(&event, 0) > 0) {
        switch (event.type) {
        case TIG_NET_TEV_CONNECT:
            if (is_host) {
                int slot = tig_net_alloc_slot();
                TigNetCtrlWelcome welcome;

                if (slot < 0) {
                    tig_debug_printf("tig_net: rejecting client, no free slots\n");
                    *tp->peer_data(event.peer) = NULL; /* decodes to -1 */
                    tp->disconnect(event.peer, false);
                    break;
                }

                clients[slot].peer = event.peer;
                tig_net_peer_set_slot(event.peer, slot);
                clients[slot].active = true;
                clients[slot].waiting = false;
                clients[slot].loading = false;
                clients[slot].refused = false;
                clients[slot].handshaken = false;
                clients[slot].connect_ms = tp->now_ms();
                snprintf(clients[slot].name, sizeof(clients[slot].name), "Player%d", slot);

                memset(&welcome, 0, sizeof(welcome));
                welcome.magic = TIG_NET_CTRL_WELCOME;
                welcome.slot = slot;
                welcome.max_players = max_players;
                welcome.protocol = TIG_NET_PROTOCOL;
                tig_net_send_ctrl_to_peer(event.peer, &welcome, sizeof(welcome));
                tp->flush();

                tig_debug_printf("tig_net: client connected -> slot %d (awaiting handshake)\n", slot);
            } else {
                clients[0].peer = event.peer;
                tig_net_peer_set_slot(event.peer, 0);
                tig_net_send_hello(event.peer);
            }
            break;

        case TIG_NET_TEV_RECEIVE:
            {
                int from = tig_net_peer_get_slot(event.peer);
                if (from < 0) {
                    /* Rejected / unassigned peer: drop everything it sends. */
                } else if (event.channel == TIG_NET_CHANNEL_CTRL) {
                    tig_net_handle_ctrl(event.data, event.size, from);
                } else if (is_host && from > 0
                    && event.size >= sizeof(int)
                    && *(const int*)event.data == TIG_NET_APP_HELLO) {
                    tig_net_handle_ctrl(event.data, event.size, from);
                } else if (is_host ? clients[from].refused : host_refused) {
                    /* Protocol mismatch: disconnect pending, drop it. */
                } else if (is_host && from > 0 && !clients[from].handshaken) {
                    /* No valid HELLO yet: app traffic is not accepted. */
                    tig_debug_printf("tig_net: dropping app packet from slot %d before handshake\n", from);
                } else if (!is_host && from != 0) {
                    /* Clients only accept app traffic from the host. */
                } else if (is_host && from == 0) {
                    /* Host never receives from itself over the wire. */
                } else {
                    tig_net_stat_add(tig_net_stat_rx_pk, tig_net_stat_rx_bytes, event.data, event.size, 1);
                    tig_net_dispatch_app(event.data, event.size, from);
                }
            }
            tp->event_done(&event);
            break;

        case TIG_NET_TEV_DISCONNECT: {
            int slot = tig_net_peer_get_slot(event.peer);
            tig_debug_printf("tig_net: disconnect slot %d\n", slot);
            /* Unassigned/rejected peers (slot -1) must not clear anything. */
            if (slot >= 0 && slot < TIG_NET_MAX_PLAYERS) {
                clients[slot].peer = NULL;
                clients[slot].active = false;
                clients[slot].waiting = false;
                clients[slot].loading = false;
                clients[slot].refused = false;
                clients[slot].handshaken = false;
                clients[slot].name[0] = '\0';
                tig_net_fire_event(TIG_NET_EVENT_DISCONNECT, slot, NULL, 0);
            }
            *tp->peer_data(event.peer) = NULL;
            if (!is_host) {
                active = false;
                lost_host = true;
            }
            break;
        }

        default:
            break;
        }
    }

    if (is_host && enet_host_open()) {
        unsigned int now = tp->now_ms();
        int i;

        for (i = 1; i < max_players; i++) {
            if (clients[i].peer != NULL && !clients[i].handshaken && !clients[i].refused
                && now - clients[i].connect_ms > TIG_NET_HANDSHAKE_TIMEOUT_MS) {
                tig_debug_printf("tig_net: slot %d sent no valid handshake in %u ms - disconnecting\n",
                    i, TIG_NET_HANDSHAKE_TIMEOUT_MS);
                clients[i].refused = true;
                tp->disconnect(clients[i].peer, false);
            }
        }
    }
    if (tig_net_send_pending_flush && enet_host_open()) {
        tp->flush();
        tig_net_send_pending_flush = false;
    }
    if (lost_host) {
        /* Review M1: free the dead client host so a later Join works. */
        lost_host = false;
        tig_net_reset_connection();
    }
    tig_net_stat_maybe_log();
    if (tig_net_stat_logged) {
        tig_net_stat_logged = false;
        tig_net_peer_diag_log();
    }
}

bool tig_net_is_active(void)
{
    return coop_enabled && active && local_suspend_depth == 0;
}

bool tig_net_is_host(void)
{
    return coop_enabled && active && is_host && local_suspend_depth == 0;
}

bool tig_net_is_coop_client(void)
{
    return coop_enabled && active && !is_host;
}

void tig_net_local_suspend_begin(void)
{
    local_suspend_depth++;
}

void tig_net_local_suspend_end(void)
{
    if (local_suspend_depth > 0) {
        local_suspend_depth--;
    }
}

int tig_net_local_suspend_pause(void)
{
    int depth = local_suspend_depth;

    local_suspend_depth = 0;
    return depth;
}

void tig_net_local_suspend_resume(int depth)
{
    local_suspend_depth = depth > 0 ? depth : 0;
}

bool tig_net_is_locally_suspended(void)
{
    return local_suspend_depth > 0;
}

int tig_net_last_message_size(void)
{
    return last_message_size;
}

void tig_net_on_message(TigNetMessageFunc func)
{
    message_func = func;
}

void tig_net_on_message_validation(TigNetMessageValidationFunc func)
{
    validation_func = func;
}

void tig_net_on_network_event(TigNetNetworkEventFunc func)
{
    network_event_func = func;
}

static void tig_net_send_raw_ex(void* data, int size, int client_id, bool all, bool except, bool pose)
{
    TigNetTPeer* peers[TIG_NET_MAX_PLAYERS];
    int n = 0;
    int i;
    int sent;

    if (!tig_net_is_active() || data == NULL || size <= 0 || size > TIG_NET_MAX_PACKET || !enet_host_open()) {
        return;
    }
    if (!is_host && host_refused) {
        return; /* refused host (protocol): nothing of ours reaches it */
    }

    if (is_host) {
        for (i = 1; i < max_players; i++) {
            if (!clients[i].active || clients[i].peer == NULL) {
                continue;
            }
            if (all) {
                /* send to everyone */
            } else if (except) {
                if (i == client_id) {
                    continue;
                }
            } else {
                if (i != client_id) {
                    continue;
                }
            }
            peers[n++] = clients[i].peer;
        }
    } else if (clients[0].peer != NULL) {
        /* Clients only talk to host. */
        peers[n++] = clients[0].peer;
    }
    if (n == 0) {
        return;
    }
    /* Review M7: poses unreliable+sequenced on their own channel. */
    sent = tp->send(peers, n, pose ? TIG_NET_CHANNEL_POSE : TIG_NET_CHANNEL_APP, data, (size_t)size, !pose);
    if (sent > 0) {
        /* Batched: flushed once per frame in tig_net_pump (was per packet). */
        tig_net_send_pending_flush = true;
        tig_net_stat_add(tig_net_stat_tx_pk, tig_net_stat_tx_bytes, data, (size_t)size, sent);
    }
}

static void tig_net_send_raw(void* data, int size, int client_id, bool all, bool except)
{
    tig_net_send_raw_ex(data, size, client_id, all, except, false);
}

void tig_net_send_pose_all(void* data, int size)
{
    tig_net_send_raw_ex(data, size, -1, true, false, true);
}

void tig_net_send_pose_except(void* data, int size, int client_id)
{
    tig_net_send_raw_ex(data, size, client_id, false, true, true);
}

void tig_net_set_remote_slot_present(int slot, bool present)
{
    /* Client bookkeeping only (review H3): other clients' slots exist so
       their PCs resolve; clients never send to them (only to slot 0). */
    if (is_host || slot <= 0 || slot >= TIG_NET_MAX_PLAYERS || slot == local_slot) {
        return;
    }
    clients[slot].active = present;
    clients[slot].peer = NULL;
}

void tig_net_send_app_all(void* data, int size)
{
    tig_net_send_raw(data, size, -1, true, false);
}

void tig_net_send_app(void* data, int size, int client_id)
{
    tig_net_send_raw(data, size, client_id, false, false);
}

void tig_net_send_app_except(void* data, int size, int client_id)
{
    tig_net_send_raw(data, size, client_id, false, true);
}

int tig_net_local_client_set_name(const char* name)
{
    const char* env_name = getenv("ARCANUM_COOP_NAME");

    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    /* PT58: the launcher's player name replaces the default "Player". */
    if (strcmp(name, "Player") == 0 && env_name != NULL && env_name[0] != '\0') {
        name = env_name;
    }
    strncpy(local_name, name, TIG_NET_MAX_NAME - 1);
    local_name[TIG_NET_MAX_NAME - 1] = '\0';
    if (local_slot >= 0 && local_slot < TIG_NET_MAX_PLAYERS) {
        strncpy(clients[local_slot].name, local_name, TIG_NET_MAX_NAME - 1);
    }
    return 1;
}

int tig_net_local_server_set_max_players(int value)
{
    if (value < 1) {
        value = 1;
    }
    if (value > TIG_NET_MAX_PLAYERS) {
        value = TIG_NET_MAX_PLAYERS;
    }
    max_players = value;
    return 1;
}

int tig_net_local_server_set_description(const char* description)
{
    if (description == NULL) {
        return 0;
    }
    strncpy(server_description, description, sizeof(server_description) - 1);
    return 1;
}

void tig_net_local_server_set_name(const char* name)
{
    if (name == NULL) {
        return;
    }
    strncpy(server_name, name, sizeof(server_name) - 1);
}

int tig_net_local_server_get_max_players(void)
{
    return max_players;
}

unsigned int tig_net_local_server_get_options(void)
{
    return server_options;
}

void tig_net_local_server_set_options(unsigned int options)
{
    server_options = options;
}

/* PT77n: host drops a slot right away (same player reconnected on a new
   slot before ENet timed the old peer out). Same cleanup and event as a
   transport DISCONNECT, so the game parks that slot's PC synchronously. */
bool tig_net_kick_slot(int slot)
{
    TigNetTPeer* peer;

    if (!is_host || slot <= 0 || slot >= TIG_NET_MAX_PLAYERS || clients[slot].peer == NULL) {
        return false;
    }
    peer = clients[slot].peer;
    tig_debug_printf("tig_net: kicking stale slot %d\n", slot);
    *tp->peer_data(peer) = NULL;
    tp->disconnect(peer, true);
    clients[slot].peer = NULL;
    clients[slot].active = false;
    clients[slot].waiting = false;
    clients[slot].loading = false;
    clients[slot].refused = false;
    clients[slot].handshaken = false;
    clients[slot].name[0] = '\0';
    tig_net_fire_event(TIG_NET_EVENT_DISCONNECT, slot, NULL, 0);
    return true;
}

bool tig_net_client_is_active(int client_id)
{
    if (client_id < 0 || client_id >= TIG_NET_MAX_PLAYERS) {
        return false;
    }
    return clients[client_id].active;
}

bool tig_net_client_is_waiting(int client_id)
{
    if (client_id < 0 || client_id >= TIG_NET_MAX_PLAYERS) {
        return false;
    }
    return clients[client_id].waiting;
}

bool tig_net_client_is_loading(int client_id)
{
    if (client_id < 0 || client_id >= TIG_NET_MAX_PLAYERS) {
        return false;
    }
    return clients[client_id].loading;
}

const char* tig_net_client_info_get_name(int client_id)
{
    if (client_id < 0 || client_id >= TIG_NET_MAX_PLAYERS) {
        return "";
    }
    if (clients[client_id].name[0] == '\0') {
        return local_name;
    }
    return clients[client_id].name;
}

int tig_net_xfer_count(int client_id)
{
    (void)client_id;
    return 0;
}

void tig_net_xfer_send_as(const char* src, const char* dst, int client_id, void* unused)
{
    (void)src;
    (void)dst;
    (void)client_id;
    (void)unused;
}

void tig_net_xfer_send(const char* path, int client_id, void* unused)
{
    (void)path;
    (void)client_id;
    (void)unused;
}

int sub_529520(void)
{
    return local_slot;
}

int sub_52A530(void)
{
    return last_sender_slot;
}

int sub_52A900(void)
{
    /* Non-zero pauses game/anim time for clients waiting on sync. */
    return 0;
}

void sub_52A940(void)
{
}

void sub_52A950(void)
{
}

void sub_52B210(void)
{
}

void sub_5286E0(void)
{
}

void sub_52A9E0(int client_id)
{
    if (client_id >= 0 && client_id < TIG_NET_MAX_PLAYERS) {
        clients[client_id].loading = false;
        clients[client_id].waiting = false;
    }
}

bool tig_net_host_refused(void)
{
    return host_refused;
}
