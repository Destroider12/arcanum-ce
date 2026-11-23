# Coop Middle

Coop Middle is the development line after the **Coop Lite 1.0** release. It lives on branch `coop-middle`, which starts from `74a7076f` (tag `coop-lite-v1.0`). Coop Lite itself is maintained separately on branch `coop-lite` (same commit).

| Line | Branch | Protocol (`TIG_NET_PROTOCOL`) |
| --- | --- | --- |
| Coop Lite 1.0 | `coop-lite` (tag `coop-lite-v1.0`) | 52 |
| Coop Middle M1 | `coop-middle` (up to `23e1797d`) | 53 |
| Coop Middle M2 | `coop-middle` | **54** |

Lite and Middle builds refuse each other at the handshake. Every player needs the same build.

## Architecture rule

All Middle code lives in new files:

- `src/game/coop_rules.h/.c`: the session rules contract (pure C, also linked into the launcher and the tests).
- `src/game/coop_middle.h/.c`: the M1/M2 mechanics. Pure helpers come first (headless tests). The game side is compiled out under `COOP_LITE_TEST_STUBS`.
- `src/ui/coop_middle_ui.h/.c` (M2): trade entry points, portrait badges, Alt loot labels, F9 net HUD.

Lite and engine files only get one-line hook calls, so Coop Lite bugfixes merge into `coop-middle` without conflicts:

