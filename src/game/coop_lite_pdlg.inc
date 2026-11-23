/*
 * Coop Middle: parallel dialogs (protocol 54, host rule "pdlg").
 * Included at the end of coop_lite.c, after coop_lite_mid.inc: it shares the
 * PT36 / PT41 dialog statics (seq counter, viewer, hooks, notices).
 *
 * Host: one session per speaking slot. A session wraps one dialog_ui entry
 * (entry index = slot, see coop_lite_pdlg_entry_index), so the engine keeps
 * a separate DialogState per player and every script effect runs with that
 * player's PC. Lines go only to the speaker and to spectators (players who
 * talked to the same NPC). Every packet takes the next coop_dialog_seq, so a
 * client sees one monotonic sequence whichever session it is in.
 */

/* ---- pure ---------------------------------------------------------------- */

void coop_pdlg_sessions_reset(CoopPdlgSession* s)
{
    int i;

    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(CoopPdlgSession) * 8);
    for (i = 0; i < 8; i++) {
        coop_dialog_votes_reset(&s[i].votes);
    }
}

int coop_pdlg_find_npc(const CoopPdlgSession* s, int64_t npc, int except)
{
    int i;

    if (s == NULL || npc == OBJ_HANDLE_NULL) {
        return -1;
    }
    for (i = 0; i < 8; i++) {
        if (i != except && (s[i].open || s[i].pending) && s[i].npc == npc) {
            return i;
        }
    }
    return -1;
}

int coop_pdlg_spectating(const CoopPdlgSession* s, int slot)
{
    int i;

    if (s == NULL || slot < 1 || slot >= 8) {
        return -1;
    }
    for (i = 0; i < 8; i++) {
        if (i != slot && (s[i].open || s[i].pending) && (s[i].spectators & (1u << slot)) != 0) {
            return i;
        }
    }
    return -1;
}

int coop_pdlg_start_decision(const CoopPdlgSession* s, int slot, int64_t npc, int* other)
{
    int o;

    if (other != NULL) {
        *other = -1;
    }
    if (s == NULL || slot < 1 || slot >= 8 || npc == OBJ_HANDLE_NULL) {
        return COOP_PDLG_START_BAD;
    }
    if (s[slot].open && s[slot].npc == npc) {
        return COOP_PDLG_START_RESEND;
    }
    o = coop_pdlg_find_npc(s, npc, slot);
    if (o >= 0) {
        if (other != NULL) {
            *other = o;
        }
        return COOP_PDLG_START_JOIN;
    }
    return COOP_PDLG_START_NEW;
}

unsigned int coop_pdlg_recipients(const CoopPdlgSession* s, int speaker)
{
    unsigned int mask;

    if (s == NULL || speaker < 0 || speaker >= 8 || !s[speaker].open) {
        return 0;
    }
    mask = s[speaker].spectators & 0xFEu;
    if (speaker >= 1) {
        mask |= 1u << speaker;
    }
    return mask;
}

bool coop_pdlg_drop_spectator(CoopPdlgSession* s, int slot)
{
    bool was = false;
    int i;

    if (s == NULL || slot < 1 || slot >= 8) {
        return false;
    }
    for (i = 0; i < 8; i++) {
        if ((s[i].spectators & (1u << slot)) != 0) {
            s[i].spectators &= ~(1u << slot);
            coop_dialog_vote_clear_slot(&s[i].votes, slot);
            s[i].sent_seq[slot] = 0;
            was = true;
        }
    }
    return was;
}

bool coop_pdlg_ui_code_ok(int code)
{
    /* 3 barter / follower exchange, 8 identify, 9 repair. Character sheet
       (5), world map (6) and schematics (7) stay on the host: they would
       change only the client's copy. */
    return code == 3 || code == 8 || code == 9;
}

int coop_pdlg_end_reason(bool speaker_active, bool alive, bool in_combat, int dist, bool same_map)
{
    if (!speaker_active) {
        return COOP_PDLG_END_LEFT;
    }
    if (!same_map) {
        return COOP_PDLG_END_MAP;
    }
    if (!alive) {
        return COOP_PDLG_END_GONE;
    }
    if (in_combat) {
        return COOP_PDLG_END_COMBAT;
    }
    if (dist < 0 || dist > COOP_PDLG_MAX_DIST) {
        return COOP_PDLG_END_FAR;
    }
    return COOP_PDLG_END_KEEP;
}

