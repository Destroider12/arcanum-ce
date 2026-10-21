#ifndef ARCANUM_GAME_COOP_LITE_H_
#define ARCANUM_GAME_COOP_LITE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Packet type for host→client forced follow teleport (within 1..130). */
#define PACKET_COOP_ATTACK_INTENT 128
#define PACKET_COOP_FOLLOW_HOST 129
#define PACKET_COOP_CHAR_OFFER 130
#define PACKET_COOP_SESSION_SYNC 131
#define PACKET_COOP_PC_POS 132
#define PACKET_COOP_FOLLOWERS 133 /* host → clients: who follows which slot PC */
#define PACKET_COOP_NPC_POS 134 /* host → clients: NPC pose batch near PCs */
#define PACKET_COOP_PARTY_STATE 135 /* host → clients: death / wipe / reload notices */
#define PACKET_COOP_BARTER 136 /* client → host: open/close barter with a merchant */
#define PACKET_COOP_INVEN_SYNC 137 /* host → client: authoritative inventory snapshot */
#define PACKET_COOP_ROSTER 138 /* host → clients: another client's PC joined / left */

#define PACKET_COOP_OBJ_REQ 139 /* client → host: full state of these items, please */
#define PACKET_COOP_OBJ_BLOB 140 /* host → client: one item as an object blob */

#define PACKET_COOP_CHAR_BUNDLE 141 /* client → host: own character bundle, chunked */
#define PACKET_COOP_WORLD_REQ 142 /* client → host: world state around me, please */
#define PACKET_COOP_WORLD_STATE 143 /* host → client: doors, containers, NPCs, ground items */

/* multiplayer_validate_message must allow up to this (exclusive). */
#define PACKET_COOP_TYPE_LIMIT 144

/* PC position broadcast interval (ms). */
#define COOP_LITE_PC_POS_INTERVAL_MS 50u

/* PacketCoopPcPos.flags */
#define COOP_PC_POS_FORCE 0x1 /* host moved an owner-driven PC; owner must apply */

/* Upper bound for a plausible basic prototype number on the wire. */
#define COOP_LITE_MAX_PROTO 100000


typedef struct PacketCoopAttackIntent {
    /* 0000 */ int type;
    /* 0004 */ int attack_kind; /* 0 = default melee/ranged AG_ATTACK */
    /* 0008 */ unsigned char attacker_oid[24]; /* ObjectID */
    /* 0020 */ unsigned char target_oid[24]; /* ObjectID */
} PacketCoopAttackIntent;

typedef struct PacketCoopFollowHost {
    /* 0000 */ int type;
    /* 0004 */ int map;
    /* 0008 */ int64_t loc;
    /* 0010 */ int map_gen; /* P0-2: host map load generation */
    /* 0014 */ int reserved;
} PacketCoopFollowHost;

/* Client → host: character chosen in menu (do not start an independent world). */
typedef struct PacketCoopCharOffer {
    /* 0000 */ int type;
    /* 0004 */ int slot;
    /* 0008 */ int basic_prototype;
    /* 000C */ char name[64];
    /* 004C */ int has_oid;
    /* 0050 */ unsigned char pc_oid[24]; /* client's own PC ObjectID — host adopts it */
    /* 0068 */ unsigned char token[16]; /* rejoin token (Players/coop_token.txt); all zero = none */
} PacketCoopCharOffer;

/*
 * PC position/art (rate limited, only on change).
 *  client → host: the client's own PC (client drives its own movement).
 *  host → clients: every slot PC; a slot's own PC is not echoed back to its
 *  owner unless the host moved it (COOP_PC_POS_FORCE).
 */
typedef struct PacketCoopPcPos {
    /* 0000 */ int type;
    /* 0004 */ int slot;
    /* 0008 */ unsigned char oid[24];
    /* 0020 */ int64_t loc;
    /* 0028 */ int offset_x;
    /* 002C */ int offset_y;
    /* 0030 */ unsigned int art_id;
    /* 0034 */ int flags;
    /* 0038 */ int map; /* coop_lite_pose_map_key: map index + load generation */
    /* 003C */ uint16_t epoch; /* P0-2: bumped by the host on every correction */
    /* 003E */ uint16_t seq; /* P0-2: per-sender order inside an epoch */
} PacketCoopPcPos;

/* Host → clients: follower/leader table (variable length: count entries). */
#define COOP_FOLLOWERS_MAX 16
typedef struct PacketCoopFollowerEntry {
    /* 0000 */ unsigned char npc_oid[24];
    /* 0018 */ int leader_slot;
    /* 001C */ int reserved;
} PacketCoopFollowerEntry;

typedef struct PacketCoopFollowers {
    /* 0000 */ int type;
    /* 0004 */ int count;
    /* 0008 */ PacketCoopFollowerEntry entries[COOP_FOLLOWERS_MAX];
} PacketCoopFollowers;

/* Host → clients: NPC poses (variable length: count entries). */
#define COOP_NPC_POS_MAX 16 /* 8 + 16 * 0x38 = 904 B: one unfragmented datagram */
#define COOP_NPC_POS_DEAD 0x1
typedef struct PacketCoopNpcPosEntry {
    /* 0000 */ unsigned char oid[24];
    /* 0018 */ int64_t loc;
    /* 0020 */ int offset_x;
    /* 0024 */ int offset_y;
    /* 0028 */ unsigned int art_id;
    /* 002C */ unsigned int flags;
    /* 0030 */ int hp_damage;
    /* 0034 */ int reserved;
} PacketCoopNpcPosEntry;

typedef struct PacketCoopNpcPos {
    /* 0000 */ int type;
    /* 0004 */ int count;
    /* 0008 */ PacketCoopNpcPosEntry entries[COOP_NPC_POS_MAX];
} PacketCoopNpcPos;

/* Bytes needed for `count` entries of `entry_size` after an 8-byte header;
   0 when count is out of range. */
size_t coop_lite_batch_size(int count, int max_count, size_t entry_size);
/* Received batch is consistent with the received byte size. */
bool coop_lite_batch_valid(int count, int max_count, size_t entry_size, int msg_size);

