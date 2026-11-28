/* Coop Middle M6: client sub-instances, pure part (see coop_subinst.h). */

#include "game/coop_subinst.h"

#include <stdlib.h>
#include <string.h>

#include <zlib.h>

static void put_u16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get_u16(const uint8_t* p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool filename_safe_n(const char* name, size_t len)
{
    size_t i;

    if (name == NULL || len < 4 || len > COOP_SUBINST_NAME_MAX || name[0] == '.') {
        return false; /* "x.md" is the shortest */
    }
    for (i = 0; i < len; i++) {
        char c = name[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.'
                || c == '-')) {
            return false; /* also '/', '\\', ':', NUL, spaces */
        }
        if (c == '.' && i + 1 < len && name[i + 1] == '.') {
            return false;
        }
    }
    {
        static const char* const exts[] = { ".dif", ".mdy", ".md", ".des" };
        size_t e;

        for (e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
            size_t el = strlen(exts[e]);
            size_t k;

            if (len <= el) {
                continue;
            }
            for (k = 0; k < el && lower(name[len - el + k]) == exts[e][k]; k++) {
            }
            if (k == el) {
                return true;
            }
        }
        return false;
    }
}

bool coop_subinst_filename_safe(const char* name)
{
    size_t len;

    if (name == NULL) {
        return false;
    }
    len = strnlen(name, COOP_SUBINST_NAME_MAX + 1);
    return len <= COOP_SUBINST_NAME_MAX && filename_safe_n(name, len);
}

int coop_subinst_pack_buf(uint32_t map_id, const CoopSubinstFile* files, int count, uint8_t* out, size_t out_cap,
    size_t* out_len)
{
    size_t raw_len = COOP_SUBINST_HDR;
    uint8_t* raw;
    uint8_t* p;
    uLongf zlen;
    int i;
    int rc;

    if (out_len != NULL) {
        *out_len = 0;
    }
    if (out == NULL || out_len == NULL || count < 0 || (count > 0 && files == NULL)) {
        return COOP_SUBINST_E_ARG;
    }
    if (count > COOP_SUBINST_MAX_FILES) {
        return COOP_SUBINST_E_LIMIT;
    }
    for (i = 0; i < count; i++) {
        if (!coop_subinst_filename_safe(files[i].name) || (files[i].len > 0 && files[i].data == NULL)) {
            return COOP_SUBINST_E_NAME;
        }
        if (files[i].len > COOP_SUBINST_MAX_RAW) {
            return COOP_SUBINST_E_LIMIT;
        }
        raw_len += 2 + strlen(files[i].name) + 4 + (size_t)files[i].len;
        if (raw_len > COOP_SUBINST_MAX_RAW) {
            return COOP_SUBINST_E_LIMIT;
        }
    }
    raw = (uint8_t*)malloc(raw_len);
    if (raw == NULL) {
        return COOP_SUBINST_E_LIMIT;
    }
    memcpy(raw, COOP_SUBINST_MAGIC, 4);
    put_u32(raw + 4, map_id);
    put_u32(raw + 8, (uint32_t)count);
    p = raw + COOP_SUBINST_HDR;
    for (i = 0; i < count; i++) {
        size_t nl = strlen(files[i].name);

        put_u16(p, (uint16_t)nl);
        p += 2;
        memcpy(p, files[i].name, nl);
        p += nl;
        put_u32(p, files[i].len);
        p += 4;
        if (files[i].len > 0) {
            memcpy(p, files[i].data, files[i].len);
            p += files[i].len;
        }
    }
    put_u32(raw + 12, (uint32_t)crc32(0L, raw + COOP_SUBINST_HDR, (uInt)(raw_len - COOP_SUBINST_HDR)));
    if (out_cap < 4 + 16) {
        free(raw);
        return COOP_SUBINST_E_LIMIT;
    }
    zlen = (uLongf)(out_cap - 4);
    if (zlen > COOP_SUBINST_MAX_WIRE - 4) {
        zlen = COOP_SUBINST_MAX_WIRE - 4;
    }
    rc = compress2(out + 4, &zlen, raw, (uLong)raw_len, COOP_SUBINST_ZLIB_LEVEL);
    free(raw);
    if (rc == Z_BUF_ERROR) {
        return COOP_SUBINST_E_LIMIT;
    }
    if (rc != Z_OK) {
        return COOP_SUBINST_E_ZLIB;
    }
    put_u32(out, (uint32_t)raw_len);
    *out_len = 4 + (size_t)zlen;
    return COOP_SUBINST_OK;
}

int coop_subinst_unpack_buf(const uint8_t* wire, size_t wire_len, uint8_t* raw, size_t raw_cap, uint32_t* map_id,
    CoopSubinstFile* files, char (*name_buf)[COOP_SUBINST_NAME_MAX + 1], int max_files, int* count)
{
    uint32_t raw_len;
    uLongf got;
    uLong src_len;
    uint32_t n;
    size_t pos;
    uint32_t i;
    int rc;

    if (count != NULL) {
        *count = 0;
    }
    if (wire == NULL || raw == NULL || files == NULL || name_buf == NULL || count == NULL || max_files < 0) {
        return COOP_SUBINST_E_ARG;
    }
    if (wire_len < 4 + 8 || wire_len > COOP_SUBINST_MAX_WIRE) {
        return wire_len > COOP_SUBINST_MAX_WIRE ? COOP_SUBINST_E_LIMIT : COOP_SUBINST_E_FORMAT;
    }
    raw_len = get_u32(wire);
    if (raw_len < COOP_SUBINST_HDR || raw_len > COOP_SUBINST_MAX_RAW) {
        return raw_len > COOP_SUBINST_MAX_RAW ? COOP_SUBINST_E_LIMIT : COOP_SUBINST_E_FORMAT;
    }
    if (raw_cap < raw_len) {
        return COOP_SUBINST_E_LIMIT;
    }
    got = (uLongf)raw_len;
    src_len = (uLong)(wire_len - 4);
    rc = uncompress2(raw, &got, wire + 4, &src_len);
    if (rc != Z_OK || got != raw_len) {
        return COOP_SUBINST_E_ZLIB; /* corrupt, truncated, or bigger than announced */
    }
    if (src_len != (uLong)(wire_len - 4)) {
        return COOP_SUBINST_E_FORMAT; /* bytes after the zlib stream */
    }
    if (memcmp(raw, COOP_SUBINST_MAGIC, 4) != 0) {
        return COOP_SUBINST_E_FORMAT;
    }
    if (get_u32(raw + 12) != (uint32_t)crc32(0L, raw + COOP_SUBINST_HDR, (uInt)(raw_len - COOP_SUBINST_HDR))) {
        return COOP_SUBINST_E_CRC;
    }
    n = get_u32(raw + 8);
    if (n > COOP_SUBINST_MAX_FILES || (int)n > max_files) {
        return COOP_SUBINST_E_LIMIT;
    }
    pos = COOP_SUBINST_HDR;
    for (i = 0; i < n; i++) {
        uint16_t nl;
        uint32_t dl;

        if (raw_len - pos < 2) {
            return COOP_SUBINST_E_FORMAT;
        }
        nl = get_u16(raw + pos);
        pos += 2;
        if (nl == 0 || nl > COOP_SUBINST_NAME_MAX || raw_len - pos < (size_t)nl + 4) {
            return nl > COOP_SUBINST_NAME_MAX ? COOP_SUBINST_E_NAME : COOP_SUBINST_E_FORMAT;
        }
        if (!filename_safe_n((const char*)raw + pos, nl)) {
            return COOP_SUBINST_E_NAME;
        }
        memcpy(name_buf[i], raw + pos, nl);
        name_buf[i][nl] = '\0';
        pos += nl;
        dl = get_u32(raw + pos);
        pos += 4;
        if (raw_len - pos < dl) {
            return COOP_SUBINST_E_FORMAT;
        }
        files[i].name = name_buf[i];
        files[i].data = raw + pos;
        files[i].len = dl;
        pos += dl;
    }
    if (pos != raw_len) {
        return COOP_SUBINST_E_FORMAT; /* trailing bytes */
    }
    /* duplicate names would overwrite each other */
    for (i = 0; i < n; i++) {
        uint32_t j;

        for (j = i + 1; j < n; j++) {
            size_t k;
            bool same = strlen(name_buf[i]) == strlen(name_buf[j]);

            for (k = 0; same && name_buf[i][k] != '\0'; k++) {
                same = lower(name_buf[i][k]) == lower(name_buf[j][k]);
            }
            if (same) {
                return COOP_SUBINST_E_NAME;
            }
        }
    }
    if (map_id != NULL) {
        *map_id = get_u32(raw + 4);
    }
    *count = (int)n;
    return COOP_SUBINST_OK;
}

size_t coop_subinst_payload_size(size_t cmap_len, size_t bundle_len, size_t qty_len)
{
    return 12 + cmap_len + bundle_len + qty_len;
}

bool coop_subinst_payload_build(CoopSubinstSection cmap, CoopSubinstSection bundle, CoopSubinstSection qty,
    uint8_t* out, size_t out_cap, size_t* out_len)
{
    size_t pos = 0;

    if (out == NULL || out_len == NULL || cmap.data == NULL || cmap.len == 0 || cmap.len > COOP_SUBINST_MAX_WIRE
        || bundle.len > COOP_SUBINST_MAX_BUNDLE || (bundle.len > 0 && bundle.data == NULL)
        || qty.len > COOP_SUBINST_MAX_QTY || (qty.len > 0 && qty.data == NULL)
        || out_cap < coop_subinst_payload_size(cmap.len, bundle.len, qty.len)) {
        return false;
    }
    put_u32(out + pos, (uint32_t)cmap.len);
    memcpy(out + pos + 4, cmap.data, cmap.len);
    pos += 4 + cmap.len;
    put_u32(out + pos, (uint32_t)bundle.len);
    if (bundle.len > 0) {
        memcpy(out + pos + 4, bundle.data, bundle.len);
    }
    pos += 4 + bundle.len;
    put_u32(out + pos, (uint32_t)qty.len);
    if (qty.len > 0) {
        memcpy(out + pos + 4, qty.data, qty.len);
    }
    pos += 4 + qty.len;
    *out_len = pos;
    return true;
}

static bool payload_take(const uint8_t* data, size_t len, size_t* pos, size_t max, CoopSubinstSection* out)
{
    uint32_t n;

    if (len - *pos < 4) {
        return false;
    }
    n = get_u32(data + *pos);
    *pos += 4;
    if (n > max || (size_t)n > len - *pos) {
        return false;
    }
    out->data = n > 0 ? data + *pos : NULL;
    out->len = n;
    *pos += n;
    return true;
}

bool coop_subinst_payload_split(const uint8_t* data, size_t len, CoopSubinstSection* cmap,
    CoopSubinstSection* bundle, CoopSubinstSection* qty)
{
    size_t pos = 0;

    if (data == NULL || cmap == NULL || bundle == NULL || qty == NULL || len < 12
        || len > COOP_SUBINST_MAX_PAYLOAD) {
        return false;
    }
    return payload_take(data, len, &pos, COOP_SUBINST_MAX_WIRE, cmap) && cmap->len > 0
        && payload_take(data, len, &pos, COOP_SUBINST_MAX_BUNDLE, bundle)
        && payload_take(data, len, &pos, COOP_SUBINST_MAX_QTY, qty) && pos == len;
}

size_t coop_subinst_qty_build(const CoopSubinstQty* items, int count, uint8_t* out, size_t out_cap)
{
    size_t need;
    int i;

    if (out == NULL || count < 0 || count > COOP_SUBINST_MAX_QTY_ITEMS || (count > 0 && items == NULL)) {
        return 0;
    }
    need = 4 + (size_t)count * COOP_SUBINST_QTY_ENTRY;
    if (out_cap < need) {
        return 0;
    }
    put_u32(out, (uint32_t)count);
    for (i = 0; i < count; i++) {
        uint8_t* p = out + 4 + (size_t)i * COOP_SUBINST_QTY_ENTRY;

        memcpy(p, items[i].oid, 24);
        put_u32(p + 24, (uint32_t)items[i].qty);
        put_u32(p + 28, (uint32_t)items[i].hp_damage);
        put_u32(p + 32, (uint32_t)items[i].charges);
        put_u32(p + 36, (uint32_t)items[i].mana);
        put_u32(p + 40, (uint32_t)items[i].inv_loc);
    }
    return need;
}