void coop_dialog_fill_ui(PacketCoopDialogState* pkt, unsigned int seq, int speaker, int code, bool follower,
    const unsigned char* oid24)
{
    memset(pkt, 0, COOP_DIALOG_HDR_SIZE + 24);
    pkt->type = PACKET_COOP_DIALOG_STATE;
    pkt->kind = COOP_DIALOG_KIND_STATE;
    pkt->action = COOP_DIALOG_ACT_UI;
    pkt->seq = seq;
    pkt->votes[0] = (unsigned char)code;
    pkt->votes[1] = follower ? 1 : 0;
    pkt->votes[COOP_DIALOG_SPEAKER_AT] = (unsigned char)speaker;
    if (oid24 != NULL) {
        memcpy(pkt->npc_name, oid24, 24);
    }
}

/* ---- game side ------------------------------------------------------------ */

#ifndef COOP_LITE_TEST_STUBS

static CoopPdlgSession coop_pdlg[8];
static bool coop_pdlg_inited;
static bool coop_pdlg_latched; /* host: parallel dialogs for this session */
static unsigned int coop_pdlg_line_ctr;
static int coop_pdlg_busy_slot = -1; /* speaker whose answer is being run */
static bool (*coop_pdlg_choose_hook)(int64_t pc_obj, int option);
static void (*coop_pdlg_drop_hook)(int64_t pc_obj);
/* client: a window to open after the host closed our dialog */
static bool coop_pdlg_client_ui_pending;
static int coop_pdlg_client_ui_code;
static uint8_t coop_pdlg_client_ui_oid[24];

static void coop_pdlg_ensure(void)
{
    if (!coop_pdlg_inited) {
        coop_pdlg_inited = true;
        coop_pdlg_sessions_reset(coop_pdlg);
    }
}

static unsigned int coop_pdlg_now(void)
{
    unsigned int now = (unsigned int)(SDL_GetTicksNS() / 1000000u);

    return now != 0 ? now : 1;
}

static const char* coop_pdlg_reason_name(int reason)
{
    switch (reason) {
    case COOP_PDLG_END_LEFT:
        return "speaker left";
    case COOP_PDLG_END_GONE:
        return "PC or NPC gone";
    case COOP_PDLG_END_COMBAT:
        return "combat";
    case COOP_PDLG_END_FAR:
        return "too far";
    case COOP_PDLG_END_MAP:
        return "map change";
    default:
        return "-";
    }
}

void coop_lite_set_pdlg_hooks(bool (*choose)(int64_t pc_obj, int option), void (*drop)(int64_t pc_obj))
{
    coop_pdlg_choose_hook = choose;
    coop_pdlg_drop_hook = drop;
}

bool coop_lite_pdlg_on(void)
{
    /* Not tig_net_is_host(): that reads false under local suspension. The
       latch is only ever set on the host. */
    return coop_pdlg_latched;
}

/* Slot of a PC on the host: 0 = the host's own. */
static int coop_pdlg_slot_of_pc(int64_t pc)
{
    if (pc == OBJ_HANDLE_NULL) {
        return -1;
    }
    if (player_is_local_pc_obj(pc)) {
        return 0;
    }
    return coop_lite_slot_of_obj(pc);
}

int coop_lite_pdlg_entry_index(int64_t pc_obj)
{
    int slot;

    if (!coop_pdlg_latched || pc_obj == OBJ_HANDLE_NULL || player_is_local_pc_obj(pc_obj)) {
        return 0;
    }
    slot = coop_lite_slot_of_obj(pc_obj);
    return slot >= 1 && slot < 8 ? slot : 0;
}

bool coop_lite_pdlg_allowed(int64_t pc_obj)
{
    int slot;

    if (!coop_pdlg_latched) {
        return false;
    }
    coop_pdlg_ensure();
    slot = coop_pdlg_slot_of_pc(pc_obj);
    if (slot < 1 || slot >= 8) {
        return false;
    }
    return (coop_pdlg[slot].pending || coop_pdlg[slot].open) && coop_pdlg[slot].pc == pc_obj;
}

int64_t coop_lite_pdlg_npc_busy(int64_t npc_obj, int64_t pc_obj)
{
    int i;

    if (!coop_pdlg_latched || npc_obj == OBJ_HANDLE_NULL) {
        return OBJ_HANDLE_NULL;
    }
    coop_pdlg_ensure();
    for (i = 0; i < 8; i++) {
        if ((coop_pdlg[i].open || coop_pdlg[i].pending) && coop_pdlg[i].npc == npc_obj && coop_pdlg[i].pc != pc_obj) {
            return coop_pdlg[i].pc;
        }
    }
    return OBJ_HANDLE_NULL;
}

/* ---- host: sending ---- */

