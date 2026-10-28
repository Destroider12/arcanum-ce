#include "game/mp_utils.h"
#include "game/coop_lite.h"

#include "game/a_name.h"
#include "game/anim.h"
#include "game/anim_private.h"
#include "game/animfx.h"
#include "game/area.h"
#include "game/combat.h"
#include "game/critter.h"
#include "game/descriptions.h"
#include "game/gsound.h"
#include "game/item.h"
#include "game/location.h"
#include "game/magictech.h"
#include "game/map.h"
#include "game/material.h"
#include "game/multiplayer.h"
#include "game/obj.h"
#include "game/obj_private.h"
#include "game/object.h"
#include "game/player.h"
#include "game/portal.h"
#include "game/proto.h"
#include "game/ui.h"
#include "game/random.h"
#include "game/sector.h"
#include "game/spell.h"
#include "game/tb.h"
#include "game/tf.h"
#include "game/tile.h"
#include "game/townmap.h"
#include "game/ui.h"
#include "game/wall.h"

// 0x4ED6C0
bool sub_4ED6C0(int64_t obj)
{
    Packet29 pkt;

    if (!tig_net_is_active()
        || tig_net_is_host()) {
        return sub_463540(obj);
    }

    pkt.type = 29;
    sub_4F0640(obj, &(pkt.oid));
    tig_net_send_app_all(&pkt, sizeof(pkt));

    return false;
}

// 0x4ED8B0
bool mp_object_create(int name, int64_t loc, int64_t* obj_ptr)
{
    if (tig_net_is_active()) {
        if (tig_net_is_host()
            && object_create(sub_4685A0(name), loc, obj_ptr)) {
            Packet70 pkt;

            pkt.type = 70;
            pkt.subtype = 0;
            pkt.s0.name = name;
            pkt.s0.oid = obj_get_id(*obj_ptr);
            pkt.s0.loc = loc;
            pkt.s0.field_30 = 0;
            tig_net_send_app_all(&pkt, sizeof(pkt));
            return true;
        }

        *obj_ptr = OBJ_HANDLE_NULL;
        return false;
    }

    return object_create(sub_4685A0(name), loc, obj_ptr);
}

// 0x4EDA60
void sub_4EDA60(UiMessage* ui_message, int player, int a3)
{
    Packet84* pkt;
    int extra_length;

    extra_length = (int)strlen(ui_message->str) + 1;
    pkt = (Packet84*)MALLOC(sizeof(*pkt) + extra_length);
    pkt->type = 84;
    pkt->extra_length = extra_length;
    pkt->field_8 = a3;
    pkt->ui_message = *ui_message;
    strncpy((char*)(pkt + 1), ui_message->str, extra_length);
    tig_net_send_app(pkt, sizeof(*pkt) + extra_length, player);
    FREE(pkt);
}

// 0x4EDCE0
void sub_4EDCE0(int64_t obj, tig_art_id_t art_id)
{
    Packet92 pkt;

    if (!tig_net_is_active()
        || tig_net_is_host()) {
        object_set_current_aid(obj, art_id);

        if (tig_net_is_host()) {
            pkt.type = 92;
            pkt.oid = obj_get_id(obj);
            pkt.art_id = art_id;
            tig_net_send_app_all(&pkt, sizeof(pkt));
        }
    }
}

// 0x4EDD50
void mp_ui_update_inven(int64_t obj)
{
    PacketUpdateInven pkt;

    ui_update_inven(obj);

    if (tig_net_is_active()
        && tig_net_is_host()) {
        pkt.type = 94;
        pkt.oid = obj_get_id(obj);
        tig_net_send_app_all(&pkt, sizeof(pkt));
    }
}

// 0x4EDDB0
void mp_handle_ui_update_inven(PacketUpdateInven* pkt)
{
    int64_t obj;

    obj = obj_pool_perm_lookup(pkt->oid);
    if (obj != OBJ_HANDLE_NULL) {
        ui_update_inven(obj);
    }
}

// 0x4EDF20
void sub_4EDF20(int64_t obj, int64_t location, int dx, int dy, bool a7)
{
    Packet99 pkt;

    if (!tig_net_is_active()
        || tig_net_is_host()) {
        if (a7 && player_is_local_pc_obj(obj)) {
            location_origin_set(location);
        }
        sub_43E770(obj, location, dx, dy);
    }

    if (tig_net_is_active()
        && tig_net_is_host()) {
        pkt.type = 99;
        pkt.oid = obj_get_id(obj);
        pkt.location = location;
        pkt.dx = dx;
        pkt.dy = dy;
        pkt.field_30 = a7;
        tig_net_send_app_all(&pkt, sizeof(pkt));
    }
}

// 0x4EDFF0
void sub_4EDFF0(Packet99* pkt)
{
    int64_t obj;

    /* Host already applied locally in sub_4EDF20 before broadcast.
       Clients must apply the authoritative location. */
    if (tig_net_is_host()) {
        return;
    }

    obj = obj_pool_perm_lookup(pkt->oid);
    if (obj == OBJ_HANDLE_NULL) {
        return;
    }
    /* Our own locally driven PC is authoritative here. */
    if (coop_lite_client_drives_obj(obj)) {
        return;
    }

    if (!coop_lite_loc_in_map(pkt->location)) {
        return; /* PT37: the host's new-map position, we are still on the old map */
    }
    multiplayer_lock();
    sub_43E770(obj, pkt->location, pkt->dx, pkt->dy);
    if (pkt->field_30 && player_is_local_pc_obj(obj)) {
        location_origin_set(pkt->location);
    }
    multiplayer_unlock();
}

// 0x4EE060
void mp_item_activate(int64_t owner_obj, int64_t item_obj)
{
    Packet100 pkt;

    if (player_is_local_pc_obj(owner_obj)) {
        ui_item_activate(owner_obj, item_obj);
        return;
    }

    if (tig_net_is_active()
        && tig_net_is_host()) {
        pkt.type = 100;
        pkt.subtype = 5;
        sub_4F0640(owner_obj, &(pkt.d.s.field_8));
        sub_4F0640(item_obj, &(pkt.d.s.field_20));
        tig_net_send_app_all(&pkt, sizeof(pkt));
    }
}

