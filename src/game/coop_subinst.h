#ifndef ARCANUM_GAME_COOP_SUBINST_H_
#define ARCANUM_GAME_COOP_SUBINST_H_

/*
 * Coop Middle M6: client sub-instances (Checkout -> Sim -> Commit of one
 * map_id). Pure part: the CMAP1 map archive (zlib), file name checks, the
 * lease arbiter, the client lease state machine and chunk reassembly.
 * No engine calls here (unit-tested in coop_middle_test, ASan/UBSan).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- CMAP1 archive ---- */

/* Raw (uncompressed) archive, little-endian:
 *   char magic[4] = "CMAP"   (format CMAP1: magic + the v1 layout below)
 *   u32 map_id, u32 file_count, u32 crc32 (zlib crc32 of everything after the header)
 *   file_count x { u16 name_len, char name[name_len], u32 data_len, u8 data[data_len] }
 * Wire form: u32 raw_len + compress2(raw, level 6). */
#define COOP_SUBINST_MAGIC "CMAP"
#define COOP_SUBINST_HDR 16
#define COOP_SUBINST_MAX_RAW (2 * 1024 * 1024)
#define COOP_SUBINST_MAX_WIRE (512 * 1024)
#define COOP_SUBINST_MAX_FILES 128
#define COOP_SUBINST_NAME_MAX 63
#define COOP_SUBINST_ZLIB_LEVEL 6

/* Errors (negative); 0 = ok. */
#define COOP_SUBINST_OK 0
#define COOP_SUBINST_E_ARG (-1)
#define COOP_SUBINST_E_LIMIT (-2) /* too many files / too big raw / wire */
#define COOP_SUBINST_E_NAME (-3) /* unsafe file name */
#define COOP_SUBINST_E_ZLIB (-4)
#define COOP_SUBINST_E_FORMAT (-5) /* magic / bounds / trailing bytes */
#define COOP_SUBINST_E_CRC (-6)

typedef struct CoopSubinstFile {
    const char* name; /* flat file name (see coop_subinst_filename_safe) */
    const uint8_t* data;
    uint32_t len;
} CoopSubinstFile;

/* Only flat names up to 63 chars of [A-Za-z0-9_.-] ending in .dif / .mdy
   (spec) or .md / .des (M6 deviation: the engine's own mobile.md object
   differences and mobile.des destroyed list live in the same map folder),
   case-insensitive, no leading dot, no "..", no '/', '\\', ':'. */
bool coop_subinst_filename_safe(const char* name);

/* Packs files into the wire form in out (cap out_cap). *out_len = bytes. */
int coop_subinst_pack_buf(uint32_t map_id, const CoopSubinstFile* files, int count, uint8_t* out, size_t out_cap,
    size_t* out_len);

/* Unpacks and validates the whole archive (sizes, magic, CRC, every name,
   no trailing bytes) before returning anything. raw (cap raw_cap >= raw_len)
   receives the raw archive; files[] point into raw (names are NOT NUL
   terminated in raw: name_buf receives them, COOP_SUBINST_NAME_MAX + 1 bytes
   each). */
int coop_subinst_unpack_buf(const uint8_t* wire, size_t wire_len, uint8_t* raw, size_t raw_cap, uint32_t* map_id,
    CoopSubinstFile* files, char (*name_buf)[COOP_SUBINST_NAME_MAX + 1], int max_files, int* count);

/* ---- commit / checkpoint payload: CMAP1 wire + character bundle + item quantities ---- */

/* u32 cmap_len, cmap wire, u32 bundle_len, bundle bytes, u32 qty_len, qty
   table (bundle and qty may be empty). */
#define COOP_SUBINST_MAX_BUNDLE (256 * 1024)
#define COOP_SUBINST_MAX_QTY_ITEMS 1024
#define COOP_SUBINST_QTY_ENTRY 44 /* 24-byte ObjectID + 5 x i32 (quantity, hp damage, charges, mana, inv loc) */
#define COOP_SUBINST_QTY_NONE (-1) /* quantity of an item that has none (not gold / ammo) */
#define COOP_SUBINST_ITEM_VAL_MAX 1000000
#define COOP_SUBINST_MAX_QTY (4 + COOP_SUBINST_MAX_QTY_ITEMS * COOP_SUBINST_QTY_ENTRY)
#define COOP_SUBINST_MAX_PAYLOAD (12 + COOP_SUBINST_MAX_WIRE + COOP_SUBINST_MAX_BUNDLE + COOP_SUBINST_MAX_QTY)

