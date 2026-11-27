# Coop Middle (release notes M1-M7)

Coop Middle is the development line after the **Coop Lite 1.0** release. It lives on branch `coop-middle`, which starts from `74a7076f` (tag `coop-lite-v1.0`). Coop Lite itself is maintained separately on branch `coop-lite` (same commit). M7 is the final Middle stage.

| Line | Branch | Protocol (`TIG_NET_PROTOCOL`) |
| --- | --- | --- |
| Coop Lite 1.0 | `coop-lite` (tag `coop-lite-v1.0`) | 52 |
| Coop Middle M1 | `coop-middle` (up to `23e1797d`) | 53 |
| Coop Middle M2 | `coop-middle` (up to `283402e5`) | 54 |
| Coop Middle M3 | `coop-middle` (up to `ecc31c26`) | 55 |
| Coop Middle M4 | `coop-middle` (up to `9f60cded`) | 56 |
| Coop Middle M5 | `coop-middle` (up to `d0c4c597`) | 57 |
| Coop Middle M6 | `coop-middle` (up to `2bd38e3b`) | 58 |
| Coop Middle M7 | `coop-middle` (up to `3e841364`) | 59 |
| Coop Middle after M7 (auto-levelling) | `coop-middle` | **60** |

Lite and Middle builds (and different Middle stages) refuse each other at the handshake. Every player needs the same build. Single player (no `-coop`) is unchanged: every Middle entry point returns at once outside a co-op session.

## Stages at a glance

| Stage | What it adds |
| --- | --- |
| M1 | Session rules from the host (`CoopRules`, `PACKET_COOP_RULES` 167), gold split, `[Loot]` feed, client rest request (Y / N on the host) |
| M2 | Direct trade between players, status badges on teammate portraits, Alt-hold loot labels, F9 net HUD |
| M3 | Shared tactical pause, enemy HP % / XP % / no friendly fire, map pings (168), `/tp` `/stuck` `/gather` |
| M4 | Shared quest items, town map fog sync (169), parallel dialogs (8 sessions), shared followers |
| M5 | Interest management (per-slot NPC_POS, sector keep window), free world map travel (170), unified sleep |
| M6 | Solo interiors / dungeons: map leases, archive transfer and commit (171-173), M badge, `[Co-op]` lines |
| M7 | Collapse notice + off-screen arrows for fallen / unconscious teammates, red dead marker on the town map, host autosave rotation, sleep inside solo instances (`SLEEP_APPLY`), pause freezes world map trips / checkpoints / autosave, trade auto-close, one full session reset, launcher Auto-save choice + lobby rules line |

## Session rules (`ARCANUM_COOP_RULES`)

The host's launcher builds `ARCANUM_COOP_RULES` (e.g. `gold=0,loot=1,rest=1,trade=1,pause=0,ff=1,qitems=1,sfol=1,ow=1,usleep=1,subinst=1,flvl=1,plvl=1,hp=100,xp=100,asave=10`). Keys are separated by `,` or `;`, values can be numbers or `on/off/yes/no/true/false`, unknown keys and bad values are ignored and missing keys keep their defaults. The host reads it once; clients get the sanitized rules in `PACKET_COOP_RULES` (type **167**, 32 bytes, host -> client only) right after SESSION_SYNC, show the summary in chat and write it to `lobby_rules.txt` (M7). A client goes back to the defaults on a session reset.

| Key (alias) | Rule | Default | Launcher control (ini `[rules]` key) | Stage |
| --- | --- | --- | --- | --- |
| `gold` (`gold_split`) | split picked-up gold among living players | off | Split gold (`gold_split`) | M1 |
| `loot` (`loot_feed`) | `[Loot]` chat lines for notable finds | on | Loot feed (`loot_feed`) | M1 |
| `rest` (`rest_vote`) | clients may ask the host to rest | on | Rest vote (`rest_vote`) | M1 |
| `trade` (`direct_trade`) | free give between players in a trade window | on | Direct trade (`direct_trade`) | M2 |
| `pause` (`client_pause`) | clients may toggle the shared pause (the host always may) | off | Client pause (`client_pause`) | M3 |
| `ff` (`no_ff_aoe`, old `noffaoe`) | the party never damages the party | on | No friendly fire (`no_ff_aoe`) | M3 |
| `qitems` (`shared_quest_items`) | quest item checks also see nearby players' packs | on | Shared quest items (`shared_quest_items`) | M4 |
| `sfol` (`shared_followers`) | any player manages / barters with any follower | on | Shared followers (`shared_followers`) | M4 |
| `ow` (`free_overworld`) | everyone travels the world map alone, no clock jump. Since 2026-11-27 always on unless `lite=1` (`coop_rules_effective`; `ow=0` is still parsed but ignored) | on | none (the box was removed; old `free_overworld` in the ini is ignored) | M5 |
| `usleep` (`unified_sleep`) | sleep needs every living player ready | on | Unified sleep (`unified_sleep`) | M5 |
| `subinst` (`solo_instances`, old `subinstances`) | a client may enter an interior / dungeon alone | on | Solo interiors/dungeons (`solo_instances`) | M6 |
| `flvl` (`follower_autolevel`) | a follower below its leader PC's level is raised to it (vanilla follower leveling, see below) | on | Auto-level followers (`follower_autolevel`) | after M7 |
| `plvl` (`party_level`) | every player PC's XP is kept at the party maximum, so everyone levels together (late joiners too) | on | Party level (`party_level`) | after M7 |
| `hp` (`enemy_hp_pct`) | enemy HP %, 100..300 | 100 | HP % combo (`enemy_hp_pct`) | M3 |
| `xp` (`xp_mult_pct`) | XP %, 100..250 | 100 | XP % combo (`xp_mult_pct`) | M3 |
| `asave` (`autosave_min`, `autosave`) | host autosave interval in minutes: 0 = off, else 5..60 (1 -> 5, 999 -> 60) | 10 | Auto-save combo Off / 5 / 10 / 15 / 30 min (`autosave_min`) | M7 |
| `lite` (`lite_mode`, `coop_lite`) | Coop Lite mode: the session plays as the Coop Lite 1.0 release (see below); written to the env string only when on | off | Coop Lite mode (as release 1.0) (`lite_mode`) | after M7 |

### Auto-levelling (`flvl` 0x1000, `plvl` 0x2000, protocol 60)

Host only, `coop_lvl_host_tick` in `coop_middle.c`, once a second (not on the main menu, during a load or a map change). It looks at every connected slot PC (dead and parked ones too).

