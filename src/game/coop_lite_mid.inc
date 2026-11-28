/*
 * Coop Middle (protocol 53). Included at the end of coop_lite.c (it shares
 * that file's statics: chat hooks, notices, rate limits, sync helpers).
 *
 * Pure helpers first (built into the policy tests too), then the game side
 * under !COOP_LITE_TEST_STUBS:
 *   - host rules (CoopRules) sent to every client right after it joins
 *   - loot feed, gold split, rest vote, direct trade, portrait marks
 *   - pings, shared pause, difficulty scaling, /tp + /gather, F9 net HUD
 *   - shared quest pocket, shared town map fog, follower trade permission
 * Everything new rides one multiplexed packet (PACKET_COOP_MID) plus the
 * town map bit array (PACKET_COOP_TOWNMAP).
 */

#include <ctype.h>

/* ---- pure ---------------------------------------------------------------- */

void coop_lite_rules_default(CoopRules* r)
{
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof(*r));
    r->flags = COOP_RULE_FF_AOE | COOP_RULE_REST_VOTE | COOP_RULE_TP | COOP_RULE_QUEST_POCKET
        | COOP_RULE_SHARED_FOG | COOP_RULE_LOOT_FEED | COOP_RULE_PINGS;
    r->hp_scale_pct = 0;
    r->xp_mult_pct = 100;
    r->tp_cooldown_s = 30;
}

void coop_lite_rules_sanitize(CoopRules* r)
{
    if (r == NULL) {
        return;
    }
    r->flags &= COOP_RULE_ALL;
    if (r->hp_scale_pct < 0) {
        r->hp_scale_pct = 0;
    } else if (r->hp_scale_pct > 100) {
        r->hp_scale_pct = 100;
    }
    if (r->xp_mult_pct < 100) {
        r->xp_mult_pct = 100;
    } else if (r->xp_mult_pct > 200) {
        r->xp_mult_pct = 200;
    }
    if (r->tp_cooldown_s < 0) {
        r->tp_cooldown_s = 0;
    } else if (r->tp_cooldown_s > 600) {
        r->tp_cooldown_s = 600;
    }
}

static const struct {
    const char* key;
    unsigned int flag;
} coop_rule_keys[] = {
    { "ff", COOP_RULE_FF_AOE },
    { "gold", COOP_RULE_GOLD_SPLIT },
    { "pause", COOP_RULE_CLIENT_PAUSE },
    { "rest", COOP_RULE_REST_VOTE },
    { "tp", COOP_RULE_TP },
    { "tpconfirm", COOP_RULE_TP_CONFIRM },
    { "quest", COOP_RULE_QUEST_POCKET },
    { "fog", COOP_RULE_SHARED_FOG },
    { "feed", COOP_RULE_LOOT_FEED },
    { "pings", COOP_RULE_PINGS },
};

int coop_lite_rules_parse(const char* text, CoopRules* r)
{
    char key[32];
    char val[32];
    int understood = 0;
    const char* p = text;

    if (r == NULL) {
        return 0;
    }
    coop_lite_rules_default(r);
    if (text == NULL) {
        return 0;
    }
    while (*p != '\0') {
        size_t kn = 0;
        size_t vn = 0;
        size_t i;
        int v;

        while (*p == ',' || *p == ';' || *p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        while (*p != '\0' && *p != '=' && *p != ',' && *p != ';' && kn + 1 < sizeof(key)) {
            key[kn++] = (char)tolower((unsigned char)*p);
            p++;
        }
        key[kn] = '\0';
        while (kn > 0 && key[kn - 1] == ' ') {
            key[--kn] = '\0';
        }
        if (*p != '=') {
            while (*p != '\0' && *p != ',' && *p != ';') {
                p++;
            }
            continue;
        }
        p++;
        while (*p == ' ') {
            p++;
        }
        while (*p != '\0' && *p != ',' && *p != ';' && vn + 1 < sizeof(val)) {
            val[vn++] = *p++;
        }
        val[vn] = '\0';
        while (*p != '\0' && *p != ',' && *p != ';') {
            p++;
        }
        if (vn == 0) {
            continue;
        }
        if (strcmp(val, "on") == 0 || strcmp(val, "yes") == 0 || strcmp(val, "true") == 0) {
            v = 1;
        } else if (strcmp(val, "off") == 0 || strcmp(val, "no") == 0 || strcmp(val, "false") == 0) {
            v = 0;
        } else {
            v = atoi(val);
        }
        if (strcmp(key, "hp") == 0) {
            r->hp_scale_pct = v;
            understood++;
            continue;
        }
        if (strcmp(key, "xp") == 0) {
            r->xp_mult_pct = v;
            understood++;
            continue;
        }
        if (strcmp(key, "tpcd") == 0) {
            r->tp_cooldown_s = v;
            understood++;
            continue;
        }
        for (i = 0; i < sizeof(coop_rule_keys) / sizeof(coop_rule_keys[0]); i++) {
            if (strcmp(key, coop_rule_keys[i].key) == 0) {
                if (v != 0) {
                    r->flags |= coop_rule_keys[i].flag;
                } else {
                    r->flags &= ~coop_rule_keys[i].flag;
                }
                understood++;
                break;
            }
        }
    }
    coop_lite_rules_sanitize(r);
    return understood;
}

void coop_lite_rules_format(const CoopRules* r, char* out, size_t out_size)
{
    size_t i;
    size_t n;

    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (r == NULL) {
        return;
    }
    snprintf(out, out_size, "hp=%d,xp=%d,tpcd=%d", r->hp_scale_pct, r->xp_mult_pct, r->tp_cooldown_s);
    for (i = 0; i < sizeof(coop_rule_keys) / sizeof(coop_rule_keys[0]); i++) {
        n = strlen(out);
        if (n + 1 >= out_size) {
            break;
        }
        snprintf(out + n, out_size - n, ",%s=%d", coop_rule_keys[i].key,
            (r->flags & coop_rule_keys[i].flag) != 0 ? 1 : 0);
    }
}

void coop_lite_rules_describe(const CoopRules* r, bool ru, char* out, size_t out_size)
{
    char tp[48];

    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (r == NULL) {
        return;
    }
    if ((r->flags & COOP_RULE_TP) == 0) {
        snprintf(tp, sizeof(tp), "%s", ru ? "нет" : "off");
    } else if ((r->flags & COOP_RULE_TP_CONFIRM) != 0) {
        snprintf(tp, sizeof(tp), "%s", ru ? "с подтверждением хоста" : "host confirms");
    } else {
        snprintf(tp, sizeof(tp), ru ? "раз в %d с" : "every %d s", r->tp_cooldown_s);
    }
#define COOP_YN(f) ((r->flags & (f)) != 0 ? (ru ? "да" : "on") : (ru ? "нет" : "off"))
    snprintf(out, out_size,
        ru ? "Правила: HP врагов +%d%% за игрока, опыт x%d.%02d, урон по своим от площадных: %s, "
             "деление золота: %s, пауза клиентов: %s, отдых по запросу: %s, /tp: %s, "
             "общие квестовые предметы: %s, общий туман карты: %s, лента лута: %s, метки: %s"
           : "Rules: enemy HP +%d%% per extra player, XP x%d.%02d, area friendly fire %s, "
             "gold split %s, client pause %s, rest requests %s, /tp %s, "
             "shared quest items %s, shared town map %s, loot feed %s, pings %s",
        r->hp_scale_pct, r->xp_mult_pct / 100, r->xp_mult_pct % 100,
        COOP_YN(COOP_RULE_FF_AOE), COOP_YN(COOP_RULE_GOLD_SPLIT), COOP_YN(COOP_RULE_CLIENT_PAUSE),
        COOP_YN(COOP_RULE_REST_VOTE), tp, COOP_YN(COOP_RULE_QUEST_POCKET), COOP_YN(COOP_RULE_SHARED_FOG),
        COOP_YN(COOP_RULE_LOOT_FEED), COOP_YN(COOP_RULE_PINGS));
#undef COOP_YN
}

static CoopRules coop_mid_rules_cur;
static bool coop_mid_rules_inited;

#ifndef COOP_LITE_TEST_STUBS
static void coop_mid_rules_set(const CoopRules* r)
{
    coop_mid_rules_cur = *r;
    coop_lite_rules_sanitize(&coop_mid_rules_cur);
    coop_mid_rules_inited = true;
}
#endif

const CoopRules* coop_lite_rules(void)
{
    if (!coop_mid_rules_inited) {
        coop_lite_rules_default(&coop_mid_rules_cur);
        coop_mid_rules_inited = true;
    }
    return &coop_mid_rules_cur;
}

bool coop_lite_rule_on(unsigned int flag)
{
    return (coop_lite_rules()->flags & flag) != 0;
}

int coop_lite_scaled_hp(int base, int pct, int extra)
{
    long long v;

    if (base <= 0 || pct <= 0 || extra <= 0) {
        return base;
    }
    if (extra > 7) {
        extra = 7;
    }
    if (pct > 100) {
        pct = 100;
    }
    v = (long long)base + (long long)base * pct * extra / 100;
    return v > 2000000000LL ? 2000000000 : (int)v;
}

int coop_lite_scaled_xp(int xp, int pct)
{
    long long v;

    if (xp <= 0 || pct <= 100) {
        return xp;
    }
    if (pct > 200) {
        pct = 200;
    }
    v = (long long)xp * pct / 100;
    return v > 2000000000LL ? 2000000000 : (int)v;
}

bool coop_lite_mid_op_ok(int op, bool receiver_is_host)
{
    switch (op) {
    case COOP_MID_OP_STATUS:
    case COOP_MID_OP_REST:
    case COOP_MID_OP_PING:
    case COOP_MID_OP_PAUSE:
    case COOP_MID_OP_TP:
    case COOP_MID_OP_FOLLOWER_SHARE:
        return true; /* both directions */
    case COOP_MID_OP_GOLD_GIVE:
        return receiver_is_host;
    case COOP_MID_OP_RULES:
    case COOP_MID_OP_LOOT:
    case COOP_MID_OP_GOLD_SPLIT:
        return !receiver_is_host;
    default:
        return false;
    }
}

bool coop_lite_mid_valid(const PacketCoopMid* pkt, int size)
{
    return pkt != NULL && size == (int)sizeof(PacketCoopMid) && pkt->type == PACKET_COOP_MID
        && pkt->op > 0 && pkt->op < COOP_MID_OP_COUNT
        && memchr(pkt->text, '\0', sizeof(pkt->text)) != NULL;
}

bool coop_lite_townmap_valid(const PacketCoopTownmap* pkt, int size)
{
    return pkt != NULL && size == (int)sizeof(PacketCoopTownmap) && pkt->type == PACKET_COOP_TOWNMAP
        && pkt->map > 0 && pkt->map < 100000 && pkt->size > 0 && pkt->size <= COOP_TOWNMAP_BYTES;
}

int coop_lite_bits_union(unsigned char* dst, const unsigned char* src, int n)
{
    int i;
    int changed = 0;

    if (dst == NULL || src == NULL) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        unsigned char v = (unsigned char)(dst[i] | src[i]);

        if (v != dst[i]) {
            dst[i] = v;
            changed++;
        }
    }
    return changed;
}