int coop_subinst_qty_parse(const uint8_t* data, size_t len, CoopSubinstQty* items, int max)
{
    uint32_t n;
    uint32_t i;

    if (data == NULL || len < 4 || items == NULL || max < 0) {
        return -1;
    }
    n = get_u32(data);
    if (n > COOP_SUBINST_MAX_QTY_ITEMS || (int)n > max || len != 4 + (size_t)n * COOP_SUBINST_QTY_ENTRY) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        const uint8_t* p = data + 4 + (size_t)i * COOP_SUBINST_QTY_ENTRY;
        int32_t q = (int32_t)get_u32(p + 24);
        int32_t hp = (int32_t)get_u32(p + 28);
        int32_t ch = (int32_t)get_u32(p + 32);
        int32_t mana = (int32_t)get_u32(p + 36);
        int32_t loc = (int32_t)get_u32(p + 40);

        if (q < COOP_SUBINST_QTY_NONE || q > COOP_SUBINST_ITEM_VAL_MAX || hp < 0 || hp > COOP_SUBINST_ITEM_VAL_MAX
            || ch < -COOP_SUBINST_ITEM_VAL_MAX || ch > COOP_SUBINST_ITEM_VAL_MAX || mana < -COOP_SUBINST_ITEM_VAL_MAX
            || mana > COOP_SUBINST_ITEM_VAL_MAX || loc < 0 || loc > COOP_SUBINST_ITEM_VAL_MAX) {
            return -1;
        }
        memcpy(items[i].oid, p, 24);
        items[i].qty = q;
        items[i].hp_damage = hp;
        items[i].charges = ch;
        items[i].mana = mana;
        items[i].inv_loc = loc;
    }
    return (int)n;
}

size_t coop_subinst_des_filter(uint8_t* data, size_t len, const uint8_t (*oids)[24], int n)
{
    size_t in;
    size_t out = 0;
    int k;

    if (data == NULL) {
        return 0;
    }
    for (in = 0; in + 24 <= len; in += 24) {
        bool drop = false;

        for (k = 0; k < n && oids != NULL; k++) {
            if (memcmp(data + in, oids[k], 24) == 0) {
                drop = true;
                break;
            }
        }
        if (!drop) {
            if (out != in) {
                memmove(data + out, data + in, 24);
            }
            out += 24;
        }
    }
    return out;
}

int coop_subinst_chunk_count(size_t total_bytes)
{
    if (total_bytes == 0 || total_bytes > COOP_SUBINST_MAX_PAYLOAD) {
        return 0;
    }
    return (int)((total_bytes + COOP_SUBINST_CHUNK - 1) / COOP_SUBINST_CHUNK);
}

bool coop_subinst_rx_begin(CoopSubinstRx* rx, uint32_t lease_id, int chunks, size_t total_bytes, size_t max_bytes)
{
    if (rx == NULL) {
        return false;
    }
    coop_subinst_rx_free(rx);
    if (lease_id == 0 || total_bytes == 0 || total_bytes > max_bytes || total_bytes > COOP_SUBINST_MAX_PAYLOAD
        || chunks != coop_subinst_chunk_count(total_bytes)) {
        return false;
    }
    rx->data = (uint8_t*)malloc(total_bytes);
    if (rx->data == NULL) {
        return false;
    }
    rx->lease_id = lease_id;
    rx->chunks = chunks;
    rx->next = 0;
    rx->total = total_bytes;
    rx->got = 0;
    rx->active = true;
    return true;
}

int coop_subinst_rx_accept(CoopSubinstRx* rx, uint32_t lease_id, int chunk_idx, int chunk_count, int data_len,
    const uint8_t* data)
{
    size_t want;

    if (rx == NULL || !rx->active || lease_id != rx->lease_id) {
        return COOP_SUBINST_RX_STALE;
    }
    if (data == NULL || chunk_count != rx->chunks || chunk_idx != rx->next || data_len <= 0
        || data_len > COOP_SUBINST_CHUNK) {
        coop_subinst_rx_free(rx);
        return COOP_SUBINST_RX_BAD;
    }
    want = rx->total - rx->got;
    if (want > COOP_SUBINST_CHUNK) {
        want = COOP_SUBINST_CHUNK;
    }
    if ((size_t)data_len != want) {
        coop_subinst_rx_free(rx);
        return COOP_SUBINST_RX_BAD;
    }
    memcpy(rx->data + rx->got, data, (size_t)data_len);
    rx->got += (size_t)data_len;
    rx->next++;
    return rx->next == rx->chunks && rx->got == rx->total ? COOP_SUBINST_RX_DONE : COOP_SUBINST_RX_MORE;
}

void coop_subinst_rx_free(CoopSubinstRx* rx)
{
    if (rx == NULL) {
        return;
    }
    free(rx->data);
    memset(rx, 0, sizeof(*rx));
}

int coop_subinst_can_lease(int host_map, int dest_map, int shop_map, int slot, const CoopSubinstLease leases[8],
    bool in_combat, bool is_dead, int* other_slot)
{
    int owner;

    if (other_slot != NULL) {
        *other_slot = -1;
    }
    if (slot <= 0 || slot >= 8 || leases == NULL || dest_map <= 0 || dest_map == host_map
        || (shop_map > 0 && dest_map == shop_map)) {
        return COOP_SUBINST_LEASE_NORMAL;
    }
    owner = coop_subinst_lease_owner(leases, dest_map);
    if (owner >= 0 && owner != slot) {
        if (other_slot != NULL) {
            *other_slot = owner;
        }
        return COOP_SUBINST_LEASE_DENY_BUSY;
    }
    if (in_combat || is_dead) {
        return COOP_SUBINST_LEASE_DENY_STATE;
    }
    return COOP_SUBINST_LEASE_OK;
}

int coop_subinst_lease_owner(const CoopSubinstLease leases[8], int map_id)
{
    int s;

    if (leases == NULL || map_id <= 0) {
        return -1;
    }
    for (s = 0; s < 8; s++) {
        if (leases[s].active && leases[s].map_id == map_id) {
            return s;
        }
    }
    return -1;
}

int coop_subinst_client_next(int state, int event)
{
    if (event == COOP_SUBINST_EV_RESET) {
        return COOP_SUBINST_ST_IDLE;
    }
    switch (state) {
    case COOP_SUBINST_ST_IDLE:
        if (event == COOP_SUBINST_EV_REQUEST) {
            return COOP_SUBINST_ST_REQUESTED;
        }
        if (event == COOP_SUBINST_EV_GRANT) {
            return COOP_SUBINST_ST_RECEIVING; /* host-side transition, or sub -> sub after ACK */
        }
        return -1;
    case COOP_SUBINST_ST_REQUESTED:
        switch (event) {
        case COOP_SUBINST_EV_REQUEST:
            return COOP_SUBINST_ST_REQUESTED;
        case COOP_SUBINST_EV_GRANT:
            return COOP_SUBINST_ST_RECEIVING;
        case COOP_SUBINST_EV_DENY:
        case COOP_SUBINST_EV_ABORT:
            return COOP_SUBINST_ST_IDLE;
        default:
            return -1;
        }
    case COOP_SUBINST_ST_RECEIVING:
        switch (event) {
        case COOP_SUBINST_EV_ARCHIVE_OK:
            return COOP_SUBINST_ST_ACTIVE;
        case COOP_SUBINST_EV_ARCHIVE_BAD:
        case COOP_SUBINST_EV_ABORT:
            return COOP_SUBINST_ST_IDLE;
        default:
            return -1;
        }
    case COOP_SUBINST_ST_ACTIVE:
        switch (event) {
        case COOP_SUBINST_EV_COMMIT:
            return COOP_SUBINST_ST_COMMITTING;
        case COOP_SUBINST_EV_ABORT:
            return COOP_SUBINST_ST_IDLE;
        default:
            return -1;
        }
    case COOP_SUBINST_ST_COMMITTING:
        switch (event) {
        case COOP_SUBINST_EV_ACK:
        case COOP_SUBINST_EV_ABORT:
            return COOP_SUBINST_ST_IDLE;
        case COOP_SUBINST_EV_DENY:
            return COOP_SUBINST_ST_COMMITTING; /* next map refused: the ACK(0) follows */
        case COOP_SUBINST_EV_ACK_NEXT:
            return COOP_SUBINST_ST_REQUESTED;
        default:
            return -1;
        }
    default:
        return -1;
    }
}

bool coop_subinst_client_stalled(int state, bool holding, unsigned int since_ms, unsigned int now_ms)
{
    if (!holding || (state != COOP_SUBINST_ST_REQUESTED && state != COOP_SUBINST_ST_RECEIVING
                        && state != COOP_SUBINST_ST_IDLE)) {
        return false;
    }
    return (unsigned int)(now_ms - since_ms) >= COOP_SUBINST_STALL_MS;
}

bool coop_subinst_ckpt_due(unsigned int* ckpt_ms, unsigned int* ping_ms, unsigned int now_ms, bool paused,
    bool requested)
{
    unsigned int delta = 0;

    if (ckpt_ms == NULL || ping_ms == NULL) {
        return requested;
    }
    if (*ping_ms != 0) {
        delta = now_ms - *ping_ms;
    }
    *ping_ms = now_ms != 0 ? now_ms : 1u;
    if (requested) {
        return true;
    }
    if (paused) {
        *ckpt_ms += delta; /* the pause does not count */
    }
    if ((int)(now_ms - *ckpt_ms) < 0) {
        *ckpt_ms = now_ms; /* ahead of now: never a wrap storm */
    }
    if (paused) {
        return false;
    }
    return (unsigned int)(now_ms - *ckpt_ms) >= COOP_SUBINST_CHECKPOINT_MS;
}

int coop_subinst_sub_next_plan(int lease_rc, bool packed)
{
    return lease_rc == COOP_SUBINST_LEASE_OK && packed ? COOP_SUBINST_NEXT_GRANT : COOP_SUBINST_NEXT_BACK;
}

/* t405u: a checkpoint fills dest_loc with the PC's tile; a final commit keeps the travel dest. */
int64_t coop_subinst_commit_dest_loc(bool checkpoint, int64_t pc_loc, int64_t travel_loc)
{
    return checkpoint ? pc_loc : travel_loc;
}

