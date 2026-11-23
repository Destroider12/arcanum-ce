/*
 * Coop Middle M2 UI (protocol 54). See ui/coop_middle_ui.h and
 * docs/coop-middle.md. Engine files only call the entry points here.
 */
#include "ui/coop_middle_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/coop_lite.h"
#include "game/coop_middle.h"
#include "game/critter.h"
#include "game/gamelib.h"
#include "game/hrp.h"
#include "game/item.h"
#include "game/location.h"
#include "game/multiplayer.h"
#include "game/obj.h"
#include "game/obj_flags.h"
#include "game/object_node.h"
#include "game/object.h"
#include "game/player.h"
#include "net/tig_net.h"
#include "tig/art.h"
#include "tig/color.h"
#include "tig/debug.h"
#include "tig/font.h"
#include "tig/kb.h"
#include "tig/mouse.h"
#include "tig/rect.h"
#include "tig/video.h"
#include "tig/window.h"
#include "ui/dialog_ui.h"
#include "ui/follower_ui.h"
#include "ui/intgame.h"
#include "ui/inven_ui.h"
#include "ui/mainmenu_ui.h"
#include "ui/textedit_ui.h"

static bool coop_mid_ui_inited;

static bool coop_mid_ui_session(void)
{
    return tig_net_is_active() && tig_net_is_coop_enabled();
}

static void coop_mid_ui_notice(const char* en, const char* ru, bool good)
{
    coop_lite_mid_notice(coop_lite_lang_ru() ? ru : en, good);
}

/* ---- direct trade ---- */

bool coop_middle_ui_trade_try(int64_t target, bool from_portrait)
{
    int64_t pc;
    char name[128];
    int dist;

    if (!coop_mid_ui_session() || target == OBJ_HANDLE_NULL) {
        return false;
    }
    pc = player_get_local_pc_obj();
    if (pc == OBJ_HANDLE_NULL || target == pc || obj_field_int32_get(target, OBJ_F_TYPE) != OBJ_TYPE_PC
        || !coop_lite_is_teammate_pc(target) || inven_ui_drag_item_obj_get() != OBJ_HANDLE_NULL) {
        return false;
    }
    if (!coop_middle_rule_on(COOP_RULE_DIRECT_TRADE)) {
        if (!from_portrait) {
            return false; /* Ctrl+click goes on as usual */
        }
        coop_mid_ui_notice("Direct trade is turned off by the host.", "Прямой обмен выключен хостом.", false);
        return true;
    }
    if (critter_is_dead(pc) || critter_is_dead(target)) {
        coop_mid_ui_notice("You cannot trade right now.", "Сейчас обмен невозможен.", false);
        return true;
    }
    dist = (int)location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION), obj_field_int64_get(target, OBJ_F_LOCATION));
    if (dist < 0 || dist > COOP_TRADE_RANGE) {
        coop_mid_ui_notice("Move closer to trade.", "Подойдите ближе для обмена.", false);
        return true;
    }
    name[0] = '\0';
    object_examine(target, pc, name);
    coop_middle_trade_local_begin(target);
    if (!inven_ui_open(pc, target, INVEN_UI_MODE_STEAL) || !inven_ui_is_created()) {
        coop_middle_trade_local_end();
        coop_mid_ui_notice("You cannot trade right now.", "Сейчас обмен невозможен.", false);
        tig_debug_printf("CoopMiddle: trade: window with '%s' did not open\n", name);
        return true;
    }
    tig_debug_printf("CoopMiddle: trade: window opened with '%s' (%s, %d tiles)\n", name,
        from_portrait ? "portrait" : "ctrl+click", dist);
    return true;
}

/* ---- status badges ---- */

#define COOP_MID_BADGE 14
#define COOP_MID_FONTS 8

/* One font per colour (the tf.c font setup; fonts carry their colour). */
static struct {
    tig_color_t color;
    tig_font_handle_t font;
} coop_mid_fonts[COOP_MID_FONTS];
static int coop_mid_font_count;
static uint32_t coop_mid_badge_sig; /* what the portraits show now */

static tig_font_handle_t coop_mid_ui_font(uint8_t r, uint8_t g, uint8_t b)
{
    tig_color_t color = tig_color_make(r, g, b);
    TigFont font_desc;
    int i;

    for (i = 0; i < coop_mid_font_count; i++) {
        if (coop_mid_fonts[i].color == color) {
            return coop_mid_fonts[i].font;
        }
    }
    if (coop_mid_font_count >= COOP_MID_FONTS) {
        return coop_mid_fonts[0].font;
    }
    memset(&font_desc, 0, sizeof(font_desc));
    font_desc.flags = TIG_FONT_NO_ALPHA_BLEND | TIG_FONT_SHADOW;
    tig_art_interface_id_create(229, 0, 0, 0, &font_desc.art_id);
    font_desc.color = color;
    coop_mid_fonts[coop_mid_font_count].color = color;
    tig_font_create(&font_desc, &(coop_mid_fonts[coop_mid_font_count].font));
    return coop_mid_fonts[coop_mid_font_count++].font;
}

