#include "ui/dialog_ui.h"

#include <string.h>

#include <stdio.h>

#include "game/ai.h"
#include "game/anim.h"
#include "game/broadcast.h"
#include "game/combat.h"
#include "game/coop_lite.h"
#include "net/tig_net.h"
#include "game/critter.h"
#include "game/dialog.h"
#include "game/gsound.h"
#include "game/hrp.h"
#include "game/obj_private.h"
#include "game/player.h"
#include "game/script.h"
#include "game/script_name.h"
#include "game/stat.h"
#include "game/tb.h"
#include "ui/charedit_ui.h"
#include "ui/intgame.h"
#include "ui/inven_ui.h"
#include "ui/schematic_ui.h"
#include "ui/tb_ui.h"
#include "ui/wmap_ui.h"

#define MAX_ENTRIES 8

typedef struct DialogUiEntry {
    /* 0000 */ int slot;
    /* 0004 */ int dlg;
    /* 0008 */ DialogState state;
    /* 1850 */ int field_1850;
    /* 1854 */ int script_num;
    /* 1858 */ int script_line;
    /* 185C */ char field_185C;
    /* 185D */ char field_185D;
    /* 185E */ char field_185E;
    /* 185F */ char field_185F;
} DialogUiEntry;

static DialogUiEntry* sub_567420(int64_t obj);
static void sub_5679C0(DialogUiEntry* entry);
static bool dialog_ui_process_option(DialogUiEntry* entry, int option);
static bool dialog_ui_message_filter(TigMessage* msg);
static void dialog_ui_execute_script(DialogUiEntry* entry, int line);
static void dialog_ui_display(DialogUiEntry* entry);
static void dialog_ui_npc_say(int64_t npc_obj, int64_t pc_obj, int type, int expires_in, const char* str, int speech_id);
static void dialog_ui_pc_say(int64_t pc_obj, int64_t npc_obj, int type, int expires_in, const char* str);
static void dialog_ui_speech_start(int64_t npc_obj, int64_t pc_obj, int speech_id);
static void dialog_ui_speech_stop(void);

static void dialog_ui_coop_publish(DialogUiEntry* entry);
static const char* dialog_ui_option_label(DialogUiEntry* entry, int index);
static void dialog_ui_coop_refresh_votes(void);
static void dialog_ui_coop_view_show(const CoopDialogView* view);
static void dialog_ui_coop_view_close(void);
static void dialog_ui_coop_view_init(void);
static void dialog_ui_coop_view_exit(void);

// 0x66DAB8
static DialogUiEntry stru_66DAB8[8];

/* PT36: host-side option text with the vote marker (tc keeps the pointer). */
static char dialog_ui_marked_options[5][1024];

// 0x67B960
static tig_sound_handle_t dialog_ui_speech_handle;

// 0x67B964
static bool dialog_ui_local_pc_dialog_active;

// 0x567330
bool dialog_ui_init(GameInitInfo* init_info)
{
    coop_lite_set_dialog_end_hook(dialog_ui_end_dialog);
    dialog_ui_coop_view_init();
    int index;

    (void)init_info;

    for (index = 0; index < MAX_ENTRIES; index++) {
        stru_66DAB8[index].field_1850 = false;
        stru_66DAB8[index].slot = index;
    }

    script_set_callbacks(dialog_ui_start_dialog, dialog_ui_float_line);
    ai_set_callbacks(dialog_ui_end_dialog, dialog_ui_float_line);
    broadcast_set_float_line_func(dialog_ui_float_line);
    dialog_ui_speech_handle = TIG_SOUND_HANDLE_INVALID;

    return true;
}

// 0x5673A0
void dialog_ui_exit(void)
{
    dialog_ui_speech_stop();
    dialog_ui_coop_view_exit();
}