bool coop_subinst_ctrl_valid(const PacketCoopSubinstCtrl* pkt, int size, bool from_host)
{
    if (pkt == NULL || size != (int)sizeof(PacketCoopSubinstCtrl) || pkt->type != PACKET_COOP_SUBINST_CTRL
        || pkt->op < 1 || pkt->op > COOP_SUBINST_OP_MAX || (pkt->flags & ~COOP_SUBINST_F_MASK) != 0) {
        return false;
    }
    if (from_host == (pkt->op == COOP_SUBINST_OP_COMMIT_BEGIN || pkt->op == COOP_SUBINST_OP_CANCEL)) {
        return false; /* COMMIT_BEGIN / CANCEL are the client ops */
    }
    switch (pkt->op) {
    case COOP_SUBINST_OP_LEASE_GRANT:
        return pkt->lease_id != 0 && pkt->lease_id <= COOP_SUBINST_MAX_LEASE_ID && pkt->map_id > 0
            && pkt->total_bytes > 0 && pkt->total_bytes <= COOP_SUBINST_MAX_WIRE
            && pkt->total_chunks == coop_subinst_chunk_count((size_t)pkt->total_bytes);
    case COOP_SUBINST_OP_LEASE_DENY:
        return pkt->reason >= COOP_SUBINST_DENY_BUSY && pkt->reason <= COOP_SUBINST_DENY_RULES && pkt->slot >= 0
            && pkt->slot < 8 && pkt->map_id >= 0;
    case COOP_SUBINST_OP_CHECKPOINT_REQ:
    case COOP_SUBINST_OP_ABORT:
        return pkt->lease_id <= COOP_SUBINST_MAX_LEASE_ID;
    case COOP_SUBINST_OP_COMMIT_BEGIN:
        return pkt->lease_id != 0 && pkt->lease_id <= COOP_SUBINST_MAX_LEASE_ID && pkt->dest_map >= 0
            && (pkt->is_checkpoint == 0 || pkt->is_checkpoint == 1) && pkt->total_bytes > 0
            && (size_t)pkt->total_bytes <= COOP_SUBINST_MAX_PAYLOAD
            && pkt->total_chunks == coop_subinst_chunk_count((size_t)pkt->total_bytes) && pkt->hp_damage >= 0
            && pkt->hp_damage <= COOP_SUBINST_MAX_DAMAGE && pkt->fatigue_damage >= 0
            && pkt->fatigue_damage <= COOP_SUBINST_MAX_DAMAGE && (pkt->flags & COOP_SUBINST_F_NEXT) == 0;
    case COOP_SUBINST_OP_COMMIT_ACK:
        return pkt->lease_id != 0 && pkt->lease_id <= COOP_SUBINST_MAX_LEASE_ID && (pkt->flags & COOP_SUBINST_F_DEAD) == 0;
    case COOP_SUBINST_OP_STATE:
        return pkt->slot > 0 && pkt->slot < 8 && pkt->map_id >= 0;
    case COOP_SUBINST_OP_CANCEL:
        return pkt->lease_id != 0 && pkt->lease_id <= COOP_SUBINST_MAX_LEASE_ID && pkt->flags == 0;
    default:
        return false;
    }
}

bool coop_subinst_chunk_valid(const PacketCoopSubinstChunk* pkt, int size)
{
    return pkt != NULL && size >= COOP_SUBINST_CHUNK_HDR + 1 && pkt->type == PACKET_COOP_SUBINST_CHUNK
        && pkt->lease_id != 0 && pkt->lease_id <= COOP_SUBINST_MAX_LEASE_ID && pkt->data_len > 0
        && pkt->data_len <= COOP_SUBINST_CHUNK && size == COOP_SUBINST_CHUNK_HDR + pkt->data_len
        && pkt->chunk_count > 0 && pkt->chunk_count <= (int)COOP_SUBINST_MAX_CHUNKS && pkt->chunk_idx >= 0
        && pkt->chunk_idx < pkt->chunk_count && pkt->flags == 0;
}

bool coop_subinst_delta_valid(const PacketCoopSubinstDelta* pkt, int size)
{
    if (pkt == NULL || size != (int)sizeof(PacketCoopSubinstDelta) || pkt->type != PACKET_COOP_SUBINST_DELTA
        || pkt->lease_id == 0 || pkt->lease_id > COOP_SUBINST_MAX_LEASE_ID) {
        return false;
    }
    switch (pkt->kind) {
    case COOP_SUBINST_DELTA_GF:
        return pkt->index >= 0 && pkt->index < COOP_SUBINST_GF_MAX && (pkt->val == 0 || pkt->val == 1);
    case COOP_SUBINST_DELTA_GV:
        return pkt->index >= 0 && pkt->index < COOP_SUBINST_GV_MAX;
    case COOP_SUBINST_DELTA_QUEST:
        return pkt->index > 0 && pkt->index < 100000 && pkt->val >= 0 && pkt->val <= 16;
    case COOP_SUBINST_DELTA_RUMOR:
        return pkt->index >= 0 && pkt->index < 100000 && (pkt->val == 0 || pkt->val == 1);
    case COOP_SUBINST_DELTA_REP:
        return pkt->index >= 0 && pkt->index < 100000 && (pkt->val == 1 || pkt->val == -1);
    case COOP_SUBINST_DELTA_AREA:
        return pkt->index > 0 && pkt->index < 100000 && pkt->val == 0;
    case COOP_SUBINST_DELTA_XP:
        return pkt->index == 0 && pkt->val > 0 && pkt->val <= COOP_SUBINST_XP_MAX;
    case COOP_SUBINST_DELTA_FATE:
        return pkt->index >= 0 && pkt->index < COOP_SUBINST_FATE_MAX && (pkt->val == 0 || pkt->val == 1);
    default:
        return false;
    }
}


/* ======================================================================
 * Game side (not in the unit tests).
 * ==================================================================== */
#ifndef COOP_LITE_TEST_STUBS

#include <stdio.h>

#include <SDL3/SDL.h>

#include "game/anim.h"
#include "game/area.h"
#include "game/combat.h"
#include "game/coop_lite.h"
#include "game/coop_middle.h"
#include "game/coop_rules.h"
#include "game/critter.h"
#include "game/fate.h"
#include "game/item.h"
#include "game/location.h"
#include "game/map.h"
#include "game/multiplayer.h"
#include "game/obj.h"
#include "game/obj_flags.h"
#include "game/obj_pool.h"
#include "game/object.h"
#include "game/player.h"
#include "game/reputation.h"
#include "game/rumor.h"
#include "game/script.h"
#include "game/teleport.h"
#include "net/tig_net.h"
#include "tig/debug.h"
#include "tig/file.h"

int quest_state_set_internal(int64_t pc_obj, int num, int state, int64_t npc_obj); /* quest.c */

#define SI_HIDE_MAX 9
#define SI_HIDE_FLAGS (OF_OFF | OF_INVULNERABLE)
#define SI_PARTY_MAX 2048
#define SI_RESYNC_DELAY_MS 300u
#define SI_RESYNC_GIVEUP_MS 10000u
#define SI_STATE_MS 5000u
#define SI_DEATH_COMMIT_MS 5000u
#define SI_ENTER_GIVEUP_MS 15000u

typedef struct SiHidden {
    int count;
    ObjectID oid[SI_HIDE_MAX];
    unsigned int added[SI_HIDE_MAX];
} SiHidden;

/* Both sides: who is inside (badge M, world marker). */
static bool si_inside[8];
static int si_slot_map[8];
static int si_entry_map[8];
static int64_t si_entry_loc[8];
static unsigned int si_serial;

/* Host. */
static CoopSubinstLease si_leases[8];
static SiHidden si_hid[8];
static int64_t si_park_loc[8];
static int si_park_map[8];
static CoopSubinstRx si_host_rx[8];
static PacketCoopSubinstCtrl si_host_commit[8];
static uint8_t* si_ckpt[8]; /* last checkpoint payload (disconnect) */
static size_t si_ckpt_len[8];
static uint32_t si_next_lease;
static unsigned int si_resync_mask;
static unsigned int si_resync_since[8];
static unsigned int si_state_ms;
static unsigned int si_ckpt_done_mask; /* before save: slots that answered */
static int64_t si_merge_loc[8]; /* t405u: host entered this slot's leased map */
static int si_merge_map[8];

/* Client. */
static int si_cl_state;
static bool si_cl_active; /* inside: local suspend held */
static bool si_cl_in_pump;
static bool si_cl_left_in_pump;
static uint32_t si_cl_lease;
static int si_cl_map;
static int64_t si_cl_loc;
static int si_cl_host_map;
static CoopSubinstRx si_cl_rx;
static bool si_cl_enter_pending;
static unsigned int si_cl_enter_ms;
static unsigned int si_cl_ckpt_ms;
static unsigned int si_cl_ping_ms; /* M7 review: last checkpoint check (0 = none since entry) */
static bool si_cl_ckpt_req;
static unsigned int si_cl_dead_ms;
static bool si_cl_unhide_pending;
static unsigned int si_cl_state_ms;
static bool si_cl_entered; /* ACTIVE and our PC reached the leased map */

static unsigned int si_now(void)
{
    unsigned int now = (unsigned int)(SDL_GetTicksNS() / 1000000u);

    return now != 0 ? now : 1u;
}

static bool si_host(void)
{
    return tig_net_is_active() && tig_net_is_host() && tig_net_is_coop_enabled();
}

static bool si_rule(void)
{
    return coop_middle_rule_on(COOP_RULE_SUBINSTANCES);
}

static void si_send(void* data, int size, int slot)
{
    int depth = tig_net_local_suspend_pause();

    if (slot < 0) {
        tig_net_send_app_all(data, size);
    } else {
        tig_net_send_app(data, size, slot);
    }
    tig_net_local_suspend_resume(depth);
}

static void si_ctrl_init(PacketCoopSubinstCtrl* pkt, int op)
{
    memset(pkt, 0, sizeof(*pkt));
    pkt->type = PACKET_COOP_SUBINST_CTRL;
    pkt->op = op;
}

static void si_send_payload(uint32_t lease_id, const uint8_t* data, size_t len, int slot)
{
    PacketCoopSubinstChunk pkt;
    int count = coop_subinst_chunk_count(len);
    int i;

    for (i = 0; i < count; i++) {
        size_t off = (size_t)i * COOP_SUBINST_CHUNK;
        size_t n = len - off < COOP_SUBINST_CHUNK ? len - off : COOP_SUBINST_CHUNK;

        memset(&pkt, 0, COOP_SUBINST_CHUNK_HDR);
        pkt.type = PACKET_COOP_SUBINST_CHUNK;
        pkt.lease_id = lease_id;
        pkt.chunk_idx = i;
        pkt.chunk_count = count;
        pkt.data_len = (int)n;
        memcpy(pkt.data, data + off, n);
        si_send(&pkt, COOP_SUBINST_CHUNK_HDR + (int)n, slot);
    }
}

static const char* si_map_name(int map)
{
    char* name = NULL;

    if (map > 0 && map_get_name(map, &name) && name != NULL && name[0] != '\0') {
        return name;
    }
    return coop_lite_lang_ru() ? "локацию" : "a location";
}

static bool si_map_dir(int map, char* out, size_t sz)
{
    char* name = NULL;

    if (map <= 0 || !map_get_name(map, &name) || name == NULL || name[0] == '\0') {
        return false;
    }
    snprintf(out, sz, "Save\\Current\\maps\\%s", name);
    return true;
}

static bool si_read_file(const char* path, uint8_t** data, uint32_t* len)
{
    TigFile* f = tig_file_fopen(path, "rb");
    int n;

    *data = NULL;
    *len = 0;
    if (f == NULL) {
        return false;
    }
    n = tig_file_filelength(f);
    if (n < 0 || n > COOP_SUBINST_MAX_RAW) {
        tig_file_fclose(f);
        return false;
    }
    if (n > 0) {
        *data = (uint8_t*)malloc((size_t)n);
        if (*data == NULL || tig_file_fread(*data, 1, (size_t)n, f) != (size_t)n) {
            free(*data);
            *data = NULL;
            tig_file_fclose(f);
            return false;
        }
    }
    *len = (uint32_t)n;
    tig_file_fclose(f);
    return true;
}

static bool si_write_file(const char* path, const uint8_t* data, uint32_t len)
{
    TigFile* f = tig_file_fopen(path, "wb");
    bool ok;

    if (f == NULL) {
        return false;
    }
    ok = len == 0 || tig_file_fwrite(data, 1, len, f) == len;
    tig_file_fclose(f);
    return ok;
}

/* The map's Save\Current folder as a CMAP1 wire archive (malloc'd). A
   missing folder is a valid empty archive. */
