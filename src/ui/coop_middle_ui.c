/*
 * Coop Middle M2 UI (protocol 54). See ui/coop_middle_ui.h and
 * docs/coop-middle.md. Engine files only call the entry points here.
 */
#include "ui/coop_middle_ui.h"

#include <stdio.h>
#include <string.h>

#include "game/coop_lite.h"
#include "game/coop_middle.h"
#include "game/critter.h"
#include "game/location.h"
#include "game/multiplayer.h"
#include "game/obj.h"
#include "game/object.h"
#include "game/player.h"
#include "net/tig_net.h"
#include "tig/art.h"
#include "tig/color.h"
#include "tig/debug.h"
#include "tig/font.h"
#include "tig/window.h"
#include "ui/dialog_ui.h"
#include "ui/follower_ui.h"
#include "ui/inven_ui.h"

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

/* ---- tick / lifecycle ---- */

static void coop_mid_ui_tick(unsigned int now_ms)
{
    uint8_t bits = 0;
    uint32_t sig;

    (void)now_ms;
    if (!coop_mid_ui_inited) {
        return;
    }
    if (!inven_ui_is_created()) {
        coop_middle_trade_local_end(); /* window closed (no-op if none) */
    }
    if (!coop_mid_ui_session()) {
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
}

bool coop_middle_ui_init(GameInitInfo* init_info)
{
    (void)init_info;
    coop_middle_set_ui_tick(coop_mid_ui_tick);
    coop_mid_ui_inited = true;
    return true;
}

void coop_middle_ui_exit(void)
{
    coop_middle_set_ui_tick(NULL);
    coop_mid_ui_fonts_destroy();
    coop_mid_ui_inited = false;
}

void coop_middle_ui_reset(void)
{
    coop_middle_trade_local_end();
}

void coop_middle_ui_resize(GameResizeInfo* resize_info)
{
    (void)resize_info;
}