// 0x4EE290
void mp_ui_written_start_type(int64_t obj, WrittenType written_type, int num)
{
    if (player_get_local_pc_obj() != obj) {
        int client_id = multiplayer_find_slot_from_obj(obj);
        if (client_id != -1) {
            Packet100 pkt;

            pkt.type = 100;
            pkt.subtype = 3;
            pkt.d.a.field_8 = written_type;
            pkt.d.a.field_C = num;
            tig_net_send_app(&pkt, sizeof(pkt), client_id);
        }

        return;
    }

    ui_written_start_type(written_type, num);
}

// 0x4EE310
void mp_ui_show_inven_loot(int64_t pc_obj, int64_t target_obj)
{
    if (!player_is_local_pc_obj(pc_obj)) {
        if (tig_net_is_active() && tig_net_is_host()) {
            Packet100 pkt;

            pkt.type = 100;
            pkt.subtype = 6;
            sub_4F0640(pc_obj, &(pkt.d.s.field_8));
            sub_4F0640(pc_obj, &(pkt.d.s.field_20));
            tig_net_send_app_all(&pkt, sizeof(pkt));
        }

        return;
    }

    ui_show_inven_loot(pc_obj, target_obj);
}

// 0x4EE3A0
void sub_4EE3A0(int64_t obj, int64_t a2)
{
    Packet100 pkt;

    if (player_is_local_pc_obj(obj)) {
        sub_4604F0(obj, a2);
        return;
    }

    if (tig_net_is_active()
        && tig_net_is_host()) {
        pkt.type = 100;
        pkt.subtype = 7;
        sub_4F0640(obj, &(pkt.d.s.field_8));
        sub_4F0640(obj, &(pkt.d.s.field_20));
        tig_net_send_app_all(&pkt, sizeof(pkt));
    }
}

// 0x4EE430
void mp_ui_show_inven_identify(int64_t pc_obj, int64_t target_obj)
{
    if (!player_is_local_pc_obj(pc_obj)) {
        if (tig_net_is_active() && tig_net_is_host()) {
            Packet100 pkt;

            pkt.type = 100;
            pkt.subtype = 8;
            sub_4F0640(pc_obj, &(pkt.d.s.field_8));
            sub_4F0640(pc_obj, &(pkt.d.s.field_20));
            tig_net_send_app_all(&pkt, sizeof(pkt));
        }

        return;
    }

    ui_show_inven_identify(pc_obj, target_obj);
}

// 0x4EE4C0
void sub_4EE4C0(int64_t obj, int64_t a2)
{
    Packet100 pkt;

    if (player_is_local_pc_obj(obj)) {
        sub_460930(obj, a2);
        return;
    }

    if (tig_net_is_active()
        && tig_net_is_host()) {
        pkt.type = 100;
        pkt.subtype = 10;
        sub_4F0640(obj, &(pkt.d.s.field_8));
        sub_4F0640(obj, &(pkt.d.s.field_20));
        tig_net_send_app_all(&pkt, sizeof(pkt));
    }
}

// 0x4EE5E0
void sub_4EE5E0(Packet100* pkt)
{
    int64_t v1;
    int64_t v2;
    Packet100 new_pkt;

    switch (pkt->subtype) {
    case 0:
        ui_follower_refresh();
        break;
    case 1:
        ui_refresh_health_bar(obj_pool_perm_lookup(pkt->d.b.field_8));
        break;
    case 2:
        ui_toggle_primary_button(pkt->d.a.field_8, pkt->d.a.field_C);
        break;
    case 3:
        ui_toggle_primary_button(pkt->d.a.field_8, pkt->d.a.field_C);
        break;
    case 4:
        ui_schematic_feedback(pkt->d.c.field_8,
            obj_pool_perm_lookup(pkt->d.c.field_10),
            obj_pool_perm_lookup(pkt->d.c.field_28));
        break;
    case 5:
        sub_4F0690(pkt->d.s.field_20, &v2);
        sub_4F0690(pkt->d.s.field_8, &v1);
        if (player_is_local_pc_obj(v1)) {
            ui_item_activate(v1, v2);
        }
        break;
    case 6:
        sub_4F0690(pkt->d.s.field_8, &v1);
        sub_4F0690(pkt->d.s.field_20, &v2);
        if (player_is_local_pc_obj(v1)) {
            ui_show_inven_loot(v1, v2);
        }
        break;
    case 7:
        sub_4F0690(pkt->d.s.field_8, &v1);
        sub_4F0690(pkt->d.s.field_20, &v2);
        if (player_is_local_pc_obj(v1)) {
            sub_4604F0(v1, v2);
        }
        break;
    case 8:
        sub_4F0690(pkt->d.s.field_8, &v1);
        sub_4F0690(pkt->d.s.field_20, &v2);
        if (player_is_local_pc_obj(v1)) {
            ui_show_inven_identify(v1, v2);
        }
        break;
    case 9:
        sub_4F0690(pkt->d.s.field_8, &v1);
        sub_4F0690(pkt->d.s.field_20, &v2);
        if (player_is_local_pc_obj(v1)) {
            ui_show_inven_npc_identify(v1, v2);
        }
        break;
    case 10:
        sub_4F0690(pkt->d.s.field_8, &v1);
        sub_4F0690(pkt->d.s.field_20, &v2);
        if (player_is_local_pc_obj(v1)) {
            sub_460930(v1, v2);
        }
        break;
    case 11:
        sub_4F0690(pkt->d.x.field_8, &v1);
        if (player_is_local_pc_obj(v1)) {
            item_error_msg(v1, pkt->d.x.field_20);
        }
        // FIXME: Fallthrough?
    case 12:
        sub_4F0690(pkt->d.z.field_8, &v1);
        sub_4F0690(pkt->d.z.field_20, &v2);
        switch (pkt->d.z.field_3C) {
        case 0:
            if (tig_net_is_host()) {
                if (sub_460C50(v1, v2, pkt->d.z.field_38)) {
                    new_pkt = *pkt;
                    new_pkt.d.z.field_3C = 1;
                    tig_net_send_app(&new_pkt, sizeof(new_pkt), sub_52A530());
                }
            }
            break;
        case 1:
            if (tig_net_is_host()) {
                if (sub_460C80(v1, v2, pkt->d.z.field_38)) {
                    new_pkt = *pkt;
                    new_pkt.d.z.field_3C = 2;
                    tig_net_send_app_all(&new_pkt, sizeof(new_pkt));
                }
            }
            break;
        case 2:
            if (tig_net_is_host()) {
                sub_460CB0(v1, v2, pkt->d.z.field_38);

                new_pkt = *pkt;
                new_pkt.d.z.field_3C = 3;
                tig_net_send_app(&new_pkt, sizeof(new_pkt), sub_52A530());
            }
            break;
        case 3:
            if (tig_net_is_host()) {
                ui_inven_create(v1, v2, pkt->d.z.field_38);
            }
            break;
        }
        break;
    case 13:
        /* Coop Lite vitals sync: field_8=oid, field_38=hp_damage, field_3C=fatigue.
           Host → clients only (review H1: a client could set any HP). */
        if (tig_net_is_host()) {
            break;
        }
        sub_4F0690(pkt->d.z.field_8, &v1);
        if (v1 != OBJ_HANDLE_NULL) {
            multiplayer_lock();
            object_hp_damage_set(v1, pkt->d.z.field_38);
            if (obj_type_is_critter(obj_field_int32_get(v1, OBJ_F_TYPE))) {
                critter_fatigue_damage_set(v1, pkt->d.z.field_3C);
            }
            multiplayer_unlock();
        }
        break;
    }
}