/*
 * Host → client: join the host's session. Client loads map, creates both PCs
 * with matching OIDs (host PC as NETWORK proxy, local PC as player_pc_obj).
 */
typedef struct PacketCoopSessionSync {
    /* 0000 */ int type;
    /* 0004 */ int map;
    /* 0008 */ int64_t host_loc;
    /* 0010 */ int64_t client_loc;
    /* 0018 */ unsigned char host_pc_oid[24];
    /* 0030 */ unsigned char client_pc_oid[24];
    /* 0048 */ int client_slot;
    /* 004C */ int client_proto;
    /* 0050 */ int host_proto;
    /* 0054 */ char client_name[64];
    /* 0094 */ char host_name[64];
    /* 00D4 */ int map_gen; /* P0-2: host map load generation (was padding) */
    /* 00D8 */ int pose_epoch; /* P0-2: the joining slot's pose epoch */
    /* 00DC */ int flags; /* COOP_SYNC_F_* */
} PacketCoopSessionSync;

/* The host gave this client back its parked PC (client_pc_oid): replace
   the local PC with the host's copy (requested as an object blob). */
#define COOP_SYNC_F_REJOINED 0x1

/* Wire layout must match across compilers (MSVC vs gcc/clang). */
#if defined(__cplusplus)
#define COOP_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define COOP_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
COOP_STATIC_ASSERT(sizeof(PacketCoopAttackIntent) == 0x38, "PacketCoopAttackIntent size");
COOP_STATIC_ASSERT(offsetof(PacketCoopAttackIntent, target_oid) == 0x20, "PacketCoopAttackIntent layout");
COOP_STATIC_ASSERT(sizeof(PacketCoopFollowHost) == 0x18, "PacketCoopFollowHost size");
COOP_STATIC_ASSERT(offsetof(PacketCoopFollowHost, loc) == 0x08, "PacketCoopFollowHost layout");
COOP_STATIC_ASSERT(offsetof(PacketCoopCharOffer, has_oid) == 0x4C, "PacketCoopCharOffer layout");
COOP_STATIC_ASSERT(offsetof(PacketCoopCharOffer, pc_oid) == 0x50, "PacketCoopCharOffer layout");
COOP_STATIC_ASSERT(sizeof(PacketCoopCharOffer) == 0x78, "PacketCoopCharOffer size");
COOP_STATIC_ASSERT(sizeof(PacketCoopPcPos) == 0x40, "PacketCoopPcPos size");
COOP_STATIC_ASSERT(offsetof(PacketCoopPcPos, map) == 0x38, "PacketCoopPcPos layout");
COOP_STATIC_ASSERT(offsetof(PacketCoopPcPos, loc) == 0x20, "PacketCoopPcPos layout");
COOP_STATIC_ASSERT(offsetof(PacketCoopPcPos, flags) == 0x34, "PacketCoopPcPos layout");
COOP_STATIC_ASSERT(sizeof(PacketCoopSessionSync) == 0xE0, "PacketCoopSessionSync size");
COOP_STATIC_ASSERT(sizeof(PacketCoopFollowerEntry) == 0x20, "PacketCoopFollowerEntry size");
COOP_STATIC_ASSERT(offsetof(PacketCoopFollowers, entries) == 0x08, "PacketCoopFollowers layout");
COOP_STATIC_ASSERT(sizeof(PacketCoopNpcPosEntry) == 0x38, "PacketCoopNpcPosEntry size");
COOP_STATIC_ASSERT(offsetof(PacketCoopNpcPos, entries) == 0x08, "PacketCoopNpcPos layout");

typedef struct PacketCoopPartyState {
    /* 0000 */ int type;
    /* 0004 */ int kind;
    /* 0008 */ int slot;
    /* 000C */ unsigned int countdown_ms;
    /* 0010 */ unsigned int dead_mask;
    /* 0014 */ int reserved;
} PacketCoopPartyState;

/*
 * PACKET_COOP_ROSTER (review H3): with 3+ players a client only knew its own
 * PC and the host's. The host announces every other client's PC (and its
 * departure) so each client creates, binds and keeps it like the host's.
 */
typedef struct PacketCoopRoster {
    /* 0000 */ int type;
    /* 0004 */ int slot;
    /* 0008 */ uint8_t oid[24];
    /* 0020 */ int proto;
    /* 0024 */ int present;
    /* 0028 */ int64_t loc;
    /* 0030 */ char name[64];
} PacketCoopRoster;

COOP_STATIC_ASSERT(sizeof(PacketCoopRoster) == 0x70, "PacketCoopRoster size");

/* Client-side acceptance of a roster entry: a real other-client slot. */
bool coop_lite_roster_slot_ok(int slot, int local_slot);

/* PACKET_COOP_BARTER kinds. */
#define COOP_BARTER_OPEN 0
#define COOP_BARTER_CLOSE 1
#define COOP_BARTER_SYNC 2 /* just send me my inventory (after joining) */

typedef struct PacketCoopBarter {
    /* 0000 */ int type;
    /* 0004 */ int kind;
    /* 0008 */ uint8_t pc_oid[24];
    /* 0020 */ uint8_t npc_oid[24];
} PacketCoopBarter;

/*
 * PACKET_COOP_INVEN_SYNC: one owner's inventory as the host has it, split in
 * chunks. A batch (several owners: the client PC, the merchant, its store
 * container) starts with COOP_INVEN_BATCH_FIRST and is applied as a whole on
 * COOP_INVEN_BATCH_LAST, so an item moving between two owners is never lost.
 * Gold travels as a total; ammo as a per-proto total (stacks merge, so their
 * OIDs are not stable); everything else by OID.
 */
#define COOP_INVEN_BATCH_FIRST 0x1
#define COOP_INVEN_BATCH_LAST 0x2
#define COOP_INVEN_CHUNK_ENTRIES 64 /* 0xB0 + 64 * 0x38 <= TIG_NET_MAX_PACKET */
#define COOP_INVEN_AMMO_TYPES 4

/* Review P1-3: an item the client already has is brought up to date too
   (durability, charges, keys), not only moved. */