// 0x5673B0
void dialog_ui_reset(void)
{
    int index;

    if (dialog_ui_local_pc_dialog_active) {
        intgame_dialog_end();
        dialog_ui_local_pc_dialog_active = false;
    }
    coop_lite_host_dialog_closed();
    dialog_ui_coop_view_close();

    for (index = 0; index < MAX_ENTRIES; index++) {
        if (stru_66DAB8[index].field_1850) {
            sub_5679C0(&(stru_66DAB8[index]));
        }
    }
}

// 0x567400
bool dialog_ui_is_in_dialog(int64_t obj)
{
    return sub_567420(obj)->field_1850;
}

// 0x567420
DialogUiEntry* sub_567420(int64_t obj)
{
    int index = 0;

    return &(stru_66DAB8[index]);
}

// 0x567460
void dialog_ui_start_dialog(int64_t pc_obj, int64_t npc_obj, int script_num, int script_line, int num)
{
    DialogUiEntry* entry;
    char path[TIG_MAX_PATH];
    char str[2000];
    CoopTalkPolicy talk_policy;

    /*
     * Coop Lite: clients only talk to merchants (reduced dialog, story nodes
     * filtered in display/process). Everyone else — quest givers, story NPCs,
     * other players' followers, citizens — just barks.
     */
    talk_policy = coop_lite_can_talk(npc_obj, pc_obj);
    if (tig_net_is_coop_enabled()) {
        char npc_name[256];

        npc_name[0] = '\0';
        if (npc_obj != OBJ_HANDLE_NULL) {
            object_examine(npc_obj, pc_obj, npc_name);
        }
        tig_debug_printf("DialogUI: start pc_local=%d npc='%s' script=%d line=%d policy=%d\n",
            player_is_local_pc_obj(pc_obj) ? 1 : 0, npc_name, script_num, script_line, (int)talk_policy);
    }
    /* Coop host: a remote client's PC never runs dialog here — its owner
       runs its own (merchant-only) dialog. Doing it here would also take
       the single dialog entry from the host's own conversation. */
    if (tig_net_is_coop_enabled() && tig_net_is_host() && !player_is_local_pc_obj(pc_obj)
        && pc_obj != OBJ_HANDLE_NULL && obj_field_int32_get(pc_obj, OBJ_F_TYPE) == OBJ_TYPE_PC) {
        tig_debug_printf("DialogUI: remote PC dialog skipped on host\n");
        return;
    }
    if (talk_policy == COOP_TALK_BLOCK) {
        return;
    }
    if (talk_policy == COOP_TALK_BARK || talk_policy == COOP_TALK_BARK_FOLLOWER) {
        int speech_id = -1;

        str[0] = '\0';
        if (talk_policy == COOP_TALK_BARK) {
            /* The NPC's own generic farewell (race/gender aware). */
            dialog_copy_npc_farewell_msg(npc_obj, pc_obj, str, &speech_id);
        }
        if (str[0] == '\0') {
            snprintf(str, sizeof(str), "%s", coop_lite_bark_text(talk_policy));
            speech_id = -1;
        }
        tig_debug_printf("DialogUI: coop client bark (policy %d)\n", (int)talk_policy);
        dialog_ui_float_line(npc_obj, pc_obj, str, speech_id);
        return;
    }

    if (critter_is_dead(pc_obj)) {
        return;
    }

    if (critter_is_unconscious(pc_obj)) {
        return;
    }

    if (ai_can_speak(npc_obj, pc_obj, false) != AI_SPEAK_OK) {
        return;
    }

    if (player_is_local_pc_obj(pc_obj) && intgame_mode_get() == INTGAME_MODE_DIALOG) {
        return;
    }

    if (combat_critter_is_combat_mode_active(pc_obj)) {
        if (!combat_can_exit_combat_mode(pc_obj)) {
            return;
        }
        combat_critter_deactivate_combat_mode(pc_obj);
    }

    entry = sub_567420(pc_obj);

    if (script_num != 0 && script_name_build_dlg_name(script_num, path, sizeof(path))) {
        if (!dialog_load(path, &(entry->dlg))) {
            return;
        }

        entry->state.dlg = entry->dlg;
        entry->state.pc_obj = pc_obj;
        entry->state.npc_obj = npc_obj;
        entry->state.num = num;
        entry->state.script_num = script_num;
        if (!sub_412FD0(&(entry->state))) {
            dialog_unload(entry->dlg);
            return;
        }

        if (entry->state.field_17E8 == 4) {
            dialog_ui_npc_say(entry->state.npc_obj,
                entry->state.pc_obj,
                TB_TYPE_WHITE,
                TB_EXPIRE_DEFAULT,
                entry->state.reply,
                entry->state.speech_id);
            sub_413280(&(entry->state));
            dialog_unload(entry->dlg);
            return;
        }

        if (player_is_local_pc_obj(pc_obj)) {
            if (!intgame_dialog_begin(dialog_ui_message_filter)) {
                sub_413280(&(entry->state));
                dialog_unload(entry->dlg);
                return;
            }

            dialog_ui_local_pc_dialog_active = true;

            if (!intgame_is_compact_interface()) {
                sub_57CD60(pc_obj, npc_obj, str);
                intgame_examine_object(pc_obj, npc_obj, str);
                object_hover_obj_set(npc_obj);
            }
        }

        sub_424070(pc_obj, 3, 0, true);
        anim_goal_rotate(pc_obj, object_rot(pc_obj, npc_obj));

        if (critter_is_concealed(pc_obj)) {
            critter_set_concealed(pc_obj, false);
        }

        if (critter_is_concealed(npc_obj)) {
            critter_set_concealed(npc_obj, false);
        }

        entry->script_num = script_num;
        entry->script_line = script_line;
        entry->field_1850 = 1;

        dialog_ui_display(entry);
    } else {
        sub_5681C0(pc_obj, npc_obj);
    }
}