// 0x4EECB0
void mp_gsound_play_sfx(int sound_id)
{
    gsound_play_sfx(sound_id, 1);

    if (tig_net_is_active()) {
        PacketPlaySound pkt;

        pkt.type = 106;
        pkt.subtype = 0;
        pkt.sound_id = sound_id;
        tig_net_send_app_all(&pkt, sizeof(pkt));
    }
}

// 0x4EED00
void sub_4EED00(int64_t obj, int sound_id)
{
    if (obj == OBJ_HANDLE_NULL) {
        return;
    }

    if (player_is_local_pc_obj(obj)) {
        gsound_play_sfx(sound_id, 1);
        return;
    }

    if (tig_net_is_active() && tig_net_is_host()) {
        int client_id = multiplayer_find_slot_from_obj(obj);
        if (client_id != -1) {
            PacketPlaySound pkt;

            pkt.type = 106;
            pkt.subtype = 0;
            pkt.sound_id = sound_id;
            tig_net_send_app(&pkt, sizeof(pkt), client_id);
        }
    }
}

// 0x4EED80
void mp_gsound_play_sfx_on_obj(int sound_id, int loops, int64_t obj)
{
    gsound_play_sfx_on_obj(sound_id, loops, obj);

    if (tig_net_is_active()) {
        PacketPlaySound pkt;

        pkt.type = 106;
        pkt.subtype = 1;
        pkt.sound_id = sound_id;
        pkt.loops = loops;
        pkt.oid = obj_get_id(obj);
        tig_net_send_app_all(&pkt, sizeof(pkt));
    }
}

// 0x4EEE00
void mp_gsound_play_scheme(int music_scheme_idx, int ambient_scheme_idx)
{
    gsound_play_scheme(music_scheme_idx, ambient_scheme_idx);

    if (tig_net_is_active()) {
        PacketPlaySound pkt;

        pkt.type = 106;
        pkt.subtype = 2;
        pkt.music_scheme_idx = music_scheme_idx;
        pkt.ambient_scheme_idx = ambient_scheme_idx;
        tig_net_send_app_all(&pkt, sizeof(pkt));
    }
}

// 0x4EEE50
void mp_handle_gsound_play_scheme(PacketPlaySound* pkt)
{
    switch (pkt->subtype) {
    case 0:
        gsound_play_sfx(pkt->sound_id, 1);
        break;
    case 1:
        gsound_play_sfx_on_obj(pkt->sound_id, pkt->loops, obj_pool_perm_lookup(pkt->oid));
        break;
    case 2:
        gsound_play_scheme(pkt->music_scheme_idx, pkt->ambient_scheme_idx);
        break;
    }
}

/* A click-to-move is at most a screen or two away. */
#define MP_PACKET4_MAX_TILES 128

// 0x4EF320 - Packet4: client move intent → host resolves into real anim goals.
void mp_handle_packet4(Packet4* pkt)
{
    int64_t obj;

    if (pkt == NULL || !tig_net_is_host()) {
        return;
    }

    sub_4F0690(pkt->oid, &obj);
    if (obj == OBJ_HANDLE_NULL) {
        tig_debug_printf("MP: Packet4 move intent for unknown OID (client PC not adopted?)\n");
        return;
    }
    if (!coop_lite_sender_owns_obj(obj)) {
        tig_debug_printf("MP: Packet4 from slot %d for an object it does not own — dropped\n", sub_52A530());
        return;
    }
    /* Review PT24 #5: a sane destination for a living mover only (the
       pathfinder still decides walkability). */
    {
        int64_t lim_x;
        int64_t lim_y;

        location_limits_get(&lim_x, &lim_y);
        if (critter_is_dead(obj)
            || (int64_t)LOCATION_GET_X(pkt->loc) >= lim_x || (int64_t)LOCATION_GET_Y(pkt->loc) >= lim_y
            || location_dist(obj_field_int64_get(obj, OBJ_F_LOCATION), pkt->loc) > MP_PACKET4_MAX_TILES) {
            tig_debug_printf("MP: Packet4 from slot %d refused (dead or bad/far destination)\n", sub_52A530());
            return;
        }
    }

    switch (pkt->subtype) {
    case 0:
        anim_goal_move_to_tile(obj, pkt->loc);
        break;
    case 1:
        anim_goal_run_to_tile(obj, pkt->loc);
        break;
    case 2:
        sub_433A00(obj, pkt->loc, false);
        break;
    default:
        break;
    }
}