static void coop_pdlg_send(const PacketCoopDialogState* pkt, int slot)
{
    int depth;

    if (slot < 1 || slot >= 8 || !tig_net_client_is_active(slot)) {
        return;
    }
    depth = tig_net_local_suspend_pause();
    tig_net_send_app((void*)pkt, coop_dialog_wire_size(pkt), slot);
    tig_net_local_suspend_resume(depth);
}

static void coop_pdlg_send_close(int slot)
{
    PacketCoopDialogState pkt;

    coop_dialog_fill_close(&pkt, ++coop_dialog_seq, 0);
    coop_pdlg_send(&pkt, slot);
}

/* Send the session's current packet to every recipient that has not seen it. */
static void coop_pdlg_flush(int speaker)
{
    CoopPdlgSession* s = &coop_pdlg[speaker];
    unsigned int mask = coop_pdlg_recipients(coop_pdlg, speaker);
    int r;

    for (r = 1; r < 8; r++) {
        if ((mask & (1u << r)) == 0 || !tig_net_client_is_active(r) || s->sent_seq[r] == s->tx.seq) {
            continue;
        }
        s->sent_seq[r] = s->tx.seq;
        coop_pdlg_send(&s->tx, r);
    }
}

/* New seq with the current vote tally, then send. */
static void coop_pdlg_republish(int speaker)
{
    CoopPdlgSession* s = &coop_pdlg[speaker];
    unsigned char tally[COOP_DIALOG_MAX_OPTIONS];
    int i;

    if (!s->open) {
        return;
    }
    coop_dialog_votes_tally(&s->votes, tally);
    for (i = 0; i < COOP_DIALOG_MAX_OPTIONS; i++) {
        s->tx.votes[i] = i < s->tx.num_options ? tally[i] : 0;
    }
    s->tx.seq = ++coop_dialog_seq;
    coop_pdlg_flush(speaker);
    if (speaker == 0 && coop_dialog_host_refresh_hook != NULL) {
        coop_dialog_host_refresh_hook(); /* vote markers on the host's own options */
    }
}

static void coop_pdlg_clear(CoopPdlgSession* s)
{
    s->open = false;
    s->pending = false;
    s->end_pending = false;
    s->pc = OBJ_HANDLE_NULL;
    s->npc = OBJ_HANDLE_NULL;
    s->spectators = 0;
    s->close_action = COOP_DIALOG_ACT_CLOSE;
    s->ui_code = 0;
    s->ui_follower = false;
    s->ui_npc = OBJ_HANDLE_NULL;
    s->choose_ms = 0;
    memset(s->sent_seq, 0, sizeof(s->sent_seq));
    coop_dialog_votes_reset(&s->votes);
}

/* The session is over: tell the speaker (CLOSE, or ACT_UI = open a window)
   and every spectator (CLOSE). No engine calls. */
static void coop_pdlg_close(int slot)
{
    CoopPdlgSession* s = &coop_pdlg[slot];
    PacketCoopDialogState pkt;
    int watchers = 0;
    int r;

    if (!s->open && !s->pending) {
        return;
    }
    if (s->open && slot >= 1) {
        if (s->close_action == COOP_DIALOG_ACT_UI && s->ui_npc != OBJ_HANDLE_NULL && obj_handle_is_valid(s->ui_npc)) {
            uint8_t oid[24];

            coop_lite_put_oid(oid, s->ui_npc);
            coop_dialog_fill_ui(&pkt, ++coop_dialog_seq, slot, s->ui_code, s->ui_follower, oid);
            tig_debug_printf("CoopMid: pdlg: slot %d leaves the dialog into window %d%s\n", slot, s->ui_code,
                s->ui_follower ? " (follower exchange)" : "");
        } else {
            coop_dialog_fill_close(&pkt, ++coop_dialog_seq, s->line);
        }
        coop_pdlg_send(&pkt, slot);
    }
    for (r = 1; r < 8; r++) {
        if ((s->spectators & (1u << r)) != 0) {
            coop_pdlg_send_close(r);
            watchers++;
        }
    }
    if (s->open) {
        tig_debug_printf("CoopMid: pdlg: slot %d dialog closed (%d watching)\n", slot, watchers);
    }
    coop_pdlg_clear(s);
}

