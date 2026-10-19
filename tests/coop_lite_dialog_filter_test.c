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
    /* classify always ALLOW now */
    if (coop_lite_classify_talk(true, false, false, true) != COOP_TALK_ALLOW) {
        fprintf(stderr, "story NPC should ALLOW (filtered)\n");
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