// Packet8 receive is implemented as mp_anim_modify in anim.c.

// Packet9: interrupt/clear goals (sub_424070) + optional loc snap for remotes.
void mp_handle_packet9(Packet9* pkt)
{
    int64_t obj;

    if (pkt == NULL) {
        return;
    }

    if (!sub_444110(&pkt->field_18)) {
        return;
    }
    obj = pkt->field_18.obj;
    if (obj == OBJ_HANDLE_NULL) {
        return;
    }

    /* Reject corrupt priority from peers (menu-time follow used to ASSERT then continue). */
    if (pkt->priority_level < PRIORITY_NONE || pkt->priority_level >= PRIORITY_HIGHEST) {
        tig_debug_printf("MP: drop Packet9 with bad priority_level=%d\n", pkt->priority_level);
        return;
    }

    if (tig_net_is_host()) {
        if (!coop_lite_sender_owns_obj(obj)) {
            tig_debug_printf("MP: Packet9 from slot %d for an object it does not own — dropped\n", sub_52A530());
            return;
        }
        /* Client interrupt intent: a4=true skips nested net send inside sub_424070. */
        sub_424070(obj, pkt->priority_level, pkt->field_48 != 0, true);
        /* Rebroadcast to the other peers (not back to the owner, which
           drives its own PC and already interrupted locally). */
        {
            Packet9 out = *pkt;
            out.loc = obj_field_int64_get(obj, OBJ_F_LOCATION);
            out.art_id = obj_field_int32_get(obj, OBJ_F_CURRENT_AID);
            out.offset_x = obj_field_int32_get(obj, OBJ_F_OFFSET_X);
            out.offset_y = obj_field_int32_get(obj, OBJ_F_OFFSET_Y);
            if (sub_52A530() != 0) {
                tig_net_send_app_except(&out, sizeof(out), sub_52A530());
            } else {
                tig_net_send_app_all(&out, sizeof(out));
            }
        }
        return;
    }

    /* Our own locally driven PC: never snap/interrupt it from the wire. */
    if (coop_lite_client_drives_obj(obj)) {
        return;
    }

    /* PT27 item 2: a peer's PC is drawn from its PC_POS interpolation
       ring; this reliable-channel snapshot (sent ahead of the delayed
       ring) made the host's PC jump forward and get pulled back. */
    if (tig_net_is_coop_enabled() && obj_field_int32_get(obj, OBJ_F_TYPE) == OBJ_TYPE_PC) {
        return;
    }

    multiplayer_lock();
    if (pkt->loc != 0) {
        sub_43E770(obj, pkt->loc, pkt->offset_x, pkt->offset_y);
    }
    if (pkt->art_id != (tig_art_id_t)-1) {
        object_set_current_aid(obj, pkt->art_id);
    }
    sub_424070(obj, pkt->priority_level, pkt->field_48 != 0, true);
    multiplayer_unlock();
}

// Packet10: authoritative anim pose snapshot from host.
void mp_handle_packet10(Packet10* pkt)
{
    int64_t obj;
    AnimRunInfo* run_info;

    if (pkt == NULL || tig_net_is_host()) {
        return;
    }

    sub_4F0690(pkt->oid, &obj);
    if (obj == OBJ_HANDLE_NULL) {
        return;
    }
    /* Coop: critters have their own pose channels (PT27 items 1/2). */
    if (coop_lite_client_drop_anim_pose(obj)) {
        return;
    }

    multiplayer_lock();
    if (pkt->loc != 0) {
        sub_43E770(obj, pkt->loc, pkt->offset_x, pkt->offset_y);
    }
    if (pkt->art_id != TIG_ART_ID_INVALID) {
        object_set_current_aid(obj, pkt->art_id);
    }
    if (anim_id_to_run_info(&pkt->anim_id, &run_info) && run_info != NULL) {
        run_info->flags = pkt->flags;
    }
    multiplayer_unlock();
}

// Packet11: combat mode toggle mirror.
void mp_handle_combat_mode_set(PacketCombatModeSet* pkt)
{
    int64_t obj;
    bool active;

    if (pkt == NULL) {
        return;
    }

    sub_4F0690(pkt->oid, &obj);
    if (obj == OBJ_HANDLE_NULL) {
        return;
    }

    if (tig_net_is_host()) {
        /* Coop: a client toggles its own PC only; mirror it to everyone. */
        if (sub_52A530() == 0 || !coop_lite_sender_owns_obj(obj)) {
            return;
        }
        active = combat_critter_is_combat_mode_active(obj);
        multiplayer_lock();
        if (pkt->active && !active) {
            combat_critter_activate_combat_mode(obj);
        } else if (!pkt->active && active) {
            combat_critter_deactivate_combat_mode(obj);
        }
        multiplayer_unlock();
        pkt->active = combat_critter_is_combat_mode_active(obj);
        tig_debug_printf("MP: slot %d combat mode -> %d\n", sub_52A530(), pkt->active ? 1 : 0);
        tig_net_send_app_all(pkt, sizeof(*pkt));
        return;
    }

    active = combat_critter_is_combat_mode_active(obj);
    multiplayer_lock();
    if (pkt->active && !active) {
        combat_critter_activate_combat_mode(obj);
    } else if (!pkt->active && active) {
        combat_critter_deactivate_combat_mode(obj);
    }
    multiplayer_unlock();
}

// Packet29: open container (client → host).
void mp_handle_packet29(Packet29* pkt)
{
    int64_t obj;

    if (pkt == NULL || !tig_net_is_host()) {
        return;
    }
    sub_4F0690(pkt->oid, &obj);
    if (obj == OBJ_HANDLE_NULL) {
        return;
    }
    /* Review L7: a client may only collapse a junk pile next to its PC. */
    if (sub_52A530() != 0) {
        int64_t pc = sub_4A2B60(sub_52A530());

        if (pc == OBJ_HANDLE_NULL
            || location_dist(obj_field_int64_get(pc, OBJ_F_LOCATION),
                   obj_field_int64_get(obj, OBJ_F_LOCATION))
                > 5) {
            return;
        }
    }
    sub_463540(obj);
}