static int si_pack_map(int map, uint8_t** out, size_t* out_len)
{
    char dir[TIG_MAX_PATH];
    char pattern[TIG_MAX_PATH];
    char path[TIG_MAX_PATH];
    TigFileList list;
    CoopSubinstFile files[COOP_SUBINST_MAX_FILES];
    uint8_t* bufs[COOP_SUBINST_MAX_FILES];
    int n = 0;
    unsigned int i;
    size_t raw = 0;
    int rc = COOP_SUBINST_OK;

    *out = NULL;
    *out_len = 0;
    if (!si_map_dir(map, dir, sizeof(dir))) {
        return COOP_SUBINST_E_ARG;
    }
    memset(bufs, 0, sizeof(bufs));
    if (tig_file_is_directory(dir)) {
        snprintf(pattern, sizeof(pattern), "%s\\*.*", dir);
        tig_file_list_create(&list, pattern);
        for (i = 0; i < list.count && rc == COOP_SUBINST_OK; i++) {
            const char* name = list.entries[i].path;
            uint32_t len = 0;

            if ((list.entries[i].attributes & TIG_FILE_ATTRIBUTE_SUBDIR) != 0 || name[0] == '.') {
                continue;
            }
            if (!coop_subinst_filename_safe(name)) {
                tig_debug_printf("CoopSubinst: pack %s: %s skipped (not a map state file)\n", dir, name);
                continue;
            }
            if (n >= COOP_SUBINST_MAX_FILES) {
                rc = COOP_SUBINST_E_LIMIT;
                break;
            }
            snprintf(path, sizeof(path), "%s\\%s", dir, name);
            if (!si_read_file(path, &bufs[n], &len)) {
                tig_debug_printf("CoopSubinst: pack %s: %s unreadable\n", dir, name);
                rc = COOP_SUBINST_E_LIMIT;
                break;
            }
            raw += len + 64;
            if (raw > COOP_SUBINST_MAX_RAW) {
                rc = COOP_SUBINST_E_LIMIT;
                n++;
                break;
            }
            files[n].name = list.entries[i].path; /* list lives until destroy */
            files[n].data = bufs[n];
            files[n].len = len;
            n++;
        }
        if (rc == COOP_SUBINST_OK) {
            *out = (uint8_t*)malloc(COOP_SUBINST_MAX_WIRE);
            if (*out == NULL) {
                rc = COOP_SUBINST_E_LIMIT;
            } else {
                rc = coop_subinst_pack_buf((uint32_t)map, files, n, *out, COOP_SUBINST_MAX_WIRE, out_len);
            }
        }
        tig_file_list_destroy(&list);
    } else {
        *out = (uint8_t*)malloc(COOP_SUBINST_MAX_WIRE);
        rc = *out != NULL ? coop_subinst_pack_buf((uint32_t)map, NULL, 0, *out, COOP_SUBINST_MAX_WIRE, out_len)
                          : COOP_SUBINST_E_LIMIT;
    }
    for (i = 0; i < (unsigned int)n; i++) {
        free(bufs[i]);
    }
    if (rc != COOP_SUBINST_OK) {
        free(*out);
        *out = NULL;
        *out_len = 0;
    }
    tig_debug_printf("CoopSubinst: packed map %d (%s): %d file(s), %u bytes, rc %d\n", map, dir, n,
        (unsigned int)*out_len, rc);
    return rc;
}

/* Validates the whole archive (map_id must be want_map), then replaces the
   map's state files. Never for the open map. */
static bool si_unpack_map(const uint8_t* wire, size_t len, int want_map)
{
    static CoopSubinstFile files[COOP_SUBINST_MAX_FILES];
    static char names[COOP_SUBINST_MAX_FILES][COOP_SUBINST_NAME_MAX + 1];
    char dir[TIG_MAX_PATH];
    char pattern[TIG_MAX_PATH];
    char path[TIG_MAX_PATH];
    TigFileList list;
    uint8_t* raw;
    uint32_t map_id = 0;
    int count = 0;
    int rc;
    int i;
    unsigned int k;
    bool ok = true;

    if (map_is_valid() && map_current_map() == want_map) {
        tig_debug_printf("CoopSubinst: unpack refused: map %d is open here\n", want_map);
        return false;
    }
    if (!si_map_dir(want_map, dir, sizeof(dir))) {
        return false;
    }
    raw = (uint8_t*)malloc(COOP_SUBINST_MAX_RAW);
    if (raw == NULL) {
        return false;
    }
    rc = coop_subinst_unpack_buf(wire, len, raw, COOP_SUBINST_MAX_RAW, &map_id, files, names,
        COOP_SUBINST_MAX_FILES, &count);
    if (rc != COOP_SUBINST_OK || (int)map_id != want_map) {
        tig_debug_printf("CoopSubinst: archive for map %d refused (rc %d, map %u)\n", want_map, rc, map_id);
        free(raw);
        return false;
    }
    tig_file_mkdir_ex(dir);
    snprintf(pattern, sizeof(pattern), "%s\\*.*", dir);
    tig_file_list_create(&list, pattern);
    for (k = 0; k < list.count; k++) {
        if ((list.entries[k].attributes & TIG_FILE_ATTRIBUTE_SUBDIR) == 0
            && coop_subinst_filename_safe(list.entries[k].path)) {
            snprintf(path, sizeof(path), "%s\\%s", dir, list.entries[k].path);
            tig_file_remove(path);
        }
    }
    tig_file_list_destroy(&list);
    for (i = 0; i < count; i++) {
        snprintf(path, sizeof(path), "%s\\%s", dir, files[i].name);
        if (!si_write_file(path, files[i].data, files[i].len)) {
            tig_debug_printf("CoopSubinst: %s not written\n", path);
            ok = false;
        }
    }
    free(raw);
    tig_debug_printf("CoopSubinst: map %d state written (%d file(s))%s\n", want_map, count, ok ? "" : " WITH ERRORS");
    return ok;
}

static void si_set_inside(int slot, int map, int entry_map, int64_t entry_loc)
{
    if (slot <= 0 || slot >= 8) {
        return;
    }
    if (si_inside[slot] != (map > 0) || si_slot_map[slot] != map || si_entry_loc[slot] != entry_loc) {
        si_serial++;
    }
    si_inside[slot] = map > 0;
    si_slot_map[slot] = map > 0 ? map : 0;
    si_entry_map[slot] = map > 0 ? entry_map : 0;
    si_entry_loc[slot] = map > 0 ? entry_loc : 0;
}

uint8_t coop_subinst_host_lease_mask(void)
{
    uint8_t mask = 0;
    int slot;

    for (slot = 1; slot < 8; slot++) {
        if (si_leases[slot].active) {
            mask |= (uint8_t)(1u << slot);
        }
    }
    return mask;
}

bool coop_subinst_host_commit_busy(void)
{
    int slot;

    for (slot = 1; slot < 8; slot++) {
        if (si_host_rx[slot].active) {
            return true;
        }
    }
    return false;
}

bool coop_subinst_slot_inside(int slot)
{
    return slot > 0 && slot < 8 && si_inside[slot];
}

int coop_subinst_slot_map(int slot)
{
    return slot > 0 && slot < 8 ? si_slot_map[slot] : 0;
}

bool coop_subinst_slot_entry(int slot, int* map, int64_t* loc)
{
    if (!coop_subinst_slot_inside(slot)) {
        return false;
    }
    if (map != NULL) {
        *map = si_entry_map[slot];
    }
    if (loc != NULL) {
        *loc = si_entry_loc[slot];
    }
    return true;
}

unsigned int coop_subinst_serial(void)
{
    return si_serial;
}

/* ---------------------------------------------------------------- host */

static void si_hide_obj(SiHidden* h, int64_t obj)
{
    unsigned int cur;
    unsigned int add;

    if (obj == OBJ_HANDLE_NULL || h->count >= SI_HIDE_MAX) {
        return;
    }
    cur = (unsigned int)obj_field_int32_get(obj, OBJ_F_FLAGS);
    add = SI_HIDE_FLAGS & ~cur;
    h->oid[h->count] = obj_get_id(obj);
    h->added[h->count] = add;
    h->count++;
    if (add != 0) {
        multiplayer_lock();
        sub_424070(obj, 3, false, true); /* stop its walk / attack */
        object_flags_set(obj, add);
        multiplayer_unlock();
    }
}

static void si_park(int slot, int64_t pc)
{
    ObjectList objects;
    ObjectNode* node;
    SiHidden* h = &si_hid[slot];

    h->count = 0;
    si_hide_obj(h, pc);
    object_list_all_followers(pc, &objects);
    for (node = objects.head; node != NULL; node = node->next) {
        si_hide_obj(h, node->obj);
    }
    object_list_destroy(&objects);
}

/* Back in the world, at loc when given (the PC and its followers). */
static void si_unpark(int slot, int64_t loc)
{
    SiHidden* h = &si_hid[slot];
    int i;

    for (i = 0; i < h->count; i++) {
        int64_t obj = obj_pool_perm_lookup(h->oid[i]);

        if (obj == OBJ_HANDLE_NULL) {
            continue;
        }
        if (loc != 0) {
            if (i == 0) {
                coop_lite_mid_move_pc(slot, obj, loc);
            } else {
                multiplayer_lock();
                sub_43E770(obj, loc, 0, 0);
                multiplayer_unlock();
            }
        }
        if (h->added[i] != 0) {
            multiplayer_lock();
            object_flags_unset(obj, h->added[i]);
            multiplayer_unlock();
        }
    }
    h->count = 0;
}

static void si_state_send(int slot)
{
    PacketCoopSubinstCtrl pkt;

    si_ctrl_init(&pkt, COOP_SUBINST_OP_STATE);
    pkt.slot = slot;
    pkt.map_id = si_leases[slot].active ? si_leases[slot].map_id : 0;
    pkt.dest_map = si_park_map[slot];
    pkt.dest_loc = si_park_loc[slot];
    si_send(&pkt, sizeof(pkt), -1);
    si_set_inside(slot, pkt.map_id, pkt.dest_map, pkt.dest_loc);
}

static void si_lease_free(int slot)
{
    coop_subinst_rx_free(&si_host_rx[slot]);
    free(si_ckpt[slot]);
    si_ckpt[slot] = NULL;
    si_ckpt_len[slot] = 0;
    memset(&si_leases[slot], 0, sizeof(si_leases[slot]));
}

static void si_slot_line(int slot, bool entered, int map)
{
    char name[64];
    char what[160];

    coop_middle_slot_name(slot, name, sizeof(name));
    snprintf(what, sizeof(what), "%d|%s", entered ? 1 : 0, si_map_name(map));
    coop_middle_sys_line_all(COOP_MID_CHAT_SUBINST, slot, name, what);
}

static void si_deny(int slot, int reason, int other_slot, int map)
{
    PacketCoopSubinstCtrl pkt;

    si_ctrl_init(&pkt, COOP_SUBINST_OP_LEASE_DENY);
    pkt.reason = reason;
    pkt.slot = other_slot >= 0 && other_slot < 8 ? other_slot : 0;
    pkt.map_id = map;
    si_send(&pkt, sizeof(pkt), slot);
    tig_debug_printf("CoopSubinst: slot %d denied map %d (reason %d, inside: %d)\n", slot, map, reason, other_slot);
}

/* Lease + archive to the client. from_sub: the PC is parked already
   (sub -> sub). Returns 0 or a COOP_SUBINST_DENY_* reason. */
/* wire: the packed map (taken: freed here). */
static int si_grant_packed(int slot, int map, int64_t loc, bool from_sub, uint8_t* wire, size_t wire_len)
{
    PacketCoopSubinstCtrl pkt;
    int64_t pc = sub_4A2B60(slot);

    if (pc == OBJ_HANDLE_NULL) {
        free(wire);
        return COOP_SUBINST_DENY_STATE;
    }
    si_next_lease = si_next_lease >= COOP_SUBINST_MAX_LEASE_ID ? 1u : si_next_lease + 1u;
    if (!from_sub) {
        si_park_loc[slot] = obj_field_int64_get(pc, OBJ_F_LOCATION);
        si_park_map[slot] = map_current_map();
        si_park(slot, pc);
    }
    si_lease_free(slot);
    si_leases[slot].active = true;
    si_leases[slot].map_id = map;
    si_leases[slot].lease_id = si_next_lease;
    si_leases[slot].last_checkpoint_ms = si_now();
    si_leases[slot].last_loc = loc; /* entry tile until a checkpoint updates it */
    si_ctrl_init(&pkt, COOP_SUBINST_OP_LEASE_GRANT);
    pkt.slot = slot;
    pkt.map_id = map;
    pkt.lease_id = si_next_lease;
    pkt.dest_loc = loc;
    pkt.dest_map = map;
    pkt.total_bytes = (int)wire_len;
    pkt.total_chunks = coop_subinst_chunk_count(wire_len);
    si_send(&pkt, sizeof(pkt), slot);
    si_send_payload(si_next_lease, wire, wire_len, slot);
    free(wire);
    si_state_send(slot);
    si_slot_line(slot, true, map);
    tig_debug_printf("CoopSubinst: slot %d leased map %d (lease %u, %u bytes)\n", slot, map, si_next_lease,
        (unsigned int)wire_len);
    return 0;
}