- **Party level** (`plvl`): the lower PCs get the highest XP in the party (`coop_middle_party_xp_target`) through `stat_base_set(STAT_EXPERIENCE_POINTS)`, so the vanilla `level_recalc` gives the levels and **unspent** character points (or applies the PC's auto-level scheme). The levels reach the owners through the usual PC build sync (packet 144). The XP split is unchanged; XP earned in a solo instance comes back to the host as an XP delta and raises the maximum, so everyone levels at once. A player who joins late gets all the levels at once, points free to spend. Log: `CoopMiddle: plvl: slot S xp A -> B`.
- **Follower auto-level** (`flvl`): every live NPC follower of a PC below that PC's level is raised to it by the vanilla `update_follower_level` (new hook `level_follower_raise_to` in `level.c`): character points plus the follower's own auto-level scheme, as in single player. It never lowers a level and stops at max level - 1 (vanilla cap). Followers of a PC inside a solo instance wait until it comes back. Log: `CoopMiddle: flvl: follower F of slot S level A -> B`.

Fixes after the auto-levelling build (`05f845d1`, playtest t402u / t403u):

- **Host hops on its own map are solo in Middle.** A host teleport that stays on the current map (dungeon stairs / jump points, teleport traps and spells, as on the free world map) moves only the host's PC and followers: no ready check, no party carry, no FOLLOW_HOST (`coop_middle_host_solo_hop` -> pure `coop_middle_solo_hop_rule`, hooks in `teleport_process` and `coop_lite_host_travel_gate`). Coop Lite mode keeps the Lite 1.0 carry. A **map change** of the host still takes the whole party (the host runs only one map; a client can stay behind only in a solo instance).
- **Host followers on clients.** A client's copy of a PC's follower list could miss followers whose leader field was already set (world state, load) and keep null / freed handles of followers left on another map. A map change then carried no host followers on the client ("followers not passed to the client"), and the next same-map teleport read the dead handle (client crash `sub_4D9990 <- sub_4D3D60 <- teleport_ping`, "Accessing non-existant field obj_f_npc_flags in object type 0"). Now every FOLLOWERS table rebuilds each slot PC's list on the client (`coop_middle_client_followers_fix`, pure `coop_middle_follower_list_plan`: bad entries dropped, table followers added; log `CoopMiddle: followers: slot S list repaired (A listed before, B now)`), and `schedule_teleport_obj_recursively` skips null / freed follower and item handles (log `Teleport: skipped a missing object handle`).

Limits: XP is only raised, never taken away; a rejoining character with more XP pulls the party up. With both rules on a follower follows its leader's (= party) level.

### Coop Lite mode (`lite=1`, `COOP_RULE_LITE_MODE` 0x800)

One switch that turns the session into Coop Lite 1.0 gameplay. Everything goes through one function, `coop_rules_effective` (pure, `coop_rules.c`): the host applies it to its parsed rules before it uses or sends them, a client to the rules it receives, and every rule check (`coop_middle_rules()`, `coop_middle_rule_on`) reads the result. With `lite=1` the effective rules are `flags & (COOP_RULE_LITE_MODE | COOP_RULE_LOOT_FEED)`, `hp=100`, `xp=100`; `asave` is kept. The gates that are not a rule bit read `coop_middle_lite_mode()`.

Off in Lite mode (Lite 1.0 behaviour):

- free world map (`ow`): the host drives the party, clients watch the trip (PT73 spectator), strict tie to the host;
- solo interiors / dungeons (`subinst`);
- parallel and remote dialogs: one party dialog at a time (the host's own dialog counts, the host watches a running client dialog), the leader within `COOP_PARTY_TALK_MAX_HOST_DIST`, the host's combat blocks (`coop_lite_party_talk_gate(..., lite = true)` = PT41's `coop_lite_party_talk_check`);
- follower auto-level (`flvl`) and party level (`plvl`);
- shared quest items, shared followers, unified sleep and the client rest request (Lite rest), gold split, direct trade, no-friendly-fire, enemy HP % and XP % (100);
- the shared pause (host and clients: the Pause key does nothing, requests are refused) and the `/tp`, `/stuck`, `/gather` commands (plain chat text).

What stays (UI and housekeeping only): F9 net HUD, Alt loot labels, portrait status badges, map pings, off-screen arrows and the collapse notice for fallen teammates, the red dead marker and fog sync on the town map, the `[Loot]` chat feed (its own checkbox), the host autosave (its own combo), interest management (network traffic only), the lobby rules line.

Launcher: the checkbox greys every other rule except Loot feed and Auto-save. Protocol stayed **59** for this bit (60 came with `flvl` / `plvl`): the bit lives in the existing `flags` field (`sizeof(CoopRules)` unchanged) and the host sends the effective rules, so even a peer that does not know the bit sees every Middle rule off.

```c
typedef struct CoopRules {
    /* 0000 */ uint32_t version;      /* 1 */
    /* 0004 */ uint32_t flags;        /* COOP_RULE_* */
    /* 0008 */ uint16_t enemy_hp_pct; /* 100..300 */
    /* 000A */ uint16_t xp_mult_pct;  /* 100..250 */
    /* 000C */ uint16_t autosave_min; /* M7: 0 = off, 5..60 */
    /* 000E */ uint16_t reserved16;   /* zero */
    /* 0010 */ uint32_t reserved[3];  /* zero */
} CoopRules;                          /* 28 bytes (unchanged since M1), _Static_assert on size / offsets */
```

`coop_rules_sanitize` clamps the numbers, drops unknown flag bits (known: the M1-M6 rule bits and `COOP_RULE_LITE_MODE`), zeroes the reserved fields and fixes `version`. M7 took 2 bytes of the old `reserved[0]` for `autosave_min`, so `sizeof(CoopRules)` and the packet size stayed the same.

## Hotkeys and mouse (co-op only)

| Input | Action | Stage |
| --- | --- | --- |
| hold **Alt** | loot labels for ground items, containers and corpses (22 tiles, up to 32) | M2 |
| **F9** | net HUD line (players, RTT, loss, sync) on / off | M2 |
| **Pause** | shared tactical pause on / off (clients only with the `pause` rule) | M3 |
| **MMB** / **Alt+RMB** on the map | ping the tile (danger ping over a hostile NPC or a spotted trap) | M3 |
| **Ctrl+LMB** on a teammate in the world | open the trade window (on a teammate's follower: the follower exchange) | M2 / M4 |
| **RMB** on a teammate portrait | open the trade window | M2 |
| **Y** / **N** | answer the rest request (host), join / leave a unified sleep round | M1 / M5 |

## Chat commands

| Command | Who | Effect |
| --- | --- | --- |
| `/tp`, `/stuck` | client | regroup to the host (alive, same map, no combat, no enemy fighting you; 30 s cooldown) |
| `/gather`, `/tp all` | host | pull every eligible client to the host (30 s cooldown) |

Commands are case-insensitive, run on the host before the chat relay and never show as chat. Refusals come back to the asker as a system line in their own language.

## M7: final stage

### Collapse notice and off-screen arrows

- The host watches every living, connected PC (not one inside a solo instance) four times per second. When its fatigue drops to 0 or below (`critter_fatigue_current <= 0`, not dead) a system line goes to everyone, at most once per 10 s per slot (`coop_middle_collapse_step`): `[Party] <Name> collapsed from exhaustion!` / `[Партия] <Name> упал без сознания от усталости!` (chat kind 11, localized on each receiver).
- `coop_middle_edge_indicator(sx, sy, w, h, pad, &x, &y, &octant)` (pure): a point inside `[pad..w-pad] x [pad..h-pad]` returns false; otherwise the ray from the view centre to the point is cut at the pad border, and the octant is 0 = right, 1 = up-right, 2 = up, ... 6 = down, 7 = down-right.
- `coop_middle_ui.c` draws, for every other PC on this map that is dead or at fatigue <= 0 and off screen, a solid plate at that border point (pad 28 px): a coloured frame, a dark body, a solid arrow pointing to the teammate and `X <Name> (<tiles>)` (dead, red 220,60,60) or `Z <Name> (<tiles>)` (unconscious, lilac 180,130,230). The distance is in tiles from the local PC. Plates are pre-rendered in one off-screen buffer and blitted in the iso overlay pass after the text floaters (no windows); their rects are invalidated when they change, on scroll (old and shifted copy) and when they go away.
- Town map (`wmap_ui.c`, town mode): a dead teammate's PT43 square is red instead of blue; the open town map repaints when a teammate dies or recovers.

### Host autosave

- `asave` minutes (default 10, 0 = off). The host checks once per second. `coop_middle_autosave_ready` needs: interval > 0, at least `asave` minutes since the last save (manual, client F7 or auto: every successful `gamelib_save` restarts the interval), the host in the world (valid map, alive, no main menu, no load / save running), no pause, no combat, no dialog (host or any client), no world / town map window, no teleport / sleep / Lite travel or load prompt / overworld trip, and no solo-instance COMMIT or checkpoint still arriving. If something blocks, it tries again in 15 s (the need stays). A pause freezes the interval.
- The save runs in the main loop next to the PT61 client save (outside `gamelib_ping`): `gamelib_save` first sends `CHECKPOINT_REQ` to every solo-instance client and waits up to 2 s (M6), then writes `Slot9690`..`Slot9694` round robin with the descriptions `[Host] AutoSave 1`..`5`. The first slot of a session follows the newest existing autosave. Manual saves and the client slots `Slot9700`..`Slot9789` are never touched.
- Chat to everyone: `[AutoSave] World saved (AutoSave N).` / `[Автосейв] Мир сохранён (AutoSave N).` (chat kind 12). A failed save is logged and retried in 15 s.
- `gamelib_last_save_name` (party-wipe reload, `ARCANUM_COOP_AUTOLOAD=latest`) takes the newest save, autosaves included.

### Sleep, pause and trade with solo instances

- `COOP_CTRL_SLEEP_APPLY` (PARTY_CTRL op **11**, host -> client, `a` = hours 1..168, `b` = 0). After a unified sleep the host sends it to every living client that holds a solo-instance lease. That client heals its PC and followers (`critter_resting_heal`) with local authority under a fade, and takes the host's new game time packet (both pass the solo-instance packet filter).
- A solo-instance client now counts in the sleep consensus: it uses the host's rules inside (not the local defaults), its sleep window sends a vote instead of running a vanilla local sleep, and it sees the banner. Like any player it can answer N; it never blocks a round it cannot see.
- Pause: the host freezes overworld trip steps and their encounter rolls and the autosave interval; a solo-instance client stops its local `timeevent_ping` on `COOP_CTRL_PAUSE(1)` and its 45 s checkpoint clock stands still.
- Trade (M2) closes on both sides by itself (held item put back with `inven_ui_drag_cancel_if_stale` / the vanilla drop-back, then a notice) when either side starts an overworld trip, enters a solo instance, enters combat, dies, or the partner is more than 6 tiles away / hidden.

### One session reset

`coop_lite_session_reset` calls `coop_middle_session_reset`, which clears all Middle state and is safe to call any number of times: client rules (defaults), pause, ping ring, status bits, rest / feed / gold-drop memory, the Alt label buffer, HUD, banner and arrow plates (`coop_middle_ui_reset`), all overworld trips and pending teleports (`coop_owt_reset`), the sleep round and a pending `SLEEP_APPLY`, town map fog sync, every solo-instance lease with its chunk / checkpoint buffers (`coop_subinst_session_reset`), the autosave timer and rotation, the collapse antispam and the lobby rules flag. The 8 dialog sessions live in `coop_lite.c` and are reset there in the same call chain (`coop_lite_session_reset_extra` -> `coop_lite_dialog_reset`).

### Launcher

- Host panel: **Auto-save** combo (Off / 5 / 10 / 15 / 30 min) next to Solo interiors/dungeons, `[rules] autosave_min` (default 10), sent as `asave=` in `ARCANUM_COOP_RULES`.
- The game writes `lobby_rules.txt` (one `coop_rules_describe` line, UTF-8, EN or RU by `ARCANUM_COOP_LANG`) in its role folder (`coop\host\` or `coop\client\`) at host start and when a client gets `PACKET_COOP_RULES`. The lobby panel shows it in a status line under the player list (the old file is deleted just before the game starts). The window is sized from its client area, so the line always fits.
- After M7: a **Coop Lite mode (as release 1.0)** checkbox under the rules (`[rules] lite_mode`, sent as `lite=1`); it greys the other rules except Loot feed and Auto-save.

## Feature details (M1-M6)

### M1: gold split (`COOP_RULE_GOLD_SPLIT`, host only)

When a PC (host or client) on the host takes gold:

- from the ground (`sub_42AA70` for the host's PC, item op 28 for a client), or
- from a container or a dead NPC (inventory transfer `sub_578EA0`: the host's loot window and a client's loot Packet81; item op 28 from a container),

the amount is split among **all living, connected** players with `coop_middle_split_gold`. The picker keeps its share plus the remainder, so no gold is ever lost. Other players get theirs with `item_gold_transfer` and an inventory resync (`coop_lite_host_sync_slot_inven`). For 10+ gold, a chat line goes out when the loot feed is on.

Not split:

- buying, selling, gambling (merchants and store containers), gifts between players, stealing, and moves inside a player's own pack;
- gold a player dropped (the host remembers the last 32 dropped piles by OID);
- gold a player put into a container, when it is taken back (per-container amount, cleared on map change);
- when the picker is the only living player.

### M1: loot feed (`COOP_RULE_LOOT_FEED`)

`coop_middle_loot_feed_worthy(type, flags, cost, quest_or_key)` passes keys and key rings, scrolls, written items (notes, books, schematics), quest items (`OIF_WONT_SELL`), unique items (`OIF_UBER`), and anything except gold or ammo that costs **250+**. Junk, cheap consumables and ammo never make a line.

- EN: `[Loot] <Name> picked up: <Item>`, `[Loot] <Name> found <N> gold: split, <M> each`
- RU (`ARCANUM_COOP_LANG=ru`): `[Лут] <Имя> подобрал: <Предмет>`, `[Лут] <Имя> нашёл <N> золота: поровну по <M>`

The host sends these as `PACKET_COOP_CHAT` with a system marker (`reserved = 'MI' << 16 | kind`). Each client builds the text in its own language. The host relay rebuilds every player line with `reserved = 0`, so a client can't fake a system line. Anti-spam: at most **5 lines per second**. The rest of a burst (for example "Take all" on a big chest) collapses into one `[Loot] ... and N more` line.

### M1: client rest request (`COOP_RULE_REST_VOTE`)

Two new ops in `PACKET_COOP_PARTY_CTRL` (no new packet type):

| Op | Dir | `a` |
| --- | --- | --- |
| `COOP_CTRL_REST_ASK` (5) | client -> host | 1 = 8 hours (bed), 2 = until healed (rest button) |
| `COOP_CTRL_REST_REFUSE` (6) | host -> client | reason 0..6: host said N, host in combat, busy, rule off, cancelled, can't rest here, timeout |

- **Client:** a living client outside combat presses the rest button or uses a bed. When the rule is on, it sends `REST_ASK` (at most one per 5 s) and sees "Rest request sent to the host (Y / N)." / «Запрос на отдых отправлен хосту (Y / N).». When the rule is off, the old Lite refusal stays.
- **Host:** if it is not in combat and no other Y/N prompt is up (travel, load, ready check, sleep window), it shows "<Name> wants to rest. Y = rest until healed, N = refuse." (or "Y = rest 8 hours") on the Y/N panel and in the message line.
- **Y:** the host opens its sleep window and presses "Until Healed" (or "8 Hours", also in wait-only towns): the normal party rest with fade and healing for all PCs (PT25). **N** sends `REST_REFUSE`.
- **Auto-cancel** (with `REST_REFUSE` to the asker): the host enters combat, a map change, the asker dies, another Y/N prompt opens, or there is no answer in 60 s. If the asker disconnects, the request is dropped silently.

### M2: direct trade (`COOP_RULE_DIRECT_TRADE`, default on)

A free give between two players, in the inventory window the game uses for pickpocketing (`INVEN_UI_MODE_STEAL`), run as a host-authoritative **TRADE** session.

- **Open:** **Ctrl+LMB** on another player's PC in the world, or **RMB on a teammate portrait** (right-hand column). The partner must be the PC of another active slot, both alive, within **6 tiles**; otherwise "Move closer to trade." / «Подойдите ближе для обмена.». With the rule off, Ctrl+LMB acts as before and the portrait says "Direct trade is turned off by the host.".
- **Wire:** a client sends `PacketCoopBarter` kind `COOP_BARTER_OPEN_TRADE` (5). The host checks `coop_middle_trade_live` (rule, PC of another active slot, both alive, `location_dist <= 6`) and keeps the session as target kind `COOP_TARGET_TRADE` (3); the 1 s session watch ends it when the check fails.
- **Give:** dragging from your own pack onto the partner is always allowed and free (no pickpocket roll): Packet81 `0x01|PLANT` from a client goes through Lite's gift branch (`item_gold_transfer` / `item_ammo_transfer` / `item_transfer`), the host's own give through `coop_middle_host_trade_give`. Both packs are resynced.
- **Take:** dragging from the partner's pack is refused locally with "You can only receive what your teammate gives you." / «Взять можно только то, что игрок передаст сам.», and on the host by `coop_middle_trade_transfer_ok` (only `!from_target`, flags `0x01` or `0x01|PLANT`).
- **Lines:** `[Trade] <From> gave <Item> to <To>` / `[Обмен] <From> передал <Item> игроку <To>`; gold: `[Trade] <From> gave <N> gold to <To>` / `[Обмен] <From> передал <N> золота игроку <To>`. System chat kinds 4 / 5 (`receiver|item`, `receiver|amount`). They go to both players, or to everyone when the loot feed is on.
- Symmetric: host <-> client and client <-> client (both sides run on the host's copies).

### M2: Alt-hold loot labels (local only)

While **LALT / RALT** is held in the world (intgame mode MAIN; no main menu, world map, inventory, dialog, chat input or load), up to **32** labels show:

- ground items (weapon .. generic: gold with its amount, ammo with its count, keys, scrolls, ...);
- non-empty containers (or not yet generated `OCOF_INVEN_SPAWN_ONCE` stock), locked containers as "<Name> (locked)" / «(заперт)»;
- dead NPCs with something to loot.

Collected with `object_list_rect` within **22 tiles** of the PC; visible means `OBJ_F_FLAGS & (OF_INVENTORY|OF_OFF|OF_DESTROYED|OF_DONTDRAW) == 0` and on screen. Ground items come first, then the nearest to the PC or the mouse cursor. `coop_middle_labels_deoverlap` pushes overlapping labels upward (1 px gap) and hides the ones that run out of room. Colours: gold `(240,210,90)` for keys, gold, quest / unique items and written items (schematics, notes); `(215,235,255)` for other items; `(195,175,140)` for containers and corpses. Each label is a solid plate `(16,14,12)` with a thin frame, pre-rendered in one off-screen buffer and blitted into the iso view in the overlay pass after the text floaters (no windows, clicks go to the world). Label areas are invalidated when they change (refresh every 150 ms), on scroll and on Alt release, so no trails are left.

### M2: status badges on teammate portraits

New op in `PACKET_COOP_PARTY_CTRL`:

| Op | Dir | `a` |
| --- | --- | --- |
| `COOP_CTRL_SLOT_STATUS` (7) | client -> host | own bits `INVEN 1`, `DIALOG 2` (on change, and every 5 s) |
| `COOP_CTRL_SLOT_STATUS` (7) | host -> clients | 4 bits per slot (`coop_middle_status_pack`), on change and every 5 s |

Every 250 ms the host merges each slot's reported bits with `POISON 4` (`STAT_POISON_LEVEL > 0`) and `COMBAT 8` (combat mode on). A 14x14 badge (dark square, coloured frame, letter) is drawn in the top-right corner of a teammate portrait, in this priority: **X** dead `(220,60,60)`, **Z** unconscious (fatigue <= 0) `(180,130,230)`, **P** poisoned `(90,220,90)`, **D** dialog `(240,200,80)`, **I** inventory `(100,210,240)`, **C** combat `(240,140,60)`. Dead and unconscious are read locally from the PC. Portraits are redrawn only when a badge changes; no windows are created.

### M2: F9 net HUD (local only)

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

### M3: shared tactical pause (`COOP_RULE_CLIENT_PAUSE`)

- **Pause** (the key) toggles one shared pause for the whole session. It does not work while typing in chat or in the main menu. The host can always pause. A client can pause only when the host turned on **Client pause** (`pause=1`, default **off**); otherwise it gets a notice. A client can toggle at most once per 2 s (checked on the client and again on the host).
- Wire: `PACKET_COOP_PARTY_CTRL` op `COOP_CTRL_PAUSE` (8). Client -> host: `a` = 1 pause / 0 resume, `b` = 0. Host -> all: `a` = state, `b` = the slot that paused. The host sends the state to a slot that joins during a pause, right after SESSION_SYNC.
- **What stops:** `timeevent_ping` keeps advancing only real time while paused. Game and anim time stand still, so no AI, combat, animation, spell or poison ticks, fatigue or script timers run. Because the real-time reference keeps moving, the resume has no time jump.
- **What keeps running:** the main loop, ENet / `multiplayer_ping`, the crash-handler heartbeat, rendering, camera scroll, chat (Enter), inventory, journal, character sheet (points can be spent) and the hotbar.
- **Blocked:** new world orders on the map (LMB / RMB: move, attack, cast, throw) and spell hotkeys, on host and client. A rate-limited notice explains why.
- **Banner:** top centre, under the F9 HUD when it is on. It is a solid plate (bg 20,16,12, frame 210,170,70) and clicks pass through it.
  - EN: `PAUSED (<Name>) - Press Pause to resume`
  - RU: `ПАУЗА (<Имя>) - нажмите Pause для продолжения`
  - The game fonts have no em dash, so it shows as ` - `.
- **Auto-resume:**
  - when the client who paused disconnects, with a chat line `[Co-op] <Name> left - pause lifted.` / `[Кооп] <Имя> отключился - пауза снята.`;
  - when the host opens the main menu;
  - on a map change or save load;
  - on a session reset.

### M3: difficulty (host only)

| Rule | Range / default | Env key (alias) | Effect |
| --- | --- | --- | --- |
| `enemy_hp_pct` | 100..300, default 100 | `hp` (`enemy_hp_pct`) | Damage to enemy NPCs is divided by pct%. The remainder carries per target, and every hit does at least 1. `OBJ_F_HP_ADJ` is never touched. |
| `xp_mult_pct` | 100..250, default 100 | `xp` (`xp_mult_pct`) | Party XP is multiplied before the split. |
| `COOP_RULE_NO_FRIENDLY_FIRE_AOE` | **on** by default | `ff` (`no_ff_aoe`, old `noffaoe`) | Party members never damage each other. |

- **Enemy HP:** the pure helper is `coop_middle_scale_enemy_damage(raw, pct, &rem)` = `(raw*100 + rem) / pct`.
  - It runs in `combat_dmg` after resistances, difficulty and crit bonuses, so it covers melee, ranged, spells, grenades, traps and scripts.
  - It only scales damage to NPC targets that are not on the co-op side (PCs, their followers and summons excluded). Kill shots (`CDF_FULL` / `CDF_DEATH`) stay kill shots.
  - Up to 32 targets keep a remainder; the table is cleared on map change.
- **XP:** `coop_middle_scale_xp(raw, pct)` uses 64-bit math, clamps to `INT_MAX` and never drops below `raw`. It is applied in `coop_lite_host_share_xp` before the split. A lone player also gets the scaled amount.
- **No friendly fire:** at the very start of `combat_dmg`, if the attacker and target are different objects and both are on the co-op side (`coop_lite_party_no_fight`), the HP and fatigue damage and the damage flags are cleared and the function returns.
  - That means no damage, crit effects, `SAP_TAKING_DAMAGE` script, aggro or retaliation.
  - It applies to every party-on-party hit, not only AoE.

### M3: map pings (`PACKET_COOP_PING` = 168)

```c
typedef struct PacketCoopPing {
    int type;    /* 0x00 */
    int slot;    /* 0x04: the host stamps the sender's slot */
    int map;     /* 0x08: map_current_map() */
    int kind;    /* 0x0C: 0 = look (blue-gold), 1 = danger (red) */
    int64_t loc; /* 0x10: target tile */
} PacketCoopPing; /* 0x18, _Static_assert */
```

- **Trigger:** **MMB** (release) or **Alt+RMB** on the map, in normal mode with no dialog or inventory open. Alt+LMB stays the Lite body drag.
  - The ping marks the tile under the cursor.
  - It is a danger ping when a hostile NPC (its combat focus is set, or its reaction is dislike or hatred) or a spotted trap is under the cursor.
  - **MMB conflicts:** none on the map. MMB is used only in `dialog_ui.c`, `mainmenu_ui.c` and `slide_ui.c`, which are other windows. Alt+RMB down on the map is swallowed, so the vanilla RMB action does not run.
- **Host:** `PACKET_COOP_TYPE_LIMIT` is 169 and the role is `BOTH`.
  - It checks the exact size, kind 0..1, `map == map_current_map()` and `coop_lite_loc_in_map`, and allows at most 1 ping per second per slot. The client waits 1.1 s between its own pings.
  - It stamps the slot, sends the ping to everyone (the sender included) and shows it itself.
- **Display:**
  - The last 16 pings are kept, each for 4.5 s, and cleared on map change and session reset.
  - Each ping is a pulsing diamond on the tile with a `[Name]` plate. It is drawn in the iso overlay pass (no extra windows), and its rect is invalidated on every pulse step (150 ms) and when it disappears.
  - A soft UI click plays when the ping is within 40 tiles of the local PC.
  - On the open town map (`wmap_ui.c`, town mode) a ping on the current map blinks as a cross, repainted every 300 ms.

### M3: `/tp`, `/stuck`, `/gather` (host)

- `coop_middle_parse_cmd` is case-insensitive and trims spaces: `/tp` and `/stuck` -> regroup to the host; `/gather` and `/tp all` -> the host pulls everyone. Plain text, `/unknown` and extra words stay chat.
- The host runs a command before the chat relay (`coop_lite_handle_chat`) or on its own send, so commands never show as chat. A client's command text is not echoed locally.
- **`/tp` or `/stuck` (client):** cooldown 30 s per slot. Both the host and the client must be alive, the client's PC must be on the host's map, neither may be in combat mode, and no live hostile NPC within 20 tiles may have the client as `OBJ_F_NPC_COMBAT_FOCUS`.
  - The client is moved to a free walkable tile by its slot (`coop_lite_follow_loc_for_slot`). The bridge `coop_lite_mid_move_pc` stops its anims, moves it, keeps the sector and bumps the pose epoch, and the pose tick sends `COOP_PC_POS_FORCE` to the owner and the pose to the others.
  - Line: `[Co-op] <Name> regrouped to the host.` / `[Кооп] <Имя> телепортировался к хосту.`
- **`/gather` (host only):** the host must be alive and out of combat; cooldown 30 s. Every connected client that passes the same checks is moved. Line: `[Co-op] <Host> gathered the party.`
- **Refusals** go to the asker only, as a system line in their own language: dead, other map, combat, enemy fighting you, `N s` cooldown, `/gather` is host-only, nobody to gather, or the host using `/tp`.

### M4: shared quest items (`COOP_RULE_SHARED_QUEST_ITEMS`, host only)

- `coop_middle_find_party_item(asker, key, match, &owner)`: the asker's own pack first (vanilla lookup). Then, on the host with the rule on and the asker a party PC, every other slot that `coop_middle_quest_pocket_eligible` accepts: connected, alive, on this map (`coop_lite_loc_in_map`), within `COOP_SHARED_QUEST_RADIUS` (30) tiles. In an ally's pack the backpack (location < 1000) wins over worn items (1000..1008).
- Checks: dialog `in` / `ni` (after the PC's own followers) and script "has item named". Takes: dialog action `in` and script "transfer item" / "destroy item named". The item leaves the owner's pack (the vanilla transfer unequips it), the owner's pack is resynced (`coop_lite_host_sync_slot_inven`) and everybody gets `[Quest] <Item> handed over from <Owner>` / `[Квест] <Item> передан из рюкзака <Owner>`.
- Single player and clients: the vanilla lookup only.

### M4: town map fog (`PACKET_COOP_TOWNMAP_FOG` = 169)

- Storage (`townmap.c`): one bit per town-map tile, the k-state of one town map at a time, saved as `Save/Current/<name>.tmf`; only the local PC marks tiles. `townmap_coop_fog()` gives the array (loading it), `townmap_coop_fog_touched()` marks it for saving.
- Packet: `{type, townmap_id, offset, len <= 1024, flags (REPLACE 0x1), data[1024]}`, 0x410 bytes, sent as header + `len`. `coop_middle_townmap_fog_valid` checks the size, type, id > 0, len, flags and `offset + len <= 65536`.
- Every 1.5 s each side takes the FNV-1a signature of its current town map's fog (`townmap_get` of the local PC's sector).
  - Host: changed 1 KB chunks go to everyone (OR). A full REPLACE snapshot goes to a slot after its SESSION_SYNC, and to everyone on a town-map change and after a map change / load.
  - Client: after a town-map change, a resync or a session reset it sends nothing until the host's first packet for that town map. Then its changed chunks go to the host, which ORs them in and relays them to the others.
- `coop_middle_fog_merge` returns the newly opened bits; when it is > 0 an open town map (`WMAP_UI_MODE_TOWN`) is redrawn.

### M4: parallel dialogs

- The PT36/PT41 single dialog context is now `CoopDialogSession coop_dlg_sessions[8]` (in `coop_lite.c`): session 0 is the host's own dialog, session N a client's dialog that runs headless on the host.
- On the host, `dialog_ui.c` gives every client PC its own `DialogUiEntry` (the engine always had 8, `sub_567420` was pinned to entry 0), so each session has its own `DialogState`; `dialog.c` keeps no per-conversation globals. The current session is kept in a context index that is saved / restored around each START / CHOOSE step.
- A client clicks a story NPC (START):
  - NPC free -> its own session (same distance checks as PT41; the leader must still be near).
  - NPC busy (another session talks to it) -> the client is attached to that session as a spectator and may vote. The host clicking an NPC a client talks to watches that session in the viewer panel.
- Routing (`coop_middle_dialog_route`): own session, else the attached one, else the host's, else the most recent client session. Votes go to the session the voter is routed to. The seq counter is global, so the client's stale check works across sessions.
- Closing a session (leave, end, speaker disconnect) sends CLOSE (or EXCHANGE) to its viewers only; a map change, load or session reset closes all 8.

### M4: shared followers (`COOP_RULE_SHARED_FOLLOWERS`)

- 4a: `PacketCoopNpcStats` (166) also carries `basic_skills[12]`, `tech_skills[4]`, `spell_tech[16]` (size 0xA0 -> 0x120, in the host's signature, applied on clients).
- 4b: a move inside a follower's pack (Packet81 `from == to == follower`, or `INVEN_LAYOUT` for the follower) is applied for the leader, or for any party player with the rule, when it has that follower's window open. The sender and everyone with that follower open get the pack, and NPC_POS re-sends the follower (worn items).
- 4c: with the rule, any living party player within 6 tiles may open the free follower exchange with any party follower: `coop_lite_barter_partner_ok`, `COOP_DIALOG_ACT_EXCHANGE`, the client's exchange open, and Ctrl+LMB on a teammate's follower. The leader keeps the follower (no re-binding on the client).

### M5: interest management

- Co-op sector cache 96 -> 128. Each remote slot keeps a 3x3 sector window while there are <= 4 remote slots or >= 36 cache slots would stay free; otherwise an adaptive 2x2 (own sector + the nearer neighbour per axis, margin 32). The decision uses the free count *as if* every slot held 3x3, so it does not flip back and forth. The PT77y corpse fixes stay.
- NPC_POS: each slot gets its own batch (`tig_net_send_pose`) with only NPCs within 44 tiles of its PC (Chebyshev, all maps). A per-NPC seen mask sends the state at once to a slot that enters range; a state change resets the mask.
- On map 1 only: DOOR_STATE to slots within 96 tiles; TRAP_FX, PROJECTILE_FX, sounds (106) and eye-candy adds (77) within 56 tiles of the source / target. Other maps and vanilla MP: broadcast as before. Eye-candy removals stay broadcast (no stuck particles).

### M5: free world map (`COOP_RULE_FREE_OVERWORLD`, key `ow`, always on unless Coop Lite mode)

Off (only in Coop Lite mode now) = Lite behaviour. On, and only while the party is on map 1 (the world):

- World map travel does not move the clock and gives no travel healing; speed stays vanilla.
- The host's world map does not dim clients and does not open the PT73 spectator. The host's own hop (arrival, stop or random encounter) moves only its PC and followers (no FOLLOW_HOST, no carry). Fixed after M7 (t391u): `teleport_process` still ran `coop_lite_after_party_carry` on a solo hop, which put every client PC next to the host, so a host encounter pulled the clients in; it now runs only for party travel.
- A client's route click sends `PACKET_COOP_OVERWORLD_TRAVEL` (170, 0x20 bytes, role `BOTH`) START. The host validates (rule, map 1, alive, not in combat / targeted within 15 tiles, destination on the map, no ready check / sleep running), hides the PC and its followers (`OF_OFF | OF_INVULNERABLE`), moves them one sector per 62 ms, rolls `wmap_rnd` every 5-11 sectors and sends PROGRESS every 150 ms. ARRIVED / CANCEL (closing the world map) / an encounter land the PC (sectors kept, epoch bump, PC_POS FORCE); the client closes its world map and re-centres. A new START during a trip re-routes it. Refusals: REFUSED with a reason (EN/RU notice).
- Allies' markers: refreshed every 1000 ms (PROGRESS without MOVING), coloured by slot; an encounter shows a blinking red marker for 15 s.
- Map-changing transitions keep the ready check: a transiting slot is BUSY, and party travel cancels all trips and carries everybody.

### M5: unified sleep (`COOP_RULE_UNIFIED_SLEEP`, key `usleep`, default on)

- A rest choice in the sleep window becomes a vote (`COOP_CTRL_SLEEP_VOTE` 9, mode 1 morning, 2 evening, 3 healed, 4 8 h, 5-8 the 1 / 2 / 4 / 24 h buttons). The host keeps the round (`coop_middle_sleep_step`) and sends `COOP_CTRL_SLEEP_STATE` 10 (ready mask, mode, initiator, living count) on every change and every 2 s.
- A lone living player (host) sleeps at once with the vanilla window. Otherwise a banner shows "Sleep: n/m ready (Name: mode). Y = join sleep, N = cancel" (RU behind `ARCANUM_COOP_LANG=ru`). Moving, combat, a dialog or a world map trip cancels your own vote. Ready players regain 2 fatigue per second while waiting.
- When every living player is ready: REST_BEGIN, host fade, the clock moves (morning 08:00 / evening 20:00 / fixed hours / until healed, max 168 h), all PCs and followers heal, time and vitals are synced, REST_END. Dead and disconnected players do not block.
- **With the M1 rest vote:** while `usleep=1`, unified sleep replaces the M1 client rest request (and the Lite refusal). With `usleep=0`, the M1 rest vote (`rest=1`) and the Lite behaviour are unchanged.

### M5 fixes (`d0c4c597`)

- **Teleport spell sent the whole party to the edge of the world.** Vanilla MP (`MTComponentMovement`, move_location 2) put the caster on the map's starting location (the corner of map 1) and the party carry followed. Now the caster picks the place on the world map like in single player: the host opens its own pick; a client gets `COOP_OWT_SELECT` and its pick comes back as START with `F_TELEPORT` (the host checks anti-teleport, charges the spell, hops only that PC). Off map 1 the usual party travel ask is used.
- **Client world map trips ignored the plotted route** (a straight 8-way line to the last point, over mountains and water). The client now sends its waypoints (WAYPOINT op) before START; the host plans each leg with the vanilla `wmap_path_make` (blocked terrain, bridges, passes) and steps sector by sector; an impassable route is refused (`REFUSE_BLOCKED`, vanilla text 601).

### M6: solo interiors / dungeons (`COOP_RULE_SUBINSTANCES` 0x400, key `subinst`, default on)

Off = the M5 behaviour (a client's map change is a party travel ask). On, when a **client** walks into another map (stairs, door, world map entrance) that is not the host's map, not map 1 and not the shop map:

- **Lease.** The host checks `coop_subinst_can_lease`: one player per map (else DENY busy: "<Name> is already inside <map>. Wait or ask the host for group travel." / RU "<Name> уже находится в <map>. ..."), not in combat, alive. On GRANT the host parks the client's PC and followers on its own map (`OF_OFF | OF_INVULNERABLE`, place remembered), packs `Save\Current\maps\<map>` into a **CMAP1** archive and streams it.
- **CMAP1** (`src/game/coop_subinst.c`, pure + unit tested): wire = u32 raw_len + zlib(level 6) of raw = header `CMAP`, u32 map_id, u32 count, u32 CRC32 of the body, then count x {u16 name_len, name, u32 len, data}. Limits: raw 2 MB, wire 512 KB, 128 files, names 1..63 chars of `[A-Za-z0-9_.-]`, no leading dot / `..` / path separators, extension `.dif` / `.mdy` / `.md` / `.des`, no duplicates (case-insensitive). The receiver validates everything (sizes, magic, CRC, names, trailing bytes, map id) before it touches a file; it never overwrites the map it has open.
- **Packets:** 171 `PACKET_COOP_SUBINST_CTRL` (0x40, `BOTH`, op-checked: 1 GRANT, 2 DENY, 3 CHECKPOINT_REQ, 4 COMMIT_BEGIN (the only client op), 5 COMMIT_ACK, 6 ABORT, 7 STATE), 172 `PACKET_COOP_SUBINST_CHUNK` (0x18 header + 1..4096 bytes, in order, `BOTH`), 173 `PACKET_COOP_SUBINST_DELTA` (0x18, client -> host, `HOST_RESOLVE`). Type limit 176. All three have validators in `multiplayer_validate_message`.
- **Inside** the client runs the map with local authority (`tig_net` local suspend, like single player): host world packets are dropped (chat, rules, player list, time, pause / sleep / slot status still pass), the client pumps the network itself. Story changes go to the host as deltas: global flags / vars, quest states, rumors, reputations, known areas, fates, earned XP. The host accepts them only from the slot holding that lease id.
- **Checkpoints** every 45 s and when the host saves (`CHECKPOINT_REQ`, the save waits up to 2000 ms pumping the network): the client flushes the map (its PC, its pack and followers are kept out of the files), packs it and sends map + character bundle + gold / ammo quantities. The host writes the map files and keeps the last checkpoint for a disconnect.
- **Leaving** (any map change of the local PC): final commit. The host writes the map, reconciles the PC (pack, gold / ammo, HP / fatigue), and: back to the host's map at the exit's spot, or (another leasable map) straight into the next solo instance (`F_NEXT`, sub -> sub), else back where the PC went in. The client is then resynced (SESSION_SYNC).
- **Death inside:** no game over; 5 s later a commit with `F_DEAD`, the host kills the parked PC at the entrance.
- **Disconnect:** the last checkpoint is applied, the PC is parked like any disconnected player. **Host load / map change / party travel:** every lease is aborted (ABORT, notice), the PCs come back and are resynced.
- **UI:** badge **M** (120,160,255) between Z and P; on the world map the entrance shows the ally marker; chat "[Co-op] <Name> entered <map> (solo instance)." / "[Кооп] <Name> вошёл в <map> (отдельная локация)." and "... left <map>.".

Review fixes (still protocol 58):

- **Local authority inside:** while a client holds a solo instance, `tig_net_set_local_authority` makes `tig_net_is_coop_client()`, `coop_lite_is_client_role()`, `coop_lite_client_local_goal_obj()`, `coop_lite_client_local_loot()` and `coop_lite_story_write_blocked()` read false (AI, doors, scenery, loot, floor traps, dialogs, followers, quests / rumors / reputation run locally; story setters still emit `PACKET_COOP_SUBINST_DELTA`). While the client pumps the network (suspend paused) it is a normal client again, so time, chat, pause, sleep, badges and ENet keep working; the Middle UI tick (pause / sleep banners, F9 HUD, status) and the launcher lobby tick run from `coop_subinst_ping`.
- **Checkpoints reconcile the parked PC** too, so a host save always holds map files and pack from the same moment.
- **Item state table** (44-byte entries, every carried item): quantity (gold / ammo), `OBJ_F_HP_DAMAGE`, charges (`OBJ_F_ITEM_SPELL_MANA_STORE`), `OBJ_F_ITEM_MANA_STORE` and `OBJ_F_ITEM_INV_LOCATION` are applied to items the host already had (moves / re-equips in two passes); the inventory list is repaired (PT54d) before reading and after inserting.
- **mobile.des:** the archive keeps the flush's destroyed list (static things we carry out stay gone, like a vanilla teleport); the client's local copy only drops our party's ids again; real destructions are kept.
- **No stalls:** sub -> sub packs the next map before `COMMIT_ACK(F_NEXT)`; a refused next map gets DENY + `COMMIT_ACK(0)` (the client stays COMMITTING through the DENY); a failed grant after the ACK sends ABORT. New client op **8 CANCEL** (lease given back): a broken archive stream, a refused archive, no map within 20 s (REQUESTED / RECEIVING) or no arrival within 15 s gives the lease back; the host unparks the PC and resyncs. Op 8 is new within protocol 58 (no 58 build was released before it).
- **XP:** `quest_state_set_internal` grants no XP (only `quest_state_set` does, via `critter_give_xp`, which sends the single XP delta), so QUEST + XP deltas don't double.
- **Teleport (M5):** the host remembers the spell it asked a client to place (SELECT) and accepts one pick for it; fatigue is charged only when the caster cast it itself (`source_obj == target_obj`), not from a scroll (vanilla: `wmap_ui_obj` is the scroll, no charge). Followers already travel: `coop_owt_finish` moves all of the PC's followers.

Deviations from the spec: magic is 4 bytes `CMAP` (the version is the format, not a 5th byte); `.md` and `.des` are allowed besides `.dif` / `.mdy` (the engine keeps object diffs and the destroyed list there); STATE (op 7) is an extra host -> all op for the badge / marker; a soloist's XP is not split with the party (it was earned alone); DELTA has role `HOST_RESOLVE` (client -> host); the reconcile matches items by object id and replaces the host's pack with the client's (new items read from the bundle); a dead PC is committed out to the entrance, not left inside.

## Launcher

`arcanum-coop-launcher.ini`:

```ini
[rules]
gold_split=0
loot_feed=1
rest_vote=1
direct_trade=1
client_pause=0
no_ff_aoe=1
shared_quest_items=1
shared_followers=1
unified_sleep=1
solo_instances=1
enemy_hp_pct=100
xp_mult_pct=100
autosave_min=10
```

Host mode shows five rows (disabled in Join mode):

- **Split gold**, **Loot feed**, **Rest vote**, **Direct trade**;
- M3: **Client pause**, **No friendly fire**, **HP %** (100 / 150 / 200 / 250 / 300) and **XP %** (100 / 125 / 150 / 200);
- M4: **Shared quest items**, **Shared followers**;
- M5: **Unified sleep** (the **Free world map** box was removed on 2026-11-27: always on unless Coop Lite mode);
- M6: **Solo interiors/dungeons** (`solo_instances=1`);
- M7: **Auto-save** Off / 5 / 10 / 15 / 30 min (`autosave_min=10`), and the lobby rules line from `lobby_rules.txt`.

An odd ini value lands on the nearest step below it. At start, the host gets `ARCANUM_COOP_RULES` built with `coop_rules_format_env` (the launcher links `coop_rules.c`). Clients don't get it: they receive the rules from the host. The optional self-update on this line looks for `coop-middle-*` tags, so a Middle launcher never installs a Lite (protocol 52) build.

## Log lines

```
CoopMiddle: host rules from ARCANUM_COOP_RULES='...' (N keys): gold=0,loot=1,rest=1,trade=1,pause=0,ff=1,hp=100,xp=100 | Rules: ...
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
CoopMiddle: pause ON|OFF (slot S, host key|client request|the pauser disconnected|main menu|map change / load|pauser inactive)
CoopMiddle: pause on|off asked                                (client)
CoopMiddle: pause ON|OFF from the host (slot S)               (client)
CoopMiddle: pause request from slot S refused (rule off or inactive)
CoopMiddle: pause request from slot S dropped (rate limit)
CoopMiddle: pause state sent to slot S (paused by P)
CoopMiddle: pause banner: <text>
CoopMiddle: enemy HP P%: damage D -> S (first hit on this map)
CoopMiddle: friendly fire blocked (attacker slot A -> target slot T, type N)   (1/s)
CoopMiddle: ping sent kind K at X,Y                            (client)
CoopMiddle: ping from slot S (<name>) kind K at X,Y
CoopMiddle: ping from slot S dropped (map M / off map|rate limit)
CoopMiddle: ping dropped (size N)
CoopMiddle: command C from slot S
CoopMiddle: command from slot S refused (reason R, N s)
CoopMiddle: regroup slot S -> X,Y
CoopMiddle: /gather moved N player(s)
CoopMiddle: quest item K (mode M) for slot A found in slot B's pack: '<item>' (loc L)
CoopMiddle: quest item '<item>' handed over from slot B (<name>)
CoopMiddle: town map T fog snapshot sent to everyone|slot S (N bytes)
CoopMiddle: town map T fog synced from the host                (client)
CoopMiddle: town map T fog: N tile(s) opened by a client|the party   (1/10 s)
CoopMiddle: town map fog packet dropped (size N)
CoopMiddle: dialogs: slot S watches slot O's dialog (NPC busy)
CoopMiddle: dialogs: the host watches slot O's dialog
CoopMiddle: dialogs: slot S's dialog closed (map change / load)
CoopLite: dialog: slot S votes option N (session O)
CoopMiddle: followers: slot S moved an item inside a follower's pack to loc L: done|refused
CoopMiddle: followers: slot S rearranged a follower's pack (N moved, R refused)
CoopMiddle: followers: slot S layout for a follower it may not manage
CoopMiddle: followers: exchange window with '<name>' requested
[CHAT] [Quest] ...
[CHAT] [Co-op] ...
```

M5 log lines:

```
CoopMiddle: sector keep window 3x3|adaptive 2x2 (remote R, cache C, locked L, free if 3x3 F)
CoopMiddle: world map trip of slot S: x,y -> x,y (N object(s) in transit)
CoopMiddle: world map trip of slot S re-routed to x,y
CoopMiddle: world map trip of slot S ended (arrived|stopped|random encounter|disconnected|party travels|...) at x,y
CoopMiddle: world map trip of slot S refused (reason R)
CoopMiddle: world map trip to x,y asked
CoopMiddle: slot S landed from the world map at x,y [(encounter)]
CoopMiddle: random encounter on the host's own world map trip at x,y
CoopMiddle: unified sleep: ready 0xMM mode M (vote|check|moved|dialog|combat / travel|...)
CoopMiddle: unified sleep: everyone ready (...) - sleeping, mode M
CoopMiddle: unified sleep done: mode M, H h healed, now day D hh:mm
CoopMiddle: slot S collapsed from exhaustion
CoopMiddle: autosave due but blocked (world W pause P combat C dialog D map M travel T commit K) - retry in 15 s
CoopMiddle: autosave -> SlotNNNN '[Host] AutoSave N'
CoopMiddle: autosave N FAILED - retry in 15 s
CoopMiddle: unified sleep: SLEEP_APPLY H h -> slot S (solo instance)
CoopMiddle: party slept H h - healing inside the solo instance
CoopMiddle: solo instance: healed H h with the party's sleep
CoopMiddle: trade: window auto-closed (solo instance|overworld travel|death|combat|out of range / gone)
```

## Tests

`ctest` target **`coop_middle`** (`tests/coop_middle_test.c`, headless; also built in the ASan/UBSan tree):

1. `coop_rules_default` / `coop_rules_sanitize`: bounds, unknown bits, reserved bits kept, `reserved[]` zeroed.
2. `coop_rules_format_env` <-> `coop_rules_parse_env` round trip for every flag combination, ini aliases, partial, garbage, overlong and out-of-range strings, truncated output.
3. `PACKET_COOP_RULES`: size 32, type 167 < type limit, min size, role `CLIENT_APPLY`, `coop_lite_packet_allowed` (host -> client only), version check, `TIG_NET_PROTOCOL == 56`.
4. `coop_middle_split_gold`: 1..8 players, no loss (`each * n + rem == amount`), carry, too little to split, overflow saturation.
5. `coop_middle_loot_feed_worthy` (keys, scrolls, written, quest, unique, cost 250 pass; junk, cheap potions, ammo, gold, non-items fail), the 5/s limiter with the collapse count and ms-clock wrap, and the EN/RU line formats.
6. `COOP_CTRL_REST_ASK` / `COOP_CTRL_REST_REFUSE` in `coop_lite_party_ctrl_valid` (modes, reasons, `b == 0`, Lite ops unchanged, unknown ops refused), the 5 s ask limit.
7. M2 trade: `coop_middle_trade_partner_ok` (range 6, other map, rule off, self, NPC, disconnected, dead, bad slots), `coop_middle_trade_transfer_ok` and Lite's `coop_lite_target_transfer_ok` routing (give yes, take / steal bits no), EN/RU `[Trade]` lines (item, gold, missing separator, overlong, tiny buffer).
8. M2 status: `coop_middle_status_pack` / `_get` (nibbles, masking, bounds), badge priority and colours X > Z > P > D > I > C, `COOP_CTRL_SLOT_STATUS` validity (`b == 0`, short packet).
9. M2 labels: `coop_middle_labels_deoverlap` (same spot, side by side, a stack of 32 stays pairwise apart and inside the view, limited room hides the rest, degenerate input, a label below the view is pulled up).

10. M3 rules: pause / ff defaults and active bits, `pause` / `ff` / ini aliases / old `noffaoe`, hp / xp keys, long aliases, clamps, launcher steps round trip, describe EN/RU.
11. M3 scaling: `coop_middle_scale_enemy_damage` at 100 / 150 / 200 / 300, the remainder series (6 + 7 + 7, 300 hits lose nothing), minimum 1, 0 for <= 0, bad remainder, NULL; `coop_middle_scale_xp` at 100 / 125 / 150 / 250, floor, <= 0, `INT_MAX` overflow.
12. M3 commands: `coop_middle_parse_cmd` (`/tp`, `/TP  `, `/stuck`, `/gather`, `/tp all`; plain text, `/unknown`, extra words, overlong not caught), cooldown seconds (round up, wrap), EN/RU regroup / refusal / pause-drop lines.
13. M3 pings: id 168 < limit 170, size 24, offsets, min size, role `BOTH`, direction, `coop_middle_ping_valid` (size, type, kind 0..1, slot 0..7), 1/s rate limit.
14. M3 pause: `COOP_CTRL_PAUSE` in `coop_lite_party_ctrl_valid` (`a` 0/1, `b` 0..7, short packet), 2 s rate limit, `TIG_NET_PROTOCOL == 56`, stub hooks are no-ops.
15. M4 rules: bits 0x40 / 0x80, on by default, always in the string (also when off), `qitems` / `sfol` and ini aliases, describe EN/RU not cut off.
16. M4 quest items: `coop_middle_quest_pocket_eligible` (radius 30, rule off, not host, self, disconnected, dead, other map, unknown distance, bad slots), backpack / worn rank, EN/RU `[Quest]` line, stubbed lookup.
17. M4 fog: id 169 < limit 170, size 0x410, offsets, role `BOTH`, min size, `coop_middle_townmap_fog_valid` (short / long, flags, id, len 0 / > 1024, offset cap, no wrap, type), `coop_middle_fog_merge` (OR count, idempotent, REPLACE), FNV-1a.
18. M4 dialogs: `coop_middle_dialog_route` (own > attached > host > recent, gone sessions, bad input).
19. M4 followers: `coop_middle_follower_access_ok` (leader always, range 6, rule off, not a party follower, dead), `PacketCoopNpcStats` 0x120 with the skill offsets, the old 0xA0 size refused, min size.

20. M5 rules: bits 0x100 / 0x200, on by default, `ow` / `usleep` always in the string, ini aliases, describe EN/RU.
21. M5 interest: `coop_middle_keep_sector_margin` (<= 4 remote or >= 36 free -> 3x3, else margin 32), `coop_middle_keep_ids` (3x3, 2x2 corners at offsets 31 / 32, no negative sectors, max), `coop_middle_loc_within`, radii per packet type on / off map 1, NPC_POS force on range entry.
22. M5 world map travel: id 170, limit >= 172, size 0x20, role `BOTH`, min size, `coop_middle_overworld_pkt_valid` (ops, direction, flags, slot, size), `coop_middle_owt_advance`, `TIG_NET_PROTOCOL == 57`.
23. M6 solo instances: `coop_subinst_filename_safe`; CMAP1 pack / unpack round trip for 0 / 1 / 20 files; traversal, duplicate and too many names refused; truncated, oversized raw length, trailing bytes, every flipped byte (zlib / CRC) refused; payload build / split, quantity table; chunk reassembly (order, size, lease id); `coop_subinst_can_lease` (normal / busy / state / own lease); client state machine; validators for 171 / 172 / 173 (direction, sizes, flags, ranges); badge M priority and colour; chat kind 10 EN / RU; rule default on; `TIG_NET_PROTOCOL == 58`. Review: `coop_subinst_des_filter`, the item state table, `coop_subinst_sub_next_plan` and COMMITTING through DENY (no stall in REQUESTED), `coop_subinst_client_stalled`, CANCEL validator, client role / `tig_net_is_coop_client` false under local authority and true again while pumping.
23. M5 sleep: `coop_middle_sleep_step` for 1..8 players (solo, wait, execute, mode kept, initiator hand-over, dead player not blocking, cleared, refusals), `coop_middle_sleep_minutes` across midnight, `coop_middle_sleep_mode_from_ui`, banner text EN/RU, `SLEEP_VOTE` / `SLEEP_STATE` validity.
24. M7: `coop_middle_edge_indicator` (inside / on the border = false; top, bottom, left, right and the 4 corners land exactly on the pad border with the right octant; off-axis sweeps stay on the border); `coop_middle_collapse_step` (once per fall, 10 s per slot); chat kinds 11 / 12 EN / RU; `coop_middle_autosave_ready` (pause, combat, dialog, world map, commit, teleport, not in world, interval 0, clock wrap) and the rotation 9690..9694 (never a client slot 9700..9789); rule `asave` (0, 10, 1 -> 5, 999 -> 60, aliases, describe, `sizeof(CoopRules)` unchanged); unified sleep with a solo-instance player (in the consensus, `coop_middle_sleep_apply_mask` flags it for `SLEEP_APPLY`); `SLEEP_APPLY` in `coop_middle_party_ctrl_valid` / `coop_lite_party_ctrl_valid` (1..168, `b` = 0, size); repeated session resets; `TIG_NET_PROTOCOL == 59`.
25. After M7 auto-levelling (`test_autolevel`): `flvl` / `plvl` on by default, parse + aliases (`follower_autolevel`, `party_level`), format, `coop_rules_effective` (Middle keeps them, `lite=1` clears them), describe EN / RU not cut, `coop_middle_follower_level_target` (raise only, max - 1 cap), `coop_middle_party_xp_target`, `TIG_NET_PROTOCOL == 60`.
26. `test_hop_and_followers`: `coop_middle_solo_hop_rule` (same map / -1 solo in Middle, map change and Lite not), `coop_middle_follower_list_plan` (consistent list unchanged; null, freed and duplicate entries dropped; missing table followers appended; capacity).

Fuzz (`coop_lite_fuzz8`, `coop_lite_rx_fuzz`; M7): packet types 167-173 and the Middle PARTY_CTRL ops (REST_ASK / REFUSE, SLOT_STATUS, PAUSE, SLEEP_VOTE / STATE / APPLY) through the validators; `SUBINST_CTRL` with ABORT and CANCEL in both directions (ABORT is only valid from the host, CANCEL only from a client); chunk reassembly with random chunks. `COOP_RX_FUZZ_SECONDS` sets the transport phase length, `COOP_FUZZ8_SEED` varies the fuzz8 stream for soaks.

## Architecture and hooks

All Middle code lives in new files:

- `src/game/coop_rules.h/.c`: the session rules contract (pure C, also linked into the launcher and the tests).
- `src/game/coop_middle.h/.c`: the M1/M2 mechanics. Pure helpers come first (headless tests). The game side is compiled out under `COOP_LITE_TEST_STUBS`.
- `src/ui/coop_middle_ui.h/.c` (M2): trade entry points, portrait badges, Alt loot labels, F9 net HUD; M3: pause banner, pings, the world mouse hook.

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
| **M3** | |
| `tig_net.h` | protocol 55 |
| `coop_lite.h` | `PACKET_COOP_TYPE_LIMIT` 169 |
| `coop_lite.c` | role `BOTH` + min size of `PACKET_COOP_PING`; `coop_lite_host_share_xp` -> `coop_middle_host_scale_xp`; chat relay / own send -> `coop_middle_try_chat_command`; bridge `coop_lite_mid_move_pc` |
| `multiplayer.c` | dispatch of `PACKET_COOP_PING` |
| `timeevent.c` | `timeevent_ping`: no game / anim time while `coop_middle_is_paused()` |
| `combat.c` | `combat_dmg`: `coop_middle_host_ff_block` (start), `coop_middle_host_enemy_damage` (after the bonuses) |
| `intgame.c` | `intgame_process_event` start -> `coop_middle_ui_world_event` (ping, paused clicks); Pause key -> `coop_middle_pause_key`; spell hotkey refused while paused |
| `wmap_ui.c` | town map draws live pings (blinking cross); `wmap_ui_coop_town_refresh()` |
| **M4** | |
| `tig_net.h` | protocol 56 |
| `coop_lite.h` | `PACKET_COOP_TYPE_LIMIT` 170; `PacketCoopNpcStats` + skills (0x120); new dialog-session API |
| `coop_lite.c` | role `BOTH` + min size of `PACKET_COOP_TOWNMAP_FOG`; NPC stats fill / apply of skills; `coop_lite_barter_partner_ok` / `coop_lite_follower_exchange` -> `coop_middle_shared_follower_ok`; Packet81 and `INVEN_LAYOUT` accept moves inside a follower's pack; the PT36/PT41 dialog block now keeps 8 sessions (see below); map opened ends all client dialogs |
| `multiplayer.c` | dispatch of `PACKET_COOP_TOWNMAP_FOG` |
| `dialog.c` | conditions `in` / `ni` and action `in` -> `coop_middle_find_party_item` (+ `coop_middle_quest_handed_over`) |
| `script.c` | `SCT_OBJ_HAS_ITEM_NAMED`, `SAT_TRANSFER_ITEM`, `SAT_DESTROY_ITEM_NAMED` -> the same |
| `townmap.c` | `townmap_coop_fog()` / `townmap_coop_fog_touched()` (the known-tile bit array) |
| **M5** | |
| `tig_net.h/.c` | protocol 58; `tig_net_send_pose(data, size, slot)` (addressed unreliable send for NPC_POS) |
| `coop_lite.h` | `PACKET_COOP_TYPE_LIMIT` 172 |
| `coop_lite.c` | NPC_POS per-slot batches (44 tiles, seen mask); DOOR_STATE / TRAP_FX / PROJECTILE_FX -> `coop_middle_send_app_near`; keep window -> `coop_middle_keep_sector_set`; role `BOTH` + min size of 170; host world-map dimming off while `ow` is active; party travel aborts world map trips; client world map click -> `coop_middle_owt_client_propose`; a transiting slot is BUSY in the ready check; bridges `coop_lite_mid_keep*` / `coop_lite_mid_center_local` |
| `sector.c/.h` | co-op cache 128; `sector_cache_coop_stats()` |
| `teleport.c` | host's own hop on map 1 with `ow`: carries no other PCs (no `coop_lite_after_party_carry`), no FOLLOW_HOST |
| `multiplayer.c` | dispatch of `PACKET_COOP_OVERWORLD_TRAVEL` |
| `mp_utils.c` / `magictech.c` | sound (106) and eye-candy add (77) -> `coop_middle_send_app_near` |
| `wmap_ui.c/.h` | frozen clock + own encounter rolls on map 1; client close -> CANCEL; ally markers (WORLD view); `wmap_ui_coop_marker_set` / `_local_sector` / `_refresh` |
| `wmap_rnd.c/.h` | `wmap_rnd_coop_check(loc)` |
| `sleep_ui.c` / `intgame.c` | rest choice -> `coop_middle_sleep_choose`; Y / N -> `coop_middle_sleep_key` |
| `dialog_ui.c` | `sub_567420` returns a client PC's own entry on the host; party checks by slot; the choose hook gets the slot; a client's dialog never cuts the host's voice line |
| `inven_ui.c` | follower (free) barter mode also for a shared follower |
| `coop_middle_ui.c` | Ctrl+LMB on a teammate's follower opens the follower exchange |

The earlier Middle prototype (protocol 53/54 commits `8132dc3c`..`4e1e5455`: pings, pause, difficulty, quest pocket, town map fog, parallel dialogs, ...) was reverted in one commit before M1. Ideas from it come back stage by stage in the Middle files. Parallel dialogs came back as M4.

M6 / M7 hooks:

| File | Hook |
| --- | --- |
| `coop_subinst.h/.c` (new, M6) | solo instances; M7: `coop_subinst_host_lease_mask`, `coop_subinst_host_commit_busy`, `SLEEP_APPLY` in the packet filter, `coop_middle_subinst_local_tick` + pause-frozen checkpoint clock in `coop_subinst_ping` |
| `tig_net.h` | protocol 59 (M7); 60 (after M7: `flvl` / `plvl`) |
| `teleport.c` | t403u: `coop_middle_host_solo_hop` for the solo hop; null / freed follower and item handles skipped |
| `level.c/.h` | after M7: `level_follower_raise_to` (public wrapper over the static `update_follower_level`) |
| `main.c` | M7: `coop_middle_take_autosave` / `coop_middle_autosave_done` next to the PT61 client save |
| `gamelib.c` | M6: `coop_subinst_host_before_save`; M7: `coop_middle_note_save` (only a successful save restarts the autosave interval) |
| `wmap_ui.c` | M7: dead teammate square red on the town map |
| `coop_middle_ui.c` | M7: arrow plates, trade auto-close watch, town map refresh on badge change |
| `tools/launcher/launcher.c` | M7: Auto-save combo, lobby rules line; after M7: Auto-level followers, Party level |

## Known limits / risks

### M1

- **Not tested in play yet:** only the build and headless tests.
- The rest prompt uses the Lite travel Y/N panel. If a travel or load prompt opens while a rest request waits, the rest request is cancelled (busy) rather than queued.
- An item the host places itself after a client's loot move failed (the client's grid cell was taken on the host, PT46 retry) gets no feed line. This path is rare.
- `enemy_hp_pct`, `xp_mult_pct` and the reserved flags (`pause`, `noffaoe`) are carried and validated, but have no effect yet (M3: all of them are active now).

### M2

- **Not tested in play yet:** only the builds (Linux, ASan/UBSan, Win64) and the headless tests. The overlay drawing (labels, HUD, badge letters) has not been checked on screen.
- Trade reuses the pickpocket window, so its title and look are the vanilla pickpocket ones. On open, the game turns the PC to the partner and needs line of sight (`ai_can_see`), as for pickpocketing.
- If the host refuses `OPEN_TRADE` (rare, because the client checks the same rule first), the client's window stays open. Gives are then refused and the packs resynced.
- Gold is given as the whole dragged pile (the vanilla pickpocket drag has no amount prompt).
- While the rule is on, Ctrl+LMB on a teammate no longer runs the vanilla Ctrl action on that PC.
- The HUD's sync check uses "time since any app packet". The 5 s status heartbeat keeps an idle link at OK. RTT and loss come from ENet only (Steam shows `RTT ?`).
- A client's INVEN / DIALOG badge lags by one host poll (250 ms) plus the network.

### M3

- **Not tested in play yet:** only the builds (Linux, ASan/UBSan, Win64) and the headless tests. The banner, the ping diamonds and the town-map cross have not been checked on screen.
- The pause freezes game and anim time on every machine, but real-time events still run (UI, MP, the TB-combat timer, fidget queueing). Co-op disables turn-based combat, so the TB timer does not matter in practice.
- The host opening the in-game main menu (Esc) also lifts the pause, as the spec asks ("reset on main menu").
- No friendly fire covers every party-on-party hit, not only AoE, because the spec's condition has no AoE test. A party member can no longer hurt a teammate on purpose while the rule is on.
- Enemy damage scaling divides the final HP damage only. Fatigue and poison damage to enemies are not scaled.
- A danger ping is detected on the hovered object or on objects on the cursor tile. A hostile NPC standing next to that tile gives a normal ping.
- `/tp` decides "same map" from the client PC's location on the host (`coop_lite_loc_in_map`). A client that is still loading has no PC there yet and gets "dead / not here".

### M4

- **Not tested in play yet:** only the builds (Linux, ASan/UBSan, Win64) and the headless tests.
- Quest items: the lookup by name covers what the game checks by name (dialog `in`/`ni`, script "has item named" / transfer / destroy). Only these by-name lookups (`item_find_by_name`) are hooked; `COOP_QITEM_MATCH_PROTO` is in the API but unused by the hooks. Dialog `ri` is the NPC "remove item" order (opens an inventory window), not a quest take, so it has no hook.
- Fog: the client applies host packets for any valid town map (the game keeps one town map's array in memory, so this swaps the cached array and saves the old one). A client that changes town map at the same moment as the host can wait for the next snapshot (SESSION_SYNC or map change) before it sends its own bits.
- Dialogs: two or more dialogs can run at once. Since t391u a client may start a story dialog anywhere on the shared map: `coop_lite_party_talk_gate` ignores the host's distance and the host's combat (the speaker's own session, own fight, turn-based combat and the 5-tile speaker range still count); a speaker whose NPC is not loaded on the host gets its sector window kept (`coop_lite_keep_sector`) and the NPC looked up again. Lite mode keeps PT41's leader-distance rule. The client keeps one viewer panel: it shows the routed session only. Headless dialogs play no voice while the host is in its own dialog.
- Followers: `spell_tech[16]` carries the 16 spell colleges as the spec asks; tech degrees (`OBJ_F_CRITTER_SPELL_TECH_IDX` 17..24) are not in the packet. The client does not send a follower's grid layout (`INVEN_LAYOUT`); its follower-window moves arrive as Packet81, and the host also accepts a follower layout if one comes. With the rule on, Ctrl+LMB on a teammate's follower opens the exchange instead of the vanilla Ctrl action.

### M5

- **Not tested in play yet:** only the builds (Linux, ASan/UBSan, Win64) and the headless tests.
- Distances are Chebyshev (tile squares), not the engine's path distance.
- (fixed in `d0c4c597`: client trips follow the plotted vanilla route.) An encounter roll uses the vanilla `wmap_rnd_check` at the trip's sector (its level / table quirks come from the host PC). The landing spot of a stopped trip is the nearest known area, else the sector centre.
- Markers are drawn in the WORLD view only (not on the continent view). "Clicking" a marker is the normal route click on that spot.
- ARRIVED is broadcast to all (others un-hide the traveller). Closing the world map during a trip stops it.
- Morning is 08:00 as the spec says (vanilla uses 07:00). Unified sleep always heals (the vanilla "wait" choice is not separate). Execution is synchronous on the host with a fade.
- Doors and eye-candy removals outside map 1 are not filtered; on map 1 a door changed far away reaches a slot through the radius WORLD_REQ when it enters the sector.

### M6

- **Not tested in play yet:** only the builds (Linux, ASan/UBSan, Win64) and the headless tests (CMAP1, payload, chunks, leases, client states, packet validators, badge, chat line).
- The commit flush hides the PC, its pack and followers by marking them destroyed during `map_flush` and restores `mobile.des` afterwards; an engine path that reacts to these flags during the flush would break it.
- Reconcile trusts the client's bundle for its own PC's pack (bounded, PC id checked) - fine for friends, not for strangers.
- Things done inside that are not deltas (e.g. NPC states on other maps changed by scripts, faction / blessing / curse changes, time passing) stay local to the client and are lost on commit. Time inside does not move the host's clock.
- A map over the limits (2 MB raw / 512 KB compressed) falls back to the normal party travel ask (DENY "too big" notice).
- The save wait pumps only the network (no frames) for up to 2 s; a slow client misses that save's checkpoint (the previous one is used).
- Followers that walked in with the client are parked with the PC on the host map and come back with it; followers recruited inside stay in the client's map files only.

### M7

- **Not tested in play yet:** only the builds (Linux, ASan/UBSan, Win64), the headless tests and the fuzz soaks.
- The collapse notice and the arrows use the host's view of a PC; a PC inside a solo instance is skipped (its parked copy on the host is stale).
- Autosave writes on the main loop like the PT61 client save; the 2 s checkpoint wait pumps only the network, so the game freezes for up to 2 s when solo-instance clients are slow.
- `gamelib_last_save_name` keeps the PT77q behaviour: client F7 world saves (9700..9789) still count as the newest save for party-wipe reload / `AUTOLOAD=latest` (they are full host world saves); autosaves count too.
- `SLEEP_APPLY` heals the solo-instance PC with the hours the host slept; the client's clock inside still does not move (M6 limit), only the host's game time packet arrives.
- The trade auto-close on the partner's side relies on the partner PC being hidden (OF_OFF) during a trip / instance and on the 6-tile trade rule; a lost hide flag leaves the window to the host's existing transfer checks.