/* Packet70s that arrived while the client was still loading the host map. */
#define MP_DEFERRED_PACKET70_MAX 64
static Packet70 mp_deferred_packet70[MP_DEFERRED_PACKET70_MAX];
static int mp_deferred_packet70_count;
/* Review PT24 #11: overflow drops are counted (logged first + every 64th
   and on replay) instead of one line per packet. */
static int mp_deferred_packet70_dropped;

int mp_deferred_packet70_pending(void)
{
    return mp_deferred_packet70_count;
}

void mp_clear_deferred_packet70(void)
{
    mp_deferred_packet70_count = 0;
    mp_deferred_packet70_dropped = 0;
}

void mp_flush_deferred_packet70(void)
{
    int index;
    int count;

    count = mp_deferred_packet70_count;
    mp_deferred_packet70_count = 0;
    if (count > 0) {
        tig_debug_printf("MP: replaying %d deferred Packet70 (%d dropped on overflow)\n",
            count, mp_deferred_packet70_dropped);
    }
    mp_deferred_packet70_dropped = 0;
    for (index = 0; index < count; index++) {
        mp_handle_packet70(&(mp_deferred_packet70[index]));
    }
}

// Packet70: object create / blood FX mirrors.
void mp_handle_packet70(Packet70* pkt)
{
    int64_t obj;
    int64_t blood_obj;

    if (pkt == NULL || tig_net_is_host()) {
        return;
    }
    /* Avoid creating world objects onto ShopMap before SESSION_SYNC. */
    if (tig_net_is_active() && coop_lite_should_skip_independent_new_game()
        && coop_lite_client_waiting_for_sync()) {
        if (mp_deferred_packet70_count < MP_DEFERRED_PACKET70_MAX) {
            mp_deferred_packet70[mp_deferred_packet70_count++] = *pkt;
            tig_debug_printf("MP: Packet70 deferred (client waiting for session sync) subtype=%d name=%d\n",
                pkt->subtype, pkt->subtype == 0 ? pkt->s0.name : -1);
        } else {
            if (mp_deferred_packet70_dropped++ % 64 == 0) {
                tig_debug_printf("MP: Packet70 dropped — deferred queue full (%d dropped so far)\n",
                    mp_deferred_packet70_dropped);
            }
        }
        return;
    }

    if (pkt->subtype == 0 && (obj = obj_pool_perm_lookup(pkt->s0.oid)) != OBJ_HANDLE_NULL) {
        /* Already have it (e.g. our own PC adopted by the host). An item of
           ours the host just dropped (P1-3): move this very object to the
           ground so it keeps its state. */
        unsigned int flags = obj_field_int32_get(obj, OBJ_F_FLAGS);

        if (item_is_item(obj) && (flags & OF_INVENTORY) != 0 && (flags & OF_DESTROYED) == 0) {
            multiplayer_lock();
            tig_net_local_suspend_begin();
            item_remove(obj);
            object_drop(obj, pkt->s0.loc);
            tig_net_local_suspend_end();
            multiplayer_unlock();
        }
        return;
    }
    if (pkt->subtype == 0) {
        int64_t proto_obj = sub_4685A0(pkt->s0.name);

        /* Unknown proto, or a PC: PCs only come from SESSION_SYNC (a
           Packet70 PC would be a bare, unbound duplicate). */
        if (proto_obj == OBJ_HANDLE_NULL
            || obj_field_int32_get(proto_obj, OBJ_F_TYPE) == OBJ_TYPE_PC) {
            tig_debug_printf("MP: Packet70 ignored (proto %d is %s)\n", pkt->s0.name,
                proto_obj == OBJ_HANDLE_NULL ? "unknown" : "a PC");
            return;
        }
        /* P1-2: a ground item keeps its state (HP, charges, names) when it
           comes as the host's object blob. */
        if (tig_net_is_active() && item_is_item(proto_obj)
            && coop_lite_client_want_ground_blob((const unsigned char*)&pkt->s0.oid, pkt->s0.loc, pkt->s0.name)) {
            return;
        }
    }

    multiplayer_lock();
    switch (pkt->subtype) {
    case 0:
        object_create_ex(sub_4685A0(pkt->s0.name), pkt->s0.loc, pkt->s0.oid, &obj);
        break;
    case 5:
        /* Blood pool FX at death location (art animate is best-effort). */
        if (object_create(sub_4685A0(BP_POOL_OF_BLOOD), pkt->s5.loc, &blood_obj)) {
            tig_art_id_t aid = obj_field_int32_get(blood_obj, OBJ_F_CURRENT_AID);
            TigArtAnimData anim_data;

            sub_43E770(blood_obj, pkt->s5.loc, pkt->s5.offset_x, pkt->s5.offset_y);
            /* PT19: the host grows its pool with an AG_ANIMATE; here it sat
               on frame 0. Show the spread pool. */
            if (tig_art_anim_data(aid, &anim_data) == TIG_OK && anim_data.num_frames > 1) {
                object_set_current_aid(blood_obj, tig_art_id_frame_set(aid, anim_data.num_frames - 1));
            }
            tig_debug_printf("MP: blood pool at %d,%d (%d frames)\n",
                (int)LOCATION_GET_X(pkt->s5.loc), (int)LOCATION_GET_Y(pkt->s5.loc),
                tig_art_anim_data(aid, &anim_data) == TIG_OK ? anim_data.num_frames : -1);
        } else {
            tig_debug_printf("MP: blood pool create failed\n");
        }
        break;
    default:
        break;
    }
    multiplayer_unlock();
}

// Packet92: current art id mirror.
void mp_handle_packet92(Packet92* pkt)
{
    int64_t obj;

    if (pkt == NULL || tig_net_is_host()) {
        return;
    }
    obj = obj_pool_perm_lookup(pkt->oid);
    if (obj == OBJ_HANDLE_NULL) {
        return;
    }
    multiplayer_lock();
    object_set_current_aid(obj, pkt->art_id);
    multiplayer_unlock();
}