#define COOP_INVEN_E_STATE 0x1 /* hp_damage / mana fields are valid */

typedef struct PacketCoopInvenEntry {
    /* 0000 */ uint8_t oid[24];
    /* 0018 */ int proto;
    /* 001C */ int inventory_location;
    /* 0020 */ unsigned int keyring_hash; /* key ids on a key ring, else 0 */
    /* 0024 */ unsigned int item_flags;
    /* 0028 */ int hp_damage;
    /* 002C */ int mana_store;
    /* 0030 */ int spell_mana_store;
    /* 0034 */ unsigned int state_flags; /* COOP_INVEN_E_* */
} PacketCoopInvenEntry;

typedef struct PacketCoopInvenSync {
    /* 0000 */ int type;
    /* 0004 */ int flags;
    /* 0008 */ uint8_t owner_oid[24];
    /* 0020 */ uint8_t gold_oid[24]; /* host's gold stack (a host-created one
                                        reaches clients as a stray Packet70) */
    /* 0038 */ uint8_t ammo_oid[COOP_INVEN_AMMO_TYPES][24];
    /* 0098 */ int gold;
    /* 009C */ int ammo[COOP_INVEN_AMMO_TYPES];
    /* 00AC */ int count;
    /* 00B0 */ PacketCoopInvenEntry entries[COOP_INVEN_CHUNK_ENTRIES];
} PacketCoopInvenSync;

COOP_STATIC_ASSERT(sizeof(PacketCoopBarter) == 0x38, "PacketCoopBarter size");
COOP_STATIC_ASSERT(sizeof(PacketCoopInvenEntry) == 0x38, "PacketCoopInvenEntry size");
COOP_STATIC_ASSERT(0xB0 + COOP_INVEN_CHUNK_ENTRIES * 0x38 <= 4096, "INVEN_SYNC chunk fits a packet");
/* FNV-1a over a key ring's key ids (order matters, as on the host). */
unsigned int coop_lite_keyring_hash(const unsigned int* keys, int count);

/*
 * Review P1-4: bounded walk of a blob's serialized fields (kinds are
 * OBJ_MEM_K_*, elem the expected array element size). Returns the bytes
 * consumed, -1 when a length runs past size or out of range. Pure.
 */
#ifndef OBJ_MEM_K_I32
#define OBJ_MEM_K_I32 1
#define OBJ_MEM_K_I64 2 /* presence byte [+ 8] */
#define OBJ_MEM_K_HANDLE 3 /* presence byte [+ 24] */
#define OBJ_MEM_K_STRING 4 /* presence byte [+ int len + len + 1] */
#define OBJ_MEM_K_ARRAY 5 /* presence byte [+ sa header + data + bitset] */
#endif
#define COOP_BLOB_STR_MAX 4096
#define COOP_BLOB_SA_MAX_BYTES 16384
#define COOP_BLOB_BITSET_MAX 512
int coop_lite_blob_walk(const uint8_t* p, int size, const uint8_t* kinds, const int* elem, int n);
COOP_STATIC_ASSERT(offsetof(PacketCoopInvenSync, entries) == 0xB0, "PacketCoopInvenSync layout");
COOP_STATIC_ASSERT(sizeof(PacketCoopInvenSync) <= 4096, "PacketCoopInvenSync fits a net packet");

/* Bytes a PACKET_COOP_INVEN_SYNC with `count` entries occupies on the wire. */
size_t coop_lite_inven_sync_size(int count);
/* Wire-level sanity of a received INVEN_SYNC of `size` bytes. */
bool coop_lite_inven_sync_valid(const PacketCoopInvenSync* pkt, int size);

/*
 * P1-2 item fidelity. A client that lacks a listed item asks the host for
 * it (PACKET_COOP_OBJ_REQ) instead of creating a bare proto copy; the host
 * answers with the sub_442050 blob (HP, charges, flags, names, scripts),
 * which the client loads with obj_read_mem + obj_load_postprocess. Proto
 * creation stays as the fallback if no blob arrives in time.
 */
#define COOP_OBJ_REQ_MAX 16
#define COOP_OBJ_BLOB_MAX 8192

typedef struct PacketCoopObjReq {
    /* 0000 */ int type;
    /* 0004 */ int count;
    /* 0008 */ uint8_t oid[COOP_OBJ_REQ_MAX][24];
} PacketCoopObjReq;

COOP_STATIC_ASSERT(sizeof(PacketCoopObjReq) == 0x188, "PacketCoopObjReq size");

typedef struct PacketCoopObjBlob {
    /* 0000 */ int type;
    /* 0004 */ int size;
    /* 0008 */ uint8_t oid[24];
    /* 0020 */ uint8_t owner_oid[24];
    /* 0038 */ int inventory_location;
    /* 003C */ unsigned int flags; /* COOP_OBJ_BLOB_F_* */
    /* 0040 */ uint8_t data[COOP_OBJ_BLOB_MAX];
} PacketCoopObjBlob;

COOP_STATIC_ASSERT(offsetof(PacketCoopObjBlob, data) == 0x40, "PacketCoopObjBlob layout");

/* "Not available" (picked up, destroyed, or not yours): size 0, no data.
   The client forgets the request and does not fall back to the proto. */
#define COOP_OBJ_BLOB_F_GONE 0x1

/* Structure check before obj_read_mem: sizes, and the instance OID inside
   the blob ([u32 version][proto oid][oid]...) is the one announced. */
bool coop_lite_obj_req_valid(const PacketCoopObjReq* pkt, int size);

/*
 * v2 character offer: before CHAR_OFFER the client streams its character
 * bundle (PC + items, sub_442050 blobs) in order; the host builds the PC
 * from it (custom characters carry over) after checking it, and falls back
 * to the proto + name offer when it is missing or refused.
 */
#define COOP_CHAR_CHUNK 1024
#define COOP_CHAR_WIRE_MAX (64 * 1024)
#define COOP_CHAR_MAX_ITEMS 64