static void coop_mid_ui_fonts_destroy(void)
{
    int i;

    for (i = 0; i < coop_mid_font_count; i++) {
        if (coop_mid_fonts[i].font != TIG_FONT_HANDLE_INVALID) {
            tig_font_destroy(coop_mid_fonts[i].font);
        }
    }
    coop_mid_font_count = 0;
}

static bool coop_mid_ui_badge_of(int64_t obj, char* tag, uint8_t* r, uint8_t* g, uint8_t* b)
{
    int slot = coop_middle_slot_of_pc(obj);

    if (obj == OBJ_HANDLE_NULL || slot < 0) {
        return false;
    }
    return coop_middle_status_badge(critter_is_dead(obj), !critter_is_dead(obj) && critter_fatigue_current(obj) <= 0,
        coop_middle_slot_status(slot), tag, r, g, b);
}

void coop_middle_ui_draw_badge(tig_window_handle_t window_handle, int64_t obj, int width)
{
    TigRect rect;
    char tag[4];
    uint8_t r;
    uint8_t g;
    uint8_t b;

    if (!coop_mid_ui_session() || window_handle == TIG_WINDOW_HANDLE_INVALID
        || !coop_mid_ui_badge_of(obj, tag, &r, &g, &b)) {
        return;
    }
    rect.x = width - COOP_MID_BADGE;
    rect.y = 0;
    rect.width = COOP_MID_BADGE;
    rect.height = COOP_MID_BADGE;
    tig_window_fill(window_handle, &rect, tig_color_make(16, 14, 12));
    tig_window_box(window_handle, &rect, tig_color_make(r, g, b));
    rect.x += 1;
    rect.y += 1;
    rect.width -= 2;
    rect.height -= 2;
    tig_font_push(coop_mid_ui_font(r, g, b));
    tig_window_text_write(window_handle, tag, &rect);
    tig_font_pop();
}

/* A signature of every teammate's badge: redraw the portraits on change. */
static uint32_t coop_mid_ui_badge_signature(void)
{
    uint32_t sig = coop_middle_status_packed() * 2654435761u;
    int slot;

    for (slot = 0; slot < 8; slot++) {
        int64_t pc = sub_4A2B60(slot);

        if (pc != OBJ_HANDLE_NULL) {
            if (critter_is_dead(pc)) {
                sig ^= 1u << slot;
            } else if (critter_fatigue_current(pc) <= 0) {
                sig ^= 1u << (slot + 8);
            }
        }
    }
    return sig;
}

/* ---- iso overlay: shared ---- */

static tig_window_handle_t coop_mid_iso_window = TIG_WINDOW_HANDLE_INVALID;
static int coop_mid_iso_w;
static int coop_mid_iso_h;
static tig_color_t coop_mid_key; /* colour key of the off-screen buffers */

static void coop_mid_ui_set_iso(tig_window_handle_t window_handle)
{
    TigWindowData window_data;

    coop_mid_iso_window = window_handle;
    coop_mid_iso_w = hrp_iso_window_width_get();
    coop_mid_iso_h = hrp_iso_window_height_get();
    if (window_handle != TIG_WINDOW_HANDLE_INVALID && tig_window_data(window_handle, &window_data) == TIG_OK) {
        coop_mid_iso_w = window_data.rect.width;
        coop_mid_iso_h = window_data.rect.height;
    }
}

static TigVideoBuffer* coop_mid_ui_vb_create(int w, int h)
{
    TigVideoBufferCreateInfo vb_create_info;
    TigVideoBuffer* vb = NULL;

    coop_mid_key = tig_color_make(0, 0, 255);
    memset(&vb_create_info, 0, sizeof(vb_create_info));
    vb_create_info.flags = TIG_VIDEO_BUFFER_CREATE_COLOR_KEY | TIG_VIDEO_BUFFER_CREATE_VIDEO_MEMORY;
    vb_create_info.width = w;
    vb_create_info.height = h;
    vb_create_info.background_color = coop_mid_key;
    vb_create_info.color_key = coop_mid_key;
    if (tig_video_buffer_create(&vb_create_info, &vb) != TIG_OK) {
        return NULL;
    }
    return vb;
}