static bool mp_cast_item_held_by(ObjectID source_oid, ObjectID parent_oid, int64_t pc)
{
    int64_t source = obj_pool_perm_lookup(source_oid);
    int64_t parent = OBJ_HANDLE_NULL;

    if (source == OBJ_HANDLE_NULL || !objid_is_equal(parent_oid, obj_get_id(pc))) {
        return false;
    }
    if (!obj_type_is_item(obj_field_int32_get(source, OBJ_F_TYPE))) {
        return false;
    }
    return item_parent(source, &parent) && parent == pc;
}

// Packet57: client spell request → host runs (rebroadcasts 58).
void mp_handle_packet57(PacketPlayerRequestCastSpell* pkt)
{
    if (pkt == NULL || !tig_net_is_host()) {
        return;
    }
    if (sub_52A530() != 0) {
        /* The caster (or parent, for item casts) must be the sender's PC. */
        int64_t pc = sub_4A2B60(sub_52A530());
        ObjectID pc_oid;

        if (pc == OBJ_HANDLE_NULL) {
            return;
        }
        pc_oid = obj_get_id(pc);
        /* Review L5: the caster is the PC itself, or an item the PC holds
           (scroll, wand) with the PC as parent. */
        if (!objid_is_equal(pkt->invocation.source_obj.field_8.objid, pc_oid)
            && !mp_cast_item_held_by(pkt->invocation.source_obj.field_8.objid,
                pkt->invocation.parent_obj.field_8.objid, pc)) {
            tig_debug_printf("MP: Packet57 cast from slot %d not by its PC — dropped\n", sub_52A530());
            return;
        }
    }
    /* PT17: the client's handles are its own; rebuild them from the OIDs. */
    if (!mp_magictech_resolve_invocation(&pkt->invocation)) {
        return;
    }
    magictech_invocation_run(&pkt->invocation);
}

// Packet58: host cast broadcast → clients apply under lock.
void mp_handle_packet58(PacketPlayerCastSpell* pkt)
{
    if (pkt == NULL || tig_net_is_host()) {
        return;
    }
    multiplayer_lock();
    mp_magictech_apply_invocation(&pkt->invocation);
    multiplayer_unlock();
}

// Packet60 / 61: spell maintain UI for the owning player.
void mp_handle_packet60(PacketPlayerSpellMaintainAdd* pkt)
{
    if (pkt == NULL) {
        return;
    }
    if (pkt->player == sub_529520()) {
        ui_spell_maintain_add(pkt->mt_id);
    }
}

void mp_handle_packet61(PacketPlayerSpellMaintainEnd* pkt)
{
    if (pkt == NULL) {
        return;
    }
    if (pkt->player == sub_529520()) {
        ui_spell_maintain_end(pkt->mt_id);
    }
}

// Packet72: destroy object.
void mp_handle_packet72(PacketObjectDestroy* pkt)
{
    int64_t obj;

    if (pkt == NULL || tig_net_is_host()) {
        return;
    }
    obj = obj_pool_perm_lookup(pkt->oid);
    if (obj == OBJ_HANDLE_NULL) {
        return;
    }
    multiplayer_lock();
    object_destroy(obj);
    multiplayer_unlock();
}

// Packet73: summon mirror (create with host-assigned oid).
void mp_handle_packet73(PacketSummon* pkt)
{
    int64_t proto_obj;
    int64_t obj;

    if (pkt == NULL || tig_net_is_host()) {
        return;
    }

    proto_obj = obj_pool_perm_lookup(pkt->summon_info.field_60);
    if (proto_obj == OBJ_HANDLE_NULL) {
        return;
    }

    multiplayer_lock();
    if (object_create_ex(proto_obj, pkt->summon_info.loc, pkt->summon_info.field_88, &obj)) {
        if (obj_field_int32_get(obj, OBJ_F_TYPE) == OBJ_TYPE_SCENERY) {
            sub_424070(obj, 5, false, true);
        }
    }
    multiplayer_unlock();
}

// Packet74: dispel / related MT component mirrors.
void mp_handle_packet74(Packet74* pkt)
{
    int64_t obj;

    if (pkt == NULL || tig_net_is_host()) {
        return;
    }
    sub_4F0690(pkt->oid, &obj);
    multiplayer_lock();
    switch (pkt->subtype) {
    case 0:
        /* End maintain / dispel variant - best-effort via maintain end. */
        ui_spell_maintain_end(pkt->mt_id);
        break;
    case 1:
        if (obj != OBJ_HANDLE_NULL) {
            magictech_component_dispel(obj, pkt->mt_id);
        }
        break;
    default:
        break;
    }
    multiplayer_unlock();
}

// Packet75: MagicTech object-flag component.
void mp_handle_packet75(PacketMagicTechObjFlag* pkt)
{
    int64_t target;
    int64_t self_obj;
    int64_t parent_obj;
    int64_t source_obj;

    if (pkt == NULL || tig_net_is_host()) {
        return;
    }
    sub_4F0690(pkt->field_8, &target);
    sub_4F0690(pkt->self_oid, &self_obj);
    sub_4F0690(pkt->parent_oid, &parent_obj);
    sub_4F0690(pkt->source_oid, &source_obj);
    multiplayer_lock();
    magictech_component_obj_flag(target, self_obj, pkt->fld, pkt->value, pkt->state,
        parent_obj, source_obj);
    multiplayer_unlock();
}

// Packet76: charge/cost mirror.
void mp_handle_packet76(Packet76* pkt)
{
    int64_t obj;

    if (pkt == NULL || tig_net_is_host()) {
        return;
    }
    obj = obj_pool_perm_lookup(pkt->oid);
    if (obj == OBJ_HANDLE_NULL) {
        return;
    }
    multiplayer_lock();
    sub_450420(obj, pkt->cost, pkt->field_24, pkt->magictech);
    multiplayer_unlock();
}