typedef struct CoopSubinstSection {
    const uint8_t* data;
    size_t len;
} CoopSubinstSection;

size_t coop_subinst_payload_size(size_t cmap_len, size_t bundle_len, size_t qty_len);
bool coop_subinst_payload_build(CoopSubinstSection cmap, CoopSubinstSection bundle, CoopSubinstSection qty,
    uint8_t* out, size_t out_cap, size_t* out_len);
bool coop_subinst_payload_split(const uint8_t* data, size_t len, CoopSubinstSection* cmap,
    CoopSubinstSection* bundle, CoopSubinstSection* qty);

/* Item state table (every item the PC carries): u32 count, count x { u8
   oid[24], i32 qty (gold / ammo, else -1), i32 hp_damage, i32 charges
   (OBJ_F_ITEM_SPELL_MANA_STORE), i32 mana (OBJ_F_ITEM_MANA_STORE), i32
   inv_loc (OBJ_F_ITEM_INV_LOCATION) }. */
typedef struct CoopSubinstQty {
    uint8_t oid[24];
    int32_t qty;
    int32_t hp_damage;
    int32_t charges;
    int32_t mana;
    int32_t inv_loc;
} CoopSubinstQty;

size_t coop_subinst_qty_build(const CoopSubinstQty* items, int count, uint8_t* out, size_t out_cap);
/* -1 = malformed (count over the max, size mismatch, qty < -1, hp damage /
   inv loc < 0, any value over COOP_SUBINST_ITEM_VAL_MAX). */
int coop_subinst_qty_parse(const uint8_t* data, size_t len, CoopSubinstQty* items, int max);

/* mobile.des (24-byte object ids): drops every record found in oids[n]
   in place; a trailing partial record is dropped too. Returns the new
   length. */
size_t coop_subinst_des_filter(uint8_t* data, size_t len, const uint8_t (*oids)[24], int n);

/* ---- chunks ---- */

#define COOP_SUBINST_CHUNK 4096
#define COOP_SUBINST_MAX_CHUNKS ((COOP_SUBINST_MAX_PAYLOAD + COOP_SUBINST_CHUNK - 1) / COOP_SUBINST_CHUNK)

int coop_subinst_chunk_count(size_t total_bytes);

/* In-order reassembly (the app channel is reliable + ordered). */
typedef struct CoopSubinstRx {
    uint32_t lease_id;
    int chunks; /* expected */
    int next; /* next chunk index */
    size_t total; /* expected bytes */
    size_t got;
    uint8_t* data; /* malloc'd, total bytes */
    bool active;
} CoopSubinstRx;

/* RX results */
#define COOP_SUBINST_RX_MORE 0
#define COOP_SUBINST_RX_DONE 1
#define COOP_SUBINST_RX_STALE 2 /* other lease / nothing expected: ignore */
#define COOP_SUBINST_RX_BAD (-1) /* broken stream: the transfer is dropped */

bool coop_subinst_rx_begin(CoopSubinstRx* rx, uint32_t lease_id, int chunks, size_t total_bytes, size_t max_bytes);
int coop_subinst_rx_accept(CoopSubinstRx* rx, uint32_t lease_id, int chunk_idx, int chunk_count, int data_len,
    const uint8_t* data);
void coop_subinst_rx_free(CoopSubinstRx* rx);

/* ---- lease arbiter (host) ---- */

typedef struct CoopSubinstLease {
    bool active;
    int map_id;
    uint32_t lease_id;
    unsigned int last_checkpoint_ms;
} CoopSubinstLease;

#define COOP_SUBINST_LEASE_OK 0 /* grant */
#define COOP_SUBINST_LEASE_NORMAL 1 /* not a sub-instance case: usual travel path */
#define COOP_SUBINST_LEASE_DENY_BUSY 2 /* map leased by *other_slot */
#define COOP_SUBINST_LEASE_DENY_STATE 3 /* the slot is in combat / dead */