typedef struct PacketCoopCharChunk {
    /* 0000 */ int type;
    /* 0004 */ int total;
    /* 0008 */ int offset;
    /* 000C */ int len;
    /* 0010 */ uint8_t data[COOP_CHAR_CHUNK];
} PacketCoopCharChunk;

COOP_STATIC_ASSERT(offsetof(PacketCoopCharChunk, data) == 0x10, "PacketCoopCharChunk layout");

typedef struct CoopCharRx {
    uint8_t* data;
    int total;
    int got;
    bool complete;
} CoopCharRx;

/* In-order reassembly; offset 0 starts over. false: chunk refused (the
   transfer is dropped). */
bool coop_char_rx_accept(CoopCharRx* rx, const PacketCoopCharChunk* pkt, int size);
void coop_char_rx_free(CoopCharRx* rx);
/* Character sanity (anti-cheat-lite for friends' co-op). */
bool coop_lite_char_plausible(int level, int xp, const int* base_stats, int num_stats,
    int gold, int num_items);
void coop_lite_handle_char_chunk(PacketCoopCharChunk* pkt);

/*
 * §7 subset: world state on join and after every map load. The client asks
 * (WORLD_REQ with its map key); the host answers with the state of what is
 * around that client's PC: door/window and container state (art frame,
 * open flags, lock flags), container contents (INVEN_SYNC batches), NPC
 * death/off state, and every ground item (unknown ones come as blobs). The
 * last chunk ends the list; the client then removes ground items near it
 * that the host does not have (picked up, destroyed).
 */
#define COOP_WORLD_RADIUS 24 /* tiles around the client PC (host side) */
#define COOP_WORLD_PRUNE_RADIUS 18 /* client prunes ground items within this */
#define COOP_WORLD_CHUNK 64
#define COOP_WORLD_F_FIRST 0x1
#define COOP_WORLD_F_LAST 0x2

typedef enum CoopWorldKind {
    COOP_WORLD_PORTAL = 1,
    COOP_WORLD_CONTAINER = 2,
    COOP_WORLD_NPC = 3,
    COOP_WORLD_GROUND_ITEM = 4,
} CoopWorldKind;

/* Object flags the snapshot may set or clear, per kind. */
#define COOP_WORLD_PORTAL_OF_MASK (OF_SEE_THROUGH_BIT | OF_SHOOT_THROUGH_BIT | OF_NO_BLOCK_BIT)
#define OF_SEE_THROUGH_BIT 0x00000010u
#define OF_SHOOT_THROUGH_BIT 0x00000020u
#define OF_NO_BLOCK_BIT 0x00000400u
#define OF_OFF_BIT 0x00000002u
#define OF_FLAT_BIT 0x00000004u
#define COOP_WORLD_NPC_OF_MASK (OF_OFF_BIT | OF_FLAT_BIT | OF_NO_BLOCK_BIT)

typedef struct PacketCoopWorldReq {
    /* 0000 */ int type;
    /* 0004 */ int map; /* coop_lite_pose_map_key */
} PacketCoopWorldReq;

typedef struct CoopWorldEntry {
    /* 0000 */ uint8_t oid[24];
    /* 0018 */ int kind; /* CoopWorldKind */
    /* 001C */ unsigned int aid; /* current art (door frame, corpse pose) */
    /* 0020 */ unsigned int of_flags; /* OBJ_F_FLAGS (masked per kind) */
    /* 0024 */ unsigned int kind_flags; /* portal/container flags */
    /* 0028 */ int hp_damage; /* NPC */
    /* 002C */ int proto; /* ground item */
    /* 0030 */ int64_t loc;
    /* 0038 */ uint8_t reserved[8];
} CoopWorldEntry;

typedef struct PacketCoopWorldState {
    /* 0000 */ int type;
    /* 0004 */ int map;
    /* 0008 */ unsigned int flags; /* COOP_WORLD_F_* */
    /* 000C */ int count;
    /* 0010 */ CoopWorldEntry entries[COOP_WORLD_CHUNK];
} PacketCoopWorldState;

COOP_STATIC_ASSERT(sizeof(CoopWorldEntry) == 0x40, "CoopWorldEntry size");
COOP_STATIC_ASSERT(offsetof(PacketCoopWorldState, entries) == 0x10, "PacketCoopWorldState layout");

bool coop_lite_world_state_valid(const PacketCoopWorldState* pkt, int size);
int coop_lite_world_state_size(int count);
/* Client: ask again after a map load (key change, once settled) and every
   30 s for sectors loaded since. */
bool coop_lite_world_req_due(int last_key, int key, unsigned int ready_ms,
    unsigned int last_req_ms, unsigned int now_ms);

/* Set of OIDs listed by the snapshot (client prune). */
#define COOP_OID_SET_MAX 1024
typedef struct CoopOidSet {
    int count;
    bool overflow;
    uint8_t oid[COOP_OID_SET_MAX][24];
} CoopOidSet;
void coop_oid_set_reset(CoopOidSet* set);
void coop_oid_set_add(CoopOidSet* set, const uint8_t oid[24]);
bool coop_oid_set_has(const CoopOidSet* set, const uint8_t oid[24]);

void coop_lite_handle_world_req(PacketCoopWorldReq* pkt);
void coop_lite_handle_world_state(PacketCoopWorldState* pkt);
bool coop_lite_obj_blob_valid(const PacketCoopObjBlob* pkt, int size);
void coop_lite_handle_obj_req(PacketCoopObjReq* pkt);
void coop_lite_handle_obj_blob(PacketCoopObjBlob* pkt);
/* Client, Packet70 of a ground item: true = asked the host for the full
   object (created from the proto only if no blob comes in time). */
bool coop_lite_client_want_ground_blob(const unsigned char oid[24], int64_t loc, int proto);
/*
 * Which barter/inventory transfers a client may ask the host to perform
 * (Packet81 flags): plain moves between its PC and the merchant it is
 * bartering with. Drop (0x02), take-to-cursor/use (0x04), gamble (0x08) and
 * steal (0x10) are refused.
 */
bool coop_lite_client_transfer_flags_allowed(unsigned int flags);

