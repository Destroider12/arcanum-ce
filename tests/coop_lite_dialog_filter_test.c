/*
 * Offline dialog filter check against real Arcanum dlg fixtures
 * (Ristezze 01040 from arcanum3/4.dat). Verifies barter nodes allowed
 * and quest/story action nodes denied for non-host companions.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/coop_lite.h"

#include "net/tig_net.h"

typedef unsigned int TeleportFlags;
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

int64_t player_get_local_pc_obj(void) { return 1; }
bool teleport_do(TeleportData* teleport_data) { (void)teleport_data; return true; }
bool sub_4A4320(void) { return true; }


static int parse_dlg_fields(const char* text, char*** fields_out, int* count_out)
{
    int cap = 256;
    int n = 0;
    char** fields = (char**)malloc(sizeof(char*) * (size_t)cap);
    const char* p = text;

    while (*p) {
        while (*p && *p != '{') {
            p++;
        }
        if (*p != '{') {
            break;
        }
        p++;
        const char* start = p;
        while (*p && *p != '}') {
            p++;
        }
        if (*p != '}') {
            break;
        }
        size_t len = (size_t)(p - start);
        char* s = (char*)malloc(len + 1);
        memcpy(s, start, len);
        s[len] = '\0';
        if (n >= cap) {
            cap *= 2;
            fields = (char**)realloc(fields, sizeof(char*) * (size_t)cap);
        }
        fields[n++] = s;
        p++;
    }
    *fields_out = fields;
    *count_out = n;
    return 0;
}

static int test_fixture(const char* path, int expect_barter, int expect_quest_denied)
{
    FILE* f;
    long sz;
    char* text;
    char** fields;
    int n;
    int i;
    int barter_ok = 0;
    int quest_denied = 0;
    int quest_seen = 0;

    f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "missing fixture %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char*)malloc((size_t)sz + 1);
    fread(text, 1, (size_t)sz, f);
    text[sz] = '\0';
    fclose(f);

    parse_dlg_fields(text, &fields, &n);
    for (i = 0; i + 6 < n; i += 7) {
        const char* txt = fields[i + 1];
        const char* acts = fields[i + 6];
        int response_code = 0;
        /* b: options become field_17F0=3 in the dialog engine. */
        if (txt[0] == 'B' || txt[0] == 'b') {
            if (txt[1] == ':') {
                response_code = 3;
            }
        }
        if (response_code == 3) {
            if (!coop_lite_dialog_option_allowed(acts, response_code)) {
                fprintf(stderr, "barter node denied: %s acts=%s\n", fields[i], acts);
                return 1;
            }
            barter_ok++;
        }
        if (strstr(acts, "qu") != NULL || strstr(acts, "gf") != NULL
            || strstr(acts, "gv") != NULL || strstr(acts, "ss") != NULL) {
            quest_seen++;
            if (coop_lite_dialog_option_allowed(acts, 0)) {
                fprintf(stderr, "quest node allowed: %s acts=%s\n", fields[i], acts);
                return 1;
            }
            quest_denied++;
        }
    }

    for (i = 0; i < n; i++) {
        free(fields[i]);
    }
    free(fields);
    free(text);

    if (barter_ok < expect_barter) {
        fprintf(stderr, "%s: expected >=%d barter, got %d\n", path, expect_barter, barter_ok);
        return 1;
    }
    if (quest_denied < expect_quest_denied) {
        fprintf(stderr, "%s: expected >=%d quest denied, got %d (seen %d)\n",
            path, expect_quest_denied, quest_denied, quest_seen);
        return 1;
    }
    printf("OK %s barter=%d quest_denied=%d\n", path, barter_ok, quest_denied);
    return 0;
}

/*
 * Virgil (dlg/01324virgil.dlg) — Playtest 7: a client picked
 * "I'm not going anywhere with you, Virgil" -> node 120 -> option 121
 * "{jo 0 74}" and Virgil joined the client. The real file is game data and
 * is not committed; when present (COOP_VIRGIL_DLG or the extracted copy in
 * /workspace/tools) every follower/plot line must be denied for clients.
 */
