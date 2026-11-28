/*
 * Coop Middle overlay, drawn into the iso view like the chat (PT77e):
 *   - hold Alt: names over loot on the ground, containers and lootable
 *     corpses near the player (local only, nothing is sent);
 *   - pings: a ring in the pinger's colour for 5 s (middle click);
 *   - shared pause banner (top centre) and the F9 net HUD (top left).
 * Everything goes into one colour-keyed buffer the size of the iso view;
 * it is re-rendered only when something changed and blitted into the
 * game's dirty rects. While labels or rings are up the view is repainted
 * ~10 times a second (they follow scrolling and blinking).
 */
#include "ui/coop_mid_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "game/coop_lite.h"
#include "game/critter.h"
#include "game/gamelib.h"
#include "game/hrp.h"
#include "game/location.h"
#include "game/obj.h"
#include "game/object.h"
#include "game/player.h"
#include "tig/art.h"
#include "tig/color.h"
#include "tig/font.h"
#include "tig/kb.h"
#include "tig/rect.h"
#include "tig/video.h"
#include "tig/window.h"
#include "ui/intgame.h"
#include "ui/inven_ui.h"
#include "ui/wmap_ui.h"

#define COOP_MID_UI_MAX_LABELS 48
#define COOP_MID_UI_LABEL_RANGE 18
#define COOP_MID_UI_REPAINT_MS 100u

static tig_window_handle_t coop_mid_iso_window = TIG_WINDOW_HANDLE_INVALID;
static TigRect coop_mid_iso_rect;
static tig_font_handle_t coop_mid_font_label = TIG_FONT_HANDLE_INVALID;
static tig_font_handle_t coop_mid_font_banner = TIG_FONT_HANDLE_INVALID;
static tig_font_handle_t coop_mid_font_hud = TIG_FONT_HANDLE_INVALID;
static TigVideoBuffer* coop_mid_vb;
static int coop_mid_vb_w;
static int coop_mid_vb_h;
static tig_color_t coop_mid_key;
static bool coop_mid_dirty = true;
static bool coop_mid_inited;

/* What the last render showed. */
static bool coop_mid_shown;
static bool coop_mid_labels_on;
static bool coop_mid_rings_on;
static char coop_mid_banner[128];
static char coop_mid_hud[128];
static unsigned int coop_mid_repaint_ms;

static void coop_mid_ui_invalidate(void)
{
    if (coop_mid_inited) {
        gamelib_invalidate_rect(&coop_mid_iso_rect);
    }
}

static void coop_mid_ui_vb_destroy(void)
{
    if (coop_mid_vb != NULL) {
        tig_video_buffer_destroy(coop_mid_vb);
        coop_mid_vb = NULL;
    }
    coop_mid_vb_w = 0;
    coop_mid_vb_h = 0;
}

static void coop_mid_ui_fill(int x, int y, int w, int h, tig_color_t color)
{
    TigRect r;
    TigRect bounds;
    TigRect clip;

    r.x = x;
    r.y = y;
    r.width = w;
    r.height = h;
    bounds.x = 0;
    bounds.y = 0;
    bounds.width = coop_mid_vb_w;
    bounds.height = coop_mid_vb_h;
    if (tig_rect_intersection(&r, &bounds, &clip) == TIG_OK) {
        tig_video_buffer_fill(coop_mid_vb, &clip, color);
    }
}

static int coop_mid_ui_text_width(const char* text)
{
    TigFont f;

    memset(&f, 0, sizeof(f));
    f.str = text;
    tig_font_measure(&f);
    return f.width;
}

/* Text centred on (cx, y); skipped when it does not fit. */
static void coop_mid_ui_text(tig_font_handle_t font, const char* text, int cx, int y)
{
    TigRect rect;
    TigRect dirty;
    int w;

    tig_font_push(font);
    w = coop_mid_ui_text_width(text) + 4;
    rect.x = cx - w / 2;
    rect.y = y;
    rect.width = w;
    rect.height = 16;
    if (rect.x < 0) {
        rect.x = 0;
    }
    if (rect.x + rect.width > coop_mid_vb_w) {
        rect.x = coop_mid_vb_w - rect.width;
    }
    if (rect.y >= 0 && rect.y + rect.height <= coop_mid_vb_h && rect.x >= 0) {
        tig_font_write(coop_mid_vb, text, &rect, &dirty);
    }
    tig_font_pop();
}

static bool coop_mid_ui_screen_xy(int64_t obj, int* x, int* y)
{
    int64_t sx;
    int64_t sy;

    location_xy(obj_field_int64_get(obj, OBJ_F_LOCATION), &sx, &sy);
    sx += obj_field_int32_get(obj, OBJ_F_OFFSET_X) + 40;
    sy += obj_field_int32_get(obj, OBJ_F_OFFSET_Y) + 20;
    if (sx < -100 || sy < -100 || sx > coop_mid_iso_rect.width + 100 || sy > coop_mid_iso_rect.height + 100) {
        return false;
    }
    *x = (int)sx;
    *y = (int)sy;
    return true;
}