unsigned int coop_lite_status_pack(unsigned int packed, int slot, unsigned int flags)
{
    if (slot < 0 || slot >= 8) {
        return packed;
    }
    packed &= ~(0xFu << (slot * 4));
    packed |= (flags & 0xFu) << (slot * 4);
    return packed;
}

unsigned int coop_lite_status_unpack(unsigned int packed, int slot)
{
    if (slot < 0 || slot >= 8) {
        return 0;
    }
    return (packed >> (slot * 4)) & 0xFu;
}

int coop_lite_status_letters(unsigned int ui_flags, bool combat, bool poisoned, bool unconscious,
    bool dead, char* out, size_t out_size)
{
    char buf[8];
    int n = 0;

    if (dead) {
        buf[n++] = 'X';
    } else {
        if ((ui_flags & COOP_STATUS_INVEN) != 0) {
            buf[n++] = 'I';
        }
        if ((ui_flags & COOP_STATUS_DIALOG) != 0) {
            buf[n++] = 'D';
        }
        if (combat) {
            buf[n++] = 'C';
        }
        if (poisoned) {
            buf[n++] = 'P';
        }
        if (unconscious) {
            buf[n++] = 'Z';
        }
    }
    buf[n] = '\0';
    if (out != NULL && out_size > 0) {
        snprintf(out, out_size, "%s", buf);
    }
    return n;
}

/* Raw values (obj.h / obj_flags.h / written.h), kept here so the pure part
   builds without the game headers. */
#define COOP_MID_T_GOLD 8
#define COOP_MID_T_KEY 11
#define COOP_MID_T_WRITTEN 13
#define COOP_MID_W_SCHEMATIC 5
#define COOP_MID_OIF_WONT_SELL 0x00000002u
#define COOP_MID_OIF_NO_DISPLAY 0x00000010u
#define COOP_MID_OIF_NO_DROP 0x00000020u

int coop_lite_loot_kind(int obj_type, int written_subtype, unsigned int item_flags, int worth)
{
    if ((item_flags & COOP_MID_OIF_NO_DISPLAY) != 0 || obj_type == COOP_MID_T_GOLD) {
        return COOP_LOOT_NONE;
    }
    if (obj_type == COOP_MID_T_KEY) {
        return COOP_LOOT_KEY;
    }
    if (obj_type == COOP_MID_T_WRITTEN && written_subtype == COOP_MID_W_SCHEMATIC) {
        return COOP_LOOT_SCHEMATIC;
    }
    if ((item_flags & (COOP_MID_OIF_WONT_SELL | COOP_MID_OIF_NO_DROP)) != 0) {
        return COOP_LOOT_QUEST;
    }
    if (worth >= COOP_LOOT_VALUABLE_WORTH) {
        return COOP_LOOT_VALUABLE;
    }
    return COOP_LOOT_NONE;
}

static const char* coop_mid_skip_ws(const char* p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

int coop_lite_chat_command(const char* text, int* amount, char* arg, size_t arg_size)
{
    char word[16];
    size_t n = 0;
    const char* p;

    if (amount != NULL) {
        *amount = 0;
    }
    if (arg != NULL && arg_size > 0) {
        arg[0] = '\0';
    }
    if (text == NULL) {
        return COOP_CMD_NONE;
    }
    p = coop_mid_skip_ws(text);
    if (*p != '/') {
        return COOP_CMD_NONE;
    }
    p++;
    while (*p != '\0' && *p != ' ' && *p != '\t' && n + 1 < sizeof(word)) {
        word[n++] = (char)tolower((unsigned char)*p++);
    }
    word[n] = '\0';
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        p++; /* overlong command word */
    }
    p = coop_mid_skip_ws(p);
    if (strcmp(word, "tp") == 0) {
        return COOP_CMD_TP;
    }
    if (strcmp(word, "gather") == 0) {
        return COOP_CMD_GATHER;
    }
    if (strcmp(word, "pause") == 0) {
        return COOP_CMD_PAUSE;
    }
    if (strcmp(word, "rules") == 0) {
        return COOP_CMD_RULES;
    }
    if (strcmp(word, "help") == 0 || strcmp(word, "?") == 0) {
        return COOP_CMD_HELP;
    }
    if (strcmp(word, "share") == 0) {
        if (amount != NULL) {
            *amount = (strncmp(p, "off", 3) == 0 || p[0] == '0') ? 0 : 1;
        }
        return COOP_CMD_FOLLOWERS;
    }
    if (strcmp(word, "gold") == 0) {
        long v = 0;
        bool digits = false;

        while (*p >= '0' && *p <= '9') {
            digits = true;
            if (v < 100000000L) {
                v = v * 10 + (*p - '0');
            }
            p++;
        }
        if (!digits || v <= 0) {
            return COOP_CMD_UNKNOWN;
        }
        if (amount != NULL) {
            *amount = (int)v;
        }
        p = coop_mid_skip_ws(p);
        if (arg != NULL && arg_size > 0) {
            size_t k;

            snprintf(arg, arg_size, "%s", p);
            k = strlen(arg);
            while (k > 0 && (arg[k - 1] == ' ' || arg[k - 1] == '\t')) {
                arg[--k] = '\0';
            }
        }
        return COOP_CMD_GOLD;
    }
    return COOP_CMD_UNKNOWN;
}

int coop_lite_tp_gate(unsigned int flags, bool in_combat, unsigned int last_ms, unsigned int now_ms,
    int cooldown_s)
{
    if ((flags & COOP_RULE_TP) == 0) {
        return COOP_MID_ANS_OFF;
    }
    if (in_combat) {
        return COOP_MID_ANS_COMBAT;
    }
    if ((flags & COOP_RULE_TP_CONFIRM) != 0) {
        return COOP_MID_ANS_ASKED;
    }
    if (last_ms != 0 && cooldown_s > 0
        && !coop_lite_interval_elapsed(last_ms, now_ms, (unsigned int)cooldown_s * 1000u)) {
        return COOP_MID_ANS_COOLDOWN;
    }
    return COOP_MID_ANS_YES;
}

int coop_lite_gold_share(int amount, int n)
{
    if (amount <= 0 || n <= 1) {
        return 0;
    }
    if (n > 8) {
        n = 8;
    }
    return amount / n;
}

void coop_lite_net_hud_format(int rtt_ms, int lost, int players, bool synced, char* out, size_t out_size)
{
    char ping[24];

    if (out == NULL || out_size == 0) {
        return;
    }
    if (rtt_ms < 0) {
        snprintf(ping, sizeof(ping), "-");
    } else {
        snprintf(ping, sizeof(ping), "%d ms", rtt_ms);
    }
    snprintf(out, out_size, "Ping %s | Loss %d | Players %d | Sync %s", ping, lost < 0 ? 0 : lost,
        players, synced ? "OK" : "WAIT");
}

void coop_lite_slot_color(int slot, int* r, int* g, int* b)
{
    static const unsigned char colors[8][3] = {
        { 255, 210, 60 }, /* host: gold */
        { 80, 170, 255 },
        { 90, 230, 110 },
        { 255, 120, 200 },
        { 255, 150, 60 },
        { 180, 120, 255 },
        { 90, 230, 230 },
        { 240, 240, 240 },
    };

    if (slot < 0 || slot >= 8) {
        slot = 7;
    }
    if (r != NULL) {
        *r = colors[slot][0];
    }
    if (g != NULL) {
        *g = colors[slot][1];
    }
    if (b != NULL) {
        *b = colors[slot][2];
    }
}

/* ---- game side ----------------------------------------------------------- */

#ifndef COOP_LITE_TEST_STUBS
#include "game/townmap.h"
#include "game/written.h"
#include "ui/intgame.h"

COOP_STATIC_ASSERT(OBJ_TYPE_GOLD == COOP_MID_T_GOLD && OBJ_TYPE_KEY == COOP_MID_T_KEY
        && OBJ_TYPE_WRITTEN == COOP_MID_T_WRITTEN && WRITTEN_TYPE_SCHEMATIC == COOP_MID_W_SCHEMATIC,
    "loot classifier raw values");
COOP_STATIC_ASSERT(OIF_WONT_SELL == COOP_MID_OIF_WONT_SELL && OIF_NO_DISPLAY == COOP_MID_OIF_NO_DISPLAY
        && OIF_NO_DROP == COOP_MID_OIF_NO_DROP,
    "loot classifier flag values");

#define COOP_MID_LOOT_TABLE 4096
#define COOP_MID_GOLD_QUEUE 16
#define COOP_MID_PROMPT_MS 30000u

static void (*coop_mid_ui_tick_hook)(unsigned int now_ms);
static void (*coop_mid_ui_ping_hook)(void);

static bool coop_mid_host_rules_loaded;
static bool coop_mid_rules_sent[8];
static bool coop_mid_rules_shown; /* client: chat line once per session */
static unsigned int coop_mid_rules_file_sig;

static unsigned int coop_mid_status_packed;
static unsigned int coop_mid_status_local;
static unsigned int coop_mid_status_sent_ms;
static unsigned int coop_mid_status_shown; /* for the portrait redraw */

static struct {
    int64_t loc;
    int slot;
    int map;
    unsigned int ms;
    bool used;
} coop_mid_pings[COOP_PING_MAX];
static int coop_mid_ping_next;
static unsigned int coop_mid_ping_sent_ms;
static unsigned int coop_mid_ping_rx_ms[8];

static bool coop_mid_paused_flag;
static int coop_mid_paused_by = -1;
static unsigned int coop_mid_pause_req_ms;

static bool coop_mid_hud_on;
static int coop_mid_hud_rtt = -1;
static int coop_mid_hud_lost;
static unsigned int coop_mid_hud_ms;

static bool coop_mid_trade_active;

/* Host prompts (Y / N): one at a time. */
#define COOP_MID_PROMPT_NONE 0
#define COOP_MID_PROMPT_REST 1
#define COOP_MID_PROMPT_TP 2
static int coop_mid_prompt_kind;
static int coop_mid_prompt_slot;
static unsigned int coop_mid_prompt_ms;
static unsigned int coop_mid_tp_last_ms[8];
static unsigned int coop_mid_req_sent_ms; /* client: last rest / tp ask */

static uint64_t coop_mid_loot_seen[COOP_MID_LOOT_TABLE];
static int coop_mid_loot_count;
static int64_t coop_mid_loot_pc[8]; /* baseline taken for this PC handle */
static int coop_mid_loot_map[8];
static unsigned int coop_mid_loot_ms;

static struct {
    int64_t finder;
    int qty;
} coop_mid_gold_queue[COOP_MID_GOLD_QUEUE];
static int coop_mid_gold_queued;
static bool coop_mid_gold_busy;

static bool coop_mid_follower_share[8];
static bool coop_mid_follower_share_local;

static int coop_mid_townmap_sent;
static unsigned int coop_mid_townmap_sig;
static unsigned int coop_mid_townmap_ms;