/* Blit buffer area (src_x, src_y) of rect's size to rect in the iso view,
   inside the frame's dirty rects only. */
static void coop_mid_ui_blit(GameDrawInfo* draw_info, TigVideoBuffer* vb, const TigRect* rect, int src_x, int src_y)
{
    TigRectListNode* node;
    TigWindowBlitInfo blit;
    TigRect src_rect;
    TigRect dst_rect;

    memset(&blit, 0, sizeof(blit));
    blit.type = TIG_WINDOW_BLIT_VIDEO_BUFFER_TO_WINDOW;
    blit.vb_blit_flags = TIG_VIDEO_BUFFER_BLIT_BLEND_ALPHA_CONST;
    blit.alpha[0] = 255;
    blit.src_video_buffer = vb;
    blit.src_rect = &src_rect;
    blit.dst_rect = &dst_rect;
    blit.dst_window_handle = coop_mid_iso_window;
    for (node = *draw_info->rects; node != NULL; node = node->next) {
        if (tig_rect_intersection(&(node->rect), rect, &dst_rect) != TIG_OK) {
            continue;
        }
        src_rect.x = src_x + dst_rect.x - rect->x;
        src_rect.y = src_y + dst_rect.y - rect->y;
        src_rect.width = dst_rect.width;
        src_rect.height = dst_rect.height;
        tig_window_blit(&blit);
    }
}

static int coop_mid_ui_text_width(tig_font_handle_t font, const char* text)
{
    TigFont font_desc;

    memset(&font_desc, 0, sizeof(font_desc));
    tig_font_push(font);
    font_desc.str = text;
    font_desc.width = 0;
    tig_font_measure(&font_desc);
    tig_font_pop();
    return font_desc.width;
}

/* ---- Alt loot labels ---- */

#define COOP_MID_LABEL_H 16
#define COOP_MID_LABEL_W_MAX 220
#define COOP_MID_LABEL_PAD 4
#define COOP_MID_LABEL_CANDIDATES 256
#define COOP_MID_LABEL_REFRESH_MS 150u

typedef struct CoopMidLabel {
    int64_t obj;
    char text[64];
    uint8_t r;
    uint8_t g;
    uint8_t b;
    int cat; /* 0 ground item, 1 container / corpse */
    long long score; /* squared pixels to the PC or the cursor, nearer first */
    int cx;
    int cy;
} CoopMidLabel;

static CoopMidLabel coop_mid_labels[COOP_LABELS_MAX];
static CoopLabelRect coop_mid_label_rects[COOP_LABELS_MAX];
static int coop_mid_label_count;
static bool coop_mid_labels_on;
static unsigned int coop_mid_labels_ms;
static TigVideoBuffer* coop_mid_label_vb;

static void coop_mid_ui_labels_invalidate(void)
{
    TigRect rect;
    int i;

    for (i = 0; i < coop_mid_label_count; i++) {
        if (coop_mid_label_rects[i].hidden) {
            continue;
        }
        rect.x = coop_mid_label_rects[i].x;
        rect.y = coop_mid_label_rects[i].y;
        rect.width = coop_mid_label_rects[i].w;
        rect.height = coop_mid_label_rects[i].h;
        gamelib_invalidate_rect(&rect);
    }
}

static bool coop_mid_ui_labels_wanted(void)
{
    int64_t pc = player_get_local_pc_obj();

    return coop_mid_ui_session() && coop_mid_iso_window != TIG_WINDOW_HANDLE_INVALID && pc != OBJ_HANDLE_NULL
        && (tig_kb_is_key_pressed(SDL_SCANCODE_LALT) || tig_kb_is_key_pressed(SDL_SCANCODE_RALT))
        && intgame_mode_get() == INTGAME_MODE_MAIN && !mainmenu_ui_is_active() && !textedit_ui_is_focused()
        && !inven_ui_is_created() && !dialog_ui_is_local_pc_in_dialog() && !gamelib_in_load();
}

static int coop_mid_label_cmp(const void* a, const void* b)
{
    const CoopMidLabel* x = (const CoopMidLabel*)a;
    const CoopMidLabel* y = (const CoopMidLabel*)b;

    if (x->cat != y->cat) {
        return x->cat - y->cat;
    }
    return x->score < y->score ? -1 : (x->score > y->score ? 1 : 0);
}

static void coop_mid_label_color(CoopMidLabel* l, int type, bool notable)
{
    if (l->cat == 1) {
        l->r = 195, l->g = 175, l->b = 140; /* containers / corpses */
    } else if (notable || type == OBJ_TYPE_GOLD || type == OBJ_TYPE_KEY || type == OBJ_TYPE_KEY_RING
        || type == OBJ_TYPE_WRITTEN) {
        l->r = 240, l->g = 210, l->b = 90; /* keys, gold, quest items, schematics / notes */
    } else {
        l->r = 215, l->g = 235, l->b = 255;
    }
}