/* shop_map: the service map (ShopMap) id, or <= 0. A slot's own lease does
   not block it (sub -> sub). */
int coop_subinst_can_lease(int host_map, int dest_map, int shop_map, int slot, const CoopSubinstLease leases[8],
    bool in_combat, bool is_dead, int* other_slot);

/* Slot leasing map_id, or -1. */
int coop_subinst_lease_owner(const CoopSubinstLease leases[8], int map_id);

/* ---- client lease state machine ---- */

typedef enum CoopSubinstState {
    COOP_SUBINST_ST_IDLE = 0,
    COOP_SUBINST_ST_REQUESTED, /* travel asked, waiting GRANT / DENY / normal path */
    COOP_SUBINST_ST_RECEIVING, /* GRANT got, map archive chunks coming */
    COOP_SUBINST_ST_ACTIVE, /* inside: local authority */
    COOP_SUBINST_ST_COMMITTING, /* final commit sent, waiting ACK (+ maybe GRANT for sub -> sub) */
} CoopSubinstState;

typedef enum CoopSubinstEvent {
    COOP_SUBINST_EV_REQUEST = 1,
    COOP_SUBINST_EV_GRANT,
    COOP_SUBINST_EV_DENY,
    COOP_SUBINST_EV_ARCHIVE_OK, /* all chunks unpacked, entered */
    COOP_SUBINST_EV_ARCHIVE_BAD,
    COOP_SUBINST_EV_COMMIT, /* client sent its final commit */
    COOP_SUBINST_EV_ACK,
    COOP_SUBINST_EV_ACK_NEXT, /* commit taken, a new lease follows (sub -> sub) */
    COOP_SUBINST_EV_ABORT,
    COOP_SUBINST_EV_RESET,
} CoopSubinstEvent;

/* Next state, or -1 when the event is not valid in this state (ignored). */
int coop_subinst_client_next(int state, int event);

/* Client: a lease that never became ACTIVE (REQUESTED / RECEIVING while the
   local authority is held) for longer than COOP_SUBINST_STALL_MS: give it
   back (CANCEL) and rejoin the party. */
#define COOP_SUBINST_STALL_MS 20000u
bool coop_subinst_client_stalled(int state, bool holding, unsigned int since_ms, unsigned int now_ms);

/* Host, a final commit asking for another map (sub -> sub). lease_rc is
   coop_subinst_can_lease for the next map, packed: its archive is ready.
   NEXT_GRANT = COMMIT_ACK(F_NEXT) then GRANT; NEXT_BACK = COMMIT_ACK(0) and
   back to the party (never an ACK(F_NEXT) without a GRANT behind it). */
#define COOP_SUBINST_NEXT_GRANT 1
#define COOP_SUBINST_NEXT_BACK 0
int coop_subinst_sub_next_plan(int lease_rc, bool packed);

/* ---- packets (TIG_NET_PROTOCOL 58, unchanged in 59) ---- */

#define PACKET_COOP_SUBINST_CTRL 171 /* both ways (op-checked) */
#define PACKET_COOP_SUBINST_CHUNK 172 /* both ways: map archive / commit payload */
#define PACKET_COOP_SUBINST_DELTA 173 /* client -> host: story / XP changes made inside */

#define COOP_SUBINST_OP_LEASE_GRANT 1 /* host -> client: map_id, lease_id, dest_loc, total_chunks / total_bytes */
#define COOP_SUBINST_OP_LEASE_DENY 2 /* host -> client: reason, slot = the slot inside (busy) */
#define COOP_SUBINST_OP_CHECKPOINT_REQ 3 /* host -> client: send a checkpoint now (host saves) */
#define COOP_SUBINST_OP_COMMIT_BEGIN 4 /* client -> host: dest_map, dest_loc, lease_id, total_chunks, total_bytes,
                                          is_checkpoint (+ hp / fatigue damage, flags) */
#define COOP_SUBINST_OP_COMMIT_ACK 5 /* host -> client: final commit applied (flags: NEXT = a new GRANT follows) */
#define COOP_SUBINST_OP_ABORT 6 /* host -> client: lease gone (host load / map change); reason */
#define COOP_SUBINST_OP_STATE 7 /* host -> all: slot is inside map_id (0 = left) - the M badge, world marker */
#define COOP_SUBINST_OP_CANCEL 8 /* client -> host: lease given back before entering (archive bad / enter failed) */
#define COOP_SUBINST_OP_MAX 8