/* End a remote speaker's engine dialog (dialog_ui), then the session. */
static void coop_pdlg_end_engine(int slot)
{
    CoopPdlgSession* s = &coop_pdlg[slot];
    int64_t pc = s->pc;
    int64_t npc = s->npc;

    if (s->open && pc != OBJ_HANDLE_NULL) {
        if (obj_handle_is_valid(pc) && (npc == OBJ_HANDLE_NULL || obj_handle_is_valid(npc)) && map_is_valid()
            && !map_is_clearing_objects()) {
            if (coop_dialog_end_hook != NULL) {
                coop_dialog_end_hook(pc, 0); /* -> coop_lite_host_dialog_end_pc -> close */
            }
        } else if (coop_pdlg_drop_hook != NULL) {
            tig_debug_printf("CoopMid: pdlg: slot %d dialog dropped (objects gone)\n", slot);
            coop_pdlg_drop_hook(pc);
        }
    }
    if (s->open || s->pending) {
        coop_pdlg_close(slot);
    }
}

/* ---- host: dialog_ui callbacks ---- */

void coop_lite_host_dialog_publish(int64_t pc_obj, int64_t npc_obj, const char* npc_name, const char* reply,
    int num_options, const char* const* options)
{
    CoopPdlgSession* s;
    int slot;

    if (!coop_pdlg_latched) {
        /* PT36 / PT41: the host's own dialog, or the client's party dialog */
        if (player_is_local_pc_obj(pc_obj) || (pc_obj != OBJ_HANDLE_NULL && pc_obj == coop_lite_party_dialog_pc())) {
            coop_lite_host_dialog_state(npc_name, reply, num_options, options);
        }
        return;
    }
    coop_pdlg_ensure();
    slot = coop_pdlg_slot_of_pc(pc_obj);
    if (slot < 0 || slot >= 8) {
        return;
    }
    s = &coop_pdlg[slot];
    if (slot > 0 && !s->open && !s->pending) {
        tig_debug_printf("CoopMid: pdlg: dialog of slot %d was not asked for - not sent\n", slot);
        return;
    }
    if (!s->open) {
        coop_pdlg_clear(s);
        s->open = true;
        s->map = map_current_map();
        tig_debug_printf("CoopMid: pdlg: slot %d talks to '%s' (own dialog)\n", slot, npc_name != NULL ? npc_name : "");
    }
    s->pending = false;
    s->pc = pc_obj;
    s->npc = npc_obj;
    if (++coop_pdlg_line_ctr == 0) {
        coop_pdlg_line_ctr = 1;
    }
    s->line = coop_pdlg_line_ctr;
    coop_dialog_votes_new_line(&s->votes, s->line, num_options);
    coop_dialog_fill_state(&s->tx, 0, s->line, npc_name, reply, num_options, options, NULL);
    s->tx.votes[COOP_DIALOG_SPEAKER_AT] = (unsigned char)slot;
    s->tx.votes[COOP_DIALOG_FLAGS_AT] = (unsigned char)COOP_DIALOG_FLAG_PDLG;
    coop_pdlg_republish(slot);
}

void coop_lite_host_dialog_end_pc(int64_t pc_obj)
{
    int slot;
    int i;

    if (!coop_pdlg_latched) {
        if (player_is_local_pc_obj(pc_obj)) {
            coop_lite_host_dialog_closed();
        }
        coop_lite_party_dialog_ended(pc_obj);
        return;
    }
    coop_pdlg_ensure();
    slot = coop_pdlg_slot_of_pc(pc_obj);
    if (slot < 0 || slot >= 8 || coop_pdlg[slot].pc != pc_obj) {
        slot = -1;
        for (i = 0; i < 8; i++) {
            if ((coop_pdlg[i].open || coop_pdlg[i].pending) && coop_pdlg[i].pc == pc_obj && pc_obj != OBJ_HANDLE_NULL) {
                slot = i;
                break;
            }
        }
    }
    if (slot >= 0) {
        coop_pdlg_close(slot);
    }
}

/* coop_lite_host_dialog_closed (PT67 orphan / reset / notify): the host's own. */
static void coop_pdlg_host_closed(void)
{
    coop_pdlg_ensure();
    coop_pdlg_close(0);
}

static int coop_pdlg_host_vote_count(int option)
{
    unsigned char tally[COOP_DIALOG_MAX_OPTIONS];

    coop_pdlg_ensure();
    if (!coop_pdlg[0].open || option < 0 || option >= COOP_DIALOG_MAX_OPTIONS) {
        return 0;
    }
    coop_dialog_votes_tally(&coop_pdlg[0].votes, tally);
    return tally[option];
}

bool coop_lite_pdlg_ui_request(int64_t pc_obj, int64_t npc_obj, int code)
{
    CoopPdlgSession* s;
    int slot;

    if (!coop_pdlg_latched) {
        return false;
    }
    slot = coop_pdlg_slot_of_pc(pc_obj);
    if (slot < 1 || slot >= 8 || !coop_pdlg[slot].open || coop_pdlg[slot].pc != pc_obj) {
        return false;
    }
    s = &coop_pdlg[slot];
    if (code == 3 && coop_lite_follower_exchange(pc_obj, npc_obj)) {
        s->ui_follower = true; /* PT80: the speaker's own follower */
    } else if (coop_pdlg_ui_code_ok(code)) {
        s->ui_follower = false;
    } else {
        return false;
    }
    s->close_action = COOP_DIALOG_ACT_UI;
    s->ui_code = code;
    s->ui_npc = npc_obj;
    return true;
}