static bool coop_mid_ui_labelled(int64_t obj)
{
    int type = obj_field_int32_get(obj, OBJ_F_TYPE);
    unsigned int flags = obj_field_int32_get(obj, OBJ_F_FLAGS);

    if ((flags & (OF_OFF | OF_DESTROYED | OF_DONTDRAW | OF_INVENTORY)) != 0) {
        return false;
    }
    if (type == OBJ_TYPE_CONTAINER) {
        return true;
    }
    if (obj_type_is_item(type)) {
        return (obj_field_int32_get(obj, OBJ_F_ITEM_FLAGS) & OIF_NO_DISPLAY) == 0;
    }
    if (type == OBJ_TYPE_NPC) {
        return critter_is_dead(obj) && obj_field_int32_get(obj, OBJ_F_CRITTER_INVENTORY_NUM) > 0;
    }
    return false;
}

static int coop_mid_ui_draw_labels(void)
{
    int64_t pc = player_get_local_pc_obj();
    int64_t pc_loc;
    ObjectList objects;
    ObjectNode* node;
    int count = 0;

    if (pc == OBJ_HANDLE_NULL) {
        return 0;
    }
    pc_loc = obj_field_int64_get(pc, OBJ_F_LOCATION);
    object_list_vicinity(pc, OBJ_TM_CONTAINER | OBJ_TM_ITEM | OBJ_TM_NPC, &objects);
    for (node = objects.head; node != NULL && count < COOP_MID_UI_MAX_LABELS; node = node->next) {
        char name[MAX_STRING];
        int x;
        int y;

        if (!coop_mid_ui_labelled(node->obj)
            || location_dist(pc_loc, obj_field_int64_get(node->obj, OBJ_F_LOCATION)) > COOP_MID_UI_LABEL_RANGE
            || !coop_mid_ui_screen_xy(node->obj, &x, &y)) {
            continue;
        }
        name[0] = '\0';
        object_examine(node->obj, pc, name);
        if (name[0] == '\0') {
            continue;
        }
        name[63] = '\0';
        /* Stagger a little so stacked piles stay readable. */
        coop_mid_ui_text(coop_mid_font_label, name, x, y - 34 - (count % 3) * 4);
        count++;
    }
    object_list_destroy(&objects);
    return count;
}

static int coop_mid_ui_draw_rings(void)
{
    int i;
    int n = 0;

    for (i = 0; i < COOP_PING_MAX; i++) {
        int64_t loc;
        int slot;
        unsigned int age;
        int64_t sx;
        int64_t sy;
        int r;
        int g;
        int b;
        int k;
        int rx;
        int ry;
        tig_color_t color;
        tig_color_t white = tig_color_make(255, 255, 255);

        if (!coop_lite_mid_ping_get(i, &loc, &slot, &age)) {
            continue;
        }
        n++;
        location_xy(loc, &sx, &sy);
        sx += 40;
        sy += 20;
        coop_lite_slot_color(slot, &r, &g, &b);
        color = tig_color_make(r, g, b);
        /* Pulse: the ring grows over each second. */
        rx = 22 + (int)((age % 1000u) * 18u / 1000u);
        ry = rx / 2;
        for (k = 0; k < 72; k++) {
            double a = (double)k * 3.14159265358979 / 36.0;
            int px = (int)sx + (int)(cos(a) * rx);
            int py = (int)sy + (int)(sin(a) * ry);

            coop_mid_ui_fill(px - 1, py - 1, 3, 3, color);
        }
        /* Fixed inner ring + centre dot. */
        for (k = 0; k < 36; k++) {
            double a = (double)k * 3.14159265358979 / 18.0;

            coop_mid_ui_fill((int)sx + (int)(cos(a) * 12) - 1, (int)sy + (int)(sin(a) * 6) - 1, 2, 2, white);
        }
        coop_mid_ui_fill((int)sx - 2, (int)sy - 2, 5, 5, color);
    }
    return n;
}