// 0x5678D0
void dialog_ui_end_dialog(int64_t obj, int a2)
{
    DialogUiEntry* entry;

    (void)a2;

    entry = sub_567420(obj);
    if (!entry->field_1850) {
        return;
    }

    entry->field_1850 = false;

    if (player_is_local_pc_obj(obj)) {
        intgame_dialog_end();
        dialog_ui_local_pc_dialog_active = 0;
        coop_lite_host_dialog_closed();
    }

    dialog_unload(entry->dlg);

    tb_expire_in(entry->state.npc_obj, TB_EXPIRE_DEFAULT);

    sub_413280(&(entry->state));
}

// 0x5679C0
void sub_5679C0(DialogUiEntry* entry)
{
    if (!entry->field_1850) {
        return;
    }

    entry->field_1850 = false;

    dialog_unload(entry->dlg);

    sub_413280(&(entry->state));
}

// 0x567A10
bool dialog_ui_is_local_pc_in_dialog(void)
{
    /* Coop (PT11: host Esc menu had no Save/Load): a dialog flag left set
       after the dialog window went away — e.g. a remote PC's dialog reusing
       the single entry — must not lock the menu forever. */
    if (dialog_ui_local_pc_dialog_active
        && tig_net_is_coop_enabled()
        && intgame_mode_get() != INTGAME_MODE_DIALOG
        && intgame_mode_get() != INTGAME_MODE_BARTER) {
        tig_debug_printf("DialogUI: stale local dialog flag cleared (mode %d)\n", (int)intgame_mode_get());
        dialog_ui_local_pc_dialog_active = false;
    }
    return dialog_ui_local_pc_dialog_active;
}

// 0x567A20
void dialog_ui_notify_dialog_ended(int64_t obj)
{
    sub_567420(obj)->field_1850 = false;
    if (player_is_local_pc_obj(obj)) {
        intgame_dialog_end();
        dialog_ui_local_pc_dialog_active = false;
        coop_lite_host_dialog_closed();
    }
}

// 0x567A60
void dialog_ui_notify_dialog_started(int64_t obj)
{
    sub_567420(obj)->field_1850 = true;
    if (player_is_local_pc_obj(obj)) {
        if (intgame_dialog_begin(dialog_ui_message_filter)) {
            dialog_ui_local_pc_dialog_active = true;
        }
    }
}