static int si_grant(int slot, int map, int64_t loc, bool from_sub)
{
    uint8_t* wire = NULL;
    size_t wire_len = 0;

    if (sub_4A2B60(slot) == OBJ_HANDLE_NULL) {
        return COOP_SUBINST_DENY_STATE;
    }
    if (si_pack_map(map, &wire, &wire_len) != COOP_SUBINST_OK) {
        return COOP_SUBINST_DENY_TOO_BIG;
    }
    return si_grant_packed(slot, map, loc, from_sub, wire, wire_len);
}

bool coop_subinst_host_travel(int slot, int map, int64_t loc)
{
    return coop_subinst_host_travel_ex(slot, map, loc, NULL);
}

bool coop_subinst_host_travel_ex(int slot, int map, int64_t loc, const char** why)
{
    int64_t pc;
    int other = -1;
    int r;
    int reason;
    int shop;

    if (why != NULL) {
        *why = NULL;
    }
    if (!si_host() || !si_rule()) {
        if (why != NULL) {
            *why = "subinstances off";
        }
        return false;
    }
    if (slot <= 0 || slot >= 8) {
        if (why != NULL) {
            *why = "bad slot";
        }
        return false;
    }
    if (map <= 0) {
        if (why != NULL) {
            *why = "bad dest map";
        }
        return false;
    }
    if (!map_is_valid()) {
        if (why != NULL) {
            *why = "map not valid";
        }
        return false;
    }
    if (map == map_by_type(MAP_TYPE_START_MAP)) {
        if (why != NULL) {
            *why = "dest is map 1";
        }
        return false;
    }
    if (si_leases[slot].active) {
        return true; /* already inside (a second report of the same exit) */
    }
    pc = sub_4A2B60(slot);
    if (pc == OBJ_HANDLE_NULL) {
        if (why != NULL) {
            *why = "pc null";
        }
        return false;
    }
    shop = map_by_type(MAP_TYPE_SHOPPING_MAP);
    if (shop > 0 && map == shop) {
        if (why != NULL) {
            *why = "shop map";
        }
        return false;
    }
    r = coop_subinst_can_lease(map_current_map(), map, shop, slot, si_leases,
        combat_critter_is_combat_mode_active(pc), critter_is_dead(pc), &other);
    switch (r) {
    case COOP_SUBINST_LEASE_NORMAL:
        if (why != NULL) {
            *why = map == map_current_map() ? "same map" : "not a solo instance";
        }
        return false;
    case COOP_SUBINST_LEASE_DENY_BUSY:
        si_deny(slot, COOP_SUBINST_DENY_BUSY, other, map);
        return true;
    case COOP_SUBINST_LEASE_DENY_STATE:
        si_deny(slot, COOP_SUBINST_DENY_STATE, -1, map);
        return true;
    default:
        break;
    }
    reason = si_grant(slot, map, loc, false);
    if (reason != 0) {
        si_deny(slot, reason, -1, map);
        if (why != NULL) {
            *why = "too big (bytes/files)";
        }
        return false; /* too big: the usual party travel prompt */
    }
    return true;
}

static void si_send_ack(int slot, uint32_t lease_id, int flags)
{
    PacketCoopSubinstCtrl pkt;

    si_ctrl_init(&pkt, COOP_SUBINST_OP_COMMIT_ACK);
    pkt.slot = slot;
    pkt.lease_id = lease_id;
    pkt.flags = flags;
    si_send(&pkt, sizeof(pkt), slot);
}

/* The slot is out: back on our map (at loc, or where it went in). */
static void si_host_release(int slot, int64_t loc, const char* why)
{
    int map = si_leases[slot].map_id;

    si_unpark(slot, loc);
    si_lease_free(slot);
    si_state_send(slot);
    if (map > 0) {
        si_slot_line(slot, false, map);
    }
    si_resync_mask |= 1u << slot;
    si_resync_since[slot] = si_now();
    tig_debug_printf("CoopSubinst: slot %d released (%s)\n", slot, why);
}

static void si_host_apply(int slot)
{
    PacketCoopSubinstCtrl* c = &si_host_commit[slot];
    CoopSubinstRx* rx = &si_host_rx[slot];
    CoopSubinstSection cmap;
    CoopSubinstSection bundle;
    CoopSubinstSection qty;
    static CoopSubinstQty qty_items[COOP_SUBINST_MAX_QTY_ITEMS];
    int qty_n = 0;
    int64_t pc = sub_4A2B60(slot);
    int lease_map = si_leases[slot].map_id;
    uint32_t lease_id = si_leases[slot].lease_id;
    bool final = c->is_checkpoint == 0;
    bool ok;

    ok = coop_subinst_payload_split(rx->data, rx->total, &cmap, &bundle, &qty);
    if (ok && qty.len > 0) {
        qty_n = coop_subinst_qty_parse(qty.data, qty.len, qty_items, COOP_SUBINST_MAX_QTY_ITEMS);
        ok = qty_n >= 0;
    }
    if (ok) {
        ok = si_unpack_map(cmap.data, cmap.len, lease_map);
    }
    if (!ok) {
        tig_debug_printf("CoopSubinst: slot %d %s refused (bad payload)\n", slot, final ? "commit" : "checkpoint");
        coop_subinst_rx_free(rx);
        if (final) {
            /* The PC must not stay parked forever: back at the entrance. */
            si_send_ack(slot, lease_id, 0);
            si_host_release(slot, 0, "bad commit");
        }
        return;
    }
    if (!final) {
        /* The parked PC follows the map files on disk (a host save now
           holds both the same way: no lost or doubled items). */
        if (pc != OBJ_HANDLE_NULL
            && !coop_lite_host_pc_reconcile(pc, bundle.data, bundle.len, qty_items, qty_n, c->hp_damage,
                c->fatigue_damage)) {
            tig_debug_printf("CoopSubinst: slot %d checkpoint: character not reconciled\n", slot);
        }
        free(si_ckpt[slot]);
        si_ckpt[slot] = rx->data; /* kept for a disconnect */
        si_ckpt_len[slot] = rx->total;
        rx->data = NULL;
        coop_subinst_rx_free(rx);
        si_leases[slot].last_checkpoint_ms = si_now();
        if (c->dest_loc != 0) {
            si_leases[slot].last_loc = c->dest_loc; /* t405u: position inside the instance */
        }
        si_ckpt_done_mask |= 1u << slot;
        tig_debug_printf("CoopSubinst: slot %d checkpoint of map %d stored (loc %d,%d)\n", slot, lease_map,
            (int)LOCATION_GET_X(si_leases[slot].last_loc), (int)LOCATION_GET_Y(si_leases[slot].last_loc));
        return;
    }
    if (pc != OBJ_HANDLE_NULL) {
        if (!coop_lite_host_pc_reconcile(pc, bundle.data, bundle.len, qty_items, qty_n, c->hp_damage,
                c->fatigue_damage)) {
            tig_debug_printf("CoopSubinst: slot %d character not reconciled (kept as it was)\n", slot);
        }
    }
    coop_subinst_rx_free(rx);
    if ((c->flags & COOP_SUBINST_F_DEAD) != 0) {
        si_send_ack(slot, lease_id, 0);
        si_host_release(slot, 0, "died inside");
        if (pc != OBJ_HANDLE_NULL && !critter_is_dead(pc)) {
            critter_kill(pc);
        }
        return;
    }
    if (c->dest_map > 0 && c->dest_map == map_current_map()) {
        si_send_ack(slot, lease_id, 0);
        si_host_release(slot, c->dest_loc, "commit");
        return;
    }
    if (c->dest_map > 0 && si_rule() && pc != OBJ_HANDLE_NULL && c->dest_map != map_by_type(MAP_TYPE_START_MAP)) {
        int other = -1;
        int r;
        uint8_t* wire = NULL;
        size_t wire_len = 0;
        bool packed = false;

        /* sub -> sub: the old lease ends, a new one starts (PC stays parked).
           The next map is packed BEFORE the ACK: an ACK(F_NEXT) always has
           its GRANT behind it. */
        si_leases[slot].active = false;
        r = coop_subinst_can_lease(map_current_map(), c->dest_map, map_by_type(MAP_TYPE_SHOPPING_MAP), slot,
            si_leases, false, critter_is_dead(pc), &other);
        si_leases[slot].active = true;
        if (r == COOP_SUBINST_LEASE_OK) {
            packed = si_pack_map(c->dest_map, &wire, &wire_len) == COOP_SUBINST_OK;
        }
        if (coop_subinst_sub_next_plan(r, packed) == COOP_SUBINST_NEXT_GRANT) {
            si_send_ack(slot, lease_id, COOP_SUBINST_F_NEXT);
            si_slot_line(slot, false, lease_map);
            if (si_grant_packed(slot, c->dest_map, c->dest_loc, true, wire, wire_len) == 0) {
                return;
            }
            /* PC gone meanwhile: the client waits in REQUESTED - tell it. */
            {
                PacketCoopSubinstCtrl ab;

                si_ctrl_init(&ab, COOP_SUBINST_OP_ABORT);
                ab.slot = slot;
                si_send(&ab, sizeof(ab), slot);
            }
            si_host_release(slot, 0, "next lease failed");
            return;
        }
        free(wire);
        if (r == COOP_SUBINST_LEASE_DENY_BUSY) {
            si_deny(slot, COOP_SUBINST_DENY_BUSY, other, c->dest_map);
        } else if (r == COOP_SUBINST_LEASE_OK) {
            si_deny(slot, COOP_SUBINST_DENY_TOO_BIG, -1, c->dest_map);
        }
        si_send_ack(slot, lease_id, 0); /* the client is still COMMITTING: this ends it */
        si_host_release(slot, 0, "commit, next map not leased");
        return;
    }
    si_send_ack(slot, lease_id, 0);
    si_host_release(slot, 0, "commit to another map");
}

static void si_host_handle_ctrl(PacketCoopSubinstCtrl* pkt, int slot)
{
    if (slot <= 0 || slot >= 8) {
        return;
    }
    if (pkt->op == COOP_SUBINST_OP_CANCEL) {
        if (si_leases[slot].active && pkt->lease_id == si_leases[slot].lease_id) {
            si_host_release(slot, 0, "client gave the lease back");
        }
        return;
    }
    if (pkt->op != COOP_SUBINST_OP_COMMIT_BEGIN) {
        return;
    }
    if (!si_leases[slot].active || pkt->lease_id != si_leases[slot].lease_id) {
        tig_debug_printf("CoopSubinst: stale commit from slot %d (lease %u)\n", slot, pkt->lease_id);
        return;
    }
    if (!coop_subinst_rx_begin(&si_host_rx[slot], pkt->lease_id, pkt->total_chunks, (size_t)pkt->total_bytes,
            COOP_SUBINST_MAX_PAYLOAD)) {
        tig_debug_printf("CoopSubinst: commit from slot %d refused (size %d)\n", slot, pkt->total_bytes);
        return;
    }
    si_host_commit[slot] = *pkt;
}