COOP_STATIC_ASSERT(sizeof(PacketCoopPartyState) == 0x18, "PacketCoopPartyState size");
COOP_STATIC_ASSERT(offsetof(PacketCoopSessionSync, host_loc) == 0x08, "PacketCoopSessionSync layout");
COOP_STATIC_ASSERT(offsetof(PacketCoopSessionSync, client_slot) == 0x48, "PacketCoopSessionSync layout");
COOP_STATIC_ASSERT(offsetof(PacketCoopSessionSync, host_name) == 0x94, "PacketCoopSessionSync layout");

typedef enum CoopTalkPolicy {
    COOP_TALK_ALLOW = 0,
    COOP_TALK_INFO_ONLY = 1,
    COOP_TALK_BLOCK = 2,
    /* Coop client, non-merchant: no dialog window, a short bark instead. */
    COOP_TALK_BARK = 3,
    /* Coop client, another player's follower: "talk to the leader" bark. */
    COOP_TALK_BARK_FOLLOWER = 4,
} CoopTalkPolicy;

/* True when world map may be opened/used: always in SP; host-only in coop. */
bool coop_lite_can_use_worldmap(void);

/* True when the local peer may drive game-time (SP or coop host). */
bool coop_lite_can_advance_game_time(void);

/* True when the local peer may write the campaign/world save (SP or host). */
bool coop_lite_can_save_world(void);

/*
 * Dialog gate for coop companions. Host / SP always ALLOW.
 * Clients always ALLOW entry; response nodes that change quest/story/gvars/
 * reputation are filtered via coop_lite_dialog_option_allowed.
 */
CoopTalkPolicy coop_lite_can_talk(int64_t npc_obj, int64_t pc_obj);

/*
 * Pure classifier used by coop_lite_can_talk and unit tests.
 * Clients always ALLOW (per-node filter elsewhere). INFO_ONLY unused.
 */
CoopTalkPolicy coop_lite_classify_talk(bool coop_active, bool is_host,
    bool is_merchant, bool dialog_has_story_effects);
/*
 * Client talk rule (PT8 design): clients only open dialog with merchants
 * (barter + neutral lines; story options still filtered per node); another
 * player's follower and every other NPC answer with a bark.
 */
CoopTalkPolicy coop_lite_classify_client_talk(bool is_merchant, bool follows_other_player);
/* Fallback bark text when the NPC has no generic line. */
const char* coop_lite_bark_text(CoopTalkPolicy policy);

/* Follower table handlers. */
void coop_lite_handle_followers(PacketCoopFollowers* pkt);
void coop_lite_handle_npc_pos(PacketCoopNpcPos* pkt);
void coop_lite_handle_party_state(PacketCoopPartyState* pkt);
void coop_lite_handle_barter(PacketCoopBarter* pkt);
void coop_lite_handle_roster(PacketCoopRoster* pkt);
/* Client: the host connection dropped (review M1). */
void coop_lite_client_host_left(void);
void coop_lite_handle_inven_sync(PacketCoopInvenSync* pkt);
/* Host: a client's Packet81 inventory transfer (barter). */
struct Packet81;
void coop_lite_host_handle_inven_transfer(struct Packet81* pkt);
/* P1-3: host side of the client item ops 26 drop, 27 drop-at, 28 transfer,
   125 identify (owner, reach, container and merchant checks). */
void coop_lite_host_handle_item_op(int type, void* msg);
/* Client: ask the host to open a barter session (it answers with a sync). */
void coop_lite_client_request_barter(int64_t pc_obj, int64_t npc_obj, int kind);
/* Client: open barter / own inventory locally (vanilla MP round-trip is
   broken: the host's phase-1 reply is ignored by clients). */
bool coop_lite_client_opens_inven_locally(int mode, int64_t pc_obj);
/* inven_ui_open, for the headless barter dev hook (game code can't call UI). */
/* dialog_ui registers dialog_ui_end_dialog (game/ cannot include ui/). */
void coop_lite_set_dialog_end_hook(void (*hook)(int64_t pc_obj, int a2));
/* UI side of "the host left": reset the game and show the main menu. */
void coop_lite_set_return_to_menu_hook(void (*hook)(void));
/* Pure: should the deferred return-to-menu fire now? */
bool coop_lite_host_left_menu_due(bool armed, unsigned int armed_ms, unsigned int now_ms);

/*
 * Parked PCs (rejoin). A leaving client's PC is switched OF_OFF instead of
 * destroyed and remembered under a hash of the client's rejoin token; a
 * CHAR_OFFER with the same token gets that PC back. The token itself is
 * never logged and never leaves the host's memory as anything but a hash.
 */
#define COOP_PARKED_MAX 16
uint64_t coop_lite_token_hash(const unsigned char token[16]); /* 0: no token */
void coop_lite_parked_reset(void);
bool coop_lite_parked_put(uint64_t hash, const unsigned char oid[24]);
/* Removes and returns the entry. */
bool coop_lite_parked_take(uint64_t hash, unsigned char oid_out[24]);
int coop_lite_parked_count(void);
bool coop_lite_parked_has_oid(const unsigned char oid[24]);

/*
 * Parked PCs in the host save: an optional block after the multiplayer
 * section ('CPK1', count, {u64 token hash, 24-byte OID}...). Older saves have
 * none; the loader peeks and rewinds. Players connected at save time are
 * stored too, so after a load everyone gets their own PC back by token.
 */
#define COOP_PARKED_SAVE_MAGIC 0x314B5043u
#define COOP_PARKED_SAVE_ENTRY 32
#define COOP_PARKED_SAVE_MAX (8 + COOP_PARKED_MAX * COOP_PARKED_SAVE_ENTRY)
/* Live table plus extra (hash, oid) pairs, without changing the table.
   Returns bytes written, -1 if cap is too small. */
int coop_lite_parked_serialize(uint8_t* out, int cap, const uint64_t* extra_hash,
    const unsigned char (*extra_oid)[24], int num_extra);
/* Replaces the table; false (table cleared) on a malformed block. */
bool coop_lite_parked_deserialize(const uint8_t* in, int len);