// 0x567E30
bool dialog_ui_process_option(DialogUiEntry* entry, int option)
{
    bool is_pc;

    is_pc = player_is_local_pc_obj(entry->state.pc_obj);

    if (coop_lite_is_client_role()
        && option >= 0 && option < entry->state.num_options
        && !coop_lite_dialog_option_allowed(entry->state.actions[option],
            entry->state.field_17F0[option])) {
        dialog_ui_float_line(entry->state.npc_obj, entry->state.pc_obj,
            "Only the party leader can discuss this matter.", -1);
        return true;
    }
    dialog_ui_pc_say(entry->state.pc_obj,
        entry->state.npc_obj,
        TB_TYPE_GREEN,
        TB_EXPIRE_DEFAULT,
        entry->state.options[option]);
    tb_remove(entry->state.npc_obj);
    dialog_ui_speech_stop();
    sub_413130(&(entry->state), option);

    switch (entry->state.field_17E8) {
    case 0:
        dialog_ui_display(entry);
        break;
    case 1:
        dialog_ui_end_dialog(entry->state.pc_obj, 0);
        dialog_ui_execute_script(entry, 0);
        break;
    case 2:
        dialog_ui_end_dialog(entry->state.pc_obj, 0);
        dialog_ui_execute_script(entry, entry->state.field_17EC);
        break;
    case 3:
        if (is_pc) {
            intgame_dialog_clear();
            inven_ui_open(entry->state.pc_obj, entry->state.npc_obj, INVEN_UI_MODE_BARTER);
        }
        break;
    case 4:
        dialog_ui_npc_say(entry->state.npc_obj,
            entry->state.pc_obj,
            TB_TYPE_WHITE,
            TB_EXPIRE_DEFAULT,
            entry->state.reply,
            entry->state.speech_id);
        dialog_ui_end_dialog(entry->state.pc_obj, 0);
        break;
    case 5:
        if (is_pc) {
            intgame_dialog_clear();
            charedit_open(entry->state.npc_obj, CHAREDIT_MODE_PASSIVE);
        }
        break;
    case 6:
        if (is_pc) {
            intgame_dialog_clear();
            wmap_ui_open();
        }
        break;
    case 7:
        if (is_pc) {
            intgame_dialog_clear();
            schematic_ui_toggle(entry->state.npc_obj, entry->state.pc_obj);
        }
        break;
    case 8:
        if (is_pc) {
            intgame_dialog_clear();
            ui_show_inven_npc_identify(entry->state.pc_obj, entry->state.npc_obj);
        }
        break;
    case 9:
        if (is_pc) {
            intgame_dialog_clear();
            inven_ui_open(entry->state.pc_obj, entry->state.npc_obj, INVEN_UI_MODE_NPC_REPAIR);
        }
        break;
    }

    return true;
}

// 0x5680A0
bool dialog_ui_message_filter(TigMessage* msg)
{
    DialogUiEntry* entry;
    int option;
    int player;

    entry = sub_567420(player_get_local_pc_obj());

    /* Coop client: Esc always leaves the (reduced) dialog, so a dialog whose
       options are all host-only can never trap the player (PT8). */
    if (coop_lite_is_client_role()
        && msg->type == TIG_MESSAGE_KEYBOARD
        && msg->data.keyboard.pressed
        && msg->data.keyboard.scancode == SDL_SCANCODE_ESCAPE) {
        dialog_ui_end_dialog(entry->state.pc_obj, 0);
        return true;
    }

    option = intgame_dialog_get_option(msg);
    if (option == -1) {
        sub_5517A0(msg);
        return true;
    }

    if (!dialog_ui_process_option(entry, option)) {
        sub_5517A0(msg);
    }

    return true;
}

// 0x5681C0
void sub_5681C0(int64_t pc_obj, int64_t npc_obj)
{
    char text[1000];

    sub_4132A0(npc_obj, pc_obj, text);
    dialog_ui_npc_say(npc_obj, pc_obj, TB_TYPE_WHITE, TB_EXPIRE_DEFAULT, text, -1);
}