static void si_host_handle_chunk(PacketCoopSubinstChunk* pkt, int slot)
{
    int r;

    if (slot <= 0 || slot >= 8 || !si_leases[slot].active) {
        return;
    }
    r = coop_subinst_rx_accept(&si_host_rx[slot], pkt->lease_id, pkt->chunk_idx, pkt->chunk_count, pkt->data_len,
        pkt->data);
    if (r == COOP_SUBINST_RX_DONE) {
        si_host_apply(slot);
    } else if (r == COOP_SUBINST_RX_BAD) {
        tig_debug_printf("CoopSubinst: commit stream from slot %d broken - dropped\n", slot);
    }
}

void coop_subinst_handle_delta(PacketCoopSubinstDelta* pkt)
{
    int slot;
    int64_t pc;

    if (pkt == NULL || !si_host()) {
        return;
    }
    slot = sub_52A530();
    if (slot <= 0 || slot >= 8 || !si_leases[slot].active || pkt->lease_id != si_leases[slot].lease_id) {
        return; /* only the leased slot, only its current lease */
    }
    pc = sub_4A2B60(slot);
    if (pc == OBJ_HANDLE_NULL) {
        return;
    }
    switch (pkt->kind) {
    case COOP_SUBINST_DELTA_GF:
        script_global_flag_set(pkt->index, pkt->val);
        break;
    case COOP_SUBINST_DELTA_GV:
        script_global_var_set(pkt->index, pkt->val);
        break;
    case COOP_SUBINST_DELTA_QUEST:
        quest_state_set_internal(pc, pkt->index, pkt->val, OBJ_HANDLE_NULL); /* no XP: that comes as XP */
        break;
    case COOP_SUBINST_DELTA_RUMOR:
        if (pkt->val == 1) {
            rumor_qstate_set(pkt->index);
        } else {
            rumor_known_set(pc, pkt->index);
        }
        break;
    case COOP_SUBINST_DELTA_REP:
        if (pkt->val > 0) {
            reputation_add(pc, pkt->index);
        } else {
            reputation_remove(pc, pkt->index);
        }
        break;
    case COOP_SUBINST_DELTA_AREA:
        area_set_known(pc, pkt->index);
        break;
    case COOP_SUBINST_DELTA_XP:
        if (!critter_is_dead(pc)) {
            critter_award_xp(pc, coop_middle_host_scale_xp(pkt->val)); /* the soloist's own (no split) */
        }
        break;
    case COOP_SUBINST_DELTA_FATE:
        if (pkt->index < FATE_COUNT) {
            if (pkt->val == 1) {
                fate_activate(pc, pkt->index);
            } else {
                fate_resolve(pc, pkt->index);
            }
        }
        break;
    default:
        break;
    }
}

void coop_subinst_host_prepare_party_travel(int dest_map)
{
    PacketCoopSubinstCtrl pkt;
    unsigned int start;
    int owner;
    int64_t loc;

    if (!si_host() || !si_rule() || dest_map <= 0 || !map_is_valid() || dest_map == map_current_map()) {
        return;
    }
    owner = coop_subinst_lease_owner(si_leases, dest_map);
    if (owner <= 0 || owner >= 8 || !si_leases[owner].active) {
        return;
    }
    /* Fresh map files, inventory and last_loc before the host opens dest_map. */
    if (tig_net_client_is_active(owner)) {
        si_ckpt_done_mask &= ~(1u << owner);
        si_ctrl_init(&pkt, COOP_SUBINST_OP_CHECKPOINT_REQ);
        pkt.slot = owner;
        pkt.lease_id = si_leases[owner].lease_id;
        si_send(&pkt, sizeof(pkt), owner);
        start = si_now();
        while ((si_ckpt_done_mask & (1u << owner)) == 0
            && (unsigned int)(si_now() - start) < COOP_SUBINST_SAVE_WAIT_MS) {
            tig_net_pump();
            SDL_Delay(5);
        }
        tig_debug_printf("CoopSubinst: merge: slot %d checkpoint %s in %u ms\n", owner,
            (si_ckpt_done_mask & (1u << owner)) != 0 ? "ok" : "timeout",
            (unsigned int)(si_now() - start));
    }
    loc = si_leases[owner].last_loc;
    si_merge_loc[owner] = loc;
    si_merge_map[owner] = dest_map;
    si_ctrl_init(&pkt, COOP_SUBINST_OP_ABORT);
    pkt.slot = owner;
    pkt.lease_id = si_leases[owner].lease_id;
    si_send(&pkt, sizeof(pkt), owner);
    /* Unpark in place on the old map: the party carry then takes the PC to dest_map. */
    si_host_release(owner, 0, "host merged into lease");
    tig_debug_printf("CoopSubinst: merge: slot %d stays at %d,%d on map %d\n", owner,
        (int)LOCATION_GET_X(loc), (int)LOCATION_GET_Y(loc), dest_map);
}

int64_t coop_subinst_take_merge_loc(int slot)
{
    int64_t loc;

    if (slot <= 0 || slot >= 8 || si_merge_map[slot] != map_current_map()) {
        return 0;
    }
    loc = si_merge_loc[slot];
    si_merge_loc[slot] = 0;
    si_merge_map[slot] = 0;
    return loc;
}

void coop_subinst_host_abort_all(const char* why)
{
    PacketCoopSubinstCtrl pkt;
    int slot;

    for (slot = 1; slot < 8; slot++) {
        if (!si_leases[slot].active) {
            continue;
        }
        si_ctrl_init(&pkt, COOP_SUBINST_OP_ABORT);
        pkt.slot = slot;
        pkt.lease_id = si_leases[slot].lease_id;
        si_send(&pkt, sizeof(pkt), slot);
        si_host_release(slot, 0, why);
    }
}

void coop_subinst_host_before_save(void)
{
    PacketCoopSubinstCtrl pkt;
    unsigned int want = 0;
    unsigned int start;
    int slot;

    if (!si_host()) {
        return;
    }
    for (slot = 1; slot < 8; slot++) {
        if (si_leases[slot].active && tig_net_client_is_active(slot)) {
            si_ctrl_init(&pkt, COOP_SUBINST_OP_CHECKPOINT_REQ);
            pkt.slot = slot;
            pkt.lease_id = si_leases[slot].lease_id;
            si_send(&pkt, sizeof(pkt), slot);
            want |= 1u << slot;
        }
    }
    if (want == 0) {
        return;
    }
    si_ckpt_done_mask = 0;
    start = si_now();
    while ((si_ckpt_done_mask & want) != want && (unsigned int)(si_now() - start) < COOP_SUBINST_SAVE_WAIT_MS) {
        tig_net_pump();
        SDL_Delay(5);
    }
    tig_debug_printf("CoopSubinst: save: checkpoints %02x of %02x in %u ms\n", si_ckpt_done_mask & want, want,
        (unsigned int)(si_now() - start));
}

void coop_subinst_slot_gone(int slot)
{
    if (slot <= 0 || slot >= 8) {
        return;
    }
    if (si_leases[slot].active && tig_net_is_host()) {
        int64_t pc = sub_4A2B60(slot);
        CoopSubinstSection cmap;
        CoopSubinstSection bundle;
        CoopSubinstSection qty;

        /* The map files of the last checkpoint are on disk already; the
           PC takes that checkpoint's pack and vitals, then parks. */
        if (si_ckpt[slot] != NULL && pc != OBJ_HANDLE_NULL
            && coop_subinst_payload_split(si_ckpt[slot], si_ckpt_len[slot], &cmap, &bundle, &qty)) {
            static CoopSubinstQty items[COOP_SUBINST_MAX_QTY_ITEMS];
            int n = qty.len > 0 ? coop_subinst_qty_parse(qty.data, qty.len, items, COOP_SUBINST_MAX_QTY_ITEMS) : 0;

            coop_lite_host_pc_reconcile(pc, bundle.data, bundle.len, items, n > 0 ? n : 0,
                object_hp_damage_get(pc), critter_fatigue_damage_get(pc));
        }
        si_host_release(slot, 0, "disconnected");
        si_resync_mask &= ~(1u << slot);
    }
    si_set_inside(slot, 0, 0, 0);
}

void coop_subinst_map_changed(void)
{
    int slot;

    if (!tig_net_is_host()) {
        return; /* a client's own map changes are its business */
    }
    for (slot = 1; slot < 8; slot++) {
        if (si_leases[slot].active) {
            PacketCoopSubinstCtrl pkt;

            /* A load / map change the party carry did not see first. */
            si_ctrl_init(&pkt, COOP_SUBINST_OP_ABORT);
            pkt.slot = slot;
            pkt.lease_id = si_leases[slot].lease_id;
            si_send(&pkt, sizeof(pkt), slot);
            si_hid[slot].count = 0; /* the old map's objects are gone */
            si_lease_free(slot);
            si_state_send(slot);
            si_resync_mask |= 1u << slot;
            si_resync_since[slot] = si_now();
            tig_debug_printf("CoopSubinst: slot %d lease aborted (host map change / load)\n", slot);
        }
    }
}

/* ---------------------------------------------------------------- client */

bool coop_subinst_client_active(void)
{
    return si_cl_active;
}

static void si_client_leave(const char* why)
{
    if (si_cl_active) {
        si_cl_active = false;
        tig_net_set_local_authority(false);
        if (si_cl_in_pump) {
            si_cl_left_in_pump = true; /* coop_subinst_ping drops our suspend level */
        } else {
            tig_net_local_suspend_end();
        }
        tig_debug_printf("CoopSubinst: left the solo instance (%s)\n", why);
    }
    coop_subinst_rx_free(&si_cl_rx);
    si_cl_state = COOP_SUBINST_ST_IDLE;
    si_cl_lease = 0;
    si_cl_enter_pending = false;
    si_cl_ckpt_req = false;
    si_cl_ping_ms = 0;
    si_cl_dead_ms = 0;
    si_cl_unhide_pending = false;
    si_cl_entered = false;
}

static void si_client_event(int ev)
{
    int next = coop_subinst_client_next(si_cl_state, ev);

    if (next >= 0) {
        if (next != si_cl_state) {
            si_cl_state_ms = si_now();
        }
        si_cl_state = next;
    }
}

/* The lease goes back before we got in (archive bad, enter failed, stalled):
   the host unparks our PC and resyncs us; our local authority ends now. */
static void si_client_cancel(uint32_t lease_id, const char* why)
{
    PacketCoopSubinstCtrl pkt;

    if (lease_id != 0) {
        si_ctrl_init(&pkt, COOP_SUBINST_OP_CANCEL);
        pkt.slot = sub_529520();
        pkt.lease_id = lease_id;
        si_send(&pkt, sizeof(pkt), 0);
    }
    tig_debug_printf("CoopSubinst: lease %u given back (%s)\n", lease_id, why);
    si_client_leave(why);
    coop_lite_mid_notice(coop_lite_lang_ru() ? "Не удалось войти в отдельную локацию - возвращаемся к группе."
                                             : "Could not enter the solo instance - back to the party.",
        false);
}

static void si_client_deny(const PacketCoopSubinstCtrl* pkt)
{
    char text[256];
    bool ru = coop_lite_lang_ru();

    switch (pkt->reason) {
    case COOP_SUBINST_DENY_BUSY: {
        char name[64];

        coop_middle_slot_name(pkt->slot, name, sizeof(name));
        snprintf(text, sizeof(text),
            ru ? "%s уже находится в %s. Подождите или попросите хоста о переходе всей группой."
               : "%s is already inside %s. Wait or ask the host for group travel.",
            name, si_map_name(pkt->map_id));
        break;
    }
    case COOP_SUBINST_DENY_STATE:
        snprintf(text, sizeof(text), "%s",
            ru ? "Нельзя войти одному: идёт бой или персонаж мёртв." : "Can't go in alone: combat is on or you are dead.");
        break;
    case COOP_SUBINST_DENY_TOO_BIG:
        snprintf(text, sizeof(text), "%s",
            ru ? "Эта локация слишком велика для отдельного входа - нужен переход всей группой."
               : "This location is too big for a solo instance - group travel is needed.");
        break;
    default:
        snprintf(text, sizeof(text), "%s",
            ru ? "Отдельные локации выключены правилами сессии." : "Solo interiors are off in this session's rules.");
        break;
    }
    coop_lite_mid_notice(text, false);
}