struct TigFile;
bool coop_lite_save_appendix(struct TigFile* stream);
bool coop_lite_load_appendix(struct TigFile* stream);
/* hotkey_ui_assign_spell (dev: Resurrect on the hotbar). */
void coop_lite_set_hotkey_spell_hook(int (*hook)(int spell));
void coop_lite_set_inven_open_hook(bool (*hook)(int64_t pc_obj, int64_t target_obj, int mode));

/* Teleport hooks (teleport.c): extra PCs to carry with the local PC, and
   placement once the teleport is done. */
int coop_lite_party_carry_list(int64_t mover, int64_t* objs, int max);
void coop_lite_after_party_carry(int64_t mover, int64_t dest_loc);

/* Host: a party-wipe reload is due. Copies the save name; main loop loads it
   outside of gamelib_ping. */
bool coop_lite_take_reload_request(char* name, size_t size);
/* Host: the reload could not be started (bad save). */
void coop_lite_reload_failed(void);
/* ARCANUM_DEV_COOP_SAVE=1: host saves Slot9990 once a client is in (dev). */
bool coop_lite_take_dev_save_request(void);

/* Client camera: true when the target is off the visible area (recentre). */
bool coop_lite_follow_needs_jump(int64_t dx, int64_t dy, int64_t half_w, int64_t half_h);

/* Client handlers apply host world packets only when this holds. */
bool coop_lite_world_ready_rule(bool in_session, bool sync_pending, bool waiting_sync,
    bool map_valid, bool map_clearing, bool has_local_pc);

/* Shrouded Hills town square (gamearea.mes). */
#define COOP_LITE_DEV_SHROUDED_X 90226
#define COOP_LITE_DEV_SHROUDED_Y 84176

/* ARCANUM_DEV_START_LOC: "x,y" or "shrouded". */
bool coop_lite_parse_dev_loc(const char* value, int* x, int* y);

/*
 * Coop death (PT10). No auto-revive: a dead PC stays a corpse until it is
 * resurrected by normal game means (resolved on the host). The host watches
 * the party with this pure state machine.
 */
/* All players dead: seconds of grace before the host reloads its last save. */
#define COOP_LITE_WIPE_GRACE_MS 10000u

#define COOP_PARTY_ACT_HOST_DOWN 0x01u /* host PC fell — notify everyone */
#define COOP_PARTY_ACT_HOST_UP 0x02u /* host PC resurrected */
#define COOP_PARTY_ACT_WIPE_COUNTDOWN 0x04u /* every player is dead — grace starts */
#define COOP_PARTY_ACT_WIPE_CANCEL 0x08u /* someone got up during the grace */
#define COOP_PARTY_ACT_RELOAD 0x10u /* reload the host's last save now */

typedef struct CoopPartyWatch {
    bool was_dead[8];
    bool host_down;
    int host_down_map;
    bool wipe_pending;
    unsigned int wipe_since_ms;
    bool reload_issued;
} CoopPartyWatch;

/*
 * One step. present[s]: slot s has a bound PC (slot 0 = host). dead[s]: it is
 * dead. map: host's current map. fell/rose (optional) get per-slot masks of
 * PCs that died / got up since the last step. Returns COOP_PARTY_ACT_* bits.
 */
unsigned int coop_lite_party_step(CoopPartyWatch* w, const bool present[8], const bool dead[8],
    int map, unsigned int now_ms, unsigned int* fell, unsigned int* rose);

/* Teleport of the local PC carries the other slot PCs (alive or dead). */
bool coop_lite_should_carry_slot(int slot, int mover_slot, bool bound);

/* PACKET_COOP_PARTY_STATE kinds. */
#define COOP_PARTY_HOST_DOWN 1
#define COOP_PARTY_HOST_UP 2
#define COOP_PARTY_WIPE 3 /* countdown_ms until reload */
#define COOP_PARTY_WIPE_CANCEL 4
#define COOP_PARTY_RELOADING 5
#define COOP_PARTY_RESYNC 6 /* host loaded its save: re-offer your PC */
#define COOP_PARTY_NO_SAVE 7 /* nothing to reload: everyone is brought back */

/* Starting kit for a coop-created/offered PC, generated locally (suspended). */
void coop_lite_generate_start_inventory(int64_t pc_obj);

/* gamelib reset (Quit Game, load): drop session state; a client leaves. */
void coop_lite_on_game_reset(void);

/* map_flush / map_close: release coop sector keep-alive locks first. */
void coop_lite_before_map_flush(void);

/* Host: a client disconnected — remove the PC it left behind. */
void coop_lite_host_remove_left_pc(int slot, int64_t pc_obj);

/* Client: re-establish the configured -join connection after quit-to-menu. */
bool coop_lite_client_reconnect(void);

/* True if non-host may pick this response (barter / harmless actions). */
bool coop_lite_dialog_option_allowed(const char* actions, int response_code);
/* True on a coop client, including while running local SP paths under
   tig_net_local_suspend (net reads inactive there). */
bool coop_lite_is_client_role(void);
/* Dialog action code (2 chars) a client must not execute. */
bool coop_lite_dialog_action_blocked(const char* code);
/* Client may not open dialog with another player's follower. */
bool coop_lite_client_may_open_dialog(int64_t npc_obj, int64_t pc_obj);

/* Story scripts that resolve SFO_PLAYER / "the PC" use the host PC in coop. */
int64_t coop_lite_story_pc_obj(void);

/*
 * Preferred per-slot tile offset from host (dx, dy in tiles). Slot 0 is (0,0).
 * Exposed for tests; follow apply uses nearest free walkable near this bias.
 */
void coop_lite_slot_preferred_offset(int slot, int* dx_ptr, int* dy_ptr);

/* Nearest free walkable loc near host_loc for this client slot (host_loc if none). */
int64_t coop_lite_follow_loc_for_slot(int64_t host_loc, int slot);

/*
 * Host: after the local PC finishes a teleport/area change, broadcast follow
 * so every remote player is relocated near the host (per-slot walkable offset).
 */
void coop_lite_notify_host_teleported(int map, int64_t loc);