static int coop_mid_extra_players; /* for HP scaling, refreshed by the tick */

static unsigned int coop_mid_now(void)
{
    unsigned int now = coop_obj_now_ms != 0 ? coop_obj_now_ms : (unsigned int)(SDL_GetTicksNS() / 1000000u);

    return now != 0 ? now : 1u;
}

static bool coop_mid_session(void)
{
    return tig_net_is_active() && tig_net_is_coop_enabled();
}

void coop_lite_set_mid_ui_hooks(void (*tick)(unsigned int now_ms), void (*ping_added)(void))
{
    coop_mid_ui_tick_hook = tick;
    coop_mid_ui_ping_hook = ping_added;
}

void coop_lite_mid_session_reset(void)
{
    memset(coop_mid_rules_sent, 0, sizeof(coop_mid_rules_sent));
    coop_mid_rules_shown = false;
    coop_mid_rules_file_sig = 0;
    coop_mid_status_packed = 0;
    coop_mid_status_local = 0;
    coop_mid_status_sent_ms = 0;
    memset(coop_mid_pings, 0, sizeof(coop_mid_pings));
    coop_mid_ping_next = 0;
    coop_mid_paused_flag = false;
    coop_mid_paused_by = -1;
    coop_mid_trade_active = false;
    coop_mid_prompt_kind = COOP_MID_PROMPT_NONE;
    memset(coop_mid_tp_last_ms, 0, sizeof(coop_mid_tp_last_ms));
    memset(coop_mid_loot_seen, 0, sizeof(coop_mid_loot_seen));
    coop_mid_loot_count = 0;
    memset(coop_mid_loot_pc, 0, sizeof(coop_mid_loot_pc));
    memset(coop_mid_loot_map, 0, sizeof(coop_mid_loot_map));
    coop_mid_gold_queued = 0;
    memset(coop_mid_follower_share, 0, sizeof(coop_mid_follower_share));
    coop_mid_townmap_sent = 0;
    coop_mid_townmap_sig = 0;
    coop_mid_host_rules_loaded = false;
    coop_mid_rules_inited = false; /* back to defaults until the host's RULES */
    coop_mid_extra_players = 0;
    coop_mid_prompt_kind = COOP_MID_PROMPT_NONE;
    coop_mid_gold_busy = false;
}

/* to: -1 everyone (host) / slot (host) / -10 - slot = everyone but slot
   (host). A client always sends to the host. */
static void coop_mid_send(PacketCoopMid* pkt, int to)
{
    int depth;

    pkt->type = PACKET_COOP_MID;
    pkt->text[COOP_MID_TEXT - 1] = '\0';
    if (!coop_mid_session()) {
        return;
    }
    depth = tig_net_local_suspend_pause();
    if (!tig_net_is_host()) {
        tig_net_send_app(pkt, sizeof(*pkt), 0);
    } else if (to == -1) {
        tig_net_send_app_all(pkt, sizeof(*pkt));
    } else if (to <= -10) {
        tig_net_send_app_except(pkt, sizeof(*pkt), -10 - to);
    } else if (to > 0 && to < 8 && tig_net_client_is_active(to)) {
        tig_net_send_app(pkt, sizeof(*pkt), to);
    }
    tig_net_local_suspend_resume(depth);
}

static void coop_mid_send_op(int op, int to, int slot, int a, int b, int c, int d)
{
    PacketCoopMid pkt;

    memset(&pkt, 0, sizeof(pkt));
    pkt.op = op;
    pkt.slot = slot;
    pkt.a = a;
    pkt.b = b;
    pkt.c = c;
    pkt.d = d;
    coop_mid_send(&pkt, to);
}

/* A line in the chat log with no speaker ("[Loot] ..."). */
static void coop_mid_system_line(const char* text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    tig_debug_printf("[CHAT] %s\n", text);
    if (coop_chat_on_line_hook != NULL) {
        coop_chat_on_line_hook(text);
    }
    coop_chat_log_activity(coop_mid_now());
}

static const char* coop_mid_slot_name(int slot, char* buf, size_t size)
{
    const char* n = NULL;
    int64_t pc;

    buf[0] = '\0';
    if (slot >= 0 && slot < 8) {
        n = tig_net_client_info_get_name(slot);
        if ((n == NULL || n[0] == '\0') && slot == sub_529520()) {
            n = coop_chat_local_name();
        }
    }
    if (n != NULL && n[0] != '\0') {
        snprintf(buf, size, "%s", n);
        coop_lite_chat_sanitize(buf, size);
        return buf;
    }
    pc = (slot >= 0 && slot < 8) ? sub_4A2B60(slot) : OBJ_HANDLE_NULL;
    if (pc != OBJ_HANDLE_NULL && obj_handle_is_valid(pc)) {
        char tmp[256];

        object_examine(pc, pc, tmp);
        snprintf(buf, size, "%s", tmp);
        coop_lite_chat_sanitize(buf, size);
        return buf;
    }
    snprintf(buf, size, "Player %d", slot + 1);
    return buf;
}

static bool coop_mid_pc_ok(int64_t pc)
{
    return pc != OBJ_HANDLE_NULL && obj_handle_is_valid(pc)
        && obj_field_int32_get(pc, OBJ_F_TYPE) == OBJ_TYPE_PC
        && (obj_field_int32_get(pc, OBJ_F_FLAGS) & (OF_DESTROYED | OF_OFF)) == 0;
}

/* Slot PCs present in this session (host: connected clients only). */
static int64_t coop_mid_slot_pc(int slot)
{
    int64_t pc;

    if (slot < 0 || slot >= 8) {
        return OBJ_HANDLE_NULL;
    }
    if (slot != 0 && tig_net_is_host() && !tig_net_client_is_active(slot)) {
        return OBJ_HANDLE_NULL;
    }
    pc = sub_4A2B60(slot);
    return coop_mid_pc_ok(pc) ? pc : OBJ_HANDLE_NULL;
}

static bool coop_mid_in_combat(int64_t pc)
{
    return pc != OBJ_HANDLE_NULL
        && (combat_critter_is_combat_mode_active(pc) || combat_turn_based_is_active());
}

/* ---- rules ---- */

static void coop_mid_rules_write_lobby(void)
{
    char line[512];
    char path[260];
    unsigned int sig = 2166136261u;
    const unsigned char* b = (const unsigned char*)coop_lite_rules();
    size_t i;
    FILE* f;

    for (i = 0; i < sizeof(CoopRules); i++) {
        sig = (sig ^ b[i]) * 16777619u;
    }
    if (sig == coop_mid_rules_file_sig) {
        return;
    }
    coop_mid_rules_file_sig = sig;
    coop_paths_ensure_dir();
    if (!coop_paths_join(path, sizeof(path), "lobby_rules.txt")) {
        return;
    }
    f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    coop_lite_rules_describe(coop_lite_rules(), coop_lite_lang_ru(), line, sizeof(line));
    fprintf(f, "%s\n", line);
    coop_lite_rules_format(coop_lite_rules(), line, sizeof(line));
    fprintf(f, "%s\n", line);
    fclose(f);
}

static void coop_mid_host_rules_load(void)
{
    CoopRules r;
    char text[256];
    const char* env = getenv("ARCANUM_COOP_RULES");

    coop_lite_rules_parse(env, &r);
    coop_mid_rules_set(&r);
    coop_mid_host_rules_loaded = true;
    coop_lite_rules_format(coop_lite_rules(), text, sizeof(text));
    tig_debug_printf("CoopLite: MID rules: %s (from %s)\n", text, env != NULL ? "ARCANUM_COOP_RULES" : "defaults");
}

static void coop_mid_rules_send(int slot)
{
    const CoopRules* r = coop_lite_rules();

    coop_mid_send_op(COOP_MID_OP_RULES, slot, 0, (int)r->flags, r->hp_scale_pct, r->xp_mult_pct, r->tp_cooldown_s);
}

/* ---- status marks ---- */

static unsigned int coop_mid_local_ui_flags(void)
{
    unsigned int f = 0;

    if (inven_ui_is_created()) {
        f |= COOP_STATUS_INVEN;
    }
    if (intgame_mode_get() == INTGAME_MODE_DIALOG || coop_dialog_view.open) {
        f |= COOP_STATUS_DIALOG;
    }
    return f;
}

int coop_lite_mid_status_text(int64_t pc, char* out, size_t out_size)
{
    int slot;
    unsigned int ui = 0;

    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (pc == OBJ_HANDLE_NULL || !obj_handle_is_valid(pc) || !coop_mid_session()) {
        return 0;
    }
    slot = coop_lite_slot_of_obj(pc);
    if (slot >= 0 && slot < 8) {
        ui = coop_lite_status_unpack(coop_mid_status_packed, slot);
    }
    return coop_lite_status_letters(ui, combat_critter_is_combat_mode_active(pc),
        stat_level_get(pc, STAT_POISON_LEVEL) > 0, critter_is_unconscious(pc), critter_is_dead(pc),
        out, out_size);
}

static void coop_mid_status_tick(unsigned int now)
{
    unsigned int f = coop_mid_local_ui_flags();
    int slot;

    if (tig_net_is_host()) {
        unsigned int packed = coop_lite_status_pack(coop_mid_status_packed, 0, f);

        for (slot = 1; slot < 8; slot++) {
            if (!tig_net_client_is_active(slot)) {
                packed = coop_lite_status_pack(packed, slot, 0);
            }
        }
        if (packed != coop_mid_status_local
            || coop_lite_interval_elapsed(coop_mid_status_sent_ms, now, 5000u)) {
            coop_mid_status_local = packed; /* host: last broadcast */
            coop_mid_status_sent_ms = now;
            coop_mid_send_op(COOP_MID_OP_STATUS, -1, 0, (int)packed, 0, 0, 0);
        }
        coop_mid_status_packed = packed;
    } else if (f != coop_mid_status_local
        || coop_lite_interval_elapsed(coop_mid_status_sent_ms, now, 5000u)) {
        coop_mid_status_local = f;
        coop_mid_status_sent_ms = now;
        coop_mid_send_op(COOP_MID_OP_STATUS, 0, 0, (int)f, 0, 0, 0);
    }
    if (coop_mid_status_shown != coop_mid_status_packed) {
        coop_mid_status_shown = coop_mid_status_packed;
        if (coop_party_ui_update_hook != NULL) {
            coop_party_ui_update_hook();
        }
    }
}

/* ---- pings ---- */

static void coop_mid_ping_add(int slot, int64_t loc, int map)
{
    char name[64];
    char line[160];
    int i = coop_mid_ping_next;

    coop_mid_ping_next = (coop_mid_ping_next + 1) % COOP_PING_MAX;
    coop_mid_pings[i].loc = loc;
    coop_mid_pings[i].slot = slot;
    coop_mid_pings[i].map = map;
    coop_mid_pings[i].ms = coop_mid_now();
    coop_mid_pings[i].used = true;
    if (map == map_current_map()) {
        gsound_play_sfx(0, 1);
    }
    coop_mid_slot_name(slot, name, sizeof(name));
    snprintf(line, sizeof(line), coop_lite_lang_ru() ? "[Метка] %s: %d,%d" : "[Ping] %s: %d,%d", name,
        (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc));
    coop_mid_system_line(line);
    tig_debug_printf("CoopLite: MID ping slot=%d at %d,%d map=%d\n", slot,
        (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc), map);
    if (coop_mid_ui_ping_hook != NULL) {
        coop_mid_ui_ping_hook();
    }
}