static int test_virgil_file(void)
{
    const char* path = getenv("COOP_VIRGIL_DLG");
    FILE* f;
    long sz;
    char* text;
    char** fields;
    int n;
    int i;
    int join_denied = 0;
    int plot_denied = 0;
    int allowed = 0;

    if (path == NULL || path[0] == '\0') {
        path = "/workspace/tools/01324virgil.dlg";
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        printf("SKIP virgil real dlg (%s not present)\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char*)malloc((size_t)sz + 1);
    fread(text, 1, (size_t)sz, f);
    text[sz] = '\0';
    fclose(f);

    parse_dlg_fields(text, &fields, &n);
    for (i = 0; i + 6 < n; i += 7) {
        const char* acts = fields[i + 6];
        int ok = coop_lite_dialog_option_allowed(acts, 0);
        if (strstr(acts, "jo") != NULL || strstr(acts, "wa") != NULL
            || strstr(acts, "lv") != NULL || strstr(acts, "uw") != NULL) {
            if (ok) {
                fprintf(stderr, "virgil follower line allowed: %s acts=%s\n", fields[i], acts);
                return 1;
            }
            join_denied++;
        } else if (strstr(acts, "gf") != NULL || strstr(acts, "qu") != NULL
            || strstr(acts, "ru") != NULL || strstr(acts, "lf") != NULL
            || strstr(acts, "in") != NULL || strstr(acts, "al") != NULL) {
            if (ok) {
                fprintf(stderr, "virgil plot line allowed: %s acts=%s\n", fields[i], acts);
                return 1;
            }
            plot_denied++;
        } else if (ok) {
            allowed++;
        }
    }
    for (i = 0; i < n; i++) {
        free(fields[i]);
    }
    free(fields);
    free(text);

    /* 01324virgil.dlg: 21 jo lines, plus wa/lv/uw. */
    if (join_denied < 21 || plot_denied < 4 || allowed < 1) {
        fprintf(stderr, "virgil: join_denied=%d plot_denied=%d allowed=%d\n",
            join_denied, plot_denied, allowed);
        return 1;
    }
    printf("OK virgil real dlg join_denied=%d plot_denied=%d allowed=%d\n",
        join_denied, plot_denied, allowed);
    return 0;
}

int main(void)
{
    if (coop_lite_dialog_option_allowed("", 0) != true) {
        fprintf(stderr, "empty actions should allow\n");
        return 1;
    }
    if (coop_lite_dialog_option_allowed(NULL, 3) != true) {
        fprintf(stderr, "barter code should allow\n");
        return 1;
    }
    if (coop_lite_dialog_option_allowed("qu 1011 2", 0) != false) {
        fprintf(stderr, "qu should deny\n");
        return 1;
    }
    if (coop_lite_dialog_option_allowed("gf2697 1", 0) != false) {
        fprintf(stderr, "gf should deny\n");
        return 1;
    }
    if (coop_lite_dialog_option_allowed("re 41", 0) != true) {
        fprintf(stderr, "reaction-only should allow\n");
        return 1;
    }
    /* Barter is no longer a blanket pass: story codes still deny. */
    if (coop_lite_dialog_option_allowed("qu 1011 2", 3) != false) {
        fprintf(stderr, "barter node with quest action should deny\n");
        return 1;
    }
    /* Local (per-NPC) flags/vars are shared campaign state too. */
    if (coop_lite_dialog_option_allowed("lf 3 1", 0) != false
        || coop_lite_dialog_option_allowed("LV 2 5", 0) != false) {
        fprintf(stderr, "lf/lv should deny\n");
        return 1;
    }
    /* Playtest 7: Virgil's real action strings (copied verbatim). */
    {
        static const struct {
            const char* acts;
            bool allowed;
        } virgil[] = {
            /* node 1 (NPC line, runs on display) */
            { "gf2004 1, gf2626 1, gf2627 1, in -2804, gf2129 1, qu1010 2, ru2013", false },
            /* option 118 "I'm not going anywhere with you" -> node 120 */
            { "", true },
            /* node 120 NPC line */
            { "mm1", true },
            /* option 121 "Alright, I will play along... Let's go." */
            { "jo 0 74", false },
            { "jo 0 535", false },
            { "wa 1", false },
            { "lv", false },
            { "uw", false },
            { "sc", false },
            { "so", false },
            { "pf 3 1", false },
            { "pv 3 1", false },
            { "$$ -100", false },
            { "re 10", true },
            { "fl 5", true },
        };
        size_t k;
        for (k = 0; k < sizeof(virgil) / sizeof(virgil[0]); k++) {
            if (coop_lite_dialog_option_allowed(virgil[k].acts, 0) != virgil[k].allowed) {
                fprintf(stderr, "virgil acts '%s' expected %s\n",
                    virgil[k].acts, virgil[k].allowed ? "allow" : "deny");
                return 1;
            }
        }
        if (!coop_lite_dialog_action_blocked("jo") || !coop_lite_dialog_action_blocked("JO")
            || coop_lite_dialog_action_blocked("re") || coop_lite_dialog_action_blocked("mm")
            || coop_lite_dialog_action_blocked("tr") || coop_lite_dialog_action_blocked("fl")) {
            fprintf(stderr, "action_blocked table wrong\n");
            return 1;
        }
    }
    /* Root cause PT7: the client's talk runs under local suspension, where
       tig_net_is_active() is false -> filter was off. Role must survive it. */
    if (coop_lite_is_client_role()) {
        fprintf(stderr, "offline must not be client role\n");
        return 1;
    }
    tig_net_local_suspend_begin();
    if (!coop_lite_is_client_role()) {
        fprintf(stderr, "suspended must be client role\n");
        tig_net_local_suspend_end();
        return 1;
    }
    tig_net_local_suspend_end();
    if (coop_lite_is_client_role()) {
        fprintf(stderr, "role must clear after suspension\n");
        return 1;
    }
    if (test_virgil_file() != 0) {
        return 1;
    }

    /* PT8: clients open dialog only with merchants (Ristezze-style shops,
       story nodes still filtered); everyone else barks. */
    if (coop_lite_classify_talk(true, false, false, true) != COOP_TALK_BARK
        || coop_lite_classify_talk(true, false, true, true) != COOP_TALK_ALLOW) {
        fprintf(stderr, "client classify: story NPC must bark, merchant opens\n");
        return 1;
    }

    /* Synthetic lines mirroring Steam dlg/01040ristezze.dlg patterns
       (barter B: + empty acts; quest lines with qu/gf/ss/xp/ru). Not game data. */
    {
        const char* synthetic =
            "{40}{B:}{}{5}{}{0}{}\n"
            "{281}{Of course.}{}{5}{}{0}{mm 21, ss 1, xp 3, ru 2031, gf2697 1, qu1011 2}\n"
            "{418}{B:}{}{5}{}{0}{}\n";
        FILE* tmp = fopen("/tmp/coop_dlg_synthetic.dlg", "wb");
        if (tmp == NULL) {
            fprintf(stderr, "cannot write synthetic dlg\n");
            return 1;
        }
        fputs(synthetic, tmp);
        fclose(tmp);
        if (test_fixture("/tmp/coop_dlg_synthetic.dlg", 2, 1) != 0) {
            return 1;
        }
    }
    return 0;
}