#define COOP_SUBINST_DENY_BUSY 1
#define COOP_SUBINST_DENY_STATE 2 /* combat / dead */
#define COOP_SUBINST_DENY_TOO_BIG 3 /* unsupported: archive over the limits */
#define COOP_SUBINST_DENY_RULES 4

#define COOP_SUBINST_F_DEAD 0x1 /* COMMIT_BEGIN: the PC died inside */
#define COOP_SUBINST_F_NEXT 0x2 /* COMMIT_ACK: a new lease follows */
#define COOP_SUBINST_F_MASK 0x3

#define COOP_SUBINST_CHECKPOINT_MS 45000u
#define COOP_SUBINST_SAVE_WAIT_MS 2000u
#define COOP_SUBINST_MAX_LEASE_ID 0x7FFFFFFFu
#define COOP_SUBINST_MAX_DAMAGE 100000

typedef struct PacketCoopSubinstCtrl {
    /* 0000 */ int type; /* PACKET_COOP_SUBINST_CTRL */
    /* 0004 */ int op; /* COOP_SUBINST_OP_* */
    /* 0008 */ int slot;
    /* 000C */ int reason;
    /* 0010 */ int map_id;
    /* 0014 */ uint32_t lease_id;
    /* 0018 */ int64_t dest_loc;
    /* 0020 */ int dest_map;
    /* 0024 */ int total_chunks;
    /* 0028 */ int total_bytes;
    /* 002C */ int is_checkpoint;
    /* 0030 */ int hp_damage; /* COMMIT_BEGIN: the PC's vitals */
    /* 0034 */ int fatigue_damage;
    /* 0038 */ int flags; /* COOP_SUBINST_F_* */
    /* 003C */ int reserved;
} PacketCoopSubinstCtrl;

#define COOP_SUBINST_CHUNK_HDR 0x18
typedef struct PacketCoopSubinstChunk {
    /* 0000 */ int type; /* PACKET_COOP_SUBINST_CHUNK */
    /* 0004 */ uint32_t lease_id;
    /* 0008 */ int chunk_idx;
    /* 000C */ int chunk_count;
    /* 0010 */ int data_len; /* 1..4096; the packet is header + data_len bytes */
    /* 0014 */ int flags; /* 0 */
    /* 0018 */ uint8_t data[COOP_SUBINST_CHUNK];
} PacketCoopSubinstChunk;

#define COOP_SUBINST_DELTA_GF 1 /* index = global flag, val 0 / 1 */
#define COOP_SUBINST_DELTA_GV 2 /* index = global var, val */
#define COOP_SUBINST_DELTA_QUEST 3 /* index = quest num, val = state */
#define COOP_SUBINST_DELTA_RUMOR 4 /* index = rumor, val 0 = known, 1 = quelled */
#define COOP_SUBINST_DELTA_REP 5 /* index = reputation, val +1 add / -1 remove */
#define COOP_SUBINST_DELTA_AREA 6 /* index = area made known */
#define COOP_SUBINST_DELTA_XP 7 /* val = raw XP the PC earned */
#define COOP_SUBINST_DELTA_FATE 8 /* index = fate, val 1 activate / 0 resolve */
#define COOP_SUBINST_DELTA_MAX 8
#define COOP_SUBINST_GF_MAX 3200 /* 100 flag words */
#define COOP_SUBINST_GV_MAX 2000
#define COOP_SUBINST_XP_MAX 1000000
#define COOP_SUBINST_FATE_MAX 16

typedef struct PacketCoopSubinstDelta {
    /* 0000 */ int type; /* PACKET_COOP_SUBINST_DELTA */
    /* 0004 */ uint32_t lease_id;
    /* 0008 */ int kind;
    /* 000C */ int index;
    /* 0010 */ int val;
    /* 0014 */ int reserved;
} PacketCoopSubinstDelta;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(PacketCoopSubinstCtrl) == 0x40, "PacketCoopSubinstCtrl size");
_Static_assert(offsetof(PacketCoopSubinstCtrl, dest_loc) == 0x18, "PacketCoopSubinstCtrl.dest_loc");
_Static_assert(offsetof(PacketCoopSubinstCtrl, flags) == 0x38, "PacketCoopSubinstCtrl.flags");
_Static_assert(offsetof(PacketCoopSubinstChunk, data) == COOP_SUBINST_CHUNK_HDR, "PacketCoopSubinstChunk.data");
_Static_assert(sizeof(PacketCoopSubinstChunk) == 0x1018, "PacketCoopSubinstChunk size");
_Static_assert(sizeof(PacketCoopSubinstDelta) == 0x18, "PacketCoopSubinstDelta size");
#endif