bool coop_lite_mid_ping_get(int index, int64_t* loc, int* slot, unsigned int* age_ms)
{
    unsigned int now = coop_mid_now();
    unsigned int age;

    if (index < 0 || index >= COOP_PING_MAX || !coop_mid_pings[index].used) {
        return false;
    }
    if (coop_mid_pings[index].map != map_current_map()) {
        return false;
    }
    age = now - coop_mid_pings[index].ms;
    if (age >= COOP_PING_MS) {
        coop_mid_pings[index].used = false;
        return false;
    }
    if (loc != NULL) {
        *loc = coop_mid_pings[index].loc;
    }
    if (slot != NULL) {
        *slot = coop_mid_pings[index].slot;
    }
    if (age_ms != NULL) {
        *age_ms = age;
    }
    return true;
}

void coop_lite_mid_ping_at(int64_t loc)
{
    PacketCoopMid pkt;
    unsigned int now = coop_mid_now();
    int map = map_current_map();

    if (!coop_mid_session() || !coop_lite_rule_on(COOP_RULE_PINGS)) {
        return;
    }
    if (coop_mid_ping_sent_ms != 0 && !coop_lite_interval_elapsed(coop_mid_ping_sent_ms, now, 1000u)) {
        return;
    }
    coop_mid_ping_sent_ms = now;
    memset(&pkt, 0, sizeof(pkt));
    pkt.op = COOP_MID_OP_PING;
    pkt.slot = sub_529520();
    pkt.a = map;
    pkt.loc = loc;
    coop_mid_ping_add(pkt.slot, loc, map);
    coop_mid_send(&pkt, tig_net_is_host() ? -1 : 0);
}

/* ---- shared pause ---- */

bool coop_lite_mid_paused(void)
{
    return coop_mid_paused_flag && coop_mid_session();
}

bool coop_lite_mid_pause_banner(char* out, size_t out_size)
{
    char name[64];

    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (!coop_lite_mid_paused() || out == NULL || out_size == 0) {
        return false;
    }
    coop_mid_slot_name(coop_mid_paused_by >= 0 ? coop_mid_paused_by : 0, name, sizeof(name));
    snprintf(out, out_size, coop_lite_lang_ru() ? "ПАУЗА (%s) - Pause, чтобы продолжить"
                                                : "PAUSED (%s) - press Pause to resume",
        name);
    return true;
}

static void coop_mid_pause_apply(bool on, int by, bool broadcast)
{
    char name[64];
    char line[160];

    if (coop_mid_paused_flag == on) {
        return;
    }
    coop_mid_paused_flag = on;
    coop_mid_paused_by = on ? by : -1;
    coop_mid_slot_name(by, name, sizeof(name));
    snprintf(line, sizeof(line), on ? (coop_lite_lang_ru() ? "[Пауза] %s поставил игру на паузу." : "[Pause] %s paused the game.")
                                    : (coop_lite_lang_ru() ? "[Пауза] %s снял паузу." : "[Pause] %s resumed the game."),
        name);
    coop_mid_system_line(line);
    tig_debug_printf("CoopLite: MID pause %s by slot %d\n", on ? "on" : "off", by);
    if (broadcast && tig_net_is_host()) {
        coop_mid_send_op(COOP_MID_OP_PAUSE, -1, by, on ? 1 : 0, 0, 0, 0);
    }
    if (coop_mid_ui_ping_hook != NULL) {
        coop_mid_ui_ping_hook(); /* repaint the banner */
    }
}

/* Host: may the game pause right now? Not while a travel / join is going. */
static bool coop_mid_pause_allowed(void)
{
    return !coop_lite_pending_follow_valid && !coop_follow_carry_pending;
}

static void coop_mid_host_pause_request(int slot, bool on)
{
    if (slot != 0 && !coop_lite_rule_on(COOP_RULE_CLIENT_PAUSE)) {
        coop_mid_send_op(COOP_MID_OP_PAUSE, slot, slot, coop_mid_paused_flag ? 1 : 0, 0, 0, 1);
        return;
    }
    if (on && !coop_mid_pause_allowed()) {
        if (slot == 0) {
            coop_lite_show_notice(coop_lite_lang_ru() ? "Сейчас нельзя поставить паузу." : "Cannot pause right now.", TF_TYPE_RED);
        } else {
            coop_mid_send_op(COOP_MID_OP_PAUSE, slot, slot, 0, 0, 0, 2);
        }
        return;
    }
    coop_mid_pause_apply(on, slot, true);
}

bool coop_lite_mid_pause_key(void)
{
    unsigned int now = coop_mid_now();

    if (!coop_mid_session()) {
        return false;
    }
    if (coop_mid_pause_req_ms != 0 && !coop_lite_interval_elapsed(coop_mid_pause_req_ms, now, 500u)) {
        return true;
    }
    coop_mid_pause_req_ms = now;
    if (tig_net_is_host()) {
        coop_mid_host_pause_request(0, !coop_mid_paused_flag);
    } else if (!coop_lite_rule_on(COOP_RULE_CLIENT_PAUSE)) {
        coop_lite_show_notice(coop_lite_lang_ru() ? "Хост не разрешил паузу клиентам." : "The host does not allow client pause.", TF_TYPE_RED);
    } else {
        coop_mid_send_op(COOP_MID_OP_PAUSE, 0, sub_529520(), coop_mid_paused_flag ? 0 : 1, 0, 0, 0);
    }
    return true;
}

/* ---- F9 net HUD ---- */

void coop_lite_mid_hud_toggle(void)
{
    coop_mid_hud_on = !coop_mid_hud_on;
    coop_mid_hud_ms = 0;
    tig_debug_printf("CoopLite: MID net HUD %s\n", coop_mid_hud_on ? "on" : "off");
    if (coop_mid_ui_ping_hook != NULL) {
        coop_mid_ui_ping_hook();
    }
}

static int coop_mid_present_players(void)
{
    int slot;
    int n = 0;

    for (slot = 0; slot < 8; slot++) {
        if (slot == sub_529520() || tig_net_client_is_active(slot) || coop_mid_slot_pc(slot) != OBJ_HANDLE_NULL) {
            n++;
        }
    }
    return n;
}

bool coop_lite_mid_hud_text(char* out, size_t out_size)
{
    static char text[96];
    unsigned int now = coop_mid_now();

    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (!coop_mid_hud_on || !coop_mid_session() || out == NULL || out_size == 0) {
        return false;
    }
    if (coop_mid_hud_ms == 0 || coop_lite_interval_elapsed(coop_mid_hud_ms, now, 1000u)) {
        int slot;
        int rtt = -1;
        int lost = 0;

        coop_mid_hud_ms = now;
        if (tig_net_is_host()) {
            /* Worst client. */
            for (slot = 1; slot < 8; slot++) {
                int r;
                int l;

                if (tig_net_client_is_active(slot) && tig_net_peer_stats(slot, &r, &l)) {
                    if (r > rtt) {
                        rtt = r;
                    }
                    lost += l;
                }
            }
        } else {
            int r;
            int l;

            if (tig_net_peer_stats(0, &r, &l)) {
                rtt = r;
                lost = l;
            }
        }
        coop_mid_hud_rtt = rtt;
        coop_mid_hud_lost = lost;
        coop_lite_net_hud_format(coop_mid_hud_rtt, coop_mid_hud_lost, coop_mid_present_players(),
            !coop_lite_pending_follow_valid && !coop_follow_carry_pending, text, sizeof(text));
    }
    snprintf(out, out_size, "%s", text);
    return true;
}

/* ---- rest vote / tp prompts ---- */

static const char* coop_mid_answer_text(int ans)
{
    bool ru = coop_lite_lang_ru();

    switch (ans) {
    case COOP_MID_ANS_ASKED:
        return ru ? "Запрос отправлен хосту." : "Asked the host.";
    case COOP_MID_ANS_YES:
        return ru ? "Хост согласился." : "The host agreed.";
    case COOP_MID_ANS_NO:
        return ru ? "Хост отказал." : "The host refused.";
    case COOP_MID_ANS_COMBAT:
        return ru ? "Нельзя во время боя." : "Not during combat.";
    case COOP_MID_ANS_COOLDOWN:
        return ru ? "Слишком часто, подождите." : "Too soon, wait a little.";
    case COOP_MID_ANS_OFF:
        return ru ? "Хост выключил это правило." : "The host turned this rule off.";
    case COOP_MID_ANS_BUSY:
        return ru ? "Хост занят другим запросом." : "The host is busy with another request.";
    default:
        return "";
    }
}

bool coop_lite_mid_client_rest_request(void)
{
    unsigned int now = coop_mid_now();

    if (!coop_mid_session() || tig_net_is_host() || !coop_lite_rule_on(COOP_RULE_REST_VOTE)) {
        return false;
    }
    if (coop_mid_req_sent_ms != 0 && !coop_lite_interval_elapsed(coop_mid_req_sent_ms, now, 10000u)) {
        coop_lite_show_notice(coop_mid_answer_text(COOP_MID_ANS_COOLDOWN), TF_TYPE_RED);
        return true;
    }
    if (coop_mid_in_combat(player_get_local_pc_obj())) {
        coop_lite_show_notice(coop_mid_answer_text(COOP_MID_ANS_COMBAT), TF_TYPE_RED);
        return true;
    }
    coop_mid_req_sent_ms = now;
    coop_mid_send_op(COOP_MID_OP_REST, 0, sub_529520(), COOP_MID_ANS_ASKED, 0, 0, 0);
    coop_lite_show_notice(coop_lite_lang_ru() ? "Запрос на отдых отправлен хосту." : "Asked the host to rest.", TF_TYPE_WHITE);
    tig_debug_printf("CoopLite: MID rest request sent\n");
    return true;
}

static void coop_mid_prompt_open(int kind, int slot)
{
    char name[64];
    char line[200];

    coop_mid_prompt_kind = kind;
    coop_mid_prompt_slot = slot;
    coop_mid_prompt_ms = coop_mid_now();
    coop_mid_slot_name(slot, name, sizeof(name));
    if (kind == COOP_MID_PROMPT_REST) {
        snprintf(line, sizeof(line), coop_lite_lang_ru() ? "%s просит отдохнуть. Y - отдых, N - отказ."
                                                         : "%s asks to rest. Y - rest, N - refuse.",
            name);
    } else {
        snprintf(line, sizeof(line), coop_lite_lang_ru() ? "%s просит телепорт к вам. Y - да, N - нет."
                                                         : "%s asks to teleport to you. Y - yes, N - no.",
            name);
    }
    coop_lite_show_notice(line, TF_TYPE_WHITE);
    coop_mid_system_line(line);
}