/* ---- host: client requests ---- */

static void coop_pdlg_refuse(int slot, const char* why)
{
    tig_debug_printf("CoopMid: pdlg: slot %d talk refused (%s)\n", slot, why);
    coop_lite_host_slot_notice(slot, COOP_PARTY_PDLG_REFUSED);
}

/* A watcher leaves the dialog it watched (moved on, Esc, started its own). */
static void coop_pdlg_stop_watching(int slot)
{
    int sp = coop_pdlg_spectating(coop_pdlg, slot);

    if (sp < 0) {
        return;
    }
    coop_pdlg_drop_spectator(coop_pdlg, slot);
    coop_pdlg_send_close(slot);
    tig_debug_printf("CoopMid: pdlg: slot %d stops watching slot %d's dialog\n", slot, sp);
    coop_pdlg_republish(sp); /* its vote is gone */
}

static void coop_pdlg_host_start(int slot, const PacketCoopDialogState* pkt)
{
    CoopPdlgSession* s = &coop_pdlg[slot];
    unsigned int now_ms = coop_pdlg_now();
    int64_t pc;
    int64_t npc;
    int other;
    int d;

    if (coop_party_start_ms[slot] != 0
        && !coop_lite_interval_elapsed(coop_party_start_ms[slot], now_ms, COOP_PARTY_TALK_INTERVAL_MS)) {
        return;
    }
    coop_party_start_ms[slot] = now_ms;
    if (!map_is_valid() || map_is_clearing_objects()) {
        return;
    }
    pc = sub_4A2B60(slot);
    npc = coop_lite_get_oid_obj((const uint8_t*)pkt->npc_name);
    if (pc == OBJ_HANDLE_NULL || npc == OBJ_HANDLE_NULL || obj_field_int32_get(npc, OBJ_F_TYPE) != OBJ_TYPE_NPC
        || critter_is_dead(npc) || critter_is_dead(pc) || critter_is_unconscious(pc)) {
        coop_pdlg_refuse(slot, "PC or NPC not found / dead");
        return;
    }
    d = coop_pdlg_start_decision(coop_pdlg, slot, npc, &other);
    if (d == COOP_PDLG_START_BAD) {
        return;
    }
    if (d == COOP_PDLG_START_RESEND) {
        s->sent_seq[slot] = 0;
        coop_pdlg_flush(slot);
        tig_debug_printf("CoopMid: pdlg: slot %d asked again - dialog resent\n", slot);
        return;
    }
    if (s->open || s->pending) {
        tig_debug_printf("CoopMid: pdlg: slot %d leaves its dialog for another NPC\n", slot);
        coop_pdlg_end_engine(slot);
    }
    coop_pdlg_stop_watching(slot);
    if ((int)object_dist(pc, npc) > (d == COOP_PDLG_START_JOIN ? COOP_PDLG_MAX_DIST : COOP_PARTY_TALK_MAX_DIST)) {
        coop_pdlg_refuse(slot, "too far");
        return;
    }
    if (d == COOP_PDLG_START_JOIN) {
        CoopPdlgSession* o = &coop_pdlg[other];

        if (!o->open) {
            coop_pdlg_refuse(slot, "NPC busy");
            return;
        }
        o->spectators |= 1u << slot;
        o->sent_seq[slot] = 0;
        coop_dialog_vote_clear_slot(&o->votes, slot);
        tig_debug_printf("CoopMid: pdlg: slot %d watches slot %d's dialog (same NPC)\n", slot, other);
        coop_pdlg_flush(other);
        return;
    }
    if (combat_turn_based_is_active() || combat_critter_is_combat_mode_active(npc)) {
        coop_pdlg_refuse(slot, "combat");
        return;
    }
    s->pending = true;
    s->pc = pc;
    s->npc = npc;
    tig_debug_printf("CoopMid: pdlg: slot %d starts a dialog\n", slot);
    object_script_execute(pc, npc, OBJ_HANDLE_NULL, SAP_DIALOG, 0);
    if (!s->open) {
        coop_pdlg_clear(s);
        tig_debug_printf("CoopMid: pdlg: slot %d got no dialog (bark / refused by the NPC)\n", slot);
    }
}

