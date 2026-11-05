/*
 * PT77: small co-op chat log (bottom-left, above the hotbar). Semi-transparent
 * text over a colour-keyed window so empty pixels do not steal mouse clicks.
 */
#include "ui/coop_chat_ui.h"

#include <stdio.h>
#include <string.h>

#include "game/coop_lite.h"
#include "game/hrp.h"
#include "net/tig_net.h"
#include "tig/art.h"
#include "tig/color.h"
#include "tig/font.h"
#include "tig/video.h"
#include "tig/window.h"

#define COOP_CHAT_UI_LINES COOP_CHAT_LOG_LINES
#define COOP_CHAT_UI_LINE_H 16
#define COOP_CHAT_UI_PAD 4
#define COOP_CHAT_UI_W 380
#define COOP_CHAT_UI_H (COOP_CHAT_UI_PAD * 2 + COOP_CHAT_UI_LINES * COOP_CHAT_UI_LINE_H)

static tig_window_handle_t coop_chat_window = TIG_WINDOW_HANDLE_INVALID;
static tig_font_handle_t coop_chat_font = TIG_FONT_HANDLE_INVALID;
static unsigned int coop_chat_color_key;
static char coop_chat_lines[COOP_CHAT_UI_LINES][192];
static int coop_chat_count;
static bool coop_chat_visible;
static bool coop_chat_inited;

static bool coop_chat_ui_filter(TigMessage* msg)
{
    (void)msg;
    return false; /* never consume input */
}

static void coop_chat_ui_redraw(void)
{
    TigRect rect;
    int i;
    int y;

    if (coop_chat_window == TIG_WINDOW_HANDLE_INVALID) {
        return;
    }
    tig_window_fill(coop_chat_window, NULL, coop_chat_color_key);
    if (!coop_chat_visible || coop_chat_count <= 0) {
        tig_window_invalidate_rect(NULL);
        return;
    }
    tig_font_push(coop_chat_font);
    y = COOP_CHAT_UI_PAD;
    for (i = 0; i < coop_chat_count; i++) {
        rect.x = COOP_CHAT_UI_PAD;
        rect.y = y;
        rect.width = COOP_CHAT_UI_W - COOP_CHAT_UI_PAD * 2;
        rect.height = COOP_CHAT_UI_LINE_H;
        tig_window_text_write(coop_chat_window, coop_chat_lines[i], &rect);
        y += COOP_CHAT_UI_LINE_H;
    }
    tig_font_pop();
    tig_window_invalidate_rect(NULL);
}

static void coop_chat_ui_on_line(const char* line)
{
    int i;

    if (line == NULL || line[0] == '\0') {
        return;
    }
    if (coop_chat_count >= COOP_CHAT_UI_LINES) {
        for (i = 1; i < COOP_CHAT_UI_LINES; i++) {
            memcpy(coop_chat_lines[i - 1], coop_chat_lines[i], sizeof(coop_chat_lines[0]));
        }
        coop_chat_count = COOP_CHAT_UI_LINES - 1;
    }
    strncpy(coop_chat_lines[coop_chat_count], line, sizeof(coop_chat_lines[0]) - 1);
    coop_chat_lines[coop_chat_count][sizeof(coop_chat_lines[0]) - 1] = '\0';
    coop_chat_count++;
    coop_chat_visible = true;
    coop_chat_ui_redraw();
}

static void coop_chat_ui_set_visible(bool on)
{
    coop_chat_visible = on;
    coop_chat_ui_redraw();
}

static void coop_chat_ui_create_window(void)
{
    TigWindowData window_data;
    TigFont font_desc;

    if (coop_chat_window != TIG_WINDOW_HANDLE_INVALID) {
        return;
    }
    memset(&window_data, 0, sizeof(window_data));
    window_data.flags = TIG_WINDOW_TRANSPARENT | TIG_WINDOW_MESSAGE_FILTER;
    window_data.rect.x = 8;
    window_data.rect.y = 420;
    window_data.rect.width = COOP_CHAT_UI_W;
    window_data.rect.height = COOP_CHAT_UI_H;
    hrp_apply(&window_data.rect, GRAVITY_LEFT | GRAVITY_BOTTOM);
    /* Nudge above the bottom HUD / hotbar strip. */
    window_data.rect.y -= 90;
    if (window_data.rect.y < 0) {
        window_data.rect.y = 0;
    }
    coop_chat_color_key = tig_color_make(255, 0, 255);
    window_data.color_key = coop_chat_color_key;
    window_data.message_filter = coop_chat_ui_filter;
    if (tig_window_create(&window_data, &coop_chat_window) != TIG_OK) {
        coop_chat_window = TIG_WINDOW_HANDLE_INVALID;
        return;
    }
    memset(&font_desc, 0, sizeof(font_desc));
    font_desc.flags = TIG_FONT_SHADOW;
    tig_art_misc_id_create(TIG_ART_SYSTEM_FONT, 0, &font_desc.art_id);
    font_desc.color = tig_color_make(220, 220, 200);
    tig_font_create(&font_desc, &coop_chat_font);
    tig_window_fill(coop_chat_window, NULL, coop_chat_color_key);
}

bool coop_chat_ui_init(GameInitInfo* init_info)
{
    (void)init_info;
    memset(coop_chat_lines, 0, sizeof(coop_chat_lines));
    coop_chat_count = 0;
    coop_chat_visible = false;
    coop_chat_ui_create_window();
    coop_lite_set_chat_ui_hooks(coop_chat_ui_on_line, coop_chat_ui_set_visible);
    coop_chat_inited = true;
    return true;
}

void coop_chat_ui_exit(void)
{
    coop_lite_set_chat_ui_hooks(NULL, NULL);
    if (coop_chat_window != TIG_WINDOW_HANDLE_INVALID) {
        tig_window_destroy(coop_chat_window);
        coop_chat_window = TIG_WINDOW_HANDLE_INVALID;
    }
    if (coop_chat_font != TIG_FONT_HANDLE_INVALID) {
        tig_font_destroy(coop_chat_font);
        coop_chat_font = TIG_FONT_HANDLE_INVALID;
    }
    coop_chat_inited = false;
}

void coop_chat_ui_reset(void)
{
    memset(coop_chat_lines, 0, sizeof(coop_chat_lines));
    coop_chat_count = 0;
    coop_chat_visible = false;
    coop_chat_ui_redraw();
}

void coop_chat_ui_resize(GameResizeInfo* resize_info)
{
    (void)resize_info;
    if (!coop_chat_inited) {
        return;
    }
    if (coop_chat_window != TIG_WINDOW_HANDLE_INVALID) {
        tig_window_destroy(coop_chat_window);
        coop_chat_window = TIG_WINDOW_HANDLE_INVALID;
    }
    if (coop_chat_font != TIG_FONT_HANDLE_INVALID) {
        tig_font_destroy(coop_chat_font);
        coop_chat_font = TIG_FONT_HANDLE_INVALID;
    }
    coop_chat_ui_create_window();
    coop_chat_ui_redraw();
}