static void coop_mid_reply(int op, int slot, int ans)
{
    coop_mid_send_op(op, slot, slot, ans, 0, 0, 0);
}

/* Host: move a client's PC next to the host (the client moves itself). */
static bool coop_mid_host_pull(int slot, bool gather)
{
    int64_t host_pc = sub_4A2B60(0);
    int64_t pc = coop_mid_slot_pc(slot);
    PacketCoopMid pkt;

    if (host_pc == OBJ_HANDLE_NULL || pc == OBJ_HANDLE_NULL || critter_is_dead(pc)) {
        return false;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.op = COOP_MID_OP_TP;
    pkt.slot = slot;
    pkt.a = COOP_MID_ANS_YES;
    pkt.b = map_current_map();
    pkt.c = gather ? 1 : 0;
    pkt.loc = coop_lite_follow_loc_for_slot(obj_field_int64_get(host_pc, OBJ_F_LOCATION), slot);
    coop_mid_send(&pkt, slot);
    coop_lite_keep_sector(slot, pkt.loc);
    tig_debug_printf("CoopLite: MID tp slot %d -> %d,%d%s\n", slot, (int)LOCATION_GET_X(pkt.loc),
        (int)LOCATION_GET_Y(pkt.loc), gather ? " (gather)" : "");
    return true;
}

/* Client: the host said go - move our own PC (PC_POS carries it to all). */
static void coop_mid_client_tp_apply(const PacketCoopMid* pkt)
{
    int64_t pc = player_get_local_pc_obj();

    if (pc == OBJ_HANDLE_NULL || critter_is_dead(pc) || pkt->b != map_current_map()) {
        tig_debug_printf("CoopLite: MID tp ignored (no PC / other map)\n");
        return;
    }
    if (coop_mid_in_combat(pc)) {
        coop_lite_show_notice(coop_mid_answer_text(COOP_MID_ANS_COMBAT), TF_TYPE_RED);
        return;
    }
    tig_net_local_suspend_begin();
    sub_424070(pc, PRIORITY_HIGHEST, false, true);
    sub_43E770(pc, pkt->loc, 0, 0);
    tig_net_local_suspend_end();
    location_origin_set(pkt->loc);
    tig_debug_printf("CoopLite: MID tp own PC to %d,%d\n", (int)LOCATION_GET_X(pkt->loc), (int)LOCATION_GET_Y(pkt->loc));
}

static void coop_mid_host_tp_request(int slot)
{
    unsigned int now = coop_mid_now();
    int ans;

    ans = coop_lite_tp_gate(coop_lite_rules()->flags,
        coop_mid_in_combat(sub_4A2B60(0)) || coop_mid_in_combat(coop_mid_slot_pc(slot)),
        coop_mid_tp_last_ms[slot], now, coop_lite_rules()->tp_cooldown_s);
    if (ans == COOP_MID_ANS_ASKED) {
        if (coop_mid_prompt_kind != COOP_MID_PROMPT_NONE) {
            ans = COOP_MID_ANS_BUSY;
        } else {
            coop_mid_prompt_open(COOP_MID_PROMPT_TP, slot);
        }
    } else if (ans == COOP_MID_ANS_YES) {
        if (coop_mid_host_pull(slot, false)) {
            coop_mid_tp_last_ms[slot] = now;
            return;
        }
        ans = COOP_MID_ANS_NO;
    }
    coop_mid_reply(COOP_MID_OP_TP, slot, ans);
}

static void coop_mid_host_rest_request(int slot)
{
    int ans = COOP_MID_ANS_ASKED;

    if (!coop_lite_rule_on(COOP_RULE_REST_VOTE)) {
        ans = COOP_MID_ANS_OFF;
    } else if (coop_mid_in_combat(sub_4A2B60(0)) || coop_mid_in_combat(coop_mid_slot_pc(slot))) {
        ans = COOP_MID_ANS_COMBAT;
    } else if (coop_mid_prompt_kind != COOP_MID_PROMPT_NONE || sleep_ui_is_active()) {
        ans = COOP_MID_ANS_BUSY;
    } else {
        coop_mid_prompt_open(COOP_MID_PROMPT_REST, slot);
    }
    coop_mid_reply(COOP_MID_OP_REST, slot, ans);
}

bool coop_lite_mid_vote_key(bool yes)
{
    int kind = coop_mid_prompt_kind;
    int slot = coop_mid_prompt_slot;

    if (!coop_mid_session() || !tig_net_is_host() || kind == COOP_MID_PROMPT_NONE) {
        return false;
    }
    coop_mid_prompt_kind = COOP_MID_PROMPT_NONE;
    tig_debug_printf("CoopLite: MID host answered %s to slot %d: %s\n",
        kind == COOP_MID_PROMPT_REST ? "rest" : "tp", slot, yes ? "yes" : "no");
    if (kind == COOP_MID_PROMPT_REST) {
        if (yes && (coop_mid_in_combat(sub_4A2B60(0)) || sleep_ui_is_active())) {
            yes = false;
        }
        coop_mid_reply(COOP_MID_OP_REST, slot, yes ? COOP_MID_ANS_YES : COOP_MID_ANS_NO);
        if (yes) {
            sleep_ui_toggle(OBJ_HANDLE_NULL); /* the host's own rest window */
        }
    } else {
        if (yes && coop_mid_host_pull(slot, false)) {
            coop_mid_tp_last_ms[slot] = coop_mid_now();
        } else {
            coop_mid_reply(COOP_MID_OP_TP, slot, COOP_MID_ANS_NO);
        }
    }
    return true;
}

static void coop_mid_prompt_tick(unsigned int now)
{
    if (coop_mid_prompt_kind == COOP_MID_PROMPT_NONE) {
        return;
    }
    if (!tig_net_client_is_active(coop_mid_prompt_slot)) {
        coop_mid_prompt_kind = COOP_MID_PROMPT_NONE;
        return;
    }
    if (coop_lite_interval_elapsed(coop_mid_prompt_ms, now, COOP_MID_PROMPT_MS)) {
        coop_mid_reply(coop_mid_prompt_kind == COOP_MID_PROMPT_REST ? COOP_MID_OP_REST : COOP_MID_OP_TP,
            coop_mid_prompt_slot, COOP_MID_ANS_NO);
        coop_mid_prompt_kind = COOP_MID_PROMPT_NONE;
        tig_debug_printf("CoopLite: MID prompt expired\n");
    }
}

static void coop_mid_host_gather(void)
{
    int slot;
    int n = 0;
    char line[128];

    if (!coop_lite_rule_on(COOP_RULE_TP)) {
        coop_lite_show_notice(coop_mid_answer_text(COOP_MID_ANS_OFF), TF_TYPE_RED);
        return;
    }
    if (coop_mid_in_combat(sub_4A2B60(0))) {
        coop_lite_show_notice(coop_mid_answer_text(COOP_MID_ANS_COMBAT), TF_TYPE_RED);
        return;
    }
    for (slot = 1; slot < 8; slot++) {
        if (tig_net_client_is_active(slot) && !coop_mid_in_combat(coop_mid_slot_pc(slot))
            && coop_mid_host_pull(slot, true)) {
            coop_mid_tp_last_ms[slot] = coop_mid_now();
            n++;
        }
    }
    snprintf(line, sizeof(line), coop_lite_lang_ru() ? "[Сбор] Хост собрал группу (%d)." : "[Gather] The host gathered the party (%d).", n);
    coop_mid_system_line(line);
    coop_mid_send_op(COOP_MID_OP_TP, -1, 0, COOP_MID_ANS_YES, -1, 2, n); /* c = 2: the gather line */
}

/* ---- gold: /gold give + auto split ---- */

static int coop_mid_find_slot_by_name(const char* name, int exclude)
{
    int slot;
    int found = -1;
    int count = 0;
    char buf[64];

    for (slot = 0; slot < 8; slot++) {
        if (slot == exclude || coop_mid_slot_pc(slot) == OBJ_HANDLE_NULL) {
            continue;
        }
        if (name == NULL || name[0] == '\0') {
            found = slot;
            count++;
            continue;
        }
        coop_mid_slot_name(slot, buf, sizeof(buf));
        if (SDL_strncasecmp(buf, name, strlen(name)) == 0) {
            found = slot;
            count++;
        }
    }
    return count == 1 ? found : (count == 0 ? -1 : -2);
}

/* Host: line for everyone (formatted by each receiver in its language). */
static void coop_mid_gold_line(int finder, int qty, int share, int players, int target)
{
    char a[64];
    char b[64];
    char line[200];
    bool ru = coop_lite_lang_ru();

    coop_mid_slot_name(finder, a, sizeof(a));
    if (players == -1) {
        coop_mid_slot_name(target, b, sizeof(b));
        snprintf(line, sizeof(line), ru ? "[Золото] %s передал %s: %d" : "[Gold] %s gave %s %d gold", a, b, qty);
    } else {
        snprintf(line, sizeof(line), ru ? "[Золото] %s нашёл %d - поровну на %d (по %d)"
                                        : "[Gold] %s found %d - split %d ways (%d each)",
            a, qty, players, share);
    }
    coop_mid_system_line(line);
}

static bool coop_mid_host_gold_give(int from_slot, int to_slot, int amount)
{
    int64_t from = coop_mid_slot_pc(from_slot);
    int64_t to = coop_mid_slot_pc(to_slot);
    bool ok;

    if (from == OBJ_HANDLE_NULL || to == OBJ_HANDLE_NULL || from == to || amount <= 0
        || item_gold_get(from) < amount) {
        return false;
    }
    coop_mid_gold_busy = true;
    multiplayer_lock();
    ok = item_gold_transfer(from, to, amount, OBJ_HANDLE_NULL);
    multiplayer_unlock();
    coop_mid_gold_busy = false;
    if (!ok) {
        return false;
    }
    coop_lite_host_pc_changed(from, false);
    coop_lite_host_pc_changed(to, false);
    coop_mid_gold_line(from_slot, amount, 0, -1, to_slot);
    coop_mid_send_op(COOP_MID_OP_GOLD_SPLIT, -1, from_slot, amount, to_slot, -1, 0);
    tig_debug_printf("CoopLite: MID gold give slot %d -> slot %d: %d\n", from_slot, to_slot, amount);
    return true;
}

void coop_lite_mid_gold_found(int64_t from_obj, int64_t to_obj, int qty)
{
    int t;

    if (qty <= 0 || coop_mid_gold_busy || !coop_mid_session() || !tig_net_is_host()
        || !coop_lite_rule_on(COOP_RULE_GOLD_SPLIT) || to_obj == OBJ_HANDLE_NULL
        || coop_lite_slot_of_obj(to_obj) < 0 || !coop_mid_pc_ok(to_obj)) {
        return;
    }
    if (from_obj != OBJ_HANDLE_NULL) {
        t = obj_field_int32_get(from_obj, OBJ_F_TYPE);
        if (t == OBJ_TYPE_PC) {
            return; /* trade / gift between players */
        }
        if (obj_type_is_critter(t) && !critter_is_dead(from_obj)) {
            return; /* merchant, pickpocket, dialog payment */
        }
    }
    if (coop_mid_gold_queued >= COOP_MID_GOLD_QUEUE) {
        return;
    }
    coop_mid_gold_queue[coop_mid_gold_queued].finder = to_obj;
    coop_mid_gold_queue[coop_mid_gold_queued].qty = qty;
    coop_mid_gold_queued++;
}

static void coop_mid_gold_flush(void)
{
    int i;

    for (i = 0; i < coop_mid_gold_queued; i++) {
        int64_t finder = coop_mid_gold_queue[i].finder;
        int qty = coop_mid_gold_queue[i].qty;
        int finder_slot;
        int slots[8];
        int n = 0;
        int share;
        int k;
        int slot;

        if (!coop_mid_pc_ok(finder)) {
            continue;
        }
        finder_slot = coop_lite_slot_of_obj(finder);
        for (slot = 0; slot < 8; slot++) {
            int64_t pc = coop_mid_slot_pc(slot);

            if (pc != OBJ_HANDLE_NULL && pc != finder && !critter_is_dead(pc)) {
                slots[n++] = slot;
            }
        }
        share = coop_lite_gold_share(qty, n + 1);
        if (share <= 0 || item_gold_get(finder) < share * n) {
            continue;
        }
        coop_mid_gold_busy = true;
        multiplayer_lock();
        for (k = 0; k < n; k++) {
            item_gold_transfer(finder, coop_mid_slot_pc(slots[k]), share, OBJ_HANDLE_NULL);
        }
        multiplayer_unlock();
        coop_mid_gold_busy = false;
        coop_lite_host_pc_changed(finder, false);
        for (k = 0; k < n; k++) {
            coop_lite_host_pc_changed(coop_mid_slot_pc(slots[k]), false);
        }
        coop_mid_gold_line(finder_slot, qty, share, n + 1, -1);
        coop_mid_send_op(COOP_MID_OP_GOLD_SPLIT, -1, finder_slot, qty, share, n + 1, 0);
        tig_debug_printf("CoopLite: MID gold split slot %d found %d, %d each to %d player(s)\n",
            finder_slot, qty, share, n);
    }
    coop_mid_gold_queued = 0;
}

/* ---- loot feed ---- */

static uint64_t coop_mid_oid_hash(int64_t obj)
{
    ObjectID oid = obj_get_id(obj);
    const unsigned char* p = (const unsigned char*)&oid;
    uint64_t h = 1469598103934665603ULL;
    size_t i;

    for (i = 0; i < sizeof(oid); i++) {
        h = (h ^ p[i]) * 1099511628211ULL;
    }
    return h != 0 ? h : 1;
}

/* true if newly added */
static bool coop_mid_loot_mark(uint64_t h)
{
    unsigned int i = (unsigned int)(h % COOP_MID_LOOT_TABLE);
    int probes;

    for (probes = 0; probes < COOP_MID_LOOT_TABLE; probes++) {
        if (coop_mid_loot_seen[i] == h) {
            return false;
        }
        if (coop_mid_loot_seen[i] == 0) {
            if (coop_mid_loot_count >= COOP_MID_LOOT_TABLE * 3 / 4) {
                /* Full: forget everything (re-baselined on the next scan). */
                memset(coop_mid_loot_seen, 0, sizeof(coop_mid_loot_seen));
                memset(coop_mid_loot_pc, 0, sizeof(coop_mid_loot_pc));
                coop_mid_loot_count = 0;
                return false;
            }
            coop_mid_loot_seen[i] = h;
            coop_mid_loot_count++;
            return true;
        }
        i = (i + 1) % COOP_MID_LOOT_TABLE;
    }
    return false;
}

static void coop_mid_loot_line(int slot, int kind, const char* item)
{
    char name[64];
    char line[200];
    const char* tag;
    bool ru = coop_lite_lang_ru();

    switch (kind) {
    case COOP_LOOT_KEY:
        tag = ru ? "ключ" : "key";
        break;
    case COOP_LOOT_QUEST:
        tag = ru ? "квест" : "quest";
        break;
    case COOP_LOOT_SCHEMATIC:
        tag = ru ? "схема" : "schematic";
        break;
    default:
        tag = ru ? "ценное" : "valuable";
        break;
    }
    coop_mid_slot_name(slot, name, sizeof(name));
    snprintf(line, sizeof(line), ru ? "[Лут] %s подобрал: %s (%s)" : "[Loot] %s picked up: %s (%s)", name, item, tag);
    coop_mid_system_line(line);
}

static void coop_mid_loot_scan(void)
{
    int slot;
    int lines = 0;

    for (slot = 0; slot < 8 && lines < 4; slot++) {
        int64_t pc = coop_mid_slot_pc(slot);
        bool baseline;
        int cnt;
        int i;

        if (pc == OBJ_HANDLE_NULL) {
            continue;
        }
        baseline = coop_mid_loot_pc[slot] != pc;
        coop_mid_loot_pc[slot] = pc;
        cnt = obj_field_int32_get(pc, OBJ_F_CRITTER_INVENTORY_NUM);
        for (i = 0; i < cnt && lines < 4; i++) {
            int64_t item = obj_arrayfield_handle_get(pc, OBJ_F_CRITTER_INVENTORY_LIST_IDX, i);
            int t;
            int kind;
            char buf[256];
            PacketCoopMid pkt;

            if (item == OBJ_HANDLE_NULL || !obj_handle_is_valid(item)) {
                continue;
            }
            if (!coop_mid_loot_mark(coop_mid_oid_hash(item)) || baseline) {
                continue;
            }
            t = obj_field_int32_get(item, OBJ_F_TYPE);
            kind = coop_lite_loot_kind(t, t == OBJ_TYPE_WRITTEN ? obj_field_int32_get(item, OBJ_F_WRITTEN_SUBTYPE) : 0,
                (unsigned int)obj_field_int32_get(item, OBJ_F_ITEM_FLAGS), item_worth(item));
            if (kind == COOP_LOOT_NONE) {
                continue;
            }
            object_examine(item, pc, buf);
            memset(&pkt, 0, sizeof(pkt));
            pkt.op = COOP_MID_OP_LOOT;
            pkt.slot = slot;
            pkt.a = kind;
            snprintf(pkt.text, sizeof(pkt.text), "%s", buf);
            coop_lite_chat_sanitize(pkt.text, sizeof(pkt.text));
            coop_mid_loot_line(slot, kind, pkt.text);
            coop_mid_send(&pkt, -1);
            tig_debug_printf("CoopLite: MID loot slot %d kind %d: %s\n", slot, kind, pkt.text);
            lines++;
        }
    }
}

/* ---- direct trade ---- */

static unsigned int coop_mid_share_mask(void)
{
    unsigned int mask = 0;
    int slot;

    for (slot = 0; slot < 8; slot++) {
        if (coop_mid_follower_share[slot]) {
            mask |= 1u << slot;
        }
    }
    return mask;
}

/* Host: everyone learns whose followers are open for exchange. */
static void coop_mid_share_broadcast(void)
{
    coop_mid_send_op(COOP_MID_OP_FOLLOWER_SHARE, -1, 0, (int)coop_mid_share_mask(), 0, 0, 0);
}

bool coop_lite_mid_direct_trade(int64_t mate_pc)
{
    int64_t pc = player_get_local_pc_obj();

    if (!coop_mid_session() || pc == OBJ_HANDLE_NULL || mate_pc == OBJ_HANDLE_NULL || mate_pc == pc) {
        return false;
    }
    /* A teammate's follower: exchange window only if its owner said /share on. */
    if (obj_field_int32_get(mate_pc, OBJ_F_TYPE) == OBJ_TYPE_NPC && !critter_is_dead(mate_pc)) {
        int64_t leader = critter_pc_leader_get(mate_pc);
        int lslot = leader != OBJ_HANDLE_NULL ? coop_lite_slot_of_obj(leader) : -1;

        if (leader == OBJ_HANDLE_NULL || leader == pc || lslot < 0 || lslot >= 8) {
            return false;
        }
        if (!coop_mid_follower_share[lslot]) {
            coop_lite_show_notice(coop_lite_lang_ru() ? "Владелец спутника не разрешил обмен (/share on)."
                                                      : "Its owner has not allowed trading (/share on).",
                TF_TYPE_WHITE);
            return true;
        }
        if (critter_is_dead(pc) || inven_ui_is_created()) {
            return true;
        }
        if (location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION), obj_field_int64_get(mate_pc, OBJ_F_LOCATION)) > 3) {
            coop_lite_show_notice(coop_lite_lang_ru() ? "Подойдите ближе." : "Walk closer.", TF_TYPE_WHITE);
            return true;
        }
        tig_debug_printf("CoopLite: MID teammate follower exchange window opened\n");
        inven_ui_open(pc, mate_pc, INVEN_UI_MODE_STEAL);
        return true;
    }
    if (!coop_mid_pc_ok(mate_pc) || coop_lite_slot_of_obj(mate_pc) < 0) {
        return false;
    }
    if (critter_is_dead(pc) || inven_ui_is_created()) {
        return true;
    }
    if (location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION), obj_field_int64_get(mate_pc, OBJ_F_LOCATION)) > 3) {
        coop_lite_show_notice(coop_lite_lang_ru() ? "Подойдите ближе к напарнику для передачи вещей." : "Walk closer to your teammate to give items.", TF_TYPE_WHITE);
        return true;
    }
    coop_mid_trade_active = true;
    tig_debug_printf("CoopLite: MID direct trade window opened\n");
    if (!inven_ui_open(pc, mate_pc, INVEN_UI_MODE_STEAL)) {
        coop_mid_trade_active = false;
    }
    return true;
}