/* One candidate, or false when the object gets no label. */
static bool coop_mid_label_make(int64_t pc, int64_t obj, CoopMidLabel* l)
{
    bool ru = coop_lite_lang_ru();
    char name[64];
    int type;
    unsigned int flags;

    flags = obj_field_int32_get(obj, OBJ_F_FLAGS);
    if ((flags & (OF_INVENTORY | OF_OFF | OF_DESTROYED | OF_DONTDRAW)) != 0) {
        return false;
    }
    type = obj_field_int32_get(obj, OBJ_F_TYPE);
    memset(l, 0, sizeof(*l));
    l->obj = obj;
    name[0] = '\0';
    if (type >= OBJ_TYPE_WEAPON && type <= OBJ_TYPE_GENERIC) {
        bool notable = (obj_field_int32_get(obj, OBJ_F_ITEM_FLAGS) & (OIF_WONT_SELL | OIF_UBER)) != 0;

        object_examine(obj, pc, name);
        l->cat = 0;
        if (type == OBJ_TYPE_GOLD) {
            snprintf(l->text, sizeof(l->text), ru ? "%d золота" : "%d gold", item_gold_get(obj));
        } else if (type == OBJ_TYPE_AMMO && obj_field_int32_get(obj, OBJ_F_AMMO_QUANTITY) > 1) {
            snprintf(l->text, sizeof(l->text), "%d %s", obj_field_int32_get(obj, OBJ_F_AMMO_QUANTITY), name);
        } else {
            snprintf(l->text, sizeof(l->text), "%s", name);
        }
        coop_mid_label_color(l, type, notable);
    } else if (type == OBJ_TYPE_CONTAINER) {
        bool locked = object_locked_get(obj);

        if (!locked && obj_field_int32_get(obj, OBJ_F_CONTAINER_INVENTORY_NUM) <= 0
            && (obj_field_int32_get(obj, OBJ_F_CONTAINER_FLAGS) & OCOF_INVEN_SPAWN_ONCE) == 0) {
            return false; /* empty */
        }
        object_examine(obj, pc, name);
        l->cat = 1;
        if (locked) {
            snprintf(l->text, sizeof(l->text), ru ? "%s (заперт)" : "%s (locked)", name);
        } else {
            snprintf(l->text, sizeof(l->text), "%s", name);
        }
        coop_mid_label_color(l, type, false);
    } else if (type == OBJ_TYPE_NPC && critter_is_dead(obj)) {
        if (obj_field_int32_get(obj, OBJ_F_CRITTER_INVENTORY_NUM) <= 0 && item_gold_get(obj) <= 0) {
            return false; /* nothing to loot */
        }
        object_examine(obj, pc, name);
        l->cat = 1;
        snprintf(l->text, sizeof(l->text), "%s", name);
        coop_mid_label_color(l, type, false);
    } else {
        return false;
    }
    coop_lite_chat_sanitize(l->text, sizeof(l->text));
    return l->text[0] != '\0';
}