static void coop_pdlg_host_choose(int slot, const PacketCoopDialogState* pkt)
{
    CoopPdlgSession* s = &coop_pdlg[slot];
    unsigned int now_ms = coop_pdlg_now();
    int idx = pkt->vote_index;

    if (!s->open) {
        if (idx == -1 && coop_pdlg_spectating(coop_pdlg, slot) >= 0) {
            coop_pdlg_stop_watching(slot);
        } else {
            tig_debug_printf("CoopMid: pdlg: answer from slot %d without a dialog dropped\n", slot);
        }
        return;
    }
    if (!coop_lite_party_choose_ok(slot, slot, pkt->line, s->line, idx, s->votes.num_options)) {
        tig_debug_printf("CoopMid: pdlg: stale answer from slot %d dropped\n", slot);
        return;
    }
    if (s->choose_ms != 0 && !coop_lite_interval_elapsed(s->choose_ms, now_ms, COOP_DIALOG_VOTE_INTERVAL_MS)) {
        return;
    }
    s->choose_ms = now_ms;
    tig_debug_printf("CoopMid: pdlg: slot %d answers %d\n", slot, idx);
    if (idx < 0) {
        coop_pdlg_end_engine(slot);
        return;
    }
    if (coop_pdlg_choose_hook == NULL) {
        return;
    }
    coop_pdlg_busy_slot = slot;
    coop_pdlg_choose_hook(s->pc, idx);
    coop_pdlg_busy_slot = -1;
}

static void coop_pdlg_host_vote(int slot, const PacketCoopDialogState* pkt)
{
    int sp = coop_pdlg_spectating(coop_pdlg, slot);

    if (sp < 0 || !coop_pdlg[sp].open) {
        return;
    }
    if (!coop_dialog_vote_apply(&coop_pdlg[sp].votes, slot, pkt->vote_index, pkt->line, coop_pdlg_now())) {
        return;
    }
    tig_debug_printf("CoopMid: pdlg: slot %d votes %d in slot %d's dialog\n", slot, (int)pkt->vote_index, sp);
    coop_pdlg_republish(sp);
}

static void coop_pdlg_host_rx(const PacketCoopDialogState* pkt)
{
    int slot = sub_52A530();

    if (slot < 1 || slot >= 8) {
        return;
    }
    coop_pdlg_ensure();
    if (pkt->kind == COOP_DIALOG_KIND_START) {
        coop_pdlg_host_start(slot, pkt);
    } else if (pkt->kind == COOP_DIALOG_KIND_CHOOSE) {
        coop_pdlg_host_choose(slot, pkt);
    } else {
        coop_pdlg_host_vote(slot, pkt);
    }
}

/* PT81 in parallel mode: an NPC starts a dialog with a client's PC. */
static bool coop_pdlg_npc_begin(int64_t pc_obj, int64_t npc_obj)
{
    unsigned int now_ms = coop_pdlg_now();
    CoopPdlgSession* s;
    int slot;

    coop_pdlg_ensure();
    slot = coop_lite_slot_of_obj(pc_obj);
    if (slot < 1 || slot >= 8 || !tig_net_client_is_active(slot)) {
        return false;
    }
    s = &coop_pdlg[slot];
    if (s->open || s->pending || coop_pdlg_find_npc(coop_pdlg, npc_obj, slot) >= 0) {
        return false; /* never cut into a running talk */
    }
    if (coop_party_npc_ms[slot] != 0
        && !coop_lite_interval_elapsed(coop_party_npc_ms[slot], now_ms, COOP_PARTY_NPC_INTERVAL_MS)) {
        return false;
    }
    if (!map_is_valid() || map_is_clearing_objects() || obj_field_int32_get(npc_obj, OBJ_F_TYPE) != OBJ_TYPE_NPC
        || critter_is_dead(npc_obj) || critter_is_dead(pc_obj) || critter_is_unconscious(pc_obj)
        || combat_critter_is_combat_mode_active(pc_obj) || combat_turn_based_is_active()) {
        return false;
    }
    coop_party_npc_ms[slot] = now_ms;
    coop_party_start_ms[slot] = now_ms;
    coop_pdlg_stop_watching(slot);
    s->pending = true;
    s->pc = pc_obj;
    s->npc = npc_obj;
    tig_debug_printf("CoopMid: pdlg: an NPC starts a dialog with slot %d\n", slot);
    return true;
}

static void coop_pdlg_npc_end(int64_t pc_obj)
{
    int slot = coop_lite_slot_of_obj(pc_obj);

    if (slot >= 1 && slot < 8 && coop_pdlg[slot].pending && !coop_pdlg[slot].open) {
        coop_pdlg_clear(&coop_pdlg[slot]);
        tig_debug_printf("CoopMid: pdlg: slot %d got no dialog (NPC start)\n", slot);
    }
}