// 0x568430
void dialog_ui_float_line(int64_t npc_obj, int64_t pc_obj, const char* str, int speech_id)
{
    int type;

    type = obj_field_int32_get(npc_obj, OBJ_F_TYPE) != OBJ_TYPE_PC
        ? TB_TYPE_WHITE
        : TB_TYPE_GREEN;
    dialog_ui_npc_say(npc_obj, pc_obj, type, TB_EXPIRE_DEFAULT, str, speech_id);
}

// 0x568480
void dialog_ui_execute_script(DialogUiEntry* entry, int line)
{
    if (line == 0) {
        line = entry->script_line + 1;
    }

    object_script_execute(entry->state.pc_obj, entry->state.npc_obj, OBJ_HANDLE_NULL, SAP_DIALOG, line);
}

// 0x5684C0
void dialog_ui_display(DialogUiEntry* entry)
{
    int index;
    int out;
    bool filter;

    dialog_ui_npc_say(entry->state.npc_obj,
        entry->state.pc_obj,
        TB_TYPE_WHITE,
        TB_EXPIRE_NEVER,
        entry->state.reply,
        entry->state.speech_id);

    if (player_is_local_pc_obj(entry->state.pc_obj)) {
        /*
         * Non-host companions: blank out quest/story responses in place.
         * Indices must not move — the dialog engine reads fixed slots
         * (e.g. rumor/skill/spell replies use options/field_17F0/field_1804[1]).
         * dialog_ui_process_option rejects the blanked entries.
         */
        /* Not tig_net_is_active(): the client's talk runs under local
           suspension, where net reads inactive (Playtest 7: no filter). */
        filter = coop_lite_is_client_role();
        if (filter) {
            out = 0;
            for (index = 0; index < entry->state.num_options; index++) {
                if (coop_lite_dialog_option_allowed(entry->state.actions[index],
                        entry->state.field_17F0[index])) {
                    out++;
                    continue;
                }
                strncpy(entry->state.options[index], "(Only the party leader can discuss this.)",
                    sizeof(entry->state.options[0]) - 1);
                entry->state.options[index][sizeof(entry->state.options[0]) - 1] = '\0';
            }
            if (out == 0) {
                /* Nothing selectable: leave a harmless goodbye so the
                   dialog can be closed. */
                strncpy(entry->state.options[0], "(Leave.)",
                    sizeof(entry->state.options[0]) - 1);
                entry->state.options[0][sizeof(entry->state.options[0]) - 1] = '\0';
                entry->state.actions[0] = NULL;
                entry->state.field_17F0[0] = 1; /* end dialog */
                entry->state.num_options = 1;
            }
        }

        dialog_ui_coop_publish(entry);

        intgame_dialog_clear();

        for (index = 0; index < entry->state.num_options; index++) {
            intgame_dialog_set_option(index, dialog_ui_option_label(entry, index));
        }
    }
}

// 0x568540
void dialog_ui_npc_say(int64_t npc_obj, int64_t pc_obj, int type, int expires_in, const char* str, int speech_id)
{
    if (pc_obj != OBJ_HANDLE_NULL
        && !critter_is_dead(npc_obj)
        && !sub_423300(npc_obj, NULL)) {
        sub_424070(npc_obj, 3, 0, 1);
        anim_goal_rotate(npc_obj, object_rot(npc_obj, pc_obj));
    }

    tb_remove(npc_obj);
    tb_add(npc_obj, type, str);
    tb_expire_in(npc_obj, expires_in);

    dialog_ui_speech_start(npc_obj, pc_obj, speech_id);
}

// 0x5686C0
void dialog_ui_pc_say(int64_t pc_obj, int64_t npc_obj, int type, int expires_in, const char* str)
{
    if (npc_obj != OBJ_HANDLE_NULL
        && !critter_is_dead(pc_obj)
        && !sub_423300(pc_obj, NULL)) {
        sub_424070(pc_obj, 3, 0, 1);
        anim_goal_rotate(pc_obj, object_rot(pc_obj, npc_obj));
    }

    if (!player_is_local_pc_obj(pc_obj)) {
        tb_remove(pc_obj);
        tb_add(pc_obj, type, str);
        tb_expire_in(pc_obj, expires_in);
    }
}