| File | Hook |
| --- | --- |
| `coop_lite.c` | session reset, SESSION_SYNC sent (rules to the slot), slot gone, map opened, ping tick, party ctrl valid/handle, client chat (system lines), player drop mark, item op 28 (client pickup / container take), packet role + size of `PACKET_COOP_RULES`, a small bridge (notice, Y/N panel, chat line); `coop_lite_host_sync_slot_inven` and `coop_lite_container_is_store` are no longer `static` |
| `anim.c` | `sub_42AA70` ground pickup: take begin / done |
| `inven_ui.c` | `sub_578EA0` inventory transfer (host loot, client loot via Packet81): take begin / done |
| `sleep_ui.c` | client rest button / bed -> rest request; `sleep_ui_coop_rest()` presses "Until Healed" / "8 Hours" for the host |
| `intgame.c` | Y / N keys -> `coop_middle_rest_key()` |
| `multiplayer.c` | dispatch of `PACKET_COOP_RULES` |
| `tig_net.h` | protocol 54; M2: `TigNetHudStats` / `tig_net_get_hud_stats()` |
| **M2** | |
| `coop_lite.c` | `coop_lite_target_transfer_ok` default case -> `coop_middle_trade_transfer_ok`; `coop_lite_target_still_ok` (TRADE sessions use Middle's partner rule) in the session watch and the target transfer; `COOP_BARTER_OPEN_TRADE` branch in `coop_lite_handle_barter`; gift branch: no "You succeed" notice for TRADE + `coop_middle_trade_given` |
| `inven_ui.c` | client open sends `coop_middle_trade_barter_kind()`; host give `coop_middle_host_trade_give()`; take from the partner `coop_middle_trade_refuse_take()` |
| `intgame.c` | Ctrl+LMB on an object -> `coop_middle_ui_trade_try()`; F9 -> `coop_middle_ui_hud_toggle()` |
| `follower_ui.c` | RMB on a teammate portrait -> `coop_middle_ui_trade_try()`; badge draw after each teammate portrait |
| `coop_chat_ui.c` | overlay draw / scroll also call `coop_middle_ui_draw()` / `coop_middle_ui_on_scroll()` (one overlay hook in gamelib) |
| `gameuilib.c` | module `CoopMiddle-UI` (init / reset / exit / resize) |
| `tig_net.c`, `tig_net_transport.h`, `tig_net_enet.c`, `tig_net_steam.c` | HUD stats: last app packet per slot, optional `peer_stats` (ENet RTT / loss; Steam NULL) |

The earlier Middle prototype (protocol 53/54 commits `8132dc3c`..`4e1e5455`: pings, pause, difficulty, quest pocket, town map fog, parallel dialogs, ...) was reverted in one commit before M1. Ideas from it come back stage by stage in the Middle files. **Parallel dialogs are dropped for now** and can return as a separate stage.

## M1: session rules (`CoopRules`, `PACKET_COOP_RULES`)

```c
typedef struct CoopRules {
    uint32_t version;      /* 1 */
    uint32_t flags;        /* COOP_RULE_* */
    uint16_t enemy_hp_pct; /* 100..300, default 100 (reserved for M3) */
    uint16_t xp_mult_pct;  /* 100..250, default 100 (reserved for M3) */
    uint32_t reserved[4];  /* zero */
} CoopRules;               /* 28 bytes, _Static_assert on size / alignment */
```

| Bit | Flag | Default | Env key (launcher ini key) |
| --- | --- | --- | --- |
| 0 | `COOP_RULE_GOLD_SPLIT` | off | `gold` (`gold_split`) |
| 1 | `COOP_RULE_LOOT_FEED` | on | `loot` (`loot_feed`) |
| 2 | `COOP_RULE_REST_VOTE` | on | `rest` (`rest_vote`) |
| 3 | `COOP_RULE_DIRECT_TRADE` (M2) | **on** | `trade` (`direct_trade`) |
| 4 | `COOP_RULE_CLIENT_PAUSE` (reserved) | off | `pause` |
| 5 | `COOP_RULE_NO_FRIENDLY_FIRE_AOE` (M3, reserved) | off | `noffaoe` |

- `coop_rules_sanitize` clamps the percentages, drops unknown flag bits (the reserved ones are kept), zeroes `reserved` and fixes `version`.
- `ARCANUM_COOP_RULES`: `gold=1,loot=1,rest=1,trade=1,hp=100,xp=100` (M2: `trade` is always written; reserved keys only when set). Keys are separated by `,` or `;`, values can be numbers or `on/off/yes/no/true/false`, unknown keys and bad values are ignored, and missing keys keep their defaults. The host reads it once.
- `PACKET_COOP_RULES` (type **167**, 32 bytes, host -> client only): the host sends it to a slot right after SESSION_SYNC (join and reconnect). The client checks the size and type, sanitizes the rules, keeps them for the session, shows the summary in chat, and goes back to the defaults on `coop_lite_session_reset`. A rules packet from a client is dropped by the direction table (and again in the handler).

## M1: gold split (`COOP_RULE_GOLD_SPLIT`, host only)

When a PC (host or client) on the host takes gold:

- from the ground (`sub_42AA70` for the host's PC, item op 28 for a client), or
- from a container or a dead NPC (inventory transfer `sub_578EA0`: the host's loot window and a client's loot Packet81; item op 28 from a container),

the amount is split among **all living, connected** players with `coop_middle_split_gold`. The picker keeps its share plus the remainder, so no gold is ever lost. Other players get theirs with `item_gold_transfer` and an inventory resync (`coop_lite_host_sync_slot_inven`). For 10+ gold, a chat line goes out when the loot feed is on.

Not split:

- buying, selling, gambling (merchants and store containers), gifts between players, stealing, and moves inside a player's own pack;
- gold a player dropped (the host remembers the last 32 dropped piles by OID);
- gold a player put into a container, when it is taken back (per-container amount, cleared on map change);
- when the picker is the only living player.

## M1: loot feed (`COOP_RULE_LOOT_FEED`)

`coop_middle_loot_feed_worthy(type, flags, cost, quest_or_key)` passes keys and key rings, scrolls, written items (notes, books, schematics), quest items (`OIF_WONT_SELL`), unique items (`OIF_UBER`), and anything except gold or ammo that costs **250+**. Junk, cheap consumables and ammo never make a line.

- EN: `[Loot] <Name> picked up: <Item>`, `[Loot] <Name> found <N> gold: split, <M> each`
- RU (`ARCANUM_COOP_LANG=ru`): `[Лут] <Имя> подобрал: <Предмет>`, `[Лут] <Имя> нашёл <N> золота: поровну по <M>`

The host sends these as `PACKET_COOP_CHAT` with a system marker (`reserved = 'MI' << 16 | kind`). Each client builds the text in its own language. The host relay rebuilds every player line with `reserved = 0`, so a client can't fake a system line. Anti-spam: at most **5 lines per second**. The rest of a burst (for example "Take all" on a big chest) collapses into one `[Loot] ... and N more` line.

## M1: client rest request (`COOP_RULE_REST_VOTE`)

Two new ops in `PACKET_COOP_PARTY_CTRL` (no new packet type):

| Op | Dir | `a` |
| --- | --- | --- |
| `COOP_CTRL_REST_ASK` (5) | client -> host | 1 = 8 hours (bed), 2 = until healed (rest button) |
| `COOP_CTRL_REST_REFUSE` (6) | host -> client | reason 0..6: host said N, host in combat, busy, rule off, cancelled, can't rest here, timeout |

- **Client:** a living client outside combat presses the rest button or uses a bed. When the rule is on, it sends `REST_ASK` (at most one per 5 s) and sees "Rest request sent to the host (Y / N)." / «Запрос на отдых отправлен хосту (Y / N).». When the rule is off, the old Lite refusal stays.
- **Host:** if it is not in combat and no other Y/N prompt is up (travel, load, ready check, sleep window), it shows "<Name> wants to rest. Y = rest until healed, N = refuse." (or "Y = rest 8 hours") on the Y/N panel and in the message line.
- **Y:** the host opens its sleep window and presses "Until Healed" (or "8 Hours", also in wait-only towns): the normal party rest with fade and healing for all PCs (PT25). **N** sends `REST_REFUSE`.
- **Auto-cancel** (with `REST_REFUSE` to the asker): the host enters combat, a map change, the asker dies, another Y/N prompt opens, or there is no answer in 60 s. If the asker disconnects, the request is dropped silently.

## M2: direct trade (`COOP_RULE_DIRECT_TRADE`, default on)

A free give between two players, in the inventory window the game uses for pickpocketing (`INVEN_UI_MODE_STEAL`), run as a host-authoritative **TRADE** session.

- **Open:** **Ctrl+LMB** on another player's PC in the world, or **RMB on a teammate portrait** (right-hand column). The partner must be the PC of another active slot, both alive, within **6 tiles**; otherwise "Move closer to trade." / «Подойдите ближе для обмена.». With the rule off, Ctrl+LMB acts as before and the portrait says "Direct trade is turned off by the host.".
- **Wire:** a client sends `PacketCoopBarter` kind `COOP_BARTER_OPEN_TRADE` (5). The host checks `coop_middle_trade_live` (rule, PC of another active slot, both alive, `location_dist <= 6`) and keeps the session as target kind `COOP_TARGET_TRADE` (3); the 1 s session watch ends it when the check fails.
- **Give:** dragging from your own pack onto the partner is always allowed and free (no pickpocket roll): Packet81 `0x01|PLANT` from a client goes through Lite's gift branch (`item_gold_transfer` / `item_ammo_transfer` / `item_transfer`), the host's own give through `coop_middle_host_trade_give`. Both packs are resynced.
- **Take:** dragging from the partner's pack is refused locally with "You can only receive what your teammate gives you." / «Взять можно только то, что игрок передаст сам.», and on the host by `coop_middle_trade_transfer_ok` (only `!from_target`, flags `0x01` or `0x01|PLANT`).
- **Lines:** `[Trade] <From> gave <Item> to <To>` / `[Обмен] <From> передал <Item> игроку <To>`; gold: `[Trade] <From> gave <N> gold to <To>` / `[Обмен] <From> передал <N> золота игроку <To>`. System chat kinds 4 / 5 (`receiver|item`, `receiver|amount`). They go to both players, or to everyone when the loot feed is on.
- Symmetric: host <-> client and client <-> client (both sides run on the host's copies).

## M2: Alt-hold loot labels (local only)

While **LALT / RALT** is held in the world (intgame mode MAIN; no main menu, world map, inventory, dialog, chat input or load), up to **32** labels show:

- ground items (weapon .. generic: gold with its amount, ammo with its count, keys, scrolls, ...);
- non-empty containers (or not yet generated `OCOF_INVEN_SPAWN_ONCE` stock), locked containers as "<Name> (locked)" / «(заперт)»;
- dead NPCs with something to loot.

Collected with `object_list_rect` within **22 tiles** of the PC; visible means `OBJ_F_FLAGS & (OF_INVENTORY|OF_OFF|OF_DESTROYED|OF_DONTDRAW) == 0` and on screen. Ground items come first, then the nearest to the PC or the mouse cursor. `coop_middle_labels_deoverlap` pushes overlapping labels upward (1 px gap) and hides the ones that run out of room. Colours: gold `(240,210,90)` for keys, gold, quest / unique items and written items (schematics, notes); `(215,235,255)` for other items; `(195,175,140)` for containers and corpses. Each label is a solid plate `(16,14,12)` with a thin frame, pre-rendered in one off-screen buffer and blitted into the iso view in the overlay pass after the text floaters (no windows, clicks go to the world). Label areas are invalidated when they change (refresh every 150 ms), on scroll and on Alt release, so no trails are left.

## M2: status badges on teammate portraits

New op in `PACKET_COOP_PARTY_CTRL`:

| Op | Dir | `a` |
| --- | --- | --- |
| `COOP_CTRL_SLOT_STATUS` (7) | client -> host | own bits `INVEN 1`, `DIALOG 2` (on change, and every 5 s) |
| `COOP_CTRL_SLOT_STATUS` (7) | host -> clients | 4 bits per slot (`coop_middle_status_pack`), on change and every 5 s |

Every 250 ms the host merges each slot's reported bits with `POISON 4` (`STAT_POISON_LEVEL > 0`) and `COMBAT 8` (combat mode on). A 14x14 badge (dark square, coloured frame, letter) is drawn in the top-right corner of a teammate portrait, in this priority: **X** dead `(220,60,60)`, **Z** unconscious (fatigue <= 0) `(180,130,230)`, **P** poisoned `(90,220,90)`, **D** dialog `(240,200,80)`, **I** inventory `(100,210,240)`, **C** combat `(240,140,60)`. Dead and unconscious are read locally from the PC. Portraits are redrawn only when a badge changes; no windows are created.

## M2: F9 net HUD (local only)

**F9** toggles one line at the top centre of the iso view (co-op only; F9 does nothing new in single player). It refreshes every 0.5 s from `tig_net_get_hud_stats()`:

```c
typedef struct TigNetHudStats {
    bool active;                /* a co-op session is running */
    bool is_host;
    int active_players;         /* connected slots, this machine included */
    int max_players;            /* 8 */
    bool rtt_known;             /* the transport reports RTT / loss (ENet: yes, Steam: no) */
    int rtt_ms;                 /* client: ENet RTT to the host; host: the worst client */
    int rtt_avg_ms;             /* host: average over clients; client: = rtt_ms */
    int loss_pct;               /* ENet packetLoss of the worst link, % */
    unsigned int since_rx_ms;   /* client: since the host's last app packet; host: the most silent client */
} TigNetHudStats;
```

Line: `Host 3/8 | RTT 45 ms (avg 30) | loss 0% | sync OK` (client: `Client 3/8 | RTT 45 ms | loss 0% | sync OK`; RU: `Хост 3/8 | RTT 45 мс (ср. 30) | потери 0% | синхр. OK`). The line turns orange with **sync LAG N s** when no app packet arrived for more than **6 s**. That is longer than the 5 s status heartbeat, so an idle but connected peer stays OK. The thresholds and fields are easy to change in `coop_middle_ui.c`.

## Launcher

`arcanum-coop-launcher.ini`:

```ini
[rules]
gold_split=0
loot_feed=1
rest_vote=1
direct_trade=1
```

Host mode shows one row of checkboxes (**Split gold**, **Loot feed**, **Rest vote**, **Direct trade**; disabled in Join mode). At start, the host gets `ARCANUM_COOP_RULES` built with `coop_rules_format_env` (the launcher links `coop_rules.c`). Clients don't get it: they receive the rules from the host. The optional self-update on this line looks for `coop-middle-*` tags, so a Middle launcher never installs a Lite (protocol 52) build.

## Log lines

```
CoopMiddle: host rules from ARCANUM_COOP_RULES='...' (N keys): gold=0,loot=1,rest=1,hp=100,xp=100 | Rules: ...
CoopMiddle: rules sent to slot S: gold=...
CoopMiddle: rules received from the host: gold=...
CoopMiddle: rules packet from slot S dropped (host -> client only)
CoopMiddle: rules packet dropped (size N)
CoopMiddle: gold split: slot S took N gold (ground|loot) -> P players, E each, picker +R, given G
CoopMiddle: gold split: slot S took N gold, nobody else alive - kept
CoopMiddle: gold split: slot S picked up N gold a player dropped - not split
CoopMiddle: gold split: N of the gold was put there by a player - not split
CoopMiddle: gold split: transfer to slot S failed
CoopMiddle: loot feed: slot S '<item>' (ground|loot)
CoopMiddle: loot feed: N line(s) collapsed
CoopMiddle: rest ask sent (mode M)
CoopMiddle: rest ask from slot S (mode M) - Y / N prompt
CoopMiddle: rest ask from slot S (mode M) refused: reason R
CoopMiddle: rest of slot S accepted - party rests (until healed|8 hours)
CoopMiddle: rest of slot S accepted, but the sleep window refused (reason 5)
CoopMiddle: rest request of slot S ended: <why> (reason R)
CoopMiddle: rest request refused by the host (reason R)
CoopMiddle: rest op O from slot S dropped (wrong direction)
[CHAT] [Loot] ...
CoopMiddle: trade: window opened with '<name>' (ctrl+click|portrait, D tiles)
CoopMiddle: trade: window with '<name>' did not open
CoopMiddle: trade: slot S asked to open a trade window with slot T: opened|refused (not a live player in range)|refused (rule off)
CoopMiddle: trade: slot S gave '<item>' (type T, qty Q) to slot R
CoopMiddle: trade: host give of '<item>' failed
CoopMiddle: trade: window closed
CoopLite: loot: slot S puts '<item>' into target: given        (Lite's line for a client's give)
CoopMiddle: status: 0x%08x                                    (host, on change)
CoopMiddle: labels: Alt held, N label(s)
CoopMiddle: net HUD on|off (F9)
CoopMiddle: net HUD: <line>                                   (when sync turns LAG / OK)
[CHAT] [Trade] ...
```

## Tests

`ctest` target **`coop_middle`** (`tests/coop_middle_test.c`, headless; also built in the ASan/UBSan tree):

1. `coop_rules_default` / `coop_rules_sanitize`: bounds, unknown bits, reserved bits kept, `reserved[]` zeroed.
2. `coop_rules_format_env` <-> `coop_rules_parse_env` round trip for every flag combination, ini aliases, partial, garbage, overlong and out-of-range strings, truncated output.
3. `PACKET_COOP_RULES`: size 32, type 167 < type limit, min size, role `CLIENT_APPLY`, `coop_lite_packet_allowed` (host -> client only), version check, `TIG_NET_PROTOCOL == 54`.
4. `coop_middle_split_gold`: 1..8 players, no loss (`each * n + rem == amount`), carry, too little to split, overflow saturation.
5. `coop_middle_loot_feed_worthy` (keys, scrolls, written, quest, unique, cost 250 pass; junk, cheap potions, ammo, gold, non-items fail), the 5/s limiter with the collapse count and ms-clock wrap, and the EN/RU line formats.
6. `COOP_CTRL_REST_ASK` / `COOP_CTRL_REST_REFUSE` in `coop_lite_party_ctrl_valid` (modes, reasons, `b == 0`, Lite ops unchanged, unknown ops refused), the 5 s ask limit.
7. M2 trade: `coop_middle_trade_partner_ok` (range 6, other map, rule off, self, NPC, disconnected, dead, bad slots), `coop_middle_trade_transfer_ok` and Lite's `coop_lite_target_transfer_ok` routing (give yes, take / steal bits no), EN/RU `[Trade]` lines (item, gold, missing separator, overlong, tiny buffer).
8. M2 status: `coop_middle_status_pack` / `_get` (nibbles, masking, bounds), badge priority and colours X > Z > P > D > I > C, `COOP_CTRL_SLOT_STATUS` validity (`b == 0`, short packet).
9. M2 labels: `coop_middle_labels_deoverlap` (same spot, side by side, a stack of 32 stays pairwise apart and inside the view, limited room hides the rest, degenerate input, a label below the view is pulled up).

## Not done / risks (M1)

- **Not tested in play yet:** only the build and headless tests.
- The rest prompt uses the Lite travel Y/N panel. If a travel or load prompt opens while a rest request waits, the rest request is cancelled (busy) rather than queued.
- An item the host places itself after a client's loot move failed (the client's grid cell was taken on the host, PT46 retry) gets no feed line. This path is rare.
- `enemy_hp_pct`, `xp_mult_pct` and the reserved flags (`pause`, `noffaoe`) are carried and validated, but have no effect yet.

## Not done / risks (M2)

- **Not tested in play yet:** only the builds (Linux, ASan/UBSan, Win64) and the headless tests. The overlay drawing (labels, HUD, badge letters) has not been checked on screen.
- Trade reuses the pickpocket window, so its title and look are the vanilla pickpocket ones. On open, the game turns the PC to the partner and needs line of sight (`ai_can_see`), as for pickpocketing.
- If the host refuses `OPEN_TRADE` (rare, because the client checks the same rule first), the client's window stays open. Gives are then refused and the packs resynced.
- Gold is given as the whole dragged pile (the vanilla pickpocket drag has no amount prompt).
- While the rule is on, Ctrl+LMB on a teammate no longer runs the vanilla Ctrl action on that PC.
- The HUD's sync check uses "time since any app packet". The 5 s status heartbeat keeps an idle link at OK. RTT and loss come from ENet only (Steam shows `RTT ?`).
- A client's INVEN / DIALOG badge lags by one host poll (250 ms) plus the network.