/* ---- host: tick ---- */

/* true = parallel dialogs run this session (the PT36 tick is skipped). */
static bool coop_pdlg_host_tick(unsigned int now_ms)
{
    int slot;
    int r;

    (void)now_ms;
    coop_pdlg_ensure();
    if (!coop_pdlg_latched) {
        if (!coop_mid_host_rules_loaded || !coop_lite_rule_on(COOP_RULE_PDLG) || !tig_net_is_coop_enabled()
            || coop_party_slot > 0 || coop_dialog_votes.open) {
            return false;
        }
        coop_pdlg_latched = true;
        tig_debug_printf("CoopMid: parallel dialogs on (rule pdlg, protocol %d)\n", TIG_NET_PROTOCOL);
    }
    for (slot = 0; slot < 8; slot++) {
        CoopPdlgSession* s = &coop_pdlg[slot];
        bool changed = false;

        if (!s->open && !s->pending) {
            continue;
        }
        for (r = 1; r < 8; r++) {
            int64_t w;

            if ((s->spectators & (1u << r)) == 0) {
                continue;
            }
            if (!tig_net_client_is_active(r)) {
                s->spectators &= ~(1u << r);
                coop_dialog_vote_clear_slot(&s->votes, r);
                s->sent_seq[r] = 0;
                changed = true;
                continue;
            }
            /* a watcher who walked away stops watching */
            w = sub_4A2B60(r);
            if (s->open && map_is_valid() && !map_is_clearing_objects() && w != OBJ_HANDLE_NULL && s->npc != OBJ_HANDLE_NULL
                && obj_handle_is_valid(w) && obj_handle_is_valid(s->npc)
                && (int)object_dist(w, s->npc) > COOP_PDLG_MAX_DIST) {
                s->spectators &= ~(1u << r);
                coop_dialog_vote_clear_slot(&s->votes, r);
                s->sent_seq[r] = 0;
                coop_pdlg_send_close(r);
                tig_debug_printf("CoopMid: pdlg: slot %d walked away from slot %d's dialog\n", r, slot);
                changed = true;
            }
        }
        if (slot >= 1 && s->open && slot != coop_pdlg_busy_slot) {
            bool same_map = s->end_pending == false && map_is_valid() && !map_is_clearing_objects()
                && s->map == map_current_map();
            bool alive = false;
            bool combat = false;
            int dist = -1;
            int reason;

            if (same_map && s->pc != OBJ_HANDLE_NULL && s->npc != OBJ_HANDLE_NULL && obj_handle_is_valid(s->pc)
                && obj_handle_is_valid(s->npc)) {
                alive = !critter_is_dead(s->pc) && !critter_is_unconscious(s->pc) && !critter_is_dead(s->npc);
                if (alive) {
                    combat = combat_critter_is_combat_mode_active(s->pc) || combat_turn_based_is_active();
                    dist = (int)object_dist(s->pc, s->npc);
                }
            }
            reason = coop_pdlg_end_reason(tig_net_client_is_active(slot), alive, combat, dist, same_map);
            if (reason != COOP_PDLG_END_KEEP) {
                tig_debug_printf("CoopMid: pdlg: slot %d dialog ended by the host (%s)\n", slot,
                    coop_pdlg_reason_name(reason));
                if (reason != COOP_PDLG_END_LEFT) {
                    coop_lite_host_slot_notice(slot, COOP_PARTY_PDLG_ENDED);
                }
                coop_pdlg_end_engine(slot);
                continue;
            }
        }
        if (slot == 0 && s->open && s->npc != OBJ_HANDLE_NULL && s->pc != OBJ_HANDLE_NULL && map_is_valid()
            && !map_is_clearing_objects() && obj_handle_is_valid(s->npc) && obj_handle_is_valid(s->pc)
            && obj_field_int32_get(s->npc, OBJ_F_TYPE) == OBJ_TYPE_NPC && sub_4C1110(s->npc) != s->pc) {
            /* the host's own dialog is over (window torn down without the
               usual end call): release its watchers */
            tig_debug_printf("CoopMid: pdlg: host dialog gone - watchers released\n");
            coop_pdlg_close(0);
            continue;
        }
        if (changed) {
            coop_pdlg_republish(slot);
        } else if (s->open) {
            coop_pdlg_flush(slot);
        }
    }
    return true;
}