/* from_host: sent by the host (GRANT / DENY / CHECKPOINT_REQ / ACK / ABORT /
   STATE), else by a client (COMMIT_BEGIN / CANCEL). */
bool coop_subinst_ctrl_valid(const PacketCoopSubinstCtrl* pkt, int size, bool from_host);
bool coop_subinst_chunk_valid(const PacketCoopSubinstChunk* pkt, int size);
bool coop_subinst_delta_valid(const PacketCoopSubinstDelta* pkt, int size);

/* ---- game side (coop_subinst.c, game build only) ---- */
#ifndef COOP_LITE_TEST_STUBS
struct TeleportData;
bool coop_subinst_client_active(void); /* inside a solo instance: local authority */
bool coop_subinst_slot_inside(int slot); /* host + clients: slot is in a solo instance (M badge) */
int coop_subinst_slot_map(int slot); /* map_id or 0 */
void coop_subinst_ping(unsigned int now_ms); /* multiplayer_ping while inside: pump + checkpoints */
bool coop_subinst_packet_filter(int type, const void* msg); /* client inside: false = drop host packet */
void coop_subinst_handle_ctrl(PacketCoopSubinstCtrl* pkt);
void coop_subinst_handle_chunk(PacketCoopSubinstChunk* pkt);
void coop_subinst_handle_delta(PacketCoopSubinstDelta* pkt);
/* Host: a client's trip to another map. True = handled (granted / denied). */
bool coop_subinst_host_travel(int slot, int map, int64_t loc);
/* Client inside: a local map change = commit. True = handled. */
bool coop_subinst_client_travel(const struct TeleportData* td);
void coop_subinst_note(int kind, int index, int val); /* client inside: a delta for the host */
void coop_subinst_note_xp(int64_t obj, int xp);
bool coop_subinst_client_death(void); /* the local PC died inside: true = no game over */
void coop_subinst_host_before_save(void);
void coop_subinst_host_abort_all(const char* why); /* host party travel / load */
void coop_subinst_map_changed(void);
void coop_subinst_slot_gone(int slot);
void coop_subinst_session_reset(void);
void coop_subinst_tick(unsigned int now_ms); /* host: resync after an abort */
bool coop_subinst_slot_entry(int slot, int* map, int64_t* loc); /* where the slot went in (world marker) */
unsigned int coop_subinst_serial(void); /* changes when who-is-inside changes */
uint8_t coop_subinst_host_lease_mask(void); /* M7 host: slots holding a lease */
bool coop_subinst_host_commit_busy(void); /* M7 host: a COMMIT / checkpoint is still arriving */

/* coop_lite.c helpers (static state lives there). */
struct CoopCharBundleBuf;
bool coop_lite_build_pc_bundle(int64_t pc, struct CoopCharBundleBuf* out);
void coop_lite_pc_inv_repair(int64_t pc); /* PT54d sparse inventory list repair */
bool coop_lite_client_subinst_teleport(int map, int64_t loc);
bool coop_lite_host_resync_slot(int slot);
bool coop_lite_host_pc_reconcile(int64_t pc, const uint8_t* bundle, size_t bundle_len, const CoopSubinstQty* qty,
    int qty_n, int hp_damage, int fatigue_damage);
#else
static inline bool coop_subinst_client_active(void) { return false; }
static inline bool coop_subinst_slot_inside(int slot) { (void)slot; return false; }
static inline uint8_t coop_subinst_host_lease_mask(void) { return 0; }
static inline bool coop_subinst_host_commit_busy(void) { return false; }
#endif

#ifdef __cplusplus
}
#endif

#endif /* ARCANUM_GAME_COOP_SUBINST_H_ */