bool coop_lite_mid_direct_trade_active(void)
{
    return coop_mid_trade_active;
}

bool coop_lite_mid_direct_trade_take_blocked(int64_t target)
{
    if (!coop_mid_trade_active || target == OBJ_HANDLE_NULL
        || obj_field_int32_get(target, OBJ_F_TYPE) != OBJ_TYPE_PC) {
        return false;
    }
    coop_lite_show_notice(coop_lite_lang_ru() ? "Это окно только для передачи. Попросите напарника отдать вещь."
                                              : "This window only gives. Ask your teammate to hand it over.",
        TF_TYPE_WHITE);
    return true;
}

void coop_lite_mid_direct_trade_end(void)
{
    if (coop_mid_trade_active) {
        tig_debug_printf("CoopLite: MID direct trade window closed\n");
    }
    coop_mid_trade_active = false;
}

/* ---- difficulty ---- */

static bool coop_mid_is_party(int64_t obj)
{
    int t;

    if (obj == OBJ_HANDLE_NULL || !obj_handle_is_valid(obj)) {
        return false;
    }
    t = obj_field_int32_get(obj, OBJ_F_TYPE);
    if (t == OBJ_TYPE_PC) {
        return true;
    }
    return obj_type_is_critter(t) && critter_pc_leader_get(obj) != OBJ_HANDLE_NULL;
}