static bool coop_mid_ui_vb_ensure(void)
{
    TigVideoBufferCreateInfo vb_create_info;

    if (coop_mid_vb != NULL && (coop_mid_vb_w != coop_mid_iso_rect.width || coop_mid_vb_h != coop_mid_iso_rect.height)) {
        coop_mid_ui_vb_destroy();
    }
    if (coop_mid_vb != NULL) {
        return true;
    }
    if (coop_mid_iso_rect.width <= 0 || coop_mid_iso_rect.height <= 0) {
        return false;
    }
    coop_mid_key = tig_color_make(0, 0, 255);
    vb_create_info.flags = TIG_VIDEO_BUFFER_CREATE_COLOR_KEY | TIG_VIDEO_BUFFER_CREATE_VIDEO_MEMORY;
    vb_create_info.width = coop_mid_iso_rect.width;
    vb_create_info.height = coop_mid_iso_rect.height;
    vb_create_info.background_color = coop_mid_key;
    vb_create_info.color_key = coop_mid_key;
    if (tig_video_buffer_create(&vb_create_info, &coop_mid_vb) != TIG_OK) {
        coop_mid_vb = NULL;
        return false;
    }
    coop_mid_vb_w = coop_mid_iso_rect.width;
    coop_mid_vb_h = coop_mid_iso_rect.height;
    coop_mid_dirty = true;
    return true;
}

static void coop_mid_ui_render(void)
{
    TigRect all;

    all.x = 0;
    all.y = 0;
    all.width = coop_mid_vb_w;
    all.height = coop_mid_vb_h;
    tig_video_buffer_fill(coop_mid_vb, &all, coop_mid_key);
    if (coop_mid_rings_on) {
        coop_mid_ui_draw_rings();
    }
    if (coop_mid_labels_on) {
        coop_mid_ui_draw_labels();
    }
    if (coop_mid_banner[0] != '\0') {
        coop_mid_ui_text(coop_mid_font_banner, coop_mid_banner, coop_mid_vb_w / 2, 36);
    }
    if (coop_mid_hud[0] != '\0') {
        tig_font_push(coop_mid_font_hud);
        {
            TigRect rect;
            TigRect dirty;

            rect.x = 8;
            rect.y = 8;
            rect.width = coop_mid_ui_text_width(coop_mid_hud) + 4;
            rect.height = 16;
            if (rect.x + rect.width <= coop_mid_vb_w) {
                tig_font_write(coop_mid_vb, coop_mid_hud, &rect, &dirty);
            }
        }
        tig_font_pop();
    }
    coop_mid_dirty = false;
}

static void coop_mid_ui_draw(GameDrawInfo* draw_info)
{
    TigRectListNode* node;
    TigWindowBlitInfo blit;
    TigRect src_rect;
    TigRect dst_rect;

    if (!coop_mid_shown || coop_mid_iso_window == TIG_WINDOW_HANDLE_INVALID
        || coop_mid_font_label == TIG_FONT_HANDLE_INVALID || !coop_mid_ui_vb_ensure()) {
        return;
    }
    if (coop_mid_dirty) {
        coop_mid_ui_render();
    }
    memset(&blit, 0, sizeof(blit));
    blit.type = TIG_WINDOW_BLIT_VIDEO_BUFFER_TO_WINDOW;
    blit.vb_blit_flags = TIG_VIDEO_BUFFER_BLIT_BLEND_ALPHA_CONST;
    blit.alpha[0] = 255;
    blit.src_video_buffer = coop_mid_vb;
    blit.src_rect = &src_rect;
    blit.dst_rect = &dst_rect;
    blit.dst_window_handle = coop_mid_iso_window;
    for (node = *draw_info->rects; node != NULL; node = node->next) {
        if (tig_rect_intersection(&(node->rect), &coop_mid_iso_rect, &dst_rect) != TIG_OK) {
            continue;
        }
        src_rect = dst_rect;
        tig_window_blit(&blit);
    }
}

static void coop_mid_ui_on_scroll(int dx, int dy)
{
    (void)dx;
    (void)dy;
    if (coop_mid_shown) {
        coop_mid_dirty = true;
        coop_mid_ui_invalidate();
    }
}

static void coop_mid_ui_tick(unsigned int now_ms)
{
    char banner[128];
    char hud[128];
    bool labels;
    bool rings = false;
    bool shown;
    bool changed = false;
    int i;

    if (!coop_mid_inited) {
        return;
    }
    wmap_ui_coop_ping_tick(now_ms);
    labels = tig_kb_get_modifier(SDL_KMOD_ALT) && intgame_mode_get() == INTGAME_MODE_MAIN
        && !inven_ui_is_created() && !wmap_ui_is_created();
    for (i = 0; i < COOP_PING_MAX; i++) {
        if (coop_lite_mid_ping_get(i, NULL, NULL, NULL)) {
            rings = true;
            break;
        }
    }
    if (!coop_lite_mid_pause_banner(banner, sizeof(banner))) {
        banner[0] = '\0';
    }
    if (!coop_lite_mid_hud_text(hud, sizeof(hud))) {
        hud[0] = '\0';
    }
    if (strcmp(banner, coop_mid_banner) != 0 || strcmp(hud, coop_mid_hud) != 0
        || labels != coop_mid_labels_on || rings != coop_mid_rings_on) {
        changed = true;
    }
    snprintf(coop_mid_banner, sizeof(coop_mid_banner), "%s", banner);
    snprintf(coop_mid_hud, sizeof(coop_mid_hud), "%s", hud);
    coop_mid_labels_on = labels;
    coop_mid_rings_on = rings;
    shown = labels || rings || banner[0] != '\0' || hud[0] != '\0';
    if (changed || (shown != coop_mid_shown)) {
        /* When nothing is left the repaint simply skips the overlay. */
        coop_mid_dirty = true;
        coop_mid_repaint_ms = now_ms;
        coop_mid_ui_invalidate();
        coop_mid_shown = shown;
        return;
    }
    if ((labels || rings) && now_ms - coop_mid_repaint_ms >= COOP_MID_UI_REPAINT_MS) {
        coop_mid_repaint_ms = now_ms;
        coop_mid_dirty = true;
        coop_mid_ui_invalidate();
    }
}