static void coop_mid_ui_labels_collect(void)
{
    static CoopMidLabel cand[COOP_MID_LABEL_CANDIDATES];
    int64_t pc = player_get_local_pc_obj();
    int64_t loc;
    int64_t sx;
    int64_t sy;
    LocRect loc_rect;
    ObjectList objects;
    ObjectNode* node;
    TigMouseState mouse_state;
    TigWindowData window_data;
    int pcx;
    int pcy;
    int mx = -100000;
    int my = -100000;
    int n = 0;
    int i;

    loc = obj_field_int64_get(pc, OBJ_F_LOCATION);
    location_xy(loc, &sx, &sy);
    pcx = (int)sx + 40;
    pcy = (int)sy + 20;
    if (tig_mouse_get_state(&mouse_state) == TIG_OK
        && tig_window_data(coop_mid_iso_window, &window_data) == TIG_OK) {
        mx = mouse_state.x - window_data.rect.x;
        my = mouse_state.y - window_data.rect.y;
    }
    loc_rect.x1 = LOCATION_GET_X(loc) - COOP_LABELS_RANGE;
    loc_rect.x2 = LOCATION_GET_X(loc) + COOP_LABELS_RANGE;
    loc_rect.y1 = LOCATION_GET_Y(loc) - COOP_LABELS_RANGE;
    loc_rect.y2 = LOCATION_GET_Y(loc) + COOP_LABELS_RANGE;
    object_list_rect(&loc_rect, OBJ_TM_ITEM | OBJ_TM_CONTAINER | OBJ_TM_NPC, &objects);
    for (node = objects.head; node != NULL && n < COOP_MID_LABEL_CANDIDATES; node = node->next) {
        CoopMidLabel* l = &cand[n];
        long long dp;
        long long dm;

        if (!coop_mid_label_make(pc, node->obj, l)) {
            continue;
        }
        location_xy(obj_field_int64_get(node->obj, OBJ_F_LOCATION), &sx, &sy);
        l->cx = (int)sx + 40;
        l->cy = (int)sy + 20 - (l->cat == 1 ? 24 : 4);
        if (l->cx < 0 || l->cx >= coop_mid_iso_w || l->cy < 0 || l->cy >= coop_mid_iso_h) {
            continue; /* off screen */
        }
        dp = (long long)(l->cx - pcx) * (l->cx - pcx) + (long long)(l->cy - pcy) * (l->cy - pcy);
        dm = (long long)(l->cx - mx) * (l->cx - mx) + (long long)(l->cy - my) * (l->cy - my);
        l->score = dp < dm ? dp : dm;
        n++;
    }
    object_list_destroy(&objects);
    qsort(cand, (size_t)n, sizeof(cand[0]), coop_mid_label_cmp);
    if (n > COOP_LABELS_MAX) {
        n = COOP_LABELS_MAX;
    }
    for (i = 0; i < n; i++) {
        tig_font_handle_t font = coop_mid_ui_font(cand[i].r, cand[i].g, cand[i].b);
        int w = coop_mid_ui_text_width(font, cand[i].text) + 2 * COOP_MID_LABEL_PAD;

        if (w > COOP_MID_LABEL_W_MAX) {
            w = COOP_MID_LABEL_W_MAX;
        }
        coop_mid_labels[i] = cand[i];
        coop_mid_label_rects[i].w = w;
        coop_mid_label_rects[i].h = COOP_MID_LABEL_H;
        coop_mid_label_rects[i].x = cand[i].cx - w / 2;
        if (coop_mid_label_rects[i].x < 0) {
            coop_mid_label_rects[i].x = 0;
        }
        if (coop_mid_label_rects[i].x + w > coop_mid_iso_w) {
            coop_mid_label_rects[i].x = coop_mid_iso_w - w;
        }
        coop_mid_label_rects[i].y = cand[i].cy - COOP_MID_LABEL_H;
        coop_mid_label_rects[i].hidden = false;
    }
    coop_mid_label_count = n;
    coop_middle_labels_deoverlap(coop_mid_label_rects, n, 0, coop_mid_iso_h);
}

/* Each label i is pre-rendered in row i of one buffer: solid plate, a
   thin frame, the text. */
static void coop_mid_ui_labels_render(void)
{
    TigRect rect;
    TigRect dirty;
    int i;

    if (coop_mid_label_vb == NULL) {
        coop_mid_label_vb = coop_mid_ui_vb_create(COOP_MID_LABEL_W_MAX, COOP_MID_LABEL_H * COOP_LABELS_MAX);
        if (coop_mid_label_vb == NULL) {
            return;
        }
    }
    for (i = 0; i < coop_mid_label_count; i++) {
        CoopMidLabel* l = &coop_mid_labels[i];
        int w = coop_mid_label_rects[i].w;
        tig_color_t frame = tig_color_make(l->r / 2, l->g / 2, l->b / 2);

        rect.x = 0;
        rect.y = i * COOP_MID_LABEL_H;
        rect.width = COOP_MID_LABEL_W_MAX;
        rect.height = COOP_MID_LABEL_H;
        tig_video_buffer_fill(coop_mid_label_vb, &rect, coop_mid_key);
        rect.width = w;
        tig_video_buffer_fill(coop_mid_label_vb, &rect, tig_color_make(16, 14, 12));
        rect.height = 1;
        tig_video_buffer_fill(coop_mid_label_vb, &rect, frame);
        rect.y = i * COOP_MID_LABEL_H + COOP_MID_LABEL_H - 1;
        tig_video_buffer_fill(coop_mid_label_vb, &rect, frame);
        rect.y = i * COOP_MID_LABEL_H;
        rect.width = 1;
        rect.height = COOP_MID_LABEL_H;
        tig_video_buffer_fill(coop_mid_label_vb, &rect, frame);
        rect.x = w - 1;
        tig_video_buffer_fill(coop_mid_label_vb, &rect, frame);
        rect.x = COOP_MID_LABEL_PAD;
        rect.y = i * COOP_MID_LABEL_H + 1;
        rect.width = w - 2 * COOP_MID_LABEL_PAD;
        rect.height = COOP_MID_LABEL_H - 2;
        tig_font_push(coop_mid_ui_font(l->r, l->g, l->b));
        tig_font_write(coop_mid_label_vb, l->text, &rect, &dirty);
        tig_font_pop();
    }
}

