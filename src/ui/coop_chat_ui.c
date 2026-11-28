/*
 * PT77e: co-op chat log as plain text lines (no window, frame or background),
 * drawn straight into the iso view right above the bottom HUD strip, after the
 * map, objects and text floaters (gamelib overlay hook). It is part of the
 * iso view's own pixels, so it never takes a click and needs no window of
 * its own; it sits in the strip above the HUD that the HUD does not cover.
 * Shown while typing and ~12 s after the last line, then the area is
 * invalidated so the map repaints over it.
 */
#include "ui/coop_chat_ui.h"

#include <stdio.h>
#include <string.h>

#include "game/coop_lite.h"
#include "game/gamelib.h"
#include "game/hrp.h"
#include "tig/art.h"
#include "tig/color.h"
#include "tig/font.h"
#include "tig/video.h"
#include "tig/rect.h"
#include "tig/window.h"

#define COOP_CHAT_UI_LINES COOP_CHAT_LOG_LINES
#define COOP_CHAT_UI_LINE_H 16

static tig_window_handle_t coop_chat_iso_window = TIG_WINDOW_HANDLE_INVALID;
static TigRect coop_chat_iso_rect; /* iso window size */
static tig_font_handle_t coop_chat_font = TIG_FONT_HANDLE_INVALID;
static char coop_chat_lines[COOP_CHAT_UI_LINES][256];
static int coop_chat_count;
static bool coop_chat_visible;
static bool coop_chat_inited;
/* Text is rendered into a colour-keyed buffer and blitted into the iso view
   the same way text floaters (tf.c) are; writing fonts straight into the iso
   window buffer does not show up. */
static TigVideoBuffer* coop_chat_vb;
static int coop_chat_vb_w;
static int coop_chat_vb_h;
static tig_color_t coop_chat_key;
static bool coop_chat_vb_dirty = true;

static void coop_chat_ui_area(TigRect* rect)
{
    coop_lite_chat_text_area(coop_chat_iso_rect.width, coop_chat_iso_rect.height,
        COOP_CHAT_UI_LINES, COOP_CHAT_UI_LINE_H,
        &rect->x, &rect->y, &rect->width, &rect->height);
}

static void coop_chat_ui_invalidate(void)
{
    TigRect rect;

    if (!coop_chat_inited) {
        return;
    }
    coop_chat_ui_area(&rect);
    gamelib_invalidate_rect(&rect);
}

/* PT77k2: the iso scroll shifts the window pixels (chat text included) and
   only repaints the newly exposed strip, which smeared the chat. Repaint
   the chat area and where its shifted copy landed. */
static void coop_chat_ui_on_scroll(int dx, int dy)
{
    TigRect rect;

    if (!coop_chat_inited || !coop_chat_visible || coop_chat_count <= 0) {
        return;
    }
    coop_chat_ui_area(&rect);
    gamelib_invalidate_rect(&rect);
    rect.x += dx;
    rect.y += dy;
    gamelib_invalidate_rect(&rect);
}

static void coop_chat_ui_vb_destroy(void)
{
    if (coop_chat_vb != NULL) {
        tig_video_buffer_destroy(coop_chat_vb);
        coop_chat_vb = NULL;
    }
    coop_chat_vb_w = 0;
    coop_chat_vb_h = 0;
}

static bool coop_chat_ui_vb_render(const TigRect* area)
{
    TigVideoBufferCreateInfo vb_create_info;
    TigRect content;
    TigRect line_rect;
    TigRect dirty;
    int i;
    int first;

    if (area->width <= 0 || area->height <= 0) {
        return false;
    }
    if (coop_chat_vb != NULL && (coop_chat_vb_w != area->width || coop_chat_vb_h != area->height)) {
        coop_chat_ui_vb_destroy();
    }
    if (coop_chat_vb == NULL) {
        coop_chat_key = tig_color_make(0, 0, 255);
        vb_create_info.flags = TIG_VIDEO_BUFFER_CREATE_COLOR_KEY | TIG_VIDEO_BUFFER_CREATE_VIDEO_MEMORY;
        vb_create_info.width = area->width;
        vb_create_info.height = area->height;
        vb_create_info.background_color = coop_chat_key;
        vb_create_info.color_key = coop_chat_key;
        if (tig_video_buffer_create(&vb_create_info, &coop_chat_vb) != TIG_OK) {
            coop_chat_vb = NULL;
            return false;
        }
        coop_chat_vb_w = area->width;
        coop_chat_vb_h = area->height;
        coop_chat_vb_dirty = true;
    }
    if (!coop_chat_vb_dirty) {
        return true;
    }
    content.x = 0;
    content.y = 0;
    content.width = area->width;
    content.height = area->height;
    tig_video_buffer_fill(coop_chat_vb, &content, coop_chat_key);
    /* Newest line at the bottom, right above the HUD. */
    first = COOP_CHAT_UI_LINES - coop_chat_count;
    tig_font_push(coop_chat_font);
    for (i = 0; i < coop_chat_count; i++) {
        line_rect.x = 0;
        line_rect.y = (first + i) * COOP_CHAT_UI_LINE_H;
        line_rect.width = area->width;
        line_rect.height = COOP_CHAT_UI_LINE_H;
        tig_font_write(coop_chat_vb, coop_chat_lines[i], &line_rect, &dirty);
    }
    tig_font_pop();
    coop_chat_vb_dirty = false;
    return true;
}