// Packet77: eye candy add/remove.
void mp_handle_packet77(PacketMagicTechEyeCandy* pkt)
{
    /* PT28 item 7: host-streamed spell eye candy. The host's spell run id
       means nothing here (client run ids are local): -1 = any run. */
    if (pkt == NULL || tig_net_is_host()) {
        return;
    }
    multiplayer_lock();
    mp_magictech_apply_eye_candy_ex(pkt->subtype, pkt->oid, pkt->fx_id, -1);
    multiplayer_unlock();
}

// 0x4EF320
void sub_4EF320(void)
{
    /* Legacy stub slot - receive path lives in mp_handle_* above. */
}

// 0x4EF3D0
void sub_4EF3D0(void)
{
}

// 0x4EF540
void sub_4EF540(void)
{
}


// 0x4EF830
void sub_4EF830(int64_t a1, int64_t a2)
{
    Packet118 pkt;

    if (!tig_net_is_active()
        || tig_net_is_host()) {
        sub_4445A0(a1, a2);
        return;
    }

    pkt.type = 118;
    sub_4F0640(a1, &(pkt.field_8));
    sub_4F0640(a2, &(pkt.field_20));
    tig_net_send_app_all(&pkt, sizeof(pkt));
}

// 0x4EF8B0
void sub_4EF8B0(Packet118* pkt)
{
    int64_t v1;
    int64_t v2;

    if (tig_net_is_host()) {
        sub_4F0690(pkt->field_8, &v1);
        sub_4F0690(pkt->field_20, &v2);
        sub_4445A0(v1, v2);
    }
}

// 0x4EFAE0
void sub_4EFAE0(int64_t obj, int a2)
{
    Packet121 pkt;

    if (!tig_net_is_active()
        || tig_net_is_host()) {
        sub_463B30(obj, a2);
    }

    if (tig_net_is_active()
        && tig_net_is_host()) {
        if (!multiplayer_is_locked()) {
            pkt.type = 121;
            sub_4F0640(obj, &(pkt.oid));
            pkt.field_20 = a2;
            tig_net_send_app_all(&pkt, sizeof(pkt));
        }
    }
}

// 0x4EFB50
void sub_4EFB50(Packet121* pkt)
{
    int64_t obj;

    /* Host → clients only (review H2: a client could empty any inventory). */
    if (tig_net_is_host()) {
        return;
    }
    multiplayer_lock();
    sub_4F0690(pkt->oid, &obj);
    sub_463B30(obj, pkt->field_20);
    multiplayer_unlock();
}

// 0x4EFBA0
void sub_4EFBA0(int64_t obj)
{
    Packet122 pkt;

    if (tig_net_is_active()
        && !tig_net_is_host()) {
        pkt.type = 122;
        sub_4F0640(obj, &(pkt.oid));
        tig_net_send_app_all(&pkt, sizeof(pkt));
    }
}

// 0x4EFBE0
void sub_4EFBE0(void)
{
    // TODO: Incomplete.
}

// 0x4EFC30
void sub_4EFC30(int64_t pc_obj, const char* name, const char* rule)
{
    int size;
    Packet123* pkt;

    // NOTE: Unclear what additional 4 bytes are for.
    size = sizeof(*pkt) + (int)strlen(name) + (int)strlen(rule) + 2 + 4;
    pkt = (Packet123*)MALLOC(size);
    pkt->type = 123;
    pkt->total_size = size;
    pkt->player = multiplayer_find_slot_from_obj(pc_obj);
    pkt->name_length = (int)strlen(name) + 1;
    pkt->rule_length = (int)strlen(rule) + 1;
    strncpy((char*)(pkt + 1), name, pkt->name_length);
    strncpy((char*)(pkt + 1) + pkt->name_length, rule, pkt->rule_length);
    tig_net_send_app_all(&pkt, size);
    FREE(pkt);
}

// 0x4F05F0
void sub_4F05F0(void)
{
    // TODO: Incomplete.
}

// 0x4F0640
void sub_4F0640(int64_t obj, ObjectID* oid_ptr)
{
    if (obj != OBJ_HANDLE_NULL) {
        *oid_ptr = obj_get_id(obj);
    } else {
        oid_ptr->type = OID_TYPE_NULL;
    }
}

// 0x4F0690
void sub_4F0690(ObjectID oid, int64_t* obj_ptr)
{
    if (oid.type != OID_TYPE_NULL) {
        *obj_ptr = obj_pool_perm_lookup(oid);
    } else {
        *obj_ptr = OBJ_HANDLE_NULL;
    }
}

/*
 * Review PT24 #9: packets go on the wire as raw structs. Pin their sizes
 * (GCC x86-64 Linux == MinGW-w64, which uses the MS layout, so MSVC builds
 * should agree); a mismatch here means a layout change -> bump
 * TIG_NET_PROTOCOL and update the number.
 */