// 0x568830
void dialog_ui_notify_object_destroyed(int64_t obj)
{
    int index;

    for (index = 0; index < 8; index++) {
        if (stru_66DAB8[index].state.pc_obj == obj
            || stru_66DAB8[index].state.npc_obj == obj) {
            dialog_ui_end_dialog(obj, 0);
        }
    }
}

// 0x5688D0
void dialog_ui_speech_start(int64_t npc_obj, int64_t pc_obj, int speech_id)
{
    int v1;
    int v2;
    char gender;
    char path[TIG_MAX_PATH];

    (void)npc_obj;

    if (speech_id == -1) {
        return;
    }

    dialog_ui_speech_stop();
    sub_418A00(speech_id, &v1, &v2);

    gender = stat_level_get(pc_obj, STAT_GENDER) != 0 ? 'm' : 'f';
    snprintf(path, sizeof(path),
        "sound\\speech\\%.5d\\v%d_%c.mp3",
        v1,
        v2,
        gender);
    if (gender == 'f' && !tig_file_exists(path, NULL)) {
        snprintf(path, sizeof(path),
            "sound\\speech\\%.5d\\v%d_%c.mp3",
            v1,
            v2,
            'm');
    }
    if (tig_file_exists(path, NULL)) {
        dialog_ui_speech_handle = gsound_play_voice(path, 0);
    }
}

// 0x5689B0
void dialog_ui_speech_stop(void)
{
    if (dialog_ui_speech_handle != TIG_SOUND_HANDLE_INVALID) {
        tig_sound_destroy(dialog_ui_speech_handle);
        dialog_ui_speech_handle = TIG_SOUND_HANDLE_INVALID;
    }
}

/* ---- PT36: co-op dialog viewer (read-only) and host vote markers ----------- */

#define CDV_WIDTH 540
#define CDV_HEIGHT 290
#define CDV_TOP 60
#define CDV_OPT_Y 150
#define CDV_OPT_STEP 22
#define CDV_OPT_CHARS 72

static tig_window_handle_t cdv_window = TIG_WINDOW_HANDLE_INVALID;
static tig_font_handle_t cdv_font_name;
static tig_font_handle_t cdv_font_text;
static tig_font_handle_t cdv_font_option;
static tig_font_handle_t cdv_font_mine;
static tig_font_handle_t cdv_font_hint;
static bool cdv_fonts_ready;
static CoopDialogView cdv_view;

static const char* dialog_ui_option_label(DialogUiEntry* entry, int index)
{
    int votes;

    if (!tig_net_is_coop_enabled() || !tig_net_is_host() || !player_is_local_pc_obj(entry->state.pc_obj)) {
        return entry->state.options[index];
    }
    votes = coop_lite_host_dialog_vote_count(index);
    if (votes <= 0 || index >= 5) {
        return entry->state.options[index];
    }
    snprintf(dialog_ui_marked_options[index], sizeof(dialog_ui_marked_options[index]), "%s  [%d]",
        entry->state.options[index], votes);
    return dialog_ui_marked_options[index];
}

/* Host: tell the clients what the local PC's dialog shows now. */
static void dialog_ui_coop_publish(DialogUiEntry* entry)
{
    const char* options[5];
    char npc_name[256];
    int index;

    if (!tig_net_is_coop_enabled() || !tig_net_is_host() || !player_is_local_pc_obj(entry->state.pc_obj)) {
        return;
    }
    npc_name[0] = '\0';
    if (entry->state.npc_obj != OBJ_HANDLE_NULL) {
        object_examine(entry->state.npc_obj, entry->state.pc_obj, npc_name);
    }
    for (index = 0; index < 5; index++) {
        options[index] = entry->state.options[index];
    }
    coop_lite_host_dialog_state(npc_name, entry->state.reply, entry->state.num_options, options);
}