int coop_lite_mid_hp_max(int64_t obj, int value)
{
    int pct;

    if (coop_mid_extra_players <= 0 || !coop_mid_rules_inited) {
        return value;
    }
    pct = coop_mid_rules_cur.hp_scale_pct;
    if (pct <= 0 || !coop_mid_session() || coop_mid_is_party(obj)) {
        return value;
    }
    return coop_lite_scaled_hp(value, pct, coop_mid_extra_players);
}

int coop_lite_mid_xp(int xp)
{
    if (!coop_mid_session()) {
        return xp;
    }
    return coop_lite_scaled_xp(xp, coop_lite_rules()->xp_mult_pct);
}

bool coop_lite_mid_ff_blocked(int64_t caster, int64_t victim, int64_t aimed)
{
    if (!coop_mid_session() || !tig_net_is_host() || coop_lite_rule_on(COOP_RULE_FF_AOE)) {
        return false;
    }
    if (victim == OBJ_HANDLE_NULL || victim == aimed || victim == caster) {
        return false;
    }
    if (!coop_mid_is_party(caster) || !coop_mid_is_party(victim)) {
        return false;
    }
    tig_debug_printf("CoopLite: MID area damage on a teammate skipped (friendly fire off)\n");
    return true;
}

/* ---- shared quest pocket ---- */

int64_t coop_lite_mid_pocket_find(int64_t pc, int name)
{
    int slot;
    int64_t loc;

    if (!coop_mid_session() || !tig_net_is_host() || !coop_lite_rule_on(COOP_RULE_QUEST_POCKET)
        || pc == OBJ_HANDLE_NULL || coop_lite_slot_of_obj(pc) < 0) {
        return OBJ_HANDLE_NULL;
    }
    loc = obj_field_int64_get(pc, OBJ_F_LOCATION);
    for (slot = 0; slot < 8; slot++) {
        int64_t mate = coop_mid_slot_pc(slot);
        int64_t item;

        if (mate == OBJ_HANDLE_NULL || mate == pc || critter_is_dead(mate)) {
            continue;
        }
        if (location_dist(loc, obj_field_int64_get(mate, OBJ_F_LOCATION)) > COOP_POCKET_RANGE) {
            continue;
        }
        item = item_find_by_name(mate, name);
        if (item != OBJ_HANDLE_NULL) {
            static int last_name;
            static unsigned int last_ms;
            unsigned int now = coop_mid_now();

            /* Scripts may ask every heartbeat: log a name once per 10 s. */
            if (name != last_name || coop_lite_interval_elapsed(last_ms, now, 10000u)) {
                last_name = name;
                last_ms = now;
                tig_debug_printf("CoopLite: MID quest pocket: item %d found on slot %d\n", name, slot);
            }
            return item;
        }
    }
    return OBJ_HANDLE_NULL;
}

void coop_lite_mid_pocket_taken(int64_t holder)
{
    if (holder != OBJ_HANDLE_NULL && coop_mid_session() && tig_net_is_host()
        && coop_lite_slot_of_obj(holder) > 0) {
        coop_lite_host_pc_changed(holder, false);
        tig_debug_printf("CoopLite: MID quest pocket: teammate's pack changed - resync\n");
    }
}

/* ---- shared town map fog ---- */

void coop_lite_mid_remote_pc_moved(int64_t obj, int64_t loc)
{
    if (!coop_mid_session() || !coop_lite_rule_on(COOP_RULE_SHARED_FOG) || obj == OBJ_HANDLE_NULL
        || obj == player_get_local_pc_obj() || obj_field_int32_get(obj, OBJ_F_TYPE) != OBJ_TYPE_PC) {
        return;
    }
    townmap_mark_visited(loc);
}