_Static_assert(sizeof(Packet10) == 64, "Packet10 wire layout");
_Static_assert(sizeof(Packet100) == 64, "Packet100 wire layout");
_Static_assert(sizeof(Packet104) == 32, "Packet104 wire layout");
_Static_assert(sizeof(Packet118) == 56, "Packet118 wire layout");
_Static_assert(sizeof(Packet121) == 40, "Packet121 wire layout");
_Static_assert(sizeof(Packet122) == 32, "Packet122 wire layout");
_Static_assert(sizeof(Packet123) == 20, "Packet123 wire layout");
_Static_assert(sizeof(Packet128) == 56, "Packet128 wire layout");
_Static_assert(sizeof(Packet26) == 40, "Packet26 wire layout");
_Static_assert(sizeof(Packet27) == 40, "Packet27 wire layout");
_Static_assert(sizeof(Packet28) == 64, "Packet28 wire layout");
_Static_assert(sizeof(Packet29) == 32, "Packet29 wire layout");
_Static_assert(sizeof(Packet4) == 40, "Packet4 wire layout");
_Static_assert(sizeof(Packet46) == 32, "Packet46 wire layout");
_Static_assert(sizeof(Packet5) == 424, "Packet5 wire layout");
_Static_assert(sizeof(Packet54) == 8, "Packet54 wire layout");
_Static_assert(sizeof(Packet6) == 120, "Packet6 wire layout");
_Static_assert(sizeof(Packet64) == 288, "Packet64 wire layout");
_Static_assert(sizeof(Packet67) == 8, "Packet67 wire layout");
_Static_assert(sizeof(Packet7) == 408, "Packet7 wire layout");
_Static_assert(sizeof(Packet70) == 104, "Packet70 wire layout");
_Static_assert(sizeof(Packet74) == 64, "Packet74 wire layout");
_Static_assert(sizeof(Packet76) == 48, "Packet76 wire layout");
_Static_assert(sizeof(Packet8) == 80, "Packet8 wire layout");
_Static_assert(sizeof(Packet80) == 64, "Packet80 wire layout");
_Static_assert(sizeof(Packet81) == 184, "Packet81 wire layout");
_Static_assert(sizeof(Packet82) == 88, "Packet82 wire layout");
_Static_assert(sizeof(Packet83) == 140, "Packet83 wire layout");
_Static_assert(sizeof(Packet84) == 48, "Packet84 wire layout");
_Static_assert(sizeof(Packet9) == 104, "Packet9 wire layout");
_Static_assert(sizeof(Packet92) == 40, "Packet92 wire layout");
_Static_assert(sizeof(Packet93) == 40, "Packet93 wire layout");
_Static_assert(sizeof(Packet97) == 56, "Packet97 wire layout");
_Static_assert(sizeof(Packet99) == 56, "Packet99 wire layout");
_Static_assert(sizeof(PacketCombatModeSet) == 40, "PacketCombatModeSet wire layout");
_Static_assert(sizeof(PacketCoopAttackIntent) == 56, "PacketCoopAttackIntent wire layout");
_Static_assert(sizeof(PacketCoopBarter) == 56, "PacketCoopBarter wire layout");
_Static_assert(sizeof(PacketCoopCharChunk) == 1040, "PacketCoopCharChunk wire layout");
_Static_assert(sizeof(PacketCoopCharOffer) == 120, "PacketCoopCharOffer wire layout");
_Static_assert(sizeof(PacketCoopFollowHost) == 24, "PacketCoopFollowHost wire layout");
_Static_assert(sizeof(PacketCoopFollowerEntry) == 32, "PacketCoopFollowerEntry wire layout");
_Static_assert(sizeof(PacketCoopFollowers) == 520, "PacketCoopFollowers wire layout");
_Static_assert(sizeof(PacketCoopInvenEntry) == 56, "PacketCoopInvenEntry wire layout");
_Static_assert(sizeof(PacketCoopInvenSync) == 3760, "PacketCoopInvenSync wire layout");
_Static_assert(sizeof(PacketCoopNpcPos) == 904, "PacketCoopNpcPos wire layout");
_Static_assert(sizeof(PacketCoopNpcPosEntry) == 56, "PacketCoopNpcPosEntry wire layout");
_Static_assert(sizeof(PacketCoopObjBlob) == 8256, "PacketCoopObjBlob wire layout");
_Static_assert(sizeof(PacketCoopObjReq) == 392, "PacketCoopObjReq wire layout");
_Static_assert(sizeof(PacketCoopPartyState) == 24, "PacketCoopPartyState wire layout");
_Static_assert(sizeof(PacketCoopSwing) == 56, "PacketCoopSwing wire layout");
_Static_assert(sizeof(PacketCoopPcPos) == 96, "PacketCoopPcPos wire layout");
_Static_assert(sizeof(PacketCoopPcStats) == 444, "PacketCoopPcStats wire layout");
_Static_assert(sizeof(PacketCoopRoster) == 112, "PacketCoopRoster wire layout");
_Static_assert(sizeof(PacketCoopSessionSync) == 224, "PacketCoopSessionSync wire layout");
_Static_assert(sizeof(PacketCoopWorldReq) == 8, "PacketCoopWorldReq wire layout");
_Static_assert(sizeof(PacketCoopWorldState) == 4112, "PacketCoopWorldState wire layout");
_Static_assert(sizeof(PacketGamePlayerList) == 200, "PacketGamePlayerList wire layout");
_Static_assert(sizeof(PacketGameTime) == 24, "PacketGameTime wire layout");
_Static_assert(sizeof(PacketMagicTechEyeCandy) == 40, "PacketMagicTechEyeCandy wire layout");
_Static_assert(sizeof(PacketMagicTechObjFlag) == 120, "PacketMagicTechObjFlag wire layout");
_Static_assert(sizeof(PacketMultiplayerFlagsChange) == 12, "PacketMultiplayerFlagsChange wire layout");
_Static_assert(sizeof(PacketNotifyPlayerLagging) == 56, "PacketNotifyPlayerLagging wire layout");
_Static_assert(sizeof(PacketNotifyPlayerRecovered) == 56, "PacketNotifyPlayerRecovered wire layout");
_Static_assert(sizeof(PacketObjectDestroy) == 32, "PacketObjectDestroy wire layout");
_Static_assert(sizeof(PacketPartyUpdate) == 36, "PacketPartyUpdate wire layout");
_Static_assert(sizeof(PacketPerformIdentifyService) == 88, "PacketPerformIdentifyService wire layout");
_Static_assert(sizeof(PacketPerformRepairService) == 88, "PacketPerformRepairService wire layout");
_Static_assert(sizeof(PacketPlaySound) == 40, "PacketPlaySound wire layout");
_Static_assert(sizeof(PacketPlayerCastSpell) == 232, "PacketPlayerCastSpell wire layout");
_Static_assert(sizeof(PacketPlayerRequestCastSpell) == 232, "PacketPlayerRequestCastSpell wire layout");
_Static_assert(sizeof(PacketPlayerSpellMaintainAdd) == 12, "PacketPlayerSpellMaintainAdd wire layout");
_Static_assert(sizeof(PacketPlayerSpellMaintainEnd) == 12, "PacketPlayerSpellMaintainEnd wire layout");
_Static_assert(sizeof(PacketSummon) == 224, "PacketSummon wire layout");
_Static_assert(sizeof(PacketUpdateInven) == 32, "PacketUpdateInven wire layout");
