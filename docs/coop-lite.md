# Arcanum Coop Lite — investigation & design notes

Branch: `coop-lite-worldmap`
Date: 2026-10-19
Target: real-time co-op (up to 8 players) on arcanum-ce, campaign-compatible.
Required compatibility: **Arcanum: ReForged** ([kurkijar/Arcanum-ReForged](https://github.com/kurkijar/Arcanum-ReForged)).

**Status (coop-lite-anim-combat):** ENet + P1–P3 + **anim/combat/magictech receive**.
Host-authoritative: clients send intents (move/cast); host resolves and broadcasts;
clients apply pose, combat mode, vitals, spells, FX. Late-join world xfer still missing.
SP without `-coop` unchanged.

Runtime testing is currently impossible: game data is not on this box. A clean
**Steam** install archive is expected soon; ReForged will be installed from that
(see §4.0 Steam vs GOG).

---

## 1. What network / multiplayer code exists in arcanum-ce

### 1.1 Transport: ENet (Coop Lite)

`src/net_compat.h` now includes `src/net/tig_net.h`. Implementation:
`src/net/tig_net.c` on vendored **ENet 1.3.18** (MIT) under `third_party/enet/`.
Inactive unless `-coop` is passed (macros replaced by real functions that no-op
when coop is off). Historical stub behaviour for reference:

| Macro | Stub behaviour |
| --- | --- |
| `tig_net_is_active()` | `0` |
| `tig_net_is_host()` | `0` |
| `tig_net_send_app_all` / `tig_net_send_app` / `tig_net_send_app_except` | no-op |
| `tig_net_start_server` / `tig_net_start_client` | no-op / `0` |
| `tig_net_on_message` / validation / network event | no-op |
| client info / xfer helpers | 0 / empty |

`first_party/tig/` has **no** net module (no sockets, no SDL_net, no enet).
The original Troika `tig_net` layer was never reimplemented. Upstream author
(Alex Batalov, medium post May 2025) stated remaining work was “mostly
networking and editor” and that he may remove multiplayer entirely.

### 1.2 Game-side MP logic: large but dead without transport

| File | Size | Role |
| --- | --- | --- |
| `src/game/multiplayer.c` / `.h` | ~3043 / 71 lines | 8 player slots (`NUM_PLAYERS 8`), save/load, start/end, ping, flags, portrait/level-scheme helpers |
| `src/game/mp_utils.c` / `.h` | ~637 / 698 lines | ~50 `Packet*` structs + host-gated send wrappers |

Notable APIs:

- `multiplayer_init` / `exit` / `reset` / `save` / `mutliplayer_load`
- `multiplayer_start` / `multiplayer_end` (registers message callbacks into stubs)
- `multiplayer_find_slot_from_obj`, `multiplayer_player_find_first/next`
- `multiplayer_is_locked` / `lock` / `unlock`
- `multiplayer_map_open_by_name`, `multiplayer_timeevent_process`

**Receive path is incomplete:**

```c
void multiplayer_handle_message(void* msg)
{
    // TODO: Incomplete.
}

bool multiplayer_handle_network_event(int type, int client_id, void* data, int size)
{
    // TODO: Incomplete.
}
```

`multiplayer_validate_message` only checks `type` in `(0, 131)`.

Packet catalogue (partial): `PacketGamePlayerList` (type 2), `PacketGameTime`,
anim/combat packets 4–10, spell packets, inventory (`PacketUpdateInven` 94),
party/object destroy/summon, sound 106, services up through ~128, plus many
numeric types used in `mp_utils.c` (29, 70, 84, 92, 99, 100, 118, 121–123…).

All sends are gated on `tig_net_is_active()` / `tig_net_is_host()`, which are
always false → **SP path only**.

### 1.3 Player / PC representation

- `player_get_local_pc_obj()`, `player_is_local_pc_obj()`, `player_is_pc_obj()`
  (`src/game/player.c`)
- Local PC is a single `player_pc_obj` / `player_pc_oid`
- MP stores per-slot `ObjectID` in `stru_5E8AD0[8]` (see `multiplayer.c`)

### 1.4 World map / travel / time / dialog / save

| Area | Location | Notes for Coop Lite |
| --- | --- | --- |
| World map UI | `src/ui/wmap_ui.c` | `wmap_ui_open()`, `wmap_ui_teleport(loc)` → `teleport_do` with `player_get_local_pc_obj()`, `map_by_type(MAP_TYPE_START_MAP)` |
| Open callers | `intgame.c`, `dialog_ui.c` | Hotkey / dialog paths into classic wmap |
| Teleport | `src/game/teleport.c` | Canonical area change; also used by scripts, AI, map, magictech, jumppoints |
| Jump points | `src/game/jumppoint.c` | Local transitions |
| Time | `src/game/timeevent.c` | Includes `TIMEEVENT_TYPE_MULTIPLAYER`, `WORLDMAP`, `TELEPORTED`; `PacketGameTime` exists but never sent |
| Dialog | `src/game/dialog.c`, `src/ui/dialog_ui.c` | Merchant / NPC strings; no host/client role checks |
| Save | `gamelib_save` / `gamelib_load` | Host-owned world save is a future concern (priority 3) |

### 1.5 Verdict for priority (1)

**Implemented** on ENet: `coop_lite_can_use_worldmap()` gates classic
`wmap_ui_open_internal` (ReForged should call the same helper for
`ModernWorldMap`); host `teleport_process` for local PC broadcasts
`PACKET_COOP_FOLLOW_HOST`; clients apply via `teleport_do`. Clients no longer
advance game/anim time locally (`coop_lite_can_advance_game_time`). Next:
dialog restrictions (prio 2), saves (prio 3), anim receive handlers.

---

## 2. Outside projects (reuse check)

| Project | URL | License | Usable for Coop Lite? |
| --- | --- | --- | --- |
| **Arcanum CE Restored** (techerrorcode) | https://github.com/techerrorcode/Arcanum-CE-Restored | Sustainable Use (CE) | Campaign co-op claimed (~85% private beta, Jul 2026 Reddit). Public repo is restoration/QoL; **working MP source not published**. Different MP design than stock Troika. Watch only; do not copy private beta. |
| **Arcanum: ReForged** | https://github.com/kurkijar/Arcanum-ReForged | Sustainable Use | **Required target environment.** Modified CE **binary** + HighRes config + DXIL shaders — not a data-only mod. Base CE commit `a7ff41b` (= this fork’s `main` tip). See §3–§4. |
| Classic Terra-Arcanum co-op / campaign MP modules | various forum threads | N/A | Old module hacks; not CE-compatible transport. Historical only. |
| Upstream alexbatalov/arcanum-ce | https://github.com/alexbatalov/arcanum-ce | Sustainable Use | Same stubs; networking unfinished by design. |

---

## 3. Arcanum: ReForged — what it changes

Release inspected: **v.1.0.0** /
`Arcanum-ReForged-V1.0.0.zip` (2026-09-29).

### 3.1 Package contents (no game data)

- `payload/arcanum-ce.exe` — **modified CE Windows executable** (not stock)
- `payload/HighRes/config.ini` — extended keys (`ModernWorldMap`, `BalanceFixes`,
  `GPUPresent`, `ModernDialog`, …)
- `payload/shaders/*.dxil` — GPU / native-UI shaders (Windows DXIL)
- `payload/HighRes/tools/arcanum-world-map-asset-export.exe` — HD map exporter
- Installer scripts: `Install-ShareableBuild.ps1`, `Build-LocalWorldMapHd.ps1`
- Docs / licenses under package root; notices copied to `legal/` at install

**Not shipped:** GOG `.dat` archives, saves, generated HD map tiles, FFplay,
UAP. UAP compatibility is explicitly untested by ReForged.

### 3.2 Installer behaviour (from `Install-ShareableBuild.ps1`)

Creates a **separate** destination folder; never mutates the source install
(GOG or Steam).

**Copied from game root (`GamePath`):**

- Files: `arcanum1.dat` … `arcanum4.dat`, `tig.dat`,
  `modules/Arcanum.dat`, `modules/Arcanum.PATCH0`, `modules/Vormantown.dat`
- Trees: `modules/Arcanum/movies/**`, `modules/Arcanum/sound/music/**`

**Copied from package payload into destination:**

- `arcanum-ce.exe` (from `payload/`)
- `shaders/*.dxil` → `shaders/`
- Fresh `HighRes/config.ini` (Width/Height/Windowed substituted) unless `-Update`
- Optional `ffplay.exe` beside the exe
- License files → `legal/`

Optional `-GenerateHdMap` builds 64 tiles into `HighRes/world-map/tiles/` and
sets `HDWorldMap=1` (Windows-only toolchain).

### 3.3 Conflict notes for Coop Lite

1. **Binary fork, not overlay.** ReForged players run *their* `arcanum-ce.exe`.
   Coop patches in this GitHub fork do **not** appear until merged into the
   ReForged binary lineage (or ReForged rebases onto this fork).
2. **Same CE base tip:** ReForged imported CE at `a7ff41b`, which matches
   `semenbuzanen-eng/arcanum-ce` `main` at investigation time — good merge base.
3. **`ModernWorldMap=1`:** gating only classic `wmap_ui_open()` is **insufficient**
   under ReForged. Authority must live in shared CE paths (`teleport_do`, map
   open, timeadvance), which both UIs should still call.
4. **`HighRes/config.ini`:** stock CE (`highres_config.c`) only understands
   Width/Height/Windowed/ShowFPS/Scroll*/Logos/Intro. Unknown ReForged keys are
   ignored by stock CE (safe). Coop should add keys carefully (e.g.
   `CoopLite=0`) without breaking ReForged’s parser when merged.
5. **Shaders are DXIL / Windows.** Linux CE build here does not consume them.
   ReForged on Linux (if ever) needs Wine for the ReForged exe, or a native
   ReForged port.
6. **No `.dat` / script patches in the zip.** Loading Steam/GOG data is unchanged;
   coop must not require exclusive module layouts that break that copy set.
7. **BalanceFixes** are engine-side rule tweaks in the ReForged binary — not
   present in stock CE. Coop must not assume stock combat numbers when testing
   under ReForged.

---

## 4. Exact ReForged install procedure (for when game data arrives)

Official package:
https://github.com/kurkijar/Arcanum-ReForged/releases/download/v.1.0.0/Arcanum-ReForged-V1.0.0.zip

Do **not** use the GitHub “Source code” archives (README + LICENSE only).

### 4.0 Steam vs GOG (user has Steam)

ReForged’s published docs and installer examples assume an **English GOG**
folder (`-GamePath 'C:\GOG Games\Arcanum'`). The user’s copy is **Steam**.
Practical differences for CE / ReForged:

| Topic | Steam | GOG | Impact |
| --- | --- | --- | --- |
| Core data layout | Same: `arcanum1–4.dat`, `tig.dat`, `modules/`, movies/music under `modules/Arcanum/` | Same | CE and the ReForged **file-copy** recipe work from either if those files exist |
| Game version | Typically 1.0.7.4 English on Steam | Same English build commonly used for CE/UAP | Prefer English; UAP (if ever used) is English-only |
| Extra launcher / DRM | Steam client / depot layout; may include legacy CD-launcher bits | DRM-free offline folder; `innoextract` friendly | For Coop Lite we only need the **data tree**, not Steamworks |
| Path on disk | e.g. `…/Steam/steamapps/common/Arcanum` (verify locally) | e.g. `C:\GOG Games\Arcanum` | Point `-GamePath` / `$GAME` at the folder that contains the `.dat` files |
| ReForged official support | **Not documented** by ReForged | Documented | Use the same required-file checklist; if the official `.ps1` rejects the path, use the Linux/manual copy recipe (§4.3) |
| Filename case | Windows installs are case-insensitive; archives may ship `Arcanum4.dat` vs `arcanum4.dat` | Same caveat | On Linux, normalize/copy with the names CE expects (`arcanum1.dat` … `arcanum4.dat`, `tig.dat`) |
| Optional folders | May or may not include loose `data/` | Same | ReForged installer does **not** require top-level `data/`; CE can use DAT repositories alone |
| UAP | Same English constraint | Same | ReForged does **not** ship or test UAP; leave UAP off for first Coop Lite bring-up |

**Checklist before install:** confirm the Steam folder (or extracted archive) contains
every path in §4.1. If movies/music live only inside DATs on some Steam layouts,
copy what exists and note missing folders — CE README’s “minimum installation”
still wants loose `modules/Arcanum/movies` and `…/sound/music` for BIK/MP3; if
absent, extract from the user’s archive or from `modules/Arcanum.dat` later.
Do **not** run Steam while testing a copied tree; keep the ReForged destination
outside `steamapps/common` so Steam cannot overwrite it.

### 4.1 Prerequisites

- English Arcanum install (**Steam** for this user; GOG also fine).
- Required under the game root (`$GAME` / `-GamePath`):
  - `arcanum1.dat` … `arcanum4.dat`, `tig.dat`
  - `modules/Arcanum.dat`, `modules/Arcanum.PATCH0`, `modules/Vormantown.dat`
  - `modules/Arcanum/movies/`, `modules/Arcanum/sound/music/`
- Destination folder must be **empty** and **separate** from the Steam/GOG
  source and from the unpacked ReForged zip (never install into
  `steamapps/common/Arcanum`).
- Optional: standalone `ffplay.exe` for BIK movies.
- Optional HD map: Windows + PowerShell; Vulkan Real-ESRGAN optional.

### 4.2 Windows (official)

```powershell
# 1. Extract Arcanum-ReForged-V1.0.0.zip to e.g. C:\Temp\ReForgedPkg
# 2. From that folder:
.\Install-ShareableBuild.ps1 `
  -GamePath 'C:\Program Files (x86)\Steam\steamapps\common\Arcanum' `  # adjust to actual Steam path
  -DestinationPath 'C:\Games\Arcanum-ReForged' `
  -Width 1920 -Height 1080 -Windowed 0

# Optional movies:
#   -MoviePlayerPath 'C:\Tools\ffplay.exe'
# Optional HD world map (Windows-only scripts/exes):
#   -GenerateHdMap -MapBackend Vulkan -UpscalerPath '...\realesrgan-ncnn-vulkan.exe'

# 3. Launch:
#   C:\Games\Arcanum-ReForged\arcanum-ce.exe
```

`-Update` refreshes package files but **preserves** existing
`HighRes/config.ini`.

### 4.3 Linux / this box (file-copy equivalent; no Wine required for layout)

ReForged’s playable bits are mostly **file copies**. The official installer is
PowerShell + a **Windows PE** `arcanum-ce.exe`. On Linux we can stage the same
tree for data/config validation; **running** the ReForged UI requires Wine
(or waiting for a native ReForged build). Stock Linux `arcanum-ce` from this
repo can run against the **copied Steam/GOG data** (classic UI; ignores modern
config keys / DXIL shaders).

Suggested layout once the Steam (or GOG) archive is unpacked to `$GAME`:

```bash
PKG=/path/to/unpacked/Arcanum-ReForged-V1.0.0   # zip contents
GAME=/path/to/clean/steam/Arcanum               # folder that contains *.dat
DST=/path/to/Arcanum-ReForged                    # empty directory (not under steamapps)

mkdir -p "$DST/modules/Arcanum" "$DST/shaders" "$DST/HighRes" "$DST/legal"

# --- game data (same set as Install-ShareableBuild.ps1) ---
# Normalize case if the archive used Arcanum4.dat etc.
for f in arcanum1.dat arcanum2.dat arcanum3.dat arcanum4.dat tig.dat; do
  src=$(find "$GAME" -maxdepth 1 -iname "$f" | head -1)
  test -n "$src" || { echo "MISSING $f"; exit 1; }
  cp -a "$src" "$DST/$f"
done
cp -a "$GAME/modules/Arcanum.dat" "$GAME/modules/Arcanum.PATCH0" \
      "$GAME/modules/Vormantown.dat" "$DST/modules/"
cp -a "$GAME/modules/Arcanum/movies" "$DST/modules/Arcanum/"
cp -a "$GAME/modules/Arcanum/sound" "$DST/modules/Arcanum/"   # includes music/

# --- ReForged payload ---
cp -a "$PKG/payload/arcanum-ce.exe" "$DST/arcanum-ce.exe"
cp -a "$PKG/payload/shaders/"*.dxil "$DST/shaders/"
cp -a "$PKG/payload/HighRes/config.ini" "$DST/HighRes/config.ini"
# Edit Width/Height/Windowed in HighRes/config.ini as needed.

# --- legal notices ---
mkdir -p "$DST/legal/third_party"
cp -a "$PKG/LICENSE.md" "$PKG/MODIFICATIONS.md" "$PKG/THIRD_PARTY.md" "$DST/legal/"
cp -a "$PKG/third_party/." "$DST/legal/third_party/"

# Optional: ffplay for movies (Wine will need a Windows ffplay.exe;
# native Linux ffplay may work with stock Linux CE but not with Wine PE).
# cp "$(command -v ffplay)" "$DST/ffplay.exe"   # only if using Windows binary

# HD map generation: skip on Linux for now (exporter is a Windows .exe).
# Leave HDWorldMap=1; ReForged falls back to original map if tiles missing.
```

**Verify required files exist:**

```bash
for f in arcanum1.dat arcanum2.dat arcanum3.dat arcanum4.dat tig.dat \
         modules/Arcanum.dat modules/Arcanum.PATCH0 modules/Vormantown.dat \
         arcanum-ce.exe HighRes/config.ini; do
  test -e "$DST/$f" || echo "MISSING $f"
done
test -d "$DST/modules/Arcanum/movies" || echo "MISSING movies"
test -d "$DST/modules/Arcanum/sound/music" || echo "MISSING music"
```

**Run options after install:**

| Goal | Command |
| --- | --- |
| Classic CE on Linux (this fork’s binary) | `cd "$DST" && /path/to/out/build/linux-x64-release/arcanum-ce -window` |
| Full ReForged UI | Wine: `cd "$DST" && wine arcanum-ce.exe` (needs Wine + Windows deps; untested here) |

PowerShell under `pwsh` on Linux can also run `Install-ShareableBuild.ps1`
**if** available, but the script still installs a Windows exe; HD-map generation
still needs Windows tools. Prefer the explicit `cp` recipe above for
reproducibility on this box.

### 4.4 Dest folder shape (after install)

```
Arcanum-ReForged/
  arcanum-ce.exe          # ReForged Windows binary
  arcanum1.dat … arcanum4.dat
  tig.dat
  HighRes/config.ini
  shaders/*.dxil
  modules/
    Arcanum.dat
    Arcanum.PATCH0
    Vormantown.dat
    Arcanum/movies/…
    Arcanum/sound/music/…
  legal/…
  [optional] ffplay.exe
  [optional] HighRes/world-map/tiles/world-map-001.png … 064.png
```

---

## 5. Priority (1) design — when transport exists

Do **not** invent sockets here. Intended minimal CE-side hooks (ReForged-safe):

1. **`coop_lite` module** (new): role flag `host | client | off`, gated by
   setting / `HighRes` key `CoopLite=0|1` and runtime host election once net
   works. Default **off** so stock SP and ReForged SP are unchanged.
2. **World map open gate:** early return in `wmap_ui_open()` for non-host when
   coop active. Also document that ReForged `ModernWorldMap` must call the same
   gate (or share `coop_lite_can_open_world_map()`).
3. **Travel authority:** after successful host `wmap_ui_teleport` / area
   `teleport_do` that changes map for the host PC, call
   `coop_lite_broadcast_follow_host(dest_map, dest_loc)` which:
   - uses `tig_net_send_app_all` **when real**, or
   - a clearly named packet (reuse unused type ≥ 129 only after validating the
     1..130 range in `multiplayer_validate_message`) carrying map id + loc;
   - clients apply via `teleport_do` on **their** local player object (not as
     story PC for scripts).
4. **Interior / jumppoint transitions** initiated by host: same follow broadcast.
5. Until `tig_net` is real: keep send helpers as no-ops; unit-test gates in SP
   with a forced `coop_lite` role override for UI blocking only.

### Missing before priority (1) is end-to-end

- Real `tig_net` (or replacement: e.g. SDL_net / ENet) in `first_party/tig`
- Implement `multiplayer_handle_message` dispatch
- Host election / session join
- Mapping remote players to objects (stock slot table is a starting point)

### Priorities (2) and (3) — notes only

**Dialog (2):** Restrict non-host dialog starts with story/quest NPCs; allow
merchants / non-quest chatter. Likely hooks in `dialog_ui` / script dialog
entry; need quest-giver detection (quest flags / script meta). Info-only mode
for blocked NPCs. ReForged `ModernDialog=1` must call the same gate.

**Saves (3):** Host owns `gamelib_save` world state; each client keeps character
blob (stats/inventory/equipment/level). Stock MP save paths in `multiplayer.c`
may partially apply once net works; design per-client char files + host world
slot. Do not diverge save version from ReForged/CE without a migration note.

---

## 6. Build status (this machine)

- Configured: `cmake --preset linux-x64-release`
- Built: `cmake --build --preset linux-x64-release` → **success**
- Artifact: `out/build/linux-x64-release/arcanum-ce` (~7 MB)
- Deps installed via apt: cmake, ninja, X11/ALSA/Pulse, `libxss-dev`,
  `libxtst-dev`, etc.
- **No game data** → no runtime / ReForged install verification yet.

---

## 7. Recommended next steps

1. When Steam archive arrives: unpack → verify §4.1 files (§4.0) → apply §4.3
   copy recipe → confirm stock Linux CE boots; optionally try Wine for
   ReForged exe.
2. Decide transport: revive Troika-shaped `tig_net` vs new thin UDP/TCP layer
   that still feeds `multiplayer_handle_message`.
3. Implement §5 gates behind `CoopLite`, with forced-role SP tests for wmap
   block; share API with ReForged maintainers for `ModernWorldMap`.
4. Priority (2) dialog policy, then (3) split saves.
5. Track CE Restored public MP release for ideas — do not depend on private
   beta binaries.

---

## 8. What landed in coop-lite-net

- ENet transport: `src/net/tig_net.{c,h}`, vendored `third_party/enet/` (MIT)
- `src/net_compat.h` → real API (SP unchanged without `-coop`)
- Dispatch + join/leave in `multiplayer.c`; `multiplayer_ping` pumps ENet
- CLI: `-coop`, `-host [port]`, `-join host[:port]`
- Headless test: `tig_net_loopback_test` (CTest)
- Docs: host/join, Coop Anything strategy map (§9–§10)

---

## 9. How to host / join (Coop Lite net)

Requires a build with this branch. **Game data still required to play**; the
loopback test does not need data.

```bash
# Terminal A — host (default port 7777)
./arcanum-ce -window -coop -host 7777

# Terminal B — client
./arcanum-ce -window -coop -join 192.168.1.10:7777
```

Notes:

- Without `-coop`, `tig_net_is_active()` stays false — identical to pre-net SP.
- `-host` / `-join` imply configuring the session; both require `-coop`.
- Max players: 8 (slot 0 = host). File xfer (`tig_net_xfer_*`) is still a no-op
  (map streaming for late join is a known gap).
- Direct IP only for now (LAN / VPN / Tailscale). Steam relay is a future option
  per Coop Anything networking playbook — not implemented here.
- Headless proof: `tig_net_loopback_test` or `ctest -R tig_net_loopback`.

---

## 10. Coop Anything strategy map

**What it is:** [cranebeaker/coop-anything](https://github.com/cranebeaker/coop-anything)
— MIT-licensed MCP server + playbook that guides agents to revive or build
co-op for single-player games. It is **not** a drop-in netcode library; it is
process/docs (see `playbook/networking.md`, `playbook/how-coop-mods-work.md`).
License: MIT (Copyright 2026 cranebeaker and contributors).

Strategies taken from that playbook (not invented) and how Coop Lite applies them:

| Coop Anything strategy | Source | Applied in Coop Lite? | How / why not |
| --- | --- | --- | --- |
| **Host-authoritative state sync** (default for SP retrofits) | networking.md §0–1.1 | **Yes (design)** | Host is slot 0; clients send app packets to host; host broadcasts. Matches Troika `tig_net_is_host()` gating already in CE. |
| **Client-owned avatar / camera** (client-auth with host arbitration for shared state) | networking.md §1.1, §1.3 | **Partial** | Spec: each player keeps own character (stats/inventory). Story/quests/world = host. Transport ready; ownership rules not fully coded yet. |
| **ENet as C transport** | networking.md §0, §2 | **Yes** | Vendored ENet 1.3.18, reliable UDP, 2 channels (app + session control). |
| **Reliable for events, not for continuous motion** | networking.md §1.2 | **Partial** | All app sends currently reliable (CE packet model). Future: unreliable channel for high-rate pose if needed. |
| **Entity ownership (NPC AI on host)** | how-coop-mods-work.md §1.2 | **Planned** | Host runs world AI; remote players are puppets. Stock MP slots support this; puppet spawn not done. |
| **Quest/story flags: host owns, mirror writes** | networking.md §1.3; methods.md host-authority table | **Yes (P2)** | `script_has_global_side_effects` + `coop_lite_can_talk`; SFO_PLAYER → host PC. |
| **Time / weather / RNG: host broadcasts** | networking.md §1.3 | **Yes (time)** | Clients skip local clock advance; host sends type 3 ~1 Hz; clients `timeevent_sync`. Weather/RNG not specially handled. |
| **Dialog/cutscenes: one driver, others watch** | networking.md §1.3; how-coop-mods-work.md §1.5 | **Yes (P2)** | Non-hosts: merchants/safe NPCs full; story → info-only float. |
| **Follow-the-leader on map change** | networking.md §1.4 | **Yes** | Host-only wmap; `coop_lite_notify_host_teleported` → packet 129 → client `teleport_do`. |
| **Host save is canonical; clients keep char only** | networking.md §1.4; how-coop-mods-work.md §1.4 | **Yes (P3)** | `coop_lite_can_save_world`; clients export `Players/*.mpc` via `sub_4A4320`. |
| **Late join = full snapshot then deltas** | networking.md §1.4; how-coop-mods-work.md §1.8 | **Not yet** | Needs xfer + world dump; `tig_net_xfer_*` still no-op. Documented gap. |
| **Delta / snapshot compression, priority accumulator** | networking.md §1.2 | **Not yet** | Premature until entity sync volume exists; CE uses discrete event packets today. |
| **Interest management** | networking.md §1.2; how-coop-mods-work.md §1.7 | **Not yet** | Playbook: start without it for small co-op; add when profiling demands. Area-follow rule reduces need. |
| **Desync detection via state hash / soak** | networking.md §5; how-coop-mods-work.md §1.6; lessons.md | **Not yet** | Add after gameplay sync; loopback test covers transport only. |
| **Deterministic lockstep** | networking.md §1.1 | **No** | Playbook: unrealistic for non-deterministic SP engines. CE is not bit-exact. |
| **Steam Spacewar relay** | networking-steam-spacewar.md | **Not yet** | Keep direct IP; add relay later for Steam friends without port forward. |
| **Path 1: revive existing co-op** | README | **Yes (process)** | We revive CE's stubbed Troika MP layer rather than inventing parallel netcode. |
| **Automated in-game bot QA** | automated-qa.md; lessons.md | **Partial** | Headless transport test now; in-game bots need Steam data + later work. |

---

## 11. Known gaps / risks (net)

- **Unhandled packet types (residual):** inventory/party/service packets (≈80–84, 93, 97, 122–125…) still unhandled; core anim/combat/MT receive is wired (§14).
- **File xfer / late-join:** map/module streaming for join-in-progress not implemented (see §14).
- **No Steam relay / NAT traversal** beyond what the user's VPN provides.
- **ReForged:** still a separate Windows binary; must merge or rebase to pick up `tig_net`. Gate world-map at `teleport_do` so `ModernWorldMap` stays compatible.
- **Endianness / struct layout:** packets are raw structs (LAN same-arch assumption).
- **Pause / script spam:** no host-only script suppression yet (Coop Anything FNV lesson).

---

## 12. Priority 1 implementation (`coop-lite-worldmap-follow`)

| API / hook | Role |
| --- | --- |
| `coop_lite_can_use_worldmap()` | Shared CE check: allow wmap iff SP or coop host. Call from ReForged modern wmap too. |
| `coop_lite_can_advance_game_time()` | Host/SP only advance clocks |
| `coop_lite_notify_host_teleported(map, loc)` | Host broadcast after local-PC `teleport_process` |
| `PACKET_COOP_FOLLOW_HOST` (129) | Reliable follow payload `{type,map,loc}` |
| `wmap_ui_open_internal` | Early reject + feedback mes 605 for non-hosts |
| `teleport_process` | Notifies follow when `TELEPORT_IS_LOCAL_PC` |
| `timeevent_ping` | Clients skip `datetime_add_milliseconds` for game/anim |

Tests: `coop_lite_policy_test` (SP gates, host/client gates, follow round-trip);
existing `tig_net_loopback_test`.

Gaps: ReForged binary must link/call the helper; no in-game QA without Steam data;
follow uses per-slot nearest walkable offset (see §13); jumppoint/script teleports of
non-PC objects do not trigger follow (only local PC teleports).

---

## 13. Priority 2–3 (`coop-lite-dialog-saves`)

Built on `coop-lite-worldmap-follow`.

### Follow offset (P1 fix)

Clients no longer teleport onto the host’s exact tile. `coop_lite_follow_loc_for_slot(host_loc, slot)`
picks a stable per-slot neighbour, then (in-game) spirals for the nearest free walkable tile
(`tile_is_blocking` + no critter on tile). Slot 0 bias is (0,0); slots 1–7 use distinct offsets.

### Dialogs (P2)

| API / hook | Role |
| --- | --- |
| `coop_lite_can_talk(npc, pc)` | Shared gate → `ALLOW` / `INFO_ONLY` / `BLOCK` |
| `coop_lite_classify_talk(...)` | Pure policy for tests |
| `script_has_global_side_effects(num)` | Scans `.scr` actions for quest/gvar/story/reputation/etc. |
| Merchant heuristic | `OBJ_F_NPC_SUBSTITUTE_INVENTORY` or `SAP_BUY_OBJECT` script |
| `dialog_ui_start_dialog` | INFO_ONLY → float “Only the party leader…”; no flag changes |
| `coop_lite_story_pc_obj()` | Host PC (slot 0); used for `SFO_PLAYER` and rumor quell |

Non-host companions are not “the PC” for story scripts.

### Saves (P3)

| API / hook | Role |
| --- | --- |
| `coop_lite_can_save_world()` | Host/SP only |
| `mainmenu_ui_save_game_execute` | Clients blocked; still export local `.mpc` |
| `coop_lite_export_local_character()` | Wraps existing `sub_4A4320` → `Players/<oid>.mpc` |
| Host save path | Exports host char, then `gamelib_save` (SP format when coop off) |
| `multiplayer_save` | Unchanged: stores slot OIDs when coop active |

Single-player save format is untouched when `tig_net_is_active()` is false.

### Tests

`coop_lite_policy_test`: SP/host/client gates (wmap/time/save/talk), classify matrix,
per-slot offset ≠ host, follow packet applies offset dest.

### Remaining before in-game playtest

See report checklist (Steam data, ReForged link of helpers, anim/combat receive,
late-join xfer, live merchant/quest NPC soak).

---

## 14. Anim / combat / magictech receive (`coop-lite-anim-combat`)

Host-authoritative realtime sync on original CE packet types.

### Animation / movement

| Type | Direction | Handler |
| --- | --- | --- |
| 4 | Client → host | `mp_handle_packet4` → `anim_goal_move/run_to_tile` |
| 8 | Host → clients | `mp_anim_modify` (was TODO) — flags, path, target tile, pose |
| 9 | Both | `mp_handle_packet9` — interrupt/`sub_424070`; host rebroadcasts |
| 10 | Host → clients | `mp_handle_packet10` — loc/offset/art/flags snapshot |
| 99 | Host → clients | `sub_4EDFF0` **fixed** (was host-only; now clients apply) |
| 92 | Host → clients | `mp_handle_packet92` — `object_set_current_aid` |
| 70 | Host → clients | create / blood pool FX |

### Combat

| Type | Role |
| --- | --- |
| 11 `PacketCombatModeSet` | Host broadcasts on toggle; clients activate/deactivate |
| 100 subtype 13 | Vitals: host `object_hp_damage_set` / `critter_fatigue_damage_set` broadcast HP+fatigue; clients apply under `multiplayer_lock` |

Damage/death still resolved on host (anim attack goals); clients mirror HP/fatigue and pose.

### Magictech

| Type | Role |
| --- | --- |
| 57 | Client request → host `magictech_invocation_run` |
| 58 | Host cast → clients `mp_magictech_apply_invocation` (`sub_455C30`) |
| 60/61 | Maintain add/end for owning player UI |
| 72/73 | Destroy / summon mirrors |
| 74/75/76 | Dispel / obj-flag / charge cost |
| 77 | Eye candy via `mp_magictech_apply_eye_candy_ex` |

### Late join

**Not implemented.** `tig_net_xfer_*` remains no-op. On connect the host logs that no world snapshot is sent. Peers must start together or load the same campaign save before joining. To finish late-join later: host sector/object dump over xfer, client `multiplayer_map_open_by_name` + object hydrate, then delta stream.

### Tests

`coop_lite_mp_packet_role` matrix in `coop_lite_policy_test`; loopback transport still covered.

### Risks

- Packet5/7 (full AnimGoalData sync) never sent in recovered CE — pose relies on 8/9/10/99.
- Summon/create assumes OID pools stay aligned; desync if objects diverge.
- Vitals may spam Packet100 on frequent fatigue ticks.
- No automated in-game damage/anim soak without Steam data.

---

## 15. Linux Steam playtest (2026-10-19) — `coop-lite-playtest-fixes`

### 15.1 Extraction inventory (Steam RAR → `/workspace/game/`)

| Item | Path / note |
| --- | --- |
| Archive | `/workspace/game/Arcanum.rar` (~3.5 GB RAR) |
| Extract tool | `/usr/bin/unar` (236 non-fatal extract warnings; mostly loose protos) |
| Nested core | `/workspace/game/Arcanum/Arcanum/` (~3.7 GB tree) |
| Exe (Windows) | `Arcanum.exe` / Sierra launcher bits present in archive |
| DAT cores | `arcanum1.dat` … `arcanum4.dat`, `tig.dat` |
| Modules | `modules/Arcanum.dat`, `Arcanum.PATCH0`, movies/sound under module dir |
| Case issues | Archive used mixed case (`Arcanum4.dat`, `modules/` vs CE’s `Modules\\`). Staging renamed/normalized to `Modules/` + lowercase `modules` symlink; `arcanum4.dat` from `Arcanum4.dat`. |

Staging trees (game data **never** committed):

| Tree | Purpose | Size |
| --- | --- | --- |
| `Arcanum-CE/` | Vanilla CE data + Linux `arcanum-ce` | ~1.2 GB |
| `Arcanum-ReForged/` | §4.3 copy + ReForged PE/shaders/HighRes + Linux CE binary as `arcanum-ce-linux` | ~1.2 GB |
| `run-host/` / `run-client/` | Separate CWDs for local coop smoke | — |

### 15.2 ReForged install result

- Zip `Arcanum-ReForged-V1.0.0` applied via §4.3 file-copy into `Arcanum-ReForged/`.
- Payload present: `arcanum-ce.exe`, `HighRes/config.ini`, `shaders/*.dxil`, legal notices.
- **Linux playable binary** for Coop Lite is this fork’s Linux build (`arcanum-ce-linux`), not Wine ReForged PE.
- Full ReForged modern UI (ModernWorldMap, DXIL, etc.) needs Wine or a future native ReForged port.

### 15.3 ReForged source merge-check

[kurkijar/Arcanum-ReForged](https://github.com/kurkijar/Arcanum-ReForged) ships **LICENSE + README + release zip only** — **no public CE source tree**. Therefore:

- Cannot branch-merge ReForged UI (`ModernWorldMap`, modern dialog, HD map) with Coop Lite in this fork.
- Documented shared hooks remain the integration path when ReForged rebases or opens source:
  - `coop_lite_can_use_worldmap()` / `coop_lite_can_open_world_map` equivalent for ModernWorldMap
  - `coop_lite_can_talk` for ModernDialog
  - `coop_lite_can_save_world` for any modern save UI
  - Prefer calling into CE core (`teleport_do` already notifies follow) so UI forks stay thin
- Until then: run Coop Lite Linux/Windows CE binary against ReForged-staged **data**; ReForged-only features (modern UI, BalanceFixes compiled into their PE) are unavailable.

### 15.4 Smoke results (this box)

Display: `$DISPLAY=:2` (desktop available).

| Test | Result |
| --- | --- |
| SP `-window` (no `-coop`) | Reached main menu; screenshot `shots/sp-menu.png` |
| Host `-coop -host 7777` | Listened; accepted client |
| Client `-coop -join 127.0.0.1` | Welcomed as **slot 1** |
| In-game New Game / packet soak | Incomplete — main menu has no Multiplayer entry under `-coop`; xdotool New Game clicks often hit CREDITS/EXIT; host once quit via EXIT misclick (not a crash) |
| Crashes in coop code | None observed in transport path |

### 15.5 Code fix in this branch

`gamelib.c`: case-insensitive-friendly `Modules` / `modules` fallback for `gamelib_load_module_data` and `gamelib_modlist_create` (Linux Steam/archive extracts).

### 15.6 Human playtest (Windows) — exact commands

Build or copy `arcanum-ce.exe` from this fork’s Windows release build of `coop-lite-playtest-fixes` (or build from source) into a **copy** of the ReForged-staged folder (not under `steamapps`):

```bat
:: Host (machine A)
cd /d C:\Games\Arcanum-CoopLite
arcanum-ce.exe -window -coop -host 7777

:: Client (machine B — LAN IP of host)
cd /d C:\Games\Arcanum-CoopLite
arcanum-ce.exe -window -coop -join 192.168.x.y:7777
```

Same machine:

```bat
:: Terminal 1
arcanum-ce.exe -window -coop -host 7777
:: Terminal 2
arcanum-ce.exe -window -coop -join 127.0.0.1:7777
```

Checklist for human:

1. Both reach main menu; confirm log lines for listen / slot assign.
2. Host: New Game → character create → enter world (start together; late-join xfer still missing).
3. Client should follow host map changes; try merchant dialog as client; try story NPC (expect info-only).
4. Host save; client cannot world-save but should export `.mpc`.
5. Enter combat; confirm remote pose/HP updates.
6. Do **not** expect ReForged ModernWorldMap until their binary links the coop helpers.

Linux local equivalent (this box layout):

```bash
cd /workspace/game/run-host && ./arcanum-ce -window -coop -host 7777
cd /workspace/game/run-client && ./arcanum-ce -window -coop -join 127.0.0.1:7777
```

---

## 16. Design-review fixes + New-Game crash (`coop-lite-review-fixes`)

### 16.1 Host crash after New Game (playtest)

**Symptom (1st):** Host segfault (`ip 0`) after New Game + follow broadcast while client still on main menu.

**Symptom (2nd, gdb):** Still crashed after follow arm-only fix. `host-gdb.log`: SIGSEGV at `0x0` from `timeevent_ping` ← `gamelib_ping` ← `main`, a few seconds into the first map after `CoopLite: armed follow` + Animate Forever sound errors. gdb `r14=0x1e` = `TIMEEVENT_TYPE_MP_CTRL_UI` (30).

**Root causes & fixes:**

| Issue | Fix |
| --- | --- |
| (a) Follow broadcast on initial New Game / load spawn | First host PC teleport **arms** follow only (`coop_lite_follow_armed`); subsequent travel broadcasts. Skip shopping-map destinations. |
| (b) Client apply while on ShopMap / menu | Queue follow if not in-world (`coop_lite_client_in_world`); `coop_lite_flush_pending_follow` after client enters world. |
| (c) NULL anim goal/func | Abort runs with NULL `goal_node`/`func`; validate Packet9 priority. |
| (d) **NULL `TIMEEVENT_TYPE_MP_CTRL_UI` process_func** | CE removed `mp_ctrl_ui` (network UI) but left `party_add`/`party_remove` scheduling that type (500 ms delay) whenever `tig_net_is_active()`. Map transition / PC destroy with a connected client fires it → call through NULL. Restored stub `mp_ctrl_ui_process_callback` (party feedback log + local UI msg); register in `anim_ui_init`; defensive NULL check + log in `timeevent_ping`. **SP unaffected** (`party_*` only schedules when net active; SP never enqueues this type). |

### 16.2 Hard real-time combat

- `coop_lite_can_use_turn_based()` → false when coop active.
- `combat_turn_based_toggle` / `sub_4B6C90` / options UI refuse TB; CONNECT forces `TURN_BASED_KEY=0`.

**Event-level combat (PR #5 adapted):** CE already uses discrete packets (not frame streams): Packet4 move intent, Packet8/9/10 anim, Packet11 combat mode, Packet100 subtype 13 HP/fatigue (= DAMAGE_APPLIED). New **Packet128 `PACKET_COOP_ATTACK_INTENT`**: client `anim_goal_attack` → host resolves `AG_ATTACK` (DEX timing + `ai_attack` on host).

### 16.3 Spells / FX

- Keep Packet57 request + Packet58 cast (CAST_SPELL).
- **Suppress Packet77** eye-candy send/receive — clients spawn particles locally from cast apply / game data.

### 16.4 Aggression

- Host `ai_attack` path unchanged for resolution.
- `coop_lite_propagate_party_aggression`: when a companion PC attacks, mirror hostility onto host PC so NPCs fight the party.

### 16.5 Dialogs (PR #4 fix)

- No whole-NPC INFO_ONLY float.
- `coop_lite_dialog_option_allowed(actions, response_code)` denies `qu/gv/gf/ss/rp/al/ru/rq/xp`; **barter (`B:` → code 3) always allowed**.
- Offline test uses synthetic dlg lines mirroring Steam `dlg/01040ristezze.dlg` barter (`B:`) vs quest (`qu`/`gf`/…) patterns (game data not committed).

### 16.6 Human playtest notes

After updating binaries, both peers must start/load into the world before follow teleports apply. Host New Game no longer yanks a menu-only client.

## 17. Session sync + client anim time (`coop-lite-session-sync`)

### 17.1 Client world frozen

**Cause:** `timeevent_ping` skipped *both* game-time and anim-time on clients.
**Fix:** Clients still advance **real-time** and **anim-time** (movement/rendering). Only **game-time** (clock, NPC schedules, day/night) is host-authoritative via PacketGameTime.

### 17.2 Separate New Game worlds

**Cause:** Both peers ran independent New Game → different OIDs/maps; host never saw client PC.
**Fix (MVP session sync):**
1. Coop **client** accepting a premade/created char sends `PACKET_COOP_CHAR_OFFER` (130) and **does not** teleport to its own start map.
2. Host (when in-world, or when first arming follow on New Game) binds slot 0, spawns the client PC near the host (`PLAYER_CREATE_INFO_NETWORK`), binds `stru_5E8AD0[slot]`, mirrors via Packet70, and sends `PACKET_COOP_SESSION_SYNC` (131) with map + both PC OIDs/locs.
3. Client applies SESSION_SYNC: recreates local PC with host-assigned OID, creates host PC proxy, teleports to host map, closes main menu.

### 17.3 Dialog

Client talk uses existing `dialog_ui` + per-node filter once the shared map/PCs exist. Frozen anim (17.1) was the main blocker for click-to-talk.

### 17.4 Gaps

- Full late-join sector/`tig_net_xfer` snapshot still missing (dynamic NPC state, inventory deltas).
- Host PC art on clients uses generic prototype (16066) in SESSION_SYNC; premade appearance may not match until proto is plumbed.
- Orphan ShopMap PC on client after OID recreate not destroyed.
- Client-created (charedit) path should also call `coop_lite_client_offer_character` (premade path does).


## 18. Playtest 4 fixes (session ordering, movement, talk, lag)

- **Session start order.** `CHAR_OFFER` now carries the client's own PC ObjectID.
  The host creates the companion with **that OID**, clears `OF_OFF`, and binds
  it to the slot. Watch for `CoopLite: created PC ... flags=` and
  `SESSION_SYNC sent ... (companion spawned on host)` in the host log.
  On `SESSION_SYNC` the client only teleports its **existing** local PC (the
  same PC, so camera and UI stay intact) to the host map. Once the teleport
  finishes (hook in `teleport.c`), `coop_lite_client_on_teleport_done()`
  creates or binds the host proxy and binds the slots. A busy teleport retries
  every 250 ms. Packet70 skips OIDs that already exist.
- **Movement.** `anim_goal_move_to_tile` had its client checks inverted, so it
  never sent Packet4. That is fixed. The host is authoritative: it moves the
  PC and broadcasts `PACKET_COOP_PC_POS` (132) for every slot PC. Broadcasts
  go out at most every 100 ms and only when something changed. Clients apply
  them with `sub_43E770` and `object_set_current_aid`.
- **Talk.** On a coop client, clicking an NPC calls
  `coop_lite_client_talk`. If the NPC is in range, it opens a local dialog
  with the per-node filter. If not, it sends a move intent toward the NPC;
  click again to talk.
- **Lag.** The per-packet `enet_host_flush` was replaced with a single flush
  per pump. Packet 122 is dropped quietly instead of being logged as
  unhandled. A `tig_net: 10s stats` line shows the packet types sent or
  received ≥50 times in 10 s. The run dirs use the RelWithDebInfo build
  (`out/build/linux-x64-relwithdebinfo`).
- **Gaps.** Dialog runs on the client, not routed through the host, so script
  side effects stay local. The host-side proxy uses the generic PC art proto
  16066. Movement replicates by position, so it can look step-wise at
  100 ms.

## 19. Playtest 5 fixes and network hardening

### Movement and talk
- **The "stop short" bug was in the host's movement code.** In
  `AGUpdateAnimMove` (anim.c), the host-only desync check was missing its
  `distance > path.curr` comparison. As a result, every critter that moved on
  the host (PCs and NPCs) was interrupted and snapped after its first step.
  This is what printed the stream of `AGUpdateAnimMove: ERROR ... are more
  than` lines. The comparison is now restored.
- **Each client moves its own PC.** Once the session is synced, the client
  handles its own PC's ground-click moves (`anim_goal_move_to_tile`, `*_run_*`,
  `*_near_tile`, `sub_433A00`). It also runs that PC's move/talk animation
  goals (`AG_MOVE_*`, `AG_RUN_*`, `AG_TALK`). Both run inside
  `tig_net_local_suspend_begin/end`, so the exact single-player code runs
  locally: local pathing, camera follow, the NPC's dialog script, and the
  per-node dialog filter. Nothing is sent from inside that block. The result
  reaches the host as `PACKET_COOP_PC_POS` (client→host, 50 ms, only on change).
- **The host relays positions without echoing them.** A PC's own pose is
  relayed to everyone except its owner. If the host moves that PC itself
  (knockback, follow teleport), it sends `COOP_PC_POS_FORCE` to the owner.
  Clients ignore Packet9/Packet99 and unforced PC_POS for the PC they drive.
- **Talking to an NPC works on the client.** The client's `AG_TALK` used to
  wait forever for a host path, and scripts never run on clients. It now
  walks over and opens the filtered dialog locally.
- **Ambient animations already run on the client.** Comparing screen captures
  shows the fires animating.

### Duplicate PCs and the host's look
- The host's PC on the client is created from the host's real premade proto
  (`sub_49B290`). It gets the host's art from PC_POS.
- After a sync, the client logs a PC census and destroys any PC that is not
  bound to a slot. It also logs a census every 30 s. Packet70 never creates
  PCs or unknown protos. Packet70s that arrive during sync are now queued and
  replayed instead of dropped.

### Network hardening (code review)
- **WELCOME:** clients accept it only from the host, only once, and only with
  `0 < slot < MAX`. The host ignores it.
- **NAME:** applied on the host only, using the authenticated sender slot.
- **ENet peers:** `peer->data` holds slot+1, so a rejected or unassigned peer
  reads as -1 and can never clear slot 0. Packets from unknown peers are
  dropped. The connect loop no longer leaks packets.
- **Per-type checks:** `multiplayer_validate_message` checks each type's
  minimum size and its direction (`coop_lite_packet_allowed`). The host drops
  host→client types sent by clients, and clients drop client→host types.
- **Object ownership:** host handlers (Packet4, Packet9, Packet57, attack
  intent, PC_POS) require the object to be the sender's bound PC.
- **CHAR_OFFER:** the host uses the sender slot, not the slot in the payload.
  The proto must be a valid PC prototype. An OID that belongs to the host PC,
  another slot's PC or a non-PC is rejected. The offer is cleared after the
  spawn and on disconnect. CONNECT no longer triggers a spawn.
- **SESSION_SYNC:** sent per slot. Clients drop a sync whose
  `client_slot != local slot`. Names are NUL-terminated and the proto is
  sanity-checked.
- **Packet layout:** `_Static_assert` on the sizes and offsets of all coop
  packets.
- **Dialog filter:** barter nodes are filtered like any other node. `lf`/`lv`
  are now blocked. Blocked options are blanked in place instead of being
  compacted, so the engine's fixed-index reads (rumor, skill, spell and
  directions use `[1]`) stay correct.
- **MP_CTRL_UI:** party feedback is scheduled only in net play and only when a
  handler is registered (`timeevent_type_has_handler`).

### Known gaps
- Custom (charedit) characters are still offered as proto + name only. Stats,
  skills, inventory and portrait are not transferred; the host builds the
  companion from the proto.
- Remote PCs are shown by PC_POS snaps at 20 Hz (with art frames). There is
  no client-side interpolation between snaps.
- Dialog script side effects on a client stay local; the filter blocks
  shared-state actions.
- XP and score are per player.
