#include "ui/mp_ctrl_ui.h"

#include <stdio.h>

#include "game/obj.h"
#include "game/obj_pool.h"
#include "game/player.h"
#include "game/ui.h"
#include "tig/debug.h"
#include "tig/memory.h"

/**
 * Lightweight stand-in for the deleted multiplayer party-feedback UI.
 *
 * party_add / party_remove enqueue TIMEEVENT_TYPE_MP_CTRL_UI with
 * params[0]=1 and params[3]=status code. The original handler showed
 * multiplayer.mes lines via multiplayer_ui; that module is gone, so we
 * log and (when local) flash a short feedback string.
 */
static void mp_ctrl_ui_party_feedback(int64_t leader_obj, int64_t target_obj, int status)
{
    char* name = NULL;
    char buf[160];
    const char* fmt;
    const char* display;
    UiMessage ui_message;

    if (!obj_handle_is_valid(target_obj)) {
        tig_debug_printf("MP Ctrl UI: party feedback status=%d (invalid target)\n", status);
        return;
    }

    obj_field_string_get(target_obj, OBJ_F_PC_PLAYER_NAME, &name);
    display = (name != NULL && name[0] != '\0') ? name : "?";

    switch (status) {
    case 0:
        fmt = "%s joined the party.";
        break;
    case 1:
        fmt = "Join Failed: %s.";
        break;
    case 2:
        fmt = "Join Failed: %s cannot join self.";
        break;
    case 3:
    case 4:
        fmt = "Join Failed: %s is already in a party.";
        break;
    case 5:
        fmt = "Disband Failed: %s.";
        break;
    case 6:
        fmt = "Disband: %s.";
        break;
    case 7:
        fmt = "Disband Failed: %s is not in your party.";
        break;
    default:
        tig_debug_printf("MP Ctrl UI: unknown party status=%d name=%s\n", status, display);
        if (name != NULL) {
            FREE(name);
        }
        return;
    }

    snprintf(buf, sizeof(buf), fmt, display);
    if (name != NULL) {
        FREE(name);
    }

    tig_debug_printf("MP Ctrl UI: %s\n", buf);

    /* Show locally when either party member is the local PC. */
    if (player_is_local_pc_obj(leader_obj) || player_is_local_pc_obj(target_obj)) {
        ui_message.type = UI_MSG_TYPE_EXCLAMATION;
        ui_message.str = buf;
        ui_message.field_8 = 0;
        ui_message.field_C = 0;
        ui_message.field_10 = 0;
        ui_display_msg(&ui_message);
    }
}

bool mp_ctrl_ui_process_callback(TimeEvent* timeevent)
{
    if (timeevent == NULL) {
        return true;
    }

    switch (timeevent->params[0].integer_value) {
    case 0:
        /* Original: refresh MP party control panel. No panel in CE. */
        break;
    case 1:
        /* Party add/remove feedback from party.c:sub_4BA320. */
        if (obj_handle_is_valid(timeevent->params[1].object_value)
            && obj_handle_is_valid(timeevent->params[2].object_value)) {
            mp_ctrl_ui_party_feedback(timeevent->params[1].object_value,
                timeevent->params[2].object_value,
                timeevent->params[3].integer_value);
        } else {
            tig_debug_printf("MP Ctrl UI: party feedback skipped (stale handles) status=%d\n",
                timeevent->params[3].integer_value);
        }
        break;
    case 2:
        /* Original: auto-join blink animation. No-op. */
        break;
    default:
        tig_debug_printf("MP Ctrl UI: unhandled params[0]=%d\n",
            timeevent->params[0].integer_value);
        break;
    }

    return true;
}