static void coop_mid_ui_labels_off(void)
{
    if (coop_mid_labels_on) {
        coop_mid_ui_labels_invalidate(); /* the map repaints over them */
        coop_mid_labels_on = false;
        coop_mid_label_count = 0;
    }
}

static void coop_mid_ui_labels_tick(unsigned int now_ms)
{
    CoopMidLabel old_labels[COOP_LABELS_MAX];
    CoopLabelRect old_rects[COOP_LABELS_MAX];
    int old_count;

    if (!coop_mid_ui_labels_wanted()) {
        coop_mid_ui_labels_off();
        return;
    }
    if (coop_mid_labels_on && (unsigned int)(now_ms - coop_mid_labels_ms) < COOP_MID_LABEL_REFRESH_MS) {
        return;
    }
    coop_mid_labels_ms = now_ms;
    old_count = coop_mid_labels_on ? coop_mid_label_count : 0;
    memcpy(old_labels, coop_mid_labels, sizeof(old_labels));
    memcpy(old_rects, coop_mid_label_rects, sizeof(old_rects));
    coop_mid_ui_labels_collect();
    if (coop_mid_labels_on && old_count == coop_mid_label_count
        && memcmp(old_rects, coop_mid_label_rects, sizeof(old_rects[0]) * (size_t)old_count) == 0) {
        int i;
        bool same = true;

        for (i = 0; i < old_count && same; i++) {
            same = old_labels[i].obj == coop_mid_labels[i].obj
                && strcmp(old_labels[i].text, coop_mid_labels[i].text) == 0;
        }
        if (same) {
            return; /* nothing moved: no repaint */
        }
    }
    if (old_count > 0) {
        int keep = coop_mid_label_count;

        {
            CoopLabelRect new_rects[COOP_LABELS_MAX];

            memcpy(new_rects, coop_mid_label_rects, sizeof(new_rects));
            memcpy(coop_mid_label_rects, old_rects, sizeof(old_rects));
            coop_mid_label_count = old_count;
            coop_mid_ui_labels_invalidate(); /* where the old ones were */
            memcpy(coop_mid_label_rects, new_rects, sizeof(new_rects));
            coop_mid_label_count = keep;
        }
    }
    if (!coop_mid_labels_on) {
        tig_debug_printf("CoopMiddle: labels: Alt held, %d label(s)\n", coop_mid_label_count);
    }
    coop_mid_labels_on = true;
    coop_mid_ui_labels_render();
    coop_mid_ui_labels_invalidate();
}

static void coop_mid_ui_labels_draw(GameDrawInfo* draw_info)
{
    TigRect rect;
    int i;

    if (!coop_mid_labels_on || coop_mid_label_vb == NULL) {
        return;
    }
    for (i = 0; i < coop_mid_label_count; i++) {
        if (coop_mid_label_rects[i].hidden) {
            continue;
        }
        rect.x = coop_mid_label_rects[i].x;
        rect.y = coop_mid_label_rects[i].y;
        rect.width = coop_mid_label_rects[i].w;
        rect.height = coop_mid_label_rects[i].h;
        coop_mid_ui_blit(draw_info, coop_mid_label_vb, &rect, 0, i * COOP_MID_LABEL_H);
    }
}

/* ---- F9 net HUD ---- */

#define COOP_MID_HUD_W 420
#define COOP_MID_HUD_H 16
#define COOP_MID_HUD_REFRESH_MS 500u
#define COOP_MID_HUD_LAG_MS 6000u /* longer than the 5 s status heartbeat */

static bool coop_mid_hud_on;
static bool coop_mid_hud_shown;
static unsigned int coop_mid_hud_ms;
static char coop_mid_hud_text[160];
static bool coop_mid_hud_lag;
static TigVideoBuffer* coop_mid_hud_vb;
static TigRect coop_mid_hud_rect;

static void coop_mid_ui_hud_area(TigRect* rect)
{
    rect->width = COOP_MID_HUD_W;
    rect->height = COOP_MID_HUD_H;
    rect->x = (coop_mid_iso_w - COOP_MID_HUD_W) / 2;
    if (rect->x < 0) {
        rect->x = 0;
    }
    rect->y = 4;
}