/* Map state flushed to disk without our PC, its pack and its followers
   (they leave the map with us, as a vanilla teleport records them) and the
   folder packed. The archive keeps this flush's mobile.des as it is (static
   things we carry out stay gone from the map, like a vanilla teleport);
   our local copy drops only our party's ids again (we are still here) and
   keeps every real destruction. */
static bool si_client_pack(uint8_t** wire, size_t* wire_len)
{
    static int64_t objs[SI_PARTY_MAX];
    static unsigned int flags[SI_PARTY_MAX];
    int64_t pc = player_get_local_pc_obj();
    ObjectList followers;
    ObjectNode* node;
    char dir[TIG_MAX_PATH];
    char des[TIG_MAX_PATH];
    static uint8_t oids[SI_PARTY_MAX][24];
    uint8_t* des_data = NULL;
    uint32_t des_len = 0;
    int n = 0;
    int i;
    int rc;

    *wire = NULL;
    *wire_len = 0;
    if (pc == OBJ_HANDLE_NULL || !map_is_valid() || map_current_map() != si_cl_map
        || !si_map_dir(si_cl_map, dir, sizeof(dir))) {
        return false;
    }
    /* PC, followers, and everything they carry. */
    coop_lite_pc_inv_repair(pc);
    objs[n++] = pc;
    object_list_all_followers(pc, &followers);
    for (node = followers.head; node != NULL && n < SI_PARTY_MAX; node = node->next) {
        objs[n++] = node->obj;
    }
    object_list_destroy(&followers);
    for (i = 0; i < n && n < SI_PARTY_MAX; i++) {
        int k;
        int cnt;

        if (!obj_type_is_critter(obj_field_int32_get(objs[i], OBJ_F_TYPE))) {
            continue;
        }
        cnt = obj_arrayfield_length_get(objs[i], OBJ_F_CRITTER_INVENTORY_LIST_IDX);
        for (k = 0; k < cnt && n < SI_PARTY_MAX; k++) {
            int64_t item = obj_arrayfield_handle_get(objs[i], OBJ_F_CRITTER_INVENTORY_LIST_IDX, k);

            if (item != OBJ_HANDLE_NULL) {
                objs[n++] = item;
            }
        }
    }
    snprintf(des, sizeof(des), "%s\\mobile.des", dir);
    for (i = 0; i < n; i++) {
        ObjectID oid = obj_get_id(objs[i]);

        memset(oids[i], 0, sizeof(oids[i]));
        memcpy(oids[i], &oid, sizeof(oid) < sizeof(oids[i]) ? sizeof(oid) : sizeof(oids[i]));
        flags[i] = (unsigned int)obj_field_int32_get(objs[i], OBJ_F_FLAGS);
        obj_field_int32_set(objs[i], OBJ_F_FLAGS, (int)(flags[i] | OF_EXTINCT | OF_DESTROYED));
    }
    map_flush(1);
    for (i = 0; i < n; i++) {
        obj_field_int32_set(objs[i], OBJ_F_FLAGS, (int)flags[i]);
    }
    rc = si_pack_map(si_cl_map, wire, wire_len);
    if (si_read_file(des, &des_data, &des_len) && des_data != NULL) {
        size_t keep = coop_subinst_des_filter(des_data, des_len, (const uint8_t(*)[24])oids, n);

        if (keep != des_len) {
            si_write_file(des, des_data, (uint32_t)keep);
        }
    }
    free(des_data);
    return rc == COOP_SUBINST_OK;
}

static bool si_client_commit(int dest_map, int64_t dest_loc, bool checkpoint, bool dead)
{
    PacketCoopSubinstCtrl pkt;
    CoopCharBundleBuf bundle;
    static CoopSubinstQty qty[COOP_SUBINST_MAX_QTY_ITEMS];
    static uint8_t qty_buf[COOP_SUBINST_MAX_QTY];
    CoopSubinstSection s_cmap;
    CoopSubinstSection s_bundle;
    CoopSubinstSection s_qty;
    uint8_t* wire = NULL;
    size_t wire_len = 0;
    uint8_t* payload;
    size_t payload_len = 0;
    int64_t pc = player_get_local_pc_obj();
    int qty_n = 0;
    int cnt;
    int i;

    if (pc == OBJ_HANDLE_NULL || si_cl_lease == 0) {
        return false;
    }
    if (!si_client_pack(&wire, &wire_len)) {
        tig_debug_printf("CoopSubinst: %s not built (map pack failed)\n", checkpoint ? "checkpoint" : "commit");
        return false;
    }
    memset(&bundle, 0, sizeof(bundle));
    if (!coop_lite_build_pc_bundle(pc, &bundle) || bundle.size > COOP_SUBINST_MAX_BUNDLE) {
        tig_debug_printf("CoopSubinst: character bundle not sent (%u bytes)\n", (unsigned int)bundle.size);
        coop_char_bundle_free(&bundle);
        memset(&bundle, 0, sizeof(bundle));
    }
    cnt = obj_arrayfield_length_get(pc, OBJ_F_CRITTER_INVENTORY_LIST_IDX);
    for (i = 0; i < cnt && qty_n < COOP_SUBINST_MAX_QTY_ITEMS; i++) {
        int64_t item = obj_arrayfield_handle_get(pc, OBJ_F_CRITTER_INVENTORY_LIST_IDX, i);
        ObjectID oid;
        int type;

        if (item == OBJ_HANDLE_NULL) {
            continue;
        }
        type = obj_field_int32_get(item, OBJ_F_TYPE);
        if (!obj_type_is_item(type)) {
            continue;
        }
        oid = obj_get_id(item);
        memset(&qty[qty_n], 0, sizeof(qty[qty_n]));
        memcpy(qty[qty_n].oid, &oid, sizeof(qty[qty_n].oid));
        qty[qty_n].qty = type == OBJ_TYPE_GOLD ? obj_field_int32_get(item, OBJ_F_GOLD_QUANTITY)
            : type == OBJ_TYPE_AMMO           ? obj_field_int32_get(item, OBJ_F_AMMO_QUANTITY)
                                              : COOP_SUBINST_QTY_NONE;
        qty[qty_n].hp_damage = obj_field_int32_get(item, OBJ_F_HP_DAMAGE);
        qty[qty_n].charges = obj_field_int32_get(item, OBJ_F_ITEM_SPELL_MANA_STORE);
        qty[qty_n].mana = obj_field_int32_get(item, OBJ_F_ITEM_MANA_STORE);
        qty[qty_n].inv_loc = obj_field_int32_get(item, OBJ_F_ITEM_INV_LOCATION);
        if (qty[qty_n].qty >= COOP_SUBINST_QTY_NONE && qty[qty_n].qty <= COOP_SUBINST_ITEM_VAL_MAX
            && qty[qty_n].hp_damage >= 0 && qty[qty_n].hp_damage <= COOP_SUBINST_ITEM_VAL_MAX
            && qty[qty_n].charges >= -COOP_SUBINST_ITEM_VAL_MAX && qty[qty_n].charges <= COOP_SUBINST_ITEM_VAL_MAX
            && qty[qty_n].mana >= -COOP_SUBINST_ITEM_VAL_MAX && qty[qty_n].mana <= COOP_SUBINST_ITEM_VAL_MAX
            && qty[qty_n].inv_loc >= 0 && qty[qty_n].inv_loc <= COOP_SUBINST_ITEM_VAL_MAX) {
            qty_n++;
        }
    }
    s_cmap.data = wire;
    s_cmap.len = wire_len;
    s_bundle.data = bundle.data;
    s_bundle.len = bundle.size;
    s_qty.data = qty_buf;
    s_qty.len = coop_subinst_qty_build(qty, qty_n, qty_buf, sizeof(qty_buf));
    payload = (uint8_t*)malloc(coop_subinst_payload_size(s_cmap.len, s_bundle.len, s_qty.len));
    if (payload == NULL
        || !coop_subinst_payload_build(s_cmap, s_bundle, s_qty, payload,
            coop_subinst_payload_size(s_cmap.len, s_bundle.len, s_qty.len), &payload_len)) {
        free(payload);
        free(wire);
        coop_char_bundle_free(&bundle);
        return false;
    }
    si_ctrl_init(&pkt, COOP_SUBINST_OP_COMMIT_BEGIN);
    pkt.slot = sub_529520();
    pkt.map_id = si_cl_map;
    pkt.lease_id = si_cl_lease;
    pkt.dest_map = dest_map > 0 ? dest_map : 0;
    pkt.dest_loc = coop_subinst_commit_dest_loc(checkpoint,
        obj_field_int64_get(pc, OBJ_F_LOCATION), dest_loc);
    pkt.is_checkpoint = checkpoint ? 1 : 0;
    pkt.total_bytes = (int)payload_len;
    pkt.total_chunks = coop_subinst_chunk_count(payload_len);
    pkt.hp_damage = object_hp_damage_get(pc);
    pkt.fatigue_damage = critter_fatigue_damage_get(pc);
    if (pkt.hp_damage < 0 || pkt.hp_damage > COOP_SUBINST_MAX_DAMAGE) {
        pkt.hp_damage = 0;
    }
    if (pkt.fatigue_damage < 0 || pkt.fatigue_damage > COOP_SUBINST_MAX_DAMAGE) {
        pkt.fatigue_damage = 0;
    }
    pkt.flags = dead ? COOP_SUBINST_F_DEAD : 0;
    si_send(&pkt, sizeof(pkt), 0);
    si_send_payload(si_cl_lease, payload, payload_len, 0);
    tig_debug_printf("CoopSubinst: %s sent (map %d -> %d, %u bytes)\n", checkpoint ? "checkpoint" : "commit",
        si_cl_map, dest_map, (unsigned int)payload_len);
    free(payload);
    free(wire);
    coop_char_bundle_free(&bundle);
    si_cl_ckpt_ms = si_now();
    if (!checkpoint) {
        si_client_event(COOP_SUBINST_EV_COMMIT);
    }
    return true;
}

bool coop_subinst_client_travel(const TeleportData* td)
{
    if (!si_cl_active || td == NULL) {
        return false;
    }
    if (td->obj != player_get_local_pc_obj() || td->map <= 0 || (map_is_valid() && td->map == map_current_map())) {
        return false; /* NPCs, and moves inside this map, stay local */
    }
    if (si_cl_state != COOP_SUBINST_ST_ACTIVE || !si_cl_entered) {
        return true; /* a transfer is running: wait */
    }
    if (si_client_commit(td->map, td->loc, false, false)) {
        coop_lite_mid_notice(coop_lite_lang_ru() ? "Сохраняем локацию и возвращаемся к группе..."
                                                 : "Saving the location and rejoining the party...",
            true);
    } else {
        coop_lite_mid_notice(coop_lite_lang_ru() ? "Не удалось сохранить локацию - попробуйте ещё раз."
                                                 : "Could not save the location - try again.",
            false);
    }
    return true;
}

bool coop_subinst_client_death(void)
{
    if (!si_cl_active) {
        return false;
    }
    if (si_cl_dead_ms == 0) {
        si_cl_dead_ms = si_now();
        tig_debug_printf("CoopSubinst: our PC died inside - the host gets the body\n");
    }
    return true;
}