/* Dispatch helper for PACKET_COOP_FOLLOW_HOST (from multiplayer_handle_message). */
void coop_lite_handle_follow_packet(PacketCoopFollowHost* pkt);

void coop_lite_handle_attack_intent(PacketCoopAttackIntent* pkt);

/* Reset follow arming / queued follow (call on disconnect / return to main menu). */
void coop_lite_session_reset(void);

/* Flush a follow packet queued while the client was still in the main menu. */
void coop_lite_flush_pending_follow(void);

/* Coop always uses real-time combat — block turn-based toggle. */
bool coop_lite_can_use_turn_based(void);

/* Host: mirror companion aggression onto host PC (party). */
void coop_lite_propagate_party_aggression(int64_t attacker_obj, int64_t target_obj);

/*
 * Export the local PC to Players/<oid>.coopchar (character bundle, see below).
 * Non-destructive: the live PC keeps its inventory, HP and animations
 * (PT12: the old .mpc path, save_char, stripped the whole inventory). No-op
 * when coop is off or there is no local PC. Does not write the campaign save.
 */
bool coop_lite_export_local_character(void);

/*
 * Character bundle (.coopchar), little-endian:
 *   u32 magic 'CCB1', u32 version, u32 count,
 *   count x { u32 size, size bytes }   -- [0] is the PC, then its items
 *   u32 len, level-scheme rule (NUL incl.), u32 len, level-scheme name
 * Each object blob is sub_442050 output (obj_write_mem with handles saved as
 * ids), which leaves the live object untouched.
 */
#define COOP_CHAR_BUNDLE_MAGIC 0x31424343u
#define COOP_CHAR_BUNDLE_VERSION 1u
#define COOP_CHAR_BUNDLE_MAX_OBJECTS 1024
#define COOP_CHAR_BUNDLE_MAX_BLOB (1u << 20)
#define COOP_CHAR_BUNDLE_MAX_STR 4096u

typedef struct CoopCharBundleBuf {
    uint8_t* data;
    size_t size;
    size_t cap;
} CoopCharBundleBuf;

bool coop_char_bundle_begin(CoopCharBundleBuf* buf, int count);
bool coop_char_bundle_add_blob(CoopCharBundleBuf* buf, const void* blob, size_t size);
bool coop_char_bundle_finish(CoopCharBundleBuf* buf, const char* rule, const char* name);
void coop_char_bundle_free(CoopCharBundleBuf* buf);

typedef bool (*CoopCharBundleVisitFn)(void* ctx, int index, const uint8_t* blob, size_t size);

/* Validates the whole layout first, then visits every blob in order. */
bool coop_char_bundle_parse(const uint8_t* data, size_t size, CoopCharBundleVisitFn visit,
    void* ctx, int* count_out, const char** rule_out, const char** name_out);

/* Packet routing roles for anim/combat/magictech receive (docs + tests). */
typedef enum CoopMpPacketRole {
    COOP_MP_PKT_UNKNOWN = 0,
    COOP_MP_PKT_HOST_RESOLVE = 1, /* client intent → host applies/resolves */
    COOP_MP_PKT_CLIENT_APPLY = 2, /* host broadcast → clients apply */
    COOP_MP_PKT_BOTH = 3,
} CoopMpPacketRole;

CoopMpPacketRole coop_lite_mp_packet_role(int type);

/*
 * Network hardening (pure; used by multiplayer_validate_message and tests).
 * coop_lite_packet_allowed: may a packet of this type be accepted by this
 * receiver from sender_slot? (Host drops host→client-only types from clients,
 * clients drop client→host-only types and anything not from the host.)
 */
bool coop_lite_packet_allowed(int type, bool receiver_is_host, int sender_slot);

/* Minimum wire size for coop packet types (0 if not a coop type). */
size_t coop_lite_coop_packet_min_size(int type);

/* Basic sanity for a basic_prototype number received from the network. */
bool coop_lite_pc_proto_plausible(int proto);

/* Force NUL termination of a fixed-size network string. */
void coop_lite_terminate_name(char* name, size_t size);

/*
 * Client-side: true when obj is our own PC and the session is synced, i.e.
 * movement/talk for it run locally (single-player code path) and the result
 * is replicated with PC_POS.
 */
bool coop_lite_client_drives_obj(int64_t obj);
/* Client applying host state: object scripts must not run (review P0-3). */
bool coop_lite_scripts_muted(void);
/* Ground pickup: client queues a host request (sent from the coop tick);
   host resyncs a client PC that gained an item outside the item-op path. */
void coop_lite_client_queue_pickup(int64_t item);

#define COOP_PICKUP_QUEUE_MAX 8
typedef struct CoopPickupQueue {
    int count;
    int map_key[COOP_PICKUP_QUEUE_MAX];
    uint8_t oid[COOP_PICKUP_QUEUE_MAX][24];
} CoopPickupQueue;
void coop_pickup_queue_reset(CoopPickupQueue* q);
/* false = full; the same OID twice is one entry. */
bool coop_pickup_queue_push(CoopPickupQueue* q, const uint8_t oid[24], int map_key);
/* Empties the queue; returns the entries for map_key (others are stale). */
int coop_pickup_queue_take(CoopPickupQueue* q, int map_key, uint8_t out[][24], int max);
/* coop_lite_client_drives_obj, also while packets are locally suspended
   (inside our own PC's locally simulated goals). */
bool coop_lite_client_local_goal_obj(int64_t obj);
void coop_lite_host_item_gained(int64_t pc);

/* Host: a client disconnected — clear its offer / replication state. */
void coop_lite_host_on_client_disconnect(int slot);

/* Host: is obj the PC bound to the sender of the message being handled? */
bool coop_lite_sender_owns_obj(int64_t obj);

/* True if two PC_POS samples differ (loc/offset/art). Pure; used by tests. */
/* Location/offset differ (art ignored): a real move, not frame drift. */
bool coop_lite_pc_pos_moved(const PacketCoopPcPos* prev, const PacketCoopPcPos* cur);
bool coop_lite_pc_pos_changed(const PacketCoopPcPos* prev, const PacketCoopPcPos* cur);
/* Max tiles a client PC may move between two of its own PC_POS samples
   (review M3); a larger jump is refused and corrected by the host. */