bool coop_middle_ui_hud_toggle(void)
{
    if (!coop_mid_ui_session()) {
        return false;
    }
    coop_mid_hud_on = !coop_mid_hud_on;
    coop_mid_hud_ms = 0;
    coop_mid_hud_text[0] = '\0';
    tig_debug_printf("CoopMiddle: net HUD %s (F9)\n", coop_mid_hud_on ? "on" : "off");
    return true;
}

static void coop_mid_ui_hud_format(const TigNetHudStats* st, char* out, size_t sz, bool* lag)
{
    bool ru = coop_lite_lang_ru();
    char rtt[48];
    char sync[48];

    *lag = st->since_rx_ms > COOP_MID_HUD_LAG_MS;
    if (!st->rtt_known) {
        snprintf(rtt, sizeof(rtt), "RTT ?");
    } else if (st->is_host) {
        snprintf(rtt, sizeof(rtt), ru ? "RTT %d мс (ср. %d)" : "RTT %d ms (avg %d)", st->rtt_ms, st->rtt_avg_ms);
    } else {
        snprintf(rtt, sizeof(rtt), ru ? "RTT %d мс" : "RTT %d ms", st->rtt_ms);
    }
    if (*lag) {
        snprintf(sync, sizeof(sync), ru ? "синхр. ОТСТАЁТ %.1f с" : "sync LAG %.1f s", st->since_rx_ms / 1000.0);
    } else {
        snprintf(sync, sizeof(sync), ru ? "синхр. OK" : "sync OK");
    }
    if (st->rtt_known) {
        snprintf(out, sz, ru ? "%s %d/%d | %s | потери %d%% | %s" : "%s %d/%d | %s | loss %d%% | %s",
            ru ? (st->is_host ? "Хост" : "Клиент") : (st->is_host ? "Host" : "Client"),
            st->active_players, st->max_players, rtt, st->loss_pct, sync);
    } else {
        snprintf(out, sz, "%s %d/%d | %s | %s",
            ru ? (st->is_host ? "Хост" : "Клиент") : (st->is_host ? "Host" : "Client"),
            st->active_players, st->max_players, rtt, sync);
    }
}

static void coop_mid_ui_hud_tick(unsigned int now_ms)
{
    TigNetHudStats st;
    char text[160];
    bool lag = false;
    bool want = coop_mid_hud_on && coop_mid_ui_session() && coop_mid_iso_window != TIG_WINDOW_HANDLE_INVALID
        && !mainmenu_ui_is_active();
    TigRect rect;
    TigRect dirty;

    if (!want) {
        if (coop_mid_hud_shown) {
            gamelib_invalidate_rect(&coop_mid_hud_rect);
            coop_mid_hud_shown = false;
        }
        if (!coop_mid_ui_session()) {
            coop_mid_hud_on = false;
        }
        return;
    }
    if (coop_mid_hud_shown && (unsigned int)(now_ms - coop_mid_hud_ms) < COOP_MID_HUD_REFRESH_MS) {
        return;
    }
    coop_mid_hud_ms = now_ms;
    if (!tig_net_get_hud_stats(&st)) {
        return;
    }
    coop_mid_ui_hud_format(&st, text, sizeof(text), &lag);
    if (coop_mid_hud_shown && strcmp(text, coop_mid_hud_text) == 0) {
        return;
    }
    if (lag != coop_mid_hud_lag) {
        tig_debug_printf("CoopMiddle: net HUD: %s\n", text);
    }
    snprintf(coop_mid_hud_text, sizeof(coop_mid_hud_text), "%s", text);
    coop_mid_hud_lag = lag;
    if (coop_mid_hud_vb == NULL) {
        coop_mid_hud_vb = coop_mid_ui_vb_create(COOP_MID_HUD_W, COOP_MID_HUD_H);
        if (coop_mid_hud_vb == NULL) {
            return;
        }
    }
    rect.x = 0;
    rect.y = 0;
    rect.width = COOP_MID_HUD_W;
    rect.height = COOP_MID_HUD_H;
    tig_video_buffer_fill(coop_mid_hud_vb, &rect, tig_color_make(16, 14, 12));
    rect.x = 4;
    rect.y = 1;
    rect.width = COOP_MID_HUD_W - 8;
    rect.height = COOP_MID_HUD_H - 2;
    tig_font_push(lag ? coop_mid_ui_font(240, 140, 60) : coop_mid_ui_font(215, 235, 255));
    tig_font_write(coop_mid_hud_vb, coop_mid_hud_text, &rect, &dirty);
    tig_font_pop();
    coop_mid_ui_hud_area(&coop_mid_hud_rect);
    coop_mid_hud_shown = true;
    gamelib_invalidate_rect(&coop_mid_hud_rect);
}