static void coop_chat_ui_draw(GameDrawInfo* draw_info)
{
    TigRect area;
    TigRect src_rect;
    TigRect dst_rect;
    TigRectListNode* node;
    TigWindowBlitInfo blit;

    if (!coop_chat_visible || coop_chat_count <= 0
        || coop_chat_iso_window == TIG_WINDOW_HANDLE_INVALID
        || coop_chat_font == TIG_FONT_HANDLE_INVALID) {
        return;
    }
    coop_chat_ui_area(&area);
    if (!coop_chat_ui_vb_render(&area)) {
        return;
    }
    memset(&blit, 0, sizeof(blit));
    blit.type = TIG_WINDOW_BLIT_VIDEO_BUFFER_TO_WINDOW;
    blit.vb_blit_flags = TIG_VIDEO_BUFFER_BLIT_BLEND_ALPHA_CONST;
    blit.alpha[0] = 255;
    blit.src_video_buffer = coop_chat_vb;
    blit.src_rect = &src_rect;
    blit.dst_rect = &dst_rect;
    blit.dst_window_handle = coop_chat_iso_window;
    for (node = *draw_info->rects; node != NULL; node = node->next) {
        if (tig_rect_intersection(&(node->rect), &area, &dst_rect) != TIG_OK) {
            continue;
        }
        src_rect.x = dst_rect.x - area.x;
        src_rect.y = dst_rect.y - area.y;
        src_rect.width = dst_rect.width;
        src_rect.height = dst_rect.height;
        tig_window_blit(&blit);
    }
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
    coop_chat_vb_dirty = true;
    coop_chat_ui_invalidate();
}

static void coop_chat_ui_set_visible(bool on)
{
    if (coop_chat_visible != on) {
        coop_chat_visible = on;
    }
    coop_chat_ui_invalidate();
}

static void coop_chat_ui_set_iso(tig_window_handle_t window_handle)
{
    TigWindowData window_data;

    coop_chat_iso_window = window_handle;
    coop_chat_iso_rect.x = 0;
    coop_chat_iso_rect.y = 0;
    coop_chat_iso_rect.width = hrp_iso_window_width_get();
    coop_chat_iso_rect.height = hrp_iso_window_height_get();
    if (window_handle != TIG_WINDOW_HANDLE_INVALID && tig_window_data(window_handle, &window_data) == TIG_OK) {
        coop_chat_iso_rect.width = window_data.rect.width;
        coop_chat_iso_rect.height = window_data.rect.height;
    }
}

static void coop_chat_ui_create_font(void)
{
    TigFont font_desc;

    if (coop_chat_font != TIG_FONT_HANDLE_INVALID) {
        return;
    }
    memset(&font_desc, 0, sizeof(font_desc));
    /* Same font setup as the vanilla text floaters (tf.c). The system
     * font (misc art) blits nothing into an off-screen colour-keyed buffer. */
    font_desc.flags = TIG_FONT_NO_ALPHA_BLEND | TIG_FONT_SHADOW;
    tig_art_interface_id_create(229, 0, 0, 0, &font_desc.art_id);
    font_desc.color = tig_color_make(235, 235, 210);
    tig_font_create(&font_desc, &coop_chat_font);
}

bool coop_chat_ui_init(GameInitInfo* init_info)
{
    memset(coop_chat_lines, 0, sizeof(coop_chat_lines));
    coop_chat_count = 0;
    coop_chat_visible = false;
    coop_chat_ui_set_iso(init_info != NULL ? init_info->iso_window_handle : TIG_WINDOW_HANDLE_INVALID);
    coop_chat_ui_create_font();
    gamelib_set_overlay_draw_func(coop_chat_ui_draw);
    gamelib_set_overlay_scroll_func(coop_chat_ui_on_scroll);
    coop_lite_set_chat_ui_hooks(coop_chat_ui_on_line, coop_chat_ui_set_visible);
    coop_chat_inited = true;
    return true;
}

void coop_chat_ui_exit(void)
{
    coop_lite_set_chat_ui_hooks(NULL, NULL);
    gamelib_set_overlay_draw_func(NULL);
    gamelib_set_overlay_scroll_func(NULL);
    if (coop_chat_font != TIG_FONT_HANDLE_INVALID) {
        tig_font_destroy(coop_chat_font);
        coop_chat_font = TIG_FONT_HANDLE_INVALID;
    }
    coop_chat_ui_vb_destroy();
    coop_chat_iso_window = TIG_WINDOW_HANDLE_INVALID;
    coop_chat_inited = false;
}

void coop_chat_ui_reset(void)
{
    memset(coop_chat_lines, 0, sizeof(coop_chat_lines));
    coop_chat_count = 0;
    coop_chat_visible = false;
    coop_chat_vb_dirty = true;
    coop_chat_ui_invalidate();
}

void coop_chat_ui_resize(GameResizeInfo* resize_info)
{
    if (!coop_chat_inited || resize_info == NULL) {
        return;
    }
    coop_chat_ui_set_iso(resize_info->window_handle);
    coop_chat_vb_dirty = true;
    coop_chat_ui_invalidate();
}