void coop_lite_pdlg_map_closing(void)
{
    int slot;

    if (!coop_pdlg_latched) {
        return;
    }
    coop_pdlg_ensure();
    for (slot = 1; slot < 8; slot++) {
        CoopPdlgSession* s = &coop_pdlg[slot];

        if (!s->open && !s->pending) {
            continue;
        }
        if (slot == coop_pdlg_busy_slot) {
            s->end_pending = true; /* its own answer moved the map: end from the tick */
            continue;
        }
        tig_debug_printf("CoopMid: pdlg: slot %d dialog ended by the host (map change)\n", slot);
        coop_lite_host_slot_notice(slot, COOP_PARTY_PDLG_ENDED);
        coop_pdlg_end_engine(slot);
    }
    for (slot = 1; slot < 8; slot++) {
        if ((coop_pdlg[0].spectators & (1u << slot)) != 0) {
            coop_pdlg_stop_watching(slot);
        }
    }
}

void coop_lite_pdlg_reset_all(void)
{
    int slot;

    if (!coop_pdlg_latched) {
        return;
    }
    coop_pdlg_ensure();
    for (slot = 0; slot < 8; slot++) {
        coop_pdlg_close(slot); /* dialog_ui_reset ends the engine side itself */
    }
    coop_pdlg_busy_slot = -1;
}

void coop_lite_pdlg_session_reset(void)
{
    int slot;

    coop_pdlg_ensure();
    if (coop_pdlg_latched) {
        for (slot = 1; slot < 8; slot++) {
            if (coop_pdlg[slot].open || coop_pdlg[slot].pending) {
                coop_pdlg_end_engine(slot);
            }
        }
    }
    coop_pdlg_sessions_reset(coop_pdlg);
    coop_pdlg_latched = false;
    coop_pdlg_busy_slot = -1;
    coop_pdlg_client_ui_pending = false;
}

/* ---- client ---- */

bool coop_lite_client_dialog_speaker(void)
{
    return coop_dialog_is_speaker();
}

bool coop_lite_client_dialog_private(void)
{
    return coop_dialog_view.open && (coop_dialog_view.votes[COOP_DIALOG_FLAGS_AT] & COOP_DIALOG_FLAG_PDLG) != 0;
}

/* ACT_UI from the host: our dialog closed into a window on our side. */
static void coop_pdlg_client_ui(const PacketCoopDialogState* pkt)
{
    if (coop_dialog_view.open && coop_dialog_close_hook != NULL) {
        coop_dialog_close_hook();
    }
    memset(&coop_dialog_view, 0, sizeof(coop_dialog_view));
    if (pkt->votes[COOP_DIALOG_SPEAKER_AT] != sub_529520()) {
        return;
    }
    if (pkt->votes[1] != 0) {
        memcpy(coop_party_talk_oid, pkt->npc_name, sizeof(coop_party_talk_oid));
        coop_party_exchange_open = true; /* PT80 path, from the client tick */
        return;
    }
    memcpy(coop_pdlg_client_ui_oid, pkt->npc_name, sizeof(coop_pdlg_client_ui_oid));
    coop_pdlg_client_ui_code = pkt->votes[0];
    coop_pdlg_client_ui_pending = true;
}

void coop_lite_client_pdlg_ui_tick(void)
{
    int64_t pc;
    int64_t npc;
    char name[128];

    if (!coop_pdlg_client_ui_pending) {
        return;
    }
    coop_pdlg_client_ui_pending = false;
    pc = player_get_local_pc_obj();
    npc = coop_lite_get_oid_obj(coop_pdlg_client_ui_oid);
    if (pc == OBJ_HANDLE_NULL || npc == OBJ_HANDLE_NULL || critter_is_dead(npc) || critter_is_dead(pc)) {
        tig_debug_printf("CoopMid: pdlg: window %d - NPC not found here, not opened\n", coop_pdlg_client_ui_code);
        return;
    }
    name[0] = '\0';
    object_examine(npc, pc, name);
    tig_debug_printf("CoopMid: pdlg: opening window %d with '%s'\n", coop_pdlg_client_ui_code, name);
    switch (coop_pdlg_client_ui_code) {
    case 3:
        if (coop_inven_open_hook != NULL) {
            coop_inven_open_hook(pc, npc, 1 /* INVEN_UI_MODE_BARTER */);
        }
        break;
    case 8:
        ui_show_inven_npc_identify(pc, npc);
        break;
    case 9:
        if (coop_inven_open_hook != NULL) {
            coop_inven_open_hook(pc, npc, 6 /* INVEN_UI_MODE_NPC_REPAIR */);
        }
        break;
    default:
        break;
    }
}

#endif /* !COOP_LITE_TEST_STUBS */