static void si_client_handle_ctrl(PacketCoopSubinstCtrl* pkt)
{
    switch (pkt->op) {
    case COOP_SUBINST_OP_STATE:
        si_set_inside(pkt->slot, pkt->map_id, pkt->dest_map, pkt->dest_loc);
        return;
    case COOP_SUBINST_OP_LEASE_GRANT:
        if (coop_subinst_client_next(si_cl_state, COOP_SUBINST_EV_GRANT) < 0) {
            tig_debug_printf("CoopSubinst: GRANT ignored in state %d\n", si_cl_state);
            return;
        }
        if (!coop_subinst_rx_begin(&si_cl_rx, pkt->lease_id, pkt->total_chunks, (size_t)pkt->total_bytes,
                COOP_SUBINST_MAX_WIRE)) {
            return;
        }
        if (!si_cl_active) {
            si_cl_host_map = map_is_valid() ? map_current_map() : 0;
        }
        si_cl_lease = pkt->lease_id;
        si_cl_map = pkt->map_id;
        si_cl_loc = pkt->dest_loc;
        si_client_event(COOP_SUBINST_EV_GRANT);
        tig_debug_printf("CoopSubinst: lease %u for map %d - receiving %d bytes\n", pkt->lease_id, pkt->map_id,
            pkt->total_bytes);
        return;
    case COOP_SUBINST_OP_LEASE_DENY:
        si_client_deny(pkt);
        if (si_cl_active && si_cl_state == COOP_SUBINST_ST_REQUESTED) {
            /* (older host) sub -> sub refused after the ACK: the host resyncs us. */
            si_client_leave("next map denied");
        } else {
            si_client_event(COOP_SUBINST_EV_DENY); /* COMMITTING stays: the ACK(0) follows */
        }
        return;
    case COOP_SUBINST_OP_CHECKPOINT_REQ:
        if (si_cl_active && pkt->lease_id == si_cl_lease) {
            si_cl_ckpt_req = true; /* sent from coop_subinst_ping, outside the pump */
        }
        return;
    case COOP_SUBINST_OP_COMMIT_ACK:
        if (pkt->lease_id != si_cl_lease || si_cl_state != COOP_SUBINST_ST_COMMITTING) {
            return; /* stale */
        }
        if ((pkt->flags & COOP_SUBINST_F_NEXT) != 0) {
            si_client_event(COOP_SUBINST_EV_ACK_NEXT);
            si_cl_lease = 0;
        } else {
            si_client_leave("commit acknowledged");
        }
        return;
    case COOP_SUBINST_OP_ABORT:
        if (si_cl_state == COOP_SUBINST_ST_IDLE && !si_cl_active) {
            return;
        }
        if (pkt->lease_id != 0 && si_cl_lease != 0 && pkt->lease_id != si_cl_lease) {
            return; /* an older lease */
        }
        si_client_leave("host aborted");
        coop_lite_mid_notice(coop_lite_lang_ru() ? "Хост сменил карту или загрузился - возвращаемся к группе."
                                                 : "The host changed map or loaded - back to the party.",
            false);
        return;
    default:
        return;
    }
}

static void si_client_handle_chunk(PacketCoopSubinstChunk* pkt)
{
    int r;

    if (si_cl_state != COOP_SUBINST_ST_RECEIVING) {
        return;
    }
    r = coop_subinst_rx_accept(&si_cl_rx, pkt->lease_id, pkt->chunk_idx, pkt->chunk_count, pkt->data_len, pkt->data);
    if (r == COOP_SUBINST_RX_BAD) {
        si_client_cancel(si_cl_lease, "map archive stream broken");
        return;
    }
    if (r != COOP_SUBINST_RX_DONE) {
        return;
    }
    if (!si_unpack_map(si_cl_rx.data, si_cl_rx.total, si_cl_map)) {
        si_client_cancel(si_cl_lease, "map archive refused");
        return;
    }
    coop_subinst_rx_free(&si_cl_rx);
    if (!si_cl_active) {
        si_cl_active = true;
        tig_net_local_suspend_begin(); /* local authority from here on */
    }
    tig_net_set_local_authority(true); /* AI, doors, loot, traps, dialogs, story: ours */
    si_client_event(COOP_SUBINST_EV_ARCHIVE_OK);
    si_cl_enter_pending = true;
    si_cl_entered = false;
    si_cl_enter_ms = si_now();
    si_cl_ckpt_ms = si_now();
    si_cl_ping_ms = 0; /* M7 review: no delta from an earlier instance */
    si_cl_dead_ms = 0;
    si_cl_unhide_pending = true;
    tig_debug_printf("CoopSubinst: entering map %d alone\n", si_cl_map);
}

void coop_subinst_handle_ctrl(PacketCoopSubinstCtrl* pkt)
{
    if (pkt == NULL || !tig_net_is_active() || !tig_net_is_coop_enabled()) {
        return;
    }
    if (tig_net_is_host()) {
        si_host_handle_ctrl(pkt, sub_52A530());
    } else {
        si_client_handle_ctrl(pkt);
    }
}

void coop_subinst_handle_chunk(PacketCoopSubinstChunk* pkt)
{
    if (pkt == NULL || !tig_net_is_active() || !tig_net_is_coop_enabled()) {
        return;
    }
    if (tig_net_is_host()) {
        si_host_handle_chunk(pkt, sub_52A530());
    } else {
        si_client_handle_chunk(pkt);
    }
}

bool coop_subinst_packet_filter(int type, const void* msg)
{
    if (!si_cl_active || tig_net_is_host()) {
        return true;
    }
    switch (type) {
    case PACKET_COOP_SUBINST_CTRL:
    case PACKET_COOP_SUBINST_CHUNK:
    case PACKET_COOP_CHAT:
    case PACKET_COOP_RULES:
    case 2: /* player list */
    case 3: /* game time */
    case 68: /* lagging */
    case 69: /* recovered */
        return true;
    case PACKET_COOP_PARTY_CTRL: {
        int op = ((const PacketCoopPartyCtrl*)msg)->op;

        return op == COOP_CTRL_PAUSE || op == COOP_CTRL_SLEEP_STATE || op == COOP_CTRL_SLOT_STATUS
            || op == COOP_CTRL_SLEEP_APPLY; /* M7 */
    }
    default:
        return false; /* the host's map: not ours while inside */
    }
}

void coop_subinst_note(int kind, int index, int val)
{
    PacketCoopSubinstDelta pkt;

    if (!si_cl_active || si_cl_lease == 0 || si_cl_state != COOP_SUBINST_ST_ACTIVE) {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_COOP_SUBINST_DELTA;
    pkt.lease_id = si_cl_lease;
    pkt.kind = kind;
    pkt.index = index;
    pkt.val = val;
    if (!coop_subinst_delta_valid(&pkt, (int)sizeof(pkt))) {
        return;
    }
    si_send(&pkt, sizeof(pkt), 0);
}

void coop_subinst_note_xp(int64_t obj, int xp)
{
    if (si_cl_active && obj != OBJ_HANDLE_NULL && obj == player_get_local_pc_obj() && xp > 0) {
        coop_subinst_note(COOP_SUBINST_DELTA_XP, 0, xp > COOP_SUBINST_XP_MAX ? COOP_SUBINST_XP_MAX : xp);
    }
}

void coop_subinst_ping(unsigned int now_ms)
{
    int depth;

    depth = tig_net_local_suspend_pause();
    si_cl_in_pump = true;
    tig_net_pump();
    /* The Middle client ticks (pause / sleep banners, F9 HUD, status
       badges) and the launcher lobby keep running inside. */
    coop_middle_tick(now_ms);
    coop_lite_lobby_tick(now_ms);
    si_cl_in_pump = false;
    if (si_cl_left_in_pump) {
        si_cl_left_in_pump = false;
        depth = depth > 0 ? depth - 1 : 0;
    }
    tig_net_local_suspend_resume(depth);
    /* M7: a party sleep's heal runs here, with local authority. */
    coop_middle_subinst_local_tick();
    if (!si_cl_active) {
        return;
    }
    if (coop_subinst_client_stalled(si_cl_state, si_cl_active, si_cl_state_ms, si_now())) {
        si_client_cancel(si_cl_lease, "no map from the host");
        return;
    }
    if (si_cl_state == COOP_SUBINST_ST_ACTIVE && !si_cl_entered) {
        if (map_is_valid() && map_current_map() == si_cl_map) {
            si_cl_entered = true;
            si_cl_enter_pending = false;
        } else if ((unsigned int)(si_now() - si_cl_enter_ms) > SI_ENTER_GIVEUP_MS) {
            si_client_cancel(si_cl_lease, "could not enter the map");
            return;
        } else if (si_cl_enter_pending && coop_lite_client_subinst_teleport(si_cl_map, si_cl_loc)) {
            si_cl_enter_pending = false;
            tig_debug_printf("CoopSubinst: teleport into map %d queued\n", si_cl_map);
        }
        if (!si_cl_entered) {
            return;
        }
    }
    if (si_cl_state != COOP_SUBINST_ST_ACTIVE || !map_is_valid() || map_current_map() != si_cl_map) {
        return;
    }
    if (si_cl_unhide_pending) {
        /* The host parked our PC (hidden, invulnerable) on its map and that
           may have reached us before the archive: not here. */
        int64_t pc = player_get_local_pc_obj();
        ObjectList followers;
        ObjectNode* node;

        si_cl_unhide_pending = false;
        if (pc != OBJ_HANDLE_NULL) {
            object_flags_unset(pc, SI_HIDE_FLAGS);
            object_list_all_followers(pc, &followers);
            for (node = followers.head; node != NULL; node = node->next) {
                object_flags_unset(node->obj, SI_HIDE_FLAGS);
            }
            object_list_destroy(&followers);
        }
    }
    if (si_cl_dead_ms != 0) {
        if ((unsigned int)(si_now() - si_cl_dead_ms) >= SI_DEATH_COMMIT_MS) {
            si_client_commit(si_cl_host_map, 0, false, true);
        }
        return;
    }
    /* M7: no timed checkpoint during the host's pause (an asked one still goes). */
    if (coop_subinst_ckpt_due(&si_cl_ckpt_ms, &si_cl_ping_ms, si_now(), coop_middle_is_paused(), si_cl_ckpt_req)) {
        si_cl_ckpt_req = false;
        si_client_commit(si_cl_host_map, 0, true, false);
        si_cl_ckpt_ms = si_now();
    }
}

/* ---------------------------------------------------------------- both */

void coop_subinst_tick(unsigned int now_ms)
{
    int slot;
    unsigned int now = si_now();

    (void)now_ms;
    if (!si_host()) {
        return;
    }
    for (slot = 1; slot < 8; slot++) {
        if (si_leases[slot].active && !tig_net_client_is_active(slot)) {
            coop_subinst_slot_gone(slot); /* missed disconnect */
        }
        if ((si_resync_mask & (1u << slot)) != 0
            && (unsigned int)(now - si_resync_since[slot]) >= SI_RESYNC_DELAY_MS) {
            if (coop_lite_host_resync_slot(slot)
                || (unsigned int)(now - si_resync_since[slot]) >= SI_RESYNC_GIVEUP_MS
                || !tig_net_client_is_active(slot)) {
                si_resync_mask &= ~(1u << slot);
            }
        }
    }
    if ((unsigned int)(now - si_state_ms) >= SI_STATE_MS) {
        si_state_ms = now;
        for (slot = 1; slot < 8; slot++) {
            if (si_leases[slot].active) {
                si_state_send(slot); /* late joiners get the M badge too */
            }
        }
    }
}

void coop_subinst_session_reset(void)
{
    int slot;

    si_client_leave("session reset");
    for (slot = 0; slot < 8; slot++) {
        coop_subinst_rx_free(&si_host_rx[slot]);
        free(si_ckpt[slot]);
        si_ckpt[slot] = NULL;
        si_ckpt_len[slot] = 0;
        si_hid[slot].count = 0;
    }
    memset(si_leases, 0, sizeof(si_leases));
    memset(si_merge_loc, 0, sizeof(si_merge_loc));
    memset(si_merge_map, 0, sizeof(si_merge_map));
    memset(si_inside, 0, sizeof(si_inside));
    memset(si_slot_map, 0, sizeof(si_slot_map));
    memset(si_entry_map, 0, sizeof(si_entry_map));
    memset(si_entry_loc, 0, sizeof(si_entry_loc));
    si_resync_mask = 0;
    si_serial++;
}

#endif /* !COOP_LITE_TEST_STUBS */