static void coop_mid_ui_ping_added(void)
{
    coop_mid_dirty = true;
    coop_mid_ui_invalidate();
}

static void coop_mid_ui_set_iso(tig_window_handle_t window_handle)
{
    TigWindowData window_data;

    coop_mid_iso_window = window_handle;
    coop_mid_iso_rect.x = 0;
    coop_mid_iso_rect.y = 0;
    coop_mid_iso_rect.width = hrp_iso_window_width_get();
    coop_mid_iso_rect.height = hrp_iso_window_height_get();
    if (window_handle != TIG_WINDOW_HANDLE_INVALID && tig_window_data(window_handle, &window_data) == TIG_OK) {
        coop_mid_iso_rect.width = window_data.rect.width;
        coop_mid_iso_rect.height = window_data.rect.height;
    }
}

static tig_font_handle_t coop_mid_ui_font(int r, int g, int b)
{
    TigFont font_desc;
    tig_font_handle_t font = TIG_FONT_HANDLE_INVALID;

    memset(&font_desc, 0, sizeof(font_desc));
    /* Same setup as the text floaters / chat (font art 229, shadow). */
    font_desc.flags = TIG_FONT_NO_ALPHA_BLEND | TIG_FONT_SHADOW;
    tig_art_interface_id_create(229, 0, 0, 0, &font_desc.art_id);
    font_desc.color = tig_color_make(r, g, b);
    tig_font_create(&font_desc, &font);
    return font;
}

static void coop_mid_ui_font_destroy(tig_font_handle_t* font)
{
    if (*font != TIG_FONT_HANDLE_INVALID) {
        tig_font_destroy(*font);
        *font = TIG_FONT_HANDLE_INVALID;
    }
}

bool coop_mid_ui_init(GameInitInfo* init_info)
{
    coop_mid_ui_set_iso(init_info != NULL ? init_info->iso_window_handle : TIG_WINDOW_HANDLE_INVALID);
    coop_mid_font_label = coop_mid_ui_font(240, 230, 170);
    coop_mid_font_banner = coop_mid_ui_font(255, 210, 60);
    coop_mid_font_hud = coop_mid_ui_font(150, 240, 150);
    gamelib_set_overlay2_draw_func(coop_mid_ui_draw);
    gamelib_set_overlay2_scroll_func(coop_mid_ui_on_scroll);
    coop_lite_set_mid_ui_hooks(coop_mid_ui_tick, coop_mid_ui_ping_added);
    coop_mid_inited = true;
    return true;
}

void coop_mid_ui_exit(void)
{
    coop_lite_set_mid_ui_hooks(NULL, NULL);
    gamelib_set_overlay2_draw_func(NULL);
    gamelib_set_overlay2_scroll_func(NULL);
    coop_mid_ui_font_destroy(&coop_mid_font_label);
    coop_mid_ui_font_destroy(&coop_mid_font_banner);
    coop_mid_ui_font_destroy(&coop_mid_font_hud);
    coop_mid_ui_vb_destroy();
    coop_mid_iso_window = TIG_WINDOW_HANDLE_INVALID;
    coop_mid_inited = false;
}

void coop_mid_ui_reset(void)
{
    coop_mid_shown = false;
    coop_mid_labels_on = false;
    coop_mid_rings_on = false;
    coop_mid_banner[0] = '\0';
    coop_mid_hud[0] = '\0';
    coop_mid_dirty = true;
    coop_mid_ui_invalidate();
}

void coop_mid_ui_resize(GameResizeInfo* resize_info)
{
    if (!coop_mid_inited || resize_info == NULL) {
        return;
    }
    coop_mid_ui_set_iso(resize_info->window_handle);
    coop_mid_dirty = true;
    coop_mid_ui_invalidate();
}
