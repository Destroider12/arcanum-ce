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
#include "game/obj.h"
#include "game/object.h"
#include "game/player.h"
#include "net/tig_net.h"
#include "tig/debug.h"
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

/* ---- tick / lifecycle ---- */

static void coop_mid_ui_tick(unsigned int now_ms)
{
    (void)now_ms;
    if (!coop_mid_ui_inited) {
        return;
    }
    if (!inven_ui_is_created()) {
        coop_middle_trade_local_end(); /* window closed (no-op if none) */
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