/* Host: a client's vote changed the counts; redraw the option list. */
static void dialog_ui_coop_refresh_votes(void)
{
    DialogUiEntry* entry = &(stru_66DAB8[0]);
    int index;

    if (!entry->field_1850 || !player_is_local_pc_obj(entry->state.pc_obj)
        || intgame_mode_get() != INTGAME_MODE_DIALOG) {
        return;
    }
    intgame_dialog_clear();
    for (index = 0; index < entry->state.num_options; index++) {
        intgame_dialog_set_option(index, dialog_ui_option_label(entry, index));
    }
}

static int cdv_option_at(int mouse_x, int mouse_y)
{
    TigWindowData window_data;
    int x;
    int y;
    int index;

    if (cdv_window == TIG_WINDOW_HANDLE_INVALID || tig_window_data(cdv_window, &window_data) != TIG_OK) {
        return -2;
    }
    x = mouse_x - window_data.rect.x;
    y = mouse_y - window_data.rect.y;
    if (x < 0 || y < 0 || x >= window_data.rect.width || y >= window_data.rect.height) {
        return -2; /* outside the panel */
    }
    index = (y - CDV_OPT_Y) / CDV_OPT_STEP;
    if (y >= CDV_OPT_Y && index >= 0 && index < cdv_view.num_options && x >= 10) {
        return index;
    }
    return -1; /* inside the panel, not on an option */
}

/* The viewer only ever calls coop_lite_client_dialog_vote: a vote packet to
   the host. It has no path into the dialog engine. */
static bool cdv_message_filter(TigMessage* msg)
{
    int index;

    if (!cdv_view.open || cdv_window == TIG_WINDOW_HANDLE_INVALID) {
        return false;
    }
    if (msg->type == TIG_MESSAGE_KEYBOARD) {
        if (msg->data.keyboard.pressed && msg->data.keyboard.scancode == SDL_SCANCODE_V
            && cdv_view.num_options > 0) {
            coop_lite_client_dialog_vote((cdv_view.my_vote + 1) % cdv_view.num_options);
            return true;
        }
        return false;
    }
    if (msg->type == TIG_MESSAGE_MOUSE) {
        index = cdv_option_at(msg->data.mouse.x, msg->data.mouse.y);
        if (index == -2) {
            return false;
        }
        if (msg->data.mouse.event == TIG_MESSAGE_MOUSE_LEFT_BUTTON_UP && index >= 0) {
            coop_lite_client_dialog_vote(index);
        }
        return msg->data.mouse.event == TIG_MESSAGE_MOUSE_LEFT_BUTTON_UP
            || msg->data.mouse.event == TIG_MESSAGE_MOUSE_LEFT_BUTTON_DOWN;
    }
    return false;
}

static void cdv_write(tig_font_handle_t font, const char* str, int x, int y, int width, int height)
{
    TigRect rect;

    rect.x = x;
    rect.y = y;
    rect.width = width;
    rect.height = height;
    tig_font_push(font);
    tig_window_text_write(cdv_window, str, &rect);
    tig_font_pop();
}

