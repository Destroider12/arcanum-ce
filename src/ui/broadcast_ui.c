#include "ui/broadcast_ui.h"

#include "game/broadcast.h"
#include "game/player.h"
#include "ui/intgame.h"
#include "ui/textedit_ui.h"
#include "game/coop_lite.h"
#include "net/tig_net.h"

static void broadcast_ui_text_edit_on_enter(TextEdit* textedit);
static void broadcast_ui_text_edit_on_change(TextEdit* textedit);

// 0x5CAC28
static TextEdit broadcast_ui_textedit = {
    0,
    NULL,
    125,
    broadcast_ui_text_edit_on_enter,
    broadcast_ui_text_edit_on_change,
    NULL,
};

// 0x681040
static Broadcast broadcast_ui_bcast;

// 0x6810C8
static tig_font_handle_t broadcast_ui_font;

// NOTE: Write-only, follows `broadcast_ui_opened`. Probably some leftover.
//
// 0x6810CC
static bool dword_6810CC;

// 0x6810D0
static bool broadcast_ui_opened;
/* PT77d: rotating window to restore on close (dialogue when opened from a dialog). */
static RotatingWindowType broadcast_ui_prev_rotwin = ROTWIN_TYPE_MSG;

// 0x5717F0
bool broadcast_ui_init(GameInitInfo* init_info)
{
    TigFont font_desc;

    (void)init_info;

    broadcast_ui_bcast.str[0] = '\0';
    broadcast_ui_textedit.buffer = broadcast_ui_bcast.str;

    font_desc.flags = TIG_FONT_NO_ALPHA_BLEND | TIG_FONT_SHADOW;
    tig_art_misc_id_create(TIG_ART_SYSTEM_FONT, 0, &(font_desc.art_id));
    font_desc.color = tig_color_make(120, 120, 175);
    tig_font_create(&font_desc, &broadcast_ui_font);

    return true;
}

// 0x571880
void broadcast_ui_exit(void)
{
    tig_font_destroy(broadcast_ui_font);
}

// 0x571890
void broadcast_ui_reset(void)
{
    broadcast_ui_bcast.str[0] = '\0';
}

// 0x5718A0
bool broadcast_ui_is_open(void)
{
    return broadcast_ui_opened;
}

void broadcast_ui_open(void)
{
    if (broadcast_ui_opened || iso_interface_window_get_2() == ROTWIN_TYPE_BROADCAST) {
        broadcast_ui_close();
        return;
    }

    broadcast_ui_bcast.loc = 0;
    broadcast_ui_prev_rotwin = iso_interface_window_get_2() == ROTWIN_TYPE_DIALOGUE
        ? ROTWIN_TYPE_DIALOGUE
        : ROTWIN_TYPE_MSG;
    iso_interface_window_set(ROTWIN_TYPE_BROADCAST);
    textedit_ui_focus(&broadcast_ui_textedit);
    dword_6810CC = true;
    intgame_text_edit_refresh(broadcast_ui_textedit.buffer, broadcast_ui_font);
    broadcast_ui_opened = true;
    if (tig_net_is_coop_enabled()) {
        coop_lite_chat_set_typing(true);
    }
}

// 0x571910
void broadcast_ui_close(void)
{
    if (broadcast_ui_opened) {
        broadcast_ui_opened = false;
        dword_6810CC = false;
        textedit_ui_unfocus(&broadcast_ui_textedit);
        *broadcast_ui_textedit.buffer = '\0';
        iso_interface_window_set(broadcast_ui_prev_rotwin);
        broadcast_ui_prev_rotwin = ROTWIN_TYPE_MSG;
        if (tig_net_is_coop_enabled()) {
            coop_lite_chat_set_typing(false);
        }
    }
}

// 0x571950
void broadcast_ui_text_edit_on_enter(TextEdit* textedit)
{
    if (*textedit->buffer != '\0') {
        intgame_get_location_under_cursor(&(broadcast_ui_bcast.loc));
        broadcast_msg(player_get_local_pc_obj(), &broadcast_ui_bcast);
        /* PT77: after the local bubble, relay typed chat (not follower cmds). */
        if (tig_net_is_coop_enabled() && tig_net_is_active()) {
            coop_lite_chat_on_local_broadcast(broadcast_ui_bcast.str);
        }
    }
    broadcast_ui_close();
}

// 0x571990
void broadcast_ui_text_edit_on_change(TextEdit* textedit)
{
    if (*textedit->buffer != '\0') {
        intgame_text_edit_refresh(textedit->buffer, broadcast_ui_font);
    } else {
        intgame_text_edit_refresh(" ", broadcast_ui_font);
    }
}