#define COOP_LITE_PC_POS_MAX_STEP 12
/* Host-side plausibility of a client's own PC_POS: same map, a walkable-size
   step from where the host has it, and the same art type (no swapping the
   PC for some other art). */
/*
 * P0-2 pose ordering. Unreliable poses (channel 2) and reliable FORCE
 * corrections (channel 0) are not ordered against each other, so a stale
 * pose could undo a correction. The host bumps a slot's epoch when it starts
 * a correction; the owner adopts it from the FORCE and stamps it on every
 * pose. Older epochs, and seq <= last within an epoch, are dropped.
 * owner_link: the host receiving from the owner (a newer epoch is invalid).
 */
typedef struct CoopPoseOrder {
    uint16_t epoch;
    uint16_t seq;
    bool valid;
    bool seq_valid;
} CoopPoseOrder;

bool coop_lite_pose_order_accept(CoopPoseOrder* st, uint16_t epoch, uint16_t seq,
    bool force, bool owner_link);
/* Host starts a correction for this receiver state: new epoch, seq reset. */
uint16_t coop_lite_pose_order_bump(CoopPoseOrder* st);
/* Map identity for poses: index (low 16) + load generation (high 16), so
   leaving and re-entering the same map index is distinguishable. */
int coop_lite_pose_map_key(int map, uint16_t gen);
/* map.c: a map finished opening (host bumps its load generation). */
void coop_lite_on_map_opened(void);
uint16_t coop_lite_map_gen(void);

bool coop_lite_pc_pos_plausible(int pkt_map, int cur_map, int64_t step_tiles,
    int pkt_art_type, int cur_art_type);

/* True if at least interval_ms elapsed (wrap-safe). Pure; used by tests. */
bool coop_lite_interval_elapsed(unsigned int last_ms, unsigned int now_ms, unsigned int interval_ms);

/* Per-frame coop tick (host: PC_POS broadcast). now_ms = monotonic ms. */
void coop_lite_ping(unsigned int now_ms);

/* Client: apply host PC_POS. */
void coop_lite_handle_pc_pos(PacketCoopPcPos* pkt);

/* Client: local PC teleport finished — complete a pending SESSION_SYNC. */
void coop_lite_client_on_teleport_done(void);

/* Test helper: host has an unconsumed CHAR_OFFER for this slot. */
bool coop_lite_test_pending_offer(int slot);


/* Client: send CHAR_OFFER from local premade/created PC; return true if sent. */
bool coop_lite_client_offer_character(int basic_prototype, const char* name);

/* True while a coop client is waiting for SESSION_SYNC (no independent New Game). */
bool coop_lite_client_waiting_for_sync(void);

/* True after SESSION_SYNC applied — mainmenu may close into the shared world. */
bool coop_lite_client_session_ready(void);

/* Consume the one-shot "menu may close" signal after SESSION_SYNC. */
void coop_lite_acknowledge_session_ready(void);

/* Host: remember offer / spawn + SESSION_SYNC when in-world. */
void coop_lite_handle_char_offer(PacketCoopCharOffer* pkt);

/* Client: apply SESSION_SYNC (load host map, bind PCs). */
void coop_lite_handle_session_sync(PacketCoopSessionSync* pkt);

/* Host: after entering world, bind slot 0 and sync any pending client offers. */
void coop_lite_host_on_entered_world(void);

/* True if this peer should skip the single-player New Game start-map teleport. */
bool coop_lite_should_skip_independent_new_game(void);


#ifdef __cplusplus
}
#endif

/*
 * Camera dead zone follow (pure). dx/dy: pixels the view origin would have
 * to move to centre the PC (location_calc_dist_from_screen_center). Returns
 * true and the scroll needed to bring the PC back to the dead-zone edge when
 * it is outside the zone.
 */
#define COOP_LITE_FOLLOW_DEADZONE_X 200
/* Beyond this the PC is (nearly) off screen at 800x600: recentre. */
#define COOP_LITE_FOLLOW_JUMP_X 380
#define COOP_LITE_FOLLOW_JUMP_Y 220
#define COOP_LITE_FOLLOW_DEADZONE_Y 120
/*
 * Remote-PC snapshot interpolation (clients only; never the local PC, never
 * on the host, whose positions are authoritative). Poses are shown
 * COOP_INTERP_DELAY_MS late, lerped in screen space between adjacent tiles;
 * a longer step snaps.
 */
#define COOP_INTERP_RING 3
#define COOP_INTERP_DELAY_MS 100u

typedef struct CoopInterpSample {
    int64_t loc;
    int offset_x;
    int offset_y;
    unsigned int art_id;
    unsigned int t_ms;
} CoopInterpSample;

typedef struct CoopInterpRing {
    CoopInterpSample s[COOP_INTERP_RING]; /* oldest first */
    int n;
} CoopInterpRing;

void coop_interp_reset(CoopInterpRing* ring);
/* false: snap now (first sample, or a step over 1 tile); the ring then
   holds only this sample. Repeats of the newest pose are ignored. */
bool coop_interp_push(CoopInterpRing* ring, const CoopInterpSample* sample, int64_t step_tiles);
/* For render time rt: 0 nothing, 1 show sample *ia, 2 lerp *ia -> *ib by
   num/den. */
int coop_interp_pick(const CoopInterpRing* ring, unsigned int rt, int* ia, int* ib,
    unsigned int* num, unsigned int* den);
/* Offset at num/den from (oa) to (ob + d), where d is the pixel distance
   from tile a to tile b. rel_b expresses it relative to tile b. */
int coop_interp_offset(int oa, int ob, int64_t d, unsigned int num, unsigned int den, bool rel_b);

bool coop_lite_follow_delta(int64_t dx, int64_t dy, int64_t zone_x, int64_t zone_y,
    int64_t* out_x, int64_t* out_y);

#endif /* ARCANUM_GAME_COOP_LITE_H_ */