static void cdv_draw(void)
{
    TigRect rect;
    char line[COOP_DIALOG_OPT_MAX + 48];
    int index;
    int votes;

    rect.x = 0;
    rect.y = 0;
    rect.width = CDV_WIDTH;
    rect.height = CDV_HEIGHT;
    tig_window_fill(cdv_window, &rect, tig_color_make(24, 18, 12));
    tig_window_box(cdv_window, &rect, tig_color_make(150, 120, 60));

    snprintf(line, sizeof(line), "%s", cdv_view.npc_name);
    cdv_write(cdv_font_name, line, 10, 6, CDV_WIDTH - 20, 20);
    cdv_write(cdv_font_text, cdv_view.reply, 10, 30, CDV_WIDTH - 20, 112);

    for (index = 0; index < cdv_view.num_options; index++) {
        votes = cdv_view.votes[index];
        snprintf(line, sizeof(line), "%d. %.*s%s", index + 1, CDV_OPT_CHARS, cdv_view.options[index],
            strlen(cdv_view.options[index]) > CDV_OPT_CHARS ? "..." : "");
        if (votes > 0) {
            snprintf(line + strlen(line), sizeof(line) - strlen(line), "  [%d]", votes);
        }
        if (cdv_view.my_vote == index) {
            snprintf(line + strlen(line), sizeof(line) - strlen(line), "  < your vote");
        }
        cdv_write(cdv_view.my_vote == index ? cdv_font_mine : cdv_font_option, line, 10,
            CDV_OPT_Y + index * CDV_OPT_STEP, CDV_WIDTH - 20, 20);
    }

    cdv_write(cdv_font_hint, "Read-only view of the host's dialog. Click an option or press V to vote; the host decides.",
        10, CDV_HEIGHT - 24, CDV_WIDTH - 20, 18);
}

static void dialog_ui_coop_view_show(const CoopDialogView* view)
{
    TigWindowData window_data;

    if (!cdv_fonts_ready || view == NULL || !view->open) {
        return;
    }
    cdv_view = *view;
    if (cdv_window == TIG_WINDOW_HANDLE_INVALID) {
        window_data.flags = TIG_WINDOW_MESSAGE_FILTER;
        window_data.rect.x = 0;
        window_data.rect.y = CDV_TOP;
        window_data.rect.width = CDV_WIDTH;
        window_data.rect.height = CDV_HEIGHT;
        window_data.background_color = tig_color_make(24, 18, 12);
        window_data.color_key = 0;
        window_data.message_filter = cdv_message_filter;
        hrp_apply(&(window_data.rect), GRAVITY_CENTER_HORIZONTAL | GRAVITY_TOP);
        if (tig_window_create(&window_data, &cdv_window) != TIG_OK) {
            cdv_window = TIG_WINDOW_HANDLE_INVALID;
            tig_debug_printf("DialogUI: co-op viewer window create failed\n");
            return;
        }
    }
    cdv_draw();
}

static void dialog_ui_coop_view_close(void)
{
    cdv_view.open = false;
    if (cdv_window != TIG_WINDOW_HANDLE_INVALID) {
        tig_window_destroy(cdv_window);
        cdv_window = TIG_WINDOW_HANDLE_INVALID;
    }
}

static tig_font_handle_t cdv_font_make(int r, int g, int b)
{
    TigFont font_desc;
    tig_font_handle_t handle = TIG_FONT_HANDLE_INVALID;

    memset(&font_desc, 0, sizeof(font_desc));
    font_desc.flags = TIG_FONT_NO_ALPHA_BLEND | TIG_FONT_SHADOW;
    tig_art_misc_id_create(TIG_ART_SYSTEM_FONT, 0, &(font_desc.art_id));
    font_desc.color = tig_color_make(r, g, b);
    tig_font_create(&font_desc, &handle);
    return handle;
}

static void dialog_ui_coop_view_init(void)
{
    cdv_font_name = cdv_font_make(230, 200, 110);
    cdv_font_text = cdv_font_make(235, 235, 235);
    cdv_font_option = cdv_font_make(170, 170, 170);
    cdv_font_mine = cdv_font_make(120, 230, 120);
    cdv_font_hint = cdv_font_make(130, 130, 150);
    cdv_fonts_ready = true;
    coop_lite_set_dialog_view_hooks(dialog_ui_coop_view_show, dialog_ui_coop_view_close, dialog_ui_coop_refresh_votes);
}

static void dialog_ui_coop_view_exit(void)
{
    if (!cdv_fonts_ready) {
        return;
    }
    dialog_ui_coop_view_close();
    tig_font_destroy(cdv_font_name);
    tig_font_destroy(cdv_font_text);
    tig_font_destroy(cdv_font_option);
    tig_font_destroy(cdv_font_mine);
    tig_font_destroy(cdv_font_hint);
    cdv_fonts_ready = false;
}