static void coop_mid_ui_hud_draw(GameDrawInfo* draw_info)
{
    if (coop_mid_hud_shown && coop_mid_hud_vb != NULL) {
        coop_mid_ui_blit(draw_info, coop_mid_hud_vb, &coop_mid_hud_rect, 0, 0);
    }
}

void coop_middle_ui_draw(GameDrawInfo* draw_info)
{
    if (!coop_mid_ui_inited || draw_info == NULL || coop_mid_iso_window == TIG_WINDOW_HANDLE_INVALID) {
        return;
    }
    coop_mid_ui_labels_draw(draw_info);
    coop_mid_ui_hud_draw(draw_info);
}

void coop_middle_ui_on_scroll(int dx, int dy)
{
    TigRect rect;
    int i;

    if (!coop_mid_ui_inited) {
        return;
    }
    /* The scroll moved the window pixels (overlays included) and repaints
       only the exposed strip: repaint the overlays and their shifted copies. */
    if (coop_mid_hud_shown) {
        gamelib_invalidate_rect(&coop_mid_hud_rect);
        rect = coop_mid_hud_rect;
        rect.x += dx;
        rect.y += dy;
        gamelib_invalidate_rect(&rect);
    }
    if (coop_mid_labels_on) {
        for (i = 0; i < coop_mid_label_count; i++) {
            if (coop_mid_label_rects[i].hidden) {
                continue;
            }
            rect.x = coop_mid_label_rects[i].x + dx;
            rect.y = coop_mid_label_rects[i].y + dy;
            rect.width = coop_mid_label_rects[i].w;
            rect.height = coop_mid_label_rects[i].h;
            gamelib_invalidate_rect(&rect);
        }
        coop_mid_ui_labels_invalidate();
        coop_mid_labels_ms = 0; /* labels follow the map on the next tick */
    }
}

/* ---- tick / lifecycle ---- */

static void coop_mid_ui_tick(unsigned int now_ms)
{
    uint8_t bits = 0;
    uint32_t sig;

    if (!coop_mid_ui_inited) {
        return;
    }
    if (!inven_ui_is_created()) {
        coop_middle_trade_local_end(); /* window closed (no-op if none) */
    }
    if (!coop_mid_ui_session()) {
        coop_mid_ui_labels_off();
        coop_mid_ui_hud_tick(now_ms); /* hides it */
        return;
    }
    if (inven_ui_is_created()) {
        bits |= COOP_STATUS_INVEN;
    }
    if (dialog_ui_is_local_pc_in_dialog()) {
        bits |= COOP_STATUS_DIALOG;
    }
    coop_middle_status_set_local(bits);
    sig = coop_mid_ui_badge_signature();
    if (sig != coop_mid_badge_sig) {
        coop_mid_badge_sig = sig;
        follower_ui_update(); /* portraits redraw with the new badges */
    }
    coop_mid_ui_labels_tick(now_ms);
    coop_mid_ui_hud_tick(now_ms);
}

bool coop_middle_ui_init(GameInitInfo* init_info)
{
    coop_mid_ui_set_iso(init_info != NULL ? init_info->iso_window_handle : TIG_WINDOW_HANDLE_INVALID);
    coop_middle_set_ui_tick(coop_mid_ui_tick);
    coop_mid_ui_inited = true;
    return true;
}

void coop_middle_ui_exit(void)
{
    coop_middle_set_ui_tick(NULL);
    coop_mid_ui_fonts_destroy();
    if (coop_mid_label_vb != NULL) {
        tig_video_buffer_destroy(coop_mid_label_vb);
        coop_mid_label_vb = NULL;
    }
    if (coop_mid_hud_vb != NULL) {
        tig_video_buffer_destroy(coop_mid_hud_vb);
        coop_mid_hud_vb = NULL;
    }
    coop_mid_labels_on = false;
    coop_mid_label_count = 0;
    coop_mid_hud_shown = false;
    coop_mid_iso_window = TIG_WINDOW_HANDLE_INVALID;
    coop_mid_ui_inited = false;
}

void coop_middle_ui_reset(void)
{
    coop_middle_trade_local_end();
    coop_mid_ui_labels_off();
    coop_mid_hud_on = false;
    if (coop_mid_hud_shown) {
        gamelib_invalidate_rect(&coop_mid_hud_rect);
        coop_mid_hud_shown = false;
    }
}

void coop_middle_ui_resize(GameResizeInfo* resize_info)
{
    if (!coop_mid_ui_inited || resize_info == NULL) {
        return;
    }
    coop_mid_ui_labels_off();
    coop_mid_ui_set_iso(resize_info->window_handle);
    coop_mid_hud_shown = false;
}