static void coop_mid_townmap_tick(unsigned int now)
{
    PacketCoopTownmap pkt;
    int64_t pc = player_get_local_pc_obj();
    unsigned int sig = 2166136261u;
    int map;
    int n;
    int i;

    if (!coop_lite_rule_on(COOP_RULE_SHARED_FOG) || pc == OBJ_HANDLE_NULL) {
        return;
    }
    if (coop_mid_townmap_ms != 0 && !coop_lite_interval_elapsed(coop_mid_townmap_ms, now, 2000u)) {
        return;
    }
    coop_mid_townmap_ms = now;
    map = townmap_get(sector_id_from_loc(obj_field_int64_get(pc, OBJ_F_LOCATION)));
    if (map == TOWNMAP_NONE) {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    n = townmap_known_bits_get(map, pkt.bits, COOP_TOWNMAP_BYTES);
    if (n <= 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        sig = (sig ^ pkt.bits[i]) * 16777619u;
    }
    sig ^= (unsigned int)map * 2654435761u;
    if (map == coop_mid_townmap_sent && sig == coop_mid_townmap_sig) {
        return;
    }
    coop_mid_townmap_sent = map;
    coop_mid_townmap_sig = sig;
    pkt.type = PACKET_COOP_TOWNMAP;
    pkt.map = map;
    pkt.size = n;
    {
        int depth = tig_net_local_suspend_pause();

        if (tig_net_is_host()) {
            tig_net_send_app_all(&pkt, sizeof(pkt));
        } else {
            tig_net_send_app(&pkt, sizeof(pkt), 0);
        }
        tig_net_local_suspend_resume(depth);
    }
}

void coop_lite_handle_townmap(PacketCoopTownmap* pkt)
{
    int changed;

    if (!coop_lite_townmap_valid(pkt, (int)sizeof(*pkt)) || !coop_mid_session()
        || !coop_lite_rule_on(COOP_RULE_SHARED_FOG)) {
        return;
    }
    if (tig_net_is_host() && !coop_lite_host_rate_ok(2, sub_52A530(), 1)) {
        return;
    }
    changed = townmap_known_bits_merge(pkt->map, pkt->bits, pkt->size);
    if (changed > 0) {
        tig_debug_printf("CoopLite: MID town map %d: %d byte(s) gained from a teammate\n", pkt->map, changed);
        if (tig_net_is_host()) {
            coop_mid_townmap_sig = 0; /* re-send the union on the next tick */
            coop_mid_townmap_ms = 0;
        }
    }
}

/* ---- follower share ---- */

bool coop_lite_mid_follower_trade_ok(int64_t pc, int64_t npc)
{
    int64_t leader;
    int slot;

    if (pc == OBJ_HANDLE_NULL || npc == OBJ_HANDLE_NULL) {
        return false;
    }
    leader = critter_pc_leader_get(npc);
    if (leader == OBJ_HANDLE_NULL) {
        return false;
    }
    if (leader == pc) {
        return true;
    }
    slot = coop_lite_slot_of_obj(leader);
    return slot >= 0 && slot < 8 && coop_mid_follower_share[slot] && coop_lite_slot_of_obj(pc) >= 0;
}

bool coop_lite_mid_host_follower_exchange(int64_t pc, int64_t npc, int64_t item, bool take)
{
    if (!coop_mid_session() || !tig_net_is_host() || pc == OBJ_HANDLE_NULL || npc == OBJ_HANDLE_NULL
        || item == OBJ_HANDLE_NULL || obj_field_int32_get(npc, OBJ_F_TYPE) != OBJ_TYPE_NPC
        || !coop_lite_mid_follower_trade_ok(pc, npc)) {
        return false;
    }
    item_transfer(item, take ? pc : npc);
    tig_debug_printf("CoopLite: MID host %s a follower's pack (free exchange)\n", take ? "takes from" : "gives to");
    return true;
}

/* ---- chat commands ---- */

bool coop_lite_mid_chat_command(const char* text)
{
    int amount = 0;
    char arg[64];
    char line[512];
    int cmd;
    bool ru = coop_lite_lang_ru();
    bool host = tig_net_is_host();

    cmd = coop_lite_chat_command(text, &amount, arg, sizeof(arg));
    if (cmd == COOP_CMD_NONE || !coop_mid_session()) {
        return false;
    }
    tig_debug_printf("CoopLite: MID chat command %d\n", cmd);
    switch (cmd) {
    case COOP_CMD_TP:
        if (host) {
            coop_lite_show_notice(ru ? "/tp - для клиентов. Хост собирает всех командой /gather." : "/tp is for clients. The host uses /gather.", TF_TYPE_WHITE);
        } else if (!coop_lite_rule_on(COOP_RULE_TP)) {
            coop_lite_show_notice(coop_mid_answer_text(COOP_MID_ANS_OFF), TF_TYPE_RED);
        } else if (coop_mid_in_combat(player_get_local_pc_obj())) {
            coop_lite_show_notice(coop_mid_answer_text(COOP_MID_ANS_COMBAT), TF_TYPE_RED);
        } else if (coop_mid_req_sent_ms != 0 && !coop_lite_interval_elapsed(coop_mid_req_sent_ms, coop_mid_now(), 3000u)) {
            coop_lite_show_notice(coop_mid_answer_text(COOP_MID_ANS_COOLDOWN), TF_TYPE_RED);
        } else {
            coop_mid_req_sent_ms = coop_mid_now();
            coop_mid_send_op(COOP_MID_OP_TP, 0, sub_529520(), COOP_MID_ANS_ASKED, 0, 0, 0);
        }
        return true;
    case COOP_CMD_GATHER:
        if (host) {
            coop_mid_host_gather();
        } else {
            coop_lite_show_notice(ru ? "/gather доступна только хосту. Используйте /tp." : "Only the host can /gather. Use /tp.", TF_TYPE_WHITE);
        }
        return true;
    case COOP_CMD_GOLD: {
        int target = coop_mid_find_slot_by_name(arg, sub_529520());

        if (target == -2) {
            coop_lite_show_notice(ru ? "Уточните имя: /gold N имя" : "Name who gets it: /gold N name", TF_TYPE_RED);
        } else if (target < 0) {
            coop_lite_show_notice(ru ? "Нет такого игрока рядом." : "No such player.", TF_TYPE_RED);
        } else if (item_gold_get(player_get_local_pc_obj()) < amount) {
            coop_lite_show_notice(ru ? "Недостаточно золота." : "Not enough gold.", TF_TYPE_RED);
        } else if (host) {
            if (!coop_mid_host_gold_give(0, target, amount)) {
                coop_lite_show_notice(ru ? "Не удалось передать золото." : "Could not give the gold.", TF_TYPE_RED);
            }
        } else {
            coop_mid_send_op(COOP_MID_OP_GOLD_GIVE, 0, sub_529520(), amount, target, 0, 0);
        }
        return true;
    }
    case COOP_CMD_PAUSE:
        coop_lite_mid_pause_key();
        return true;
    case COOP_CMD_RULES:
        coop_lite_rules_describe(coop_lite_rules(), ru, line, sizeof(line));
        coop_mid_system_line(line);
        return true;
    case COOP_CMD_HELP:
        coop_mid_system_line(ru ? "Команды: /tp (к хосту), /gather (хост: всех к себе), /gold N [имя], /pause, /rules, /share on|off (обмен с вашими спутниками)"
                                : "Commands: /tp (to the host), /gather (host: everyone to me), /gold N [name], /pause, /rules, /share on|off (teammates may trade with your followers)");
        coop_mid_system_line(ru ? "Клавиши: средняя кнопка - метка, Alt - подписи лута, F9 - сеть, Pause - пауза, Ctrl+клик по напарнику - передать вещи"
                                : "Keys: middle click - ping, hold Alt - loot labels, F9 - net HUD, Pause - pause, Ctrl+click a teammate - give items");
        return true;
    case COOP_CMD_FOLLOWERS:
        coop_mid_follower_share_local = amount != 0;
        if (host) {
            coop_mid_follower_share[0] = coop_mid_follower_share_local;
            coop_mid_share_broadcast();
        } else {
            coop_mid_send_op(COOP_MID_OP_FOLLOWER_SHARE, 0, sub_529520(), amount != 0 ? 1 : 0, 0, 0, 0);
        }
        coop_lite_show_notice(amount != 0 ? (ru ? "Напарники могут обмениваться с вашими спутниками." : "Teammates may trade with your followers.")
                                          : (ru ? "Обмен напарников с вашими спутниками закрыт." : "Teammates may no longer trade with your followers."),
            TF_TYPE_WHITE);
        return true;
    default:
        coop_lite_show_notice(ru ? "Неизвестная команда. /help - список." : "Unknown command. /help lists them.", TF_TYPE_RED);
        return true;
    }
}

/* ---- packets ---- */

static void coop_mid_host_rx(PacketCoopMid* pkt, int slot)
{
    switch (pkt->op) {
    case COOP_MID_OP_STATUS:
        coop_mid_status_packed = coop_lite_status_pack(coop_mid_status_packed, slot,
            (unsigned int)pkt->a & COOP_STATUS_MASK);
        break;
    case COOP_MID_OP_REST:
        coop_mid_host_rest_request(slot);
        break;
    case COOP_MID_OP_TP:
        coop_mid_host_tp_request(slot);
        break;
    case COOP_MID_OP_PING: {
        unsigned int now = coop_mid_now();

        if (!coop_lite_rule_on(COOP_RULE_PINGS)) {
            break;
        }
        if (coop_mid_ping_rx_ms[slot] != 0 && !coop_lite_interval_elapsed(coop_mid_ping_rx_ms[slot], now, 800u)) {
            break;
        }
        coop_mid_ping_rx_ms[slot] = now;
        pkt->slot = slot;
        coop_mid_ping_add(slot, pkt->loc, pkt->a);
        coop_mid_send(pkt, -10 - slot);
        break;
    }
    case COOP_MID_OP_PAUSE:
        coop_mid_host_pause_request(slot, pkt->a != 0);
        break;
    case COOP_MID_OP_GOLD_GIVE:
        if (pkt->b < 0 || pkt->b >= 8 || !coop_mid_host_gold_give(slot, pkt->b, pkt->a)) {
            coop_mid_send_op(COOP_MID_OP_GOLD_SPLIT, slot, slot, pkt->a, pkt->b, -2, 0);
        }
        break;
    case COOP_MID_OP_FOLLOWER_SHARE:
        coop_mid_follower_share[slot] = pkt->a != 0;
        tig_debug_printf("CoopLite: MID slot %d follower share %s\n", slot, pkt->a != 0 ? "on" : "off");
        coop_mid_share_broadcast();
        break;
    default:
        break;
    }
}

static void coop_mid_client_rx(PacketCoopMid* pkt)
{
    char line[512];
    bool ru = coop_lite_lang_ru();

    switch (pkt->op) {
    case COOP_MID_OP_RULES: {
        CoopRules r;

        r.flags = (unsigned int)pkt->a;
        r.hp_scale_pct = pkt->b;
        r.xp_mult_pct = pkt->c;
        r.tp_cooldown_s = pkt->d;
        coop_mid_rules_set(&r);
        coop_lite_rules_format(coop_lite_rules(), line, sizeof(line));
        tig_debug_printf("CoopLite: MID host rules: %s\n", line);
        coop_mid_rules_write_lobby();
        if (!coop_mid_rules_shown) {
            coop_mid_rules_shown = true;
            coop_lite_rules_describe(coop_lite_rules(), ru, line, sizeof(line));
            coop_mid_system_line(line);
        }
        break;
    }
    case COOP_MID_OP_STATUS:
        coop_mid_status_packed = (unsigned int)pkt->a;
        break;
    case COOP_MID_OP_FOLLOWER_SHARE: {
        int s;

        for (s = 0; s < 8; s++) {
            coop_mid_follower_share[s] = (((unsigned int)pkt->a >> s) & 1u) != 0;
        }
        break;
    }
    case COOP_MID_OP_LOOT:
        if (pkt->slot >= 0 && pkt->slot < 8) {
            coop_lite_chat_sanitize(pkt->text, sizeof(pkt->text));
            coop_mid_loot_line(pkt->slot, pkt->a, pkt->text);
        }
        break;
    case COOP_MID_OP_GOLD_SPLIT:
        if (pkt->c == -2) {
            coop_lite_show_notice(ru ? "Не удалось передать золото." : "Could not give the gold.", TF_TYPE_RED);
        } else if (pkt->slot >= 0 && pkt->slot < 8) {
            coop_mid_gold_line(pkt->slot, pkt->a, pkt->b, pkt->c, pkt->b);
        }
        break;
    case COOP_MID_OP_REST:
        coop_lite_show_notice(coop_mid_answer_text(pkt->a), pkt->a == COOP_MID_ANS_YES || pkt->a == COOP_MID_ANS_ASKED ? TF_TYPE_WHITE : TF_TYPE_RED);
        break;
    case COOP_MID_OP_TP:
        if (pkt->c == 2) {
            snprintf(line, sizeof(line), ru ? "[Сбор] Хост собрал группу (%d)." : "[Gather] The host gathered the party (%d).", pkt->d);
            coop_mid_system_line(line);
        } else if (pkt->a == COOP_MID_ANS_YES && pkt->slot == sub_529520()) {
            coop_mid_client_tp_apply(pkt);
        } else {
            coop_lite_show_notice(coop_mid_answer_text(pkt->a), pkt->a == COOP_MID_ANS_ASKED ? TF_TYPE_WHITE : TF_TYPE_RED);
        }
        break;
    case COOP_MID_OP_PING:
        if (pkt->slot >= 0 && pkt->slot < 8 && pkt->slot != sub_529520()) {
            coop_mid_ping_add(pkt->slot, pkt->loc, pkt->a);
        }
        break;
    case COOP_MID_OP_PAUSE:
        if (pkt->d == 1) {
            coop_lite_show_notice(ru ? "Хост не разрешил паузу клиентам." : "The host does not allow client pause.", TF_TYPE_RED);
        } else if (pkt->d == 2) {
            coop_lite_show_notice(ru ? "Сейчас нельзя поставить паузу." : "Cannot pause right now.", TF_TYPE_RED);
        } else {
            coop_mid_pause_apply(pkt->a != 0, pkt->slot, false);
        }
        break;
    default:
        break;
    }
}

void coop_lite_handle_mid(PacketCoopMid* pkt)
{
    bool host = tig_net_is_host();
    int slot;

    if (!coop_lite_mid_valid(pkt, (int)sizeof(*pkt)) || !coop_mid_session()) {
        return;
    }
    if (!coop_lite_mid_op_ok(pkt->op, host)) {
        tig_debug_printf("CoopLite: MID op %d dropped (wrong direction)\n", pkt->op);
        return;
    }
    if (host) {
        slot = sub_52A530();
        if (slot <= 0 || slot >= 8 || !tig_net_client_is_active(slot)) {
            return;
        }
        if (pkt->op != COOP_MID_OP_STATUS && !coop_lite_host_rate_ok(2, slot, 1)) {
            return;
        }
        pkt->slot = slot;
        coop_mid_host_rx(pkt, slot);
    } else {
        coop_mid_client_rx(pkt);
    }
}

/* ---- tick ---- */

void coop_lite_mid_tick(unsigned int now_ms)
{
    int slot;
    unsigned int now = now_ms != 0 ? now_ms : coop_mid_now();

    if (!coop_mid_session()) {
        if (coop_mid_paused_flag) {
            coop_mid_paused_flag = false;
            coop_mid_paused_by = -1;
        }
        return;
    }
    if (tig_net_is_host()) {
        if (!coop_mid_host_rules_loaded) {
            coop_mid_host_rules_load();
        }
        coop_mid_rules_write_lobby();
        for (slot = 1; slot < 8; slot++) {
            if (!tig_net_client_is_active(slot)) {
                if (coop_mid_rules_sent[slot]) {
                    coop_mid_rules_sent[slot] = false;
                    coop_mid_follower_share[slot] = false;
                    coop_mid_status_packed = coop_lite_status_pack(coop_mid_status_packed, slot, 0);
                }
                continue;
            }
            if (!coop_mid_rules_sent[slot]) {
                coop_mid_rules_sent[slot] = true;
                coop_mid_rules_send(slot);
                coop_mid_share_broadcast();
                if (coop_mid_paused_flag) {
                    /* A join needs the world running: never hold a newcomer. */
                    coop_mid_pause_apply(false, 0, true);
                }
                tig_debug_printf("CoopLite: MID rules sent to slot %d\n", slot);
            }
        }
        coop_mid_prompt_tick(now);
        /* Never hold the world still under a travel or a rest: both run on
           game time (a frozen clock would stall them). */
        if (coop_mid_paused_flag && (!coop_mid_pause_allowed() || sleep_ui_is_active())) {
            tig_debug_printf("CoopLite: MID pause lifted (travel / rest started)\n");
            coop_mid_pause_apply(false, 0, true);
        }
        if (coop_mid_gold_queued > 0) {
            coop_mid_gold_flush();
        }
        if (coop_lite_rule_on(COOP_RULE_LOOT_FEED)
            && (coop_mid_loot_ms == 0 || coop_lite_interval_elapsed(coop_mid_loot_ms, now, 1000u))) {
            coop_mid_loot_ms = now;
            coop_mid_loot_scan();
        }
    }
    coop_mid_extra_players = coop_mid_present_players() - 1;
    coop_mid_status_tick(now);
    coop_mid_townmap_tick(now);
    if (coop_mid_ui_tick_hook != NULL) {
        coop_mid_ui_tick_hook(now);
    }
}

#else /* COOP_LITE_TEST_STUBS */

void coop_lite_handle_mid(PacketCoopMid* pkt) { (void)pkt; }
void coop_lite_handle_townmap(PacketCoopTownmap* pkt) { (void)pkt; }
void coop_lite_mid_tick(unsigned int now_ms) { (void)now_ms; }
bool coop_lite_mid_direct_trade_take_blocked(int64_t target) { (void)target; return false; }
bool coop_lite_mid_host_follower_exchange(int64_t pc, int64_t npc, int64_t item, bool take)
{
    (void)pc;
    (void)npc;
    (void)item;
    (void)take;
    return false;
}
void coop_lite_mid_session_reset(void) { }

#endif /* COOP_LITE_TEST_STUBS */
