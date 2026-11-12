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

**Trust model (explicit, PT24).** Coop Lite is a mod for a group of friends
playing together, not a public server. The host trusts its clients' *intent*
the way a tabletop group trusts its players: checks exist to keep honest
desyncs, stale packets, mismatched builds and crashes from corrupting the
host's world (bounds, ownership, protocol, handshake, plausibility), not to
stop a determined cheater with a patched client. Anti-cheat work beyond
that (full object/script sanitising of an uploaded character, per-slot rate
limiting, authenticated sessions) is deliberately out of scope; do not
expose a Coop Lite host to strangers on the Internet.

**Wire (protocol 24, PT26; HELLO since 22).** The client's first packet on the reliable app
channel is HELLO (name + protocol); ENet orders it before CHAR_OFFER etc. on
that channel. The host drops app packets from a slot until a valid HELLO,
fires the game's CONNECT for the slot only then, and disconnects a peer that
sends none within 5 s. Packet struct sizes are pinned with `_Static_assert`
(mp_utils.c, tig_net.c). Dev env switches (`ARCANUM_DEV_*`) are ignored unless
`ARCANUM_DEV=1`; env reads are cached (`coop_lite_env`). `-coop/-host/-join`
are matched as whole argv tokens. Logging: `-log <file>` or `ARCANUM_LOG_FILE`.

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

## 20. Playtest 6 fixes

- **Host could not move, and the intro looked stuck.** This was not a hang.
  The host was inside Virgil's opening dialog: Virgil's line plus two reply
  options in the text box ("What are you going on about?" / "By the gods,
  man! ..."). Single player (`-coop` off, dev autostart) shows the same screen,
  and memory reads of the running host confirmed the dialog was active. The
  game takes no ground clicks until a reply is picked. Before Playtest 6 the
  broken move guard (section 19) stopped Virgil before he reached the PC, so
  the intro never opened and the host could still walk.
- **AGUpdateAnimMove spam** (wandering wolves and kite scouts, always
  "2 tiles away, curr(1)"): the guard now allows one tile of slack.
- **`drop type 8 ... wrong direction`:** Packet8 (anim modify) is sent by both
  host and clients in vanilla MP, so its role is now BOTH; the host's handler
  ignores it. A client no longer sends Packet8 for the PC it drives:
  `turn_on_running`/`turn_on_flags` run locally under `tig_net_local_suspend`,
  so client running works too. The new policy test checks every real sender
  type against the direction table.
- **Client talk:** a click on Virgil did open the dialog. The client shows the
  same intro text box, confirmed from memory on both processes. Pick an
  option to continue.
- **"Disband Failed: Merwin Tumblebrook" x3:** these came from the client
  destroying stale PC copies (`object_destroy` → `sub_45E180` →
  `party_remove`), not from a click. Party add/remove now does nothing on
  clients; parties are host-authoritative and replicated by Packet71. The
  portrait shown after clicking Merwin is the normal target/examine panel,
  not party membership.
- **Root cause of the stale PCs (Linux):** `tig_file_is_empty_directory`
  counted `<= 2` entries as empty because it expected Windows' `.`/`..`.
  On Linux, `Save\Current`, which holds only `maps`, was "empty", so startup
  never cleared it. Every killed client then reloaded its old map state with
  old proxies and old local PCs. Fixed in tig, with the new test
  `tig_file_empty_dir`. As extra safety, the client clears
  `Save\Current\maps\<host map>` before the sync teleport, and the PC census
  still removes any unbound PC.
- **Client camera:** CE single player only recentres for town-map routes
  (anim flag 0x80000). Coop clients now keep their own PC inside a central
  200x120 px dead zone, using smooth `scroll_view_by` every PC_POS tick, and
  jump only after teleports.
- **Dev hook:** `ARCANUM_DEV_AUTOSTART_PREGEN=<1..8>` starts a New Game with
  that premade character after ~2 s on the main menu, so test runs need no
  input automation. Unset means no effect.

## 21. Playtest 7 fixes

**Client could run Virgil's whole intro and recruit him (main issue).**
Root cause: the client's own talk (and the AG_TALK goal → `dialog_ui_start_dialog`)
runs under `tig_net_local_suspend`. While suspended, `tig_net_is_active()` reads
false, so every `tig_net_is_active() && !tig_net_is_host()` check in the dialog
filter was off: no "(Only the party leader can discuss this.)" blanking, and option
actions ran locally. Virgil's option 121 is `{jo 0 74}` (critter_follow), and node 1
runs NPC-line actions (`gf… in -2804 … qu1010 2, ru2013`) through the same executor
`sub_415BA0`, which had no filter at all.
Fixes:
- `coop_lite_is_client_role()` = locally suspended || (active && !host); used by the
  dialog option display/process filters and the executor.
- `coop_lite_dialog_action_blocked(code)`: one table for the option filter and for
  `sub_415BA0` (option and NPC-line actions). Blocked: qu gv gf lv lf lc ss rp al ru rq
  xp fp pv pf jo wa uw sc so ce su ii ri et $$ in co nk or np. Allowed: re fl mm tr.
  Skipped actions are logged ("Dialog: coop client skipped action …").
- A client clicking another player's follower (critter leader != own PC) gets a float
  line "(Following the party leader. Speak with them.)" and no dialog.
- `critter_follow` / `critter_disband` refuse on clients unless applying a host packet
  (`multiplayer_is_locked()`): follower state is host-owned.
- Test: `coop_lite_dialog_filter_test` checks Virgil's real action strings (embedded),
  suspension-aware role, and — when `/workspace/tools/01324virgil.dlg` or
  `COOP_VIRGIL_DLG` is present (game data, not committed) — every jo/wa/lv/uw line and
  every gf/qu/ru/lf/in/al line in the real file is denied.

**Client walk stopped short (~190 px).** Root cause: a click calls `sub_424070`
(interrupt) on the client PC before adding the move goal; it was not suspended, so a
Packet9 went to the host, which interrupted its proxy (stand art). The host relay then
saw the proxy sample differ from the client-reported pose and sent `COOP_PC_POS_FORCE`
to the owner; applying a stand art ends `AGUpdateAnimMove` → the walk stopped a few
tiles in. Fixes: `sub_424070` on our own PC runs locally (suspended); host FORCE only
when location/offset differ (`coop_lite_pc_pos_moved`), never for art-only drift;
a client applying a FORCE to its own PC moves position only (no art), logged as
"host correction of own PC".

**Jerky client camera.** The follow ran only on the 50 ms pose tick and used the tile
centre (jumps a tile at a time). It now runs every ping (frame) and includes the
sub-tile `OBJ_F_OFFSET_X/Y`.

**Host: rapid dialog-option clicks sometimes stall.** The host dialog path has no coop
code (`tc_handle_message` / `dialog_ui` unchanged from SP: repeat-flagged button-down
events are ignored by design). Not coop-related; left as is.

## 22. Playtest 8 fixes and the merchant-only talk rule

**Client talk rule (design change).** A coop client opens a dialog window only
with merchants: NPCs with a substitute (shop) inventory, a buy-object script,
or an inventory source with a buy list (Ristezze, Dernholm and other merchant
quest-givers included). Their dialog is the reduced one: barter and neutral lines,
with quest/story/follower options blanked by the per-node filter, and the executor
still skips host-owned actions as a safety net. Every other NPC — quest givers,
story NPCs, citizens — answers with its own generic farewell bark
(`dialog_copy_npc_farewell_msg`, race/gender aware), falling back to "I have no
business with you. Talk to your leader." Another player's follower (Virgil
following the host) says "Talk to your leader." Esc always closes a client's
dialog. Policy: `coop_lite_classify_client_talk(is_merchant, follows_other_player)`
→ `COOP_TALK_ALLOW` / `COOP_TALK_BARK` / `COOP_TALK_BARK_FOLLOWER`.

**Virgil still opened his intro on the client.** Root causes: (1) the follower
link only existed on the host; `critter_follow` is not replicated, and Virgil joined
before the client connected, so the client's copy had no leader. (2)
`coop_lite_can_talk` still tested `tig_net_is_active()`, which is false during the
client's suspended talk. Fixes: suspension-aware `can_talk`; the follower check also
scans every slot PC's follower list; new `PACKET_COOP_FOLLOWERS` (133): the host sends
the follower table (NPC OID → leader slot) on change and every 5 s, and the client
applies it with `critter_follow(…, force)` / `critter_disband` under
`multiplayer_lock` + local suspension.

**Monsters invisible; client PC drained to 0 HP and stuck "Dead".** Root cause:
NPC anim goals are not replicated (no Packet5/7 path), so client NPCs never moved;
wolves attacked the client PC on the host only, and only the HP field reached the
client. Fixes:
- `PACKET_COOP_NPC_POS` (134): every 100 ms the host batches NPCs in the screen
  vicinity of each PC (position, offsets, art id = animation frame, dead flag, HP
  damage), sending only changed entries plus a 2 s keyframe, at most 24 per packet. The
  client applies them, so walking, attacks and deaths show up.
- Damage floats ("-N") on the client for NPCs and slot PCs when HP damage rises.
- Fallen client PC: the client shows the fall-down pose and "You have fallen! You will
  be revived beside the party leader." The host ignores the dead PC's own PC_POS and,
  after `COOP_LITE_REVIVE_DELAY_MS` (8 s), revives it at 1/3 HP next to the host
  (FORCE PC_POS). The client then stands up ("Revived.").

**Host PC shirtless on the client, client gold 0.** Root cause: on a client
`sub_463E20` only sends a request, and the client never runs SP's shop step
(`item_generate_inventory`). `coop_lite_generate_start_inventory` now runs
`item_generate_inventory` and wields the best items locally (suspended) for the client's
own premade before CHAR_OFFER and for every coop-created PC on both sides. Items are
peer-local (not OID-matched). The host PC_POS also resends a keyframe every second, so a
late-loading client gets the host PC's current art.

**Camera lag while running.** `location_origin_scroll` refuses a diagonal scroll
that would expose an off-map corner, so the view stayed put. On a refused scroll the
follow now retries each axis separately, and it recentres as soon as the PC leaves the
visible area (380×220 px from centre) instead of at 1000×700.

**Quit Game → New Game started single-player.** Quitting resets the network
connection. Opening New Game on a `-join` client now reconnects first
(`coop_lite_client_reconnect`, `tig_net_join_configured`), so the menu offers the
character to the host again. If the host is unreachable, New Game stays single-player.
On the host, a disconnecting player's PC is now removed (`coop_lite_host_remove_left_pc`)
instead of lingering as an unbound corpse.

**Virgil not following the host after the client left:** nothing in the logs.
Host-side follower AI is unchanged single-player code, and the walk was short (about 6
tiles), which may be inside the follow distance. Re-check with a longer walk.

## 23. Playtest 9 fixes

**Client crash on Esc → Quit Game (SIGSEGV in `coop_lite_handle_followers`).**
Quit Game runs `gamelib_reset`, which does not disconnect. The client stayed "in
session" and kept the follower list as raw object handles. The next host packet 133
called `critter_leader_get` on a freed handle. The fixes:
- `coop_lite_on_game_reset()` runs first in `multiplayer_reset`. A coop client leaves
  the session cleanly: session state is reset, then `tig_net_reset_connection()`. The
  host removes its PC, and New Game reconnects as in §22. On the host, it only drops
  session caches.
- The client follower list is now a list of `ObjectID`s, resolved with
  `obj_pool_perm_lookup` on every use.
- Every client world handler (133 followers, 134 NPC state, 132 PC pose) and the
  client ping return early unless `coop_lite_world_ready_rule(...)` holds. That needs:
  in session, no pending or awaited sync, map valid, not clearing objects, and a
  local PC.
- The host ping does nothing while the host is on the menu or a map is loading, and
  attack intents are dropped then too.
- Follow (129) and session sync (131) already queue or load safely from the menu.

**Remote PC vanished beyond about 17 tiles (census total=1).** The sector cache holds
16 sectors and evicts by LRU any sector with refcount 0. Nothing on the other machine
draws the remote PC's sector, so it was evicted and the PC went with it. The fixes:
- Each side now keeps one `sector_lock` on the sector of every bound remote PC
  (`coop_lite_keep_sector`). It moves with the PC.
- `map_flush` and `map_close` call `coop_lite_before_map_flush()` first. This
  releases the locks, because locked sectors are neither saved by `map_flush` nor
  unref'd by `map_close`. The next ping takes the locks again.
- As a backstop, `coop_lite_resolve_remote_pc` re-creates a missing remote PC from
  its OID and proto when its pose arrives. The proto is recorded per slot at spawn
  and at sync. The log line is "re-created missing remote PC".

**Host camera didn't follow the host PC.** The host ping now runs the same
every-frame dead-zone follow that clients use.

**Client "unknown AnimID" spam.** Host anim goals are not replicated in coop, so the
host's Packet8 (anim modify) named goals the client never had. Packet8 is now
ignored in coop. Coop clients also skip `ai_timeevent_process`, because only the host
runs NPC AI. That also ends the host log lines "Packet4 from slot 1 … dropped".

**Host frame time.** The new probe logs `CoopLite: 10s frame: max gap N ms, max coop
cost M ms` every 10 s. Headless runs measured gaps of 17–25 ms and a coop cost under
0.4 ms on the host, so 134 and its keyframes are cheap.

**Dev hooks.** These apply on the coop host only and are ignored in single player.
- `ARCANUM_DEV_SPAWN_HOSTILE_NEAR_CLIENT=1` spawns a wolf (BP_WOLF) 3 tiles from each
  client PC and makes it attack. The first wolf comes about 10 s after the client is
  bound, and the next one 45 s after the last wolf died. Log line: "dev: wolf spawned".
- `ARCANUM_DEV_START_LOC=shrouded` teleports the host PC on the current map 3 s after
  it enters the world. `shrouded` is Shrouded Hills, 90226,84176; `x,y` also works.
  Clients follow through the normal follow packet.

## 24. Coop death: corpses, the fallen leader, and party wipes (PT10)

This replaces the PT8 automatic revive, which put a fallen client back up 8 s later.

**Fallen client PC.** The PC stays a corpse: no timer, no revive. It can only be
brought back by normal game means, such as Resurrect, scrolls, or anything else that
reaches `combat_heal` with `CDF_RESURRECT`.
- The resurrection is resolved on the host. Before this, a client's own cast ran
  inside its locally suspended animation and so only on that client.
  `magictech_invocation_run` now leaves suspension for that one call
  (`tig_net_local_suspend_pause/resume`), so the cast becomes a Packet57 request.
  The host checks that the caster belongs to the sender, then runs it and
  broadcasts Packet58 and the vitals (Packet100 subtype 13). Item casts take the
  same path.
- While dead, the client cannot act: the vanilla `critter_is_dead(pc)` input gates
  apply, and its PC_POS reports stop. Its camera follows the first living party
  member, which makes it a spectator.
- The corpse never crawls after the party. When the host travels (teleport,
  stairs, cellar, world map), `teleport_process` hands every other bound slot PC
  to the same teleport (`coop_lite_party_carry_list`), corpses included. Across
  maps they are written into the destination map with the host's PC instead of
  being left in the old map's save. Previously they were left behind and
  re-created from proto, losing their stats.
- After the teleport, `coop_lite_after_party_carry` puts each carried PC at its
  usual follow spot and keeps corpses in the fallen pose. The client does the same
  for its copies when it applies the host's follow packet. A dead client's own
  corpse follows through the normal follow teleport.

**Fallen host.** Every screen shows "The party leader has fallen! Resurrect them to
continue." (the host sees its own variant). This uses
`PACKET_COOP_PARTY_STATE` (135, host → clients) with kind `HOST_DOWN`.
- There is no timeout: living clients can resurrect the host on the current map
  for as long as they like. `HOST_UP` announces the return.
- If the host's map changes while it is down, the location has been left and the
  host reloads at once. This should not normally happen, because only the host
  travels.

**Party wipe.** When every bound PC is dead, including a host playing alone, the
host announces `WIPE` with a 10 s grace (`COOP_LITE_WIPE_GRACE_MS`). If anyone gets
up during the grace (for example a delayed effect), `WIPE_CANCEL` follows. When
the grace ends:
1. The host announces `RELOADING`. The main loop, outside `gamelib_ping`, loads
   the most recent save (`gamelib_last_save_name`) through
   `mainmenu_ui_coop_reload_save`.
2. In the reloaded world the host re-arms follow and sends `RESYNC`.
3. Each client resets its session state, stands its PC back up, and re-offers it
   (CHAR_OFFER with its OID). The host adopts the PC that is in the save under that
   OID, or creates one if the client joined after the save. It brings every
   re-offered PC up at full health and sends SESSION_SYNC, so the client teleports
   to its saved spot.
- If there is no save, or it fails to load, the host instead raises every PC with
  full health in place and announces `NO_SAVE`.
- Limitation: a client's own world is not reloaded. Nearby NPCs converge through
  NPC_POS (pose, hp, dead), but other changes such as dropped items or opened
  containers may differ until the client re-enters the area. Client characters
  keep their own progress; they are not rolled back.

**Pure, tested state machine.** `coop_lite_party_step()` (with `CoopPartyWatch`)
produces `HOST_DOWN`, `HOST_UP`, `WIPE_COUNTDOWN`, `WIPE_CANCEL` and `RELOAD`, plus
per-slot fell/rose masks. `coop_lite_should_carry_slot()` is the carry rule. The
policy test covers:
- a client dies, its corpse waits ten minutes without any action, is carried across
  a host map change, and is resurrected;
- the host dies while a client lives: it waits fifteen minutes, then is resurrected;
- everyone dies: countdown, reload exactly once after the grace;
- a cancel during the grace;
- the host is down and the map changes: immediate reload;
- a host playing alone wipes;
- no host in the world: nothing happens;
- the direction and size rules of the 135 packet.

**Dev hooks** (coop host only).
- `ARCANUM_DEV_SPAWN_HOSTILE_NEAR_CLIENT=1` spawns one wolf at the client 10 s after
  it joins. That is once per session: PT10 had a spawn loop with 8 wolves.
  `=2` also sends a second wolf at the host, for party-wipe tests.
- `ARCANUM_DEV_COOP_SAVE=1` saves `Slot9990` "Coop dev save" 4 s after the first
  client is bound, so a wipe has something to reload.
- `ARCANUM_DEV_START_LOC=merchant` teleports to Shrouded Hills, then 2 s later moves
  the host next to the nearest living merchant within 80 tiles. Merchants are found
  with the client-talk rule, via `object_list_rect`; in town that is the Blacksmith,
  20 tiles away. The client joins beside the host, so it can barter at once.

**Other PT10 fixes.**
- A departed player's PC lingered on the host (census `slot=-1`). At DISCONNECT the
  slot is already inactive, so `sub_4A2B60` returned NULL. `multiplayer.c` now reads
  the binding directly (`sub_4A1F60`), and `coop_lite_host_remove_left_pc(slot, pc)`
  falls back to the OID remembered for the slot. It also drops that slot's sector
  keep-alive lock.
- If a loaded save bound a slot to a PC other than the one the player now offers
  (a re-picked character, or a save from another session), the stale PC is
  destroyed and the offered one is used.
- The host's own death was silent in PT10 (pool of blood, no log line). It is now
  logged as "host PC fell" and announced on every screen (see above).

## 25. Playtest 11 and review #2

**Client barter (PT11 #1, review H4).** In vanilla MP the client sent Packet100/12 and
waited for a reply that only the host acted on, so no window ever opened. Packets
26/27/28/81/125 also had no handler. Now:
- `inven_ui_open` on a coop client opens the inventory or barter window locally
  for its own PC, the same way own-PC movement runs locally. Barter sends
  `PACKET_COOP_BARTER` (136) OPEN/CLOSE.
- When the host gets OPEN, it checks the partner (a living merchant within 12
  tiles), prepares the store as `sub_572640` would, and sends
  `PACKET_COOP_INVEN_SYNC` (137) batches for the PC, the merchant and the store.
- Each item move goes to the host as Packet81. The host checks it before running it:
  - the sender owns the PC;
  - the flags are a plain move, optionally a whole stack (no drop, steal or gamble);
  - the sides are right;
  - the parent and partner are valid;
  - the merchant buys or sells the item;
  - the payer has enough gold.
  The price comes from the host's own `item_cost`. The host then runs the transfer
  and resyncs both sides. A refused move also resyncs, which undoes the client's
  local move.
- The client reconciles each sync batch:
  - non-stackables by OID;
  - gold and ammo by total;
  - stray ground stacks are destroyed.
- After SESSION_SYNC the client asks for a sync, so its starting kit carries the
  host's OIDs (M4).
- The "Yes?" on the first click is not the coop talk classification: no coop bark
  was logged. `DialogUI: start ...` logging was added to pin it down in the next
  playtest.

**Remote PCs at any distance (PT11 #2, review H3).** The PT11 census only searched the
vicinity, so it showed "own PC only" even when the remote PC was still there.
- The census now lists slots 0–7 and their distances, and marks a slot MISSING
  when its registered OID can't be resolved.
- The host sends a per-slot `PACKET_COOP_ROSTER` (138) on every spawn and leave.
  Clients create, remember and bind every other client's PC (3+ players).
- Verified headless: 61–64 tiles apart, total=3 on all three peers.
- Dev hook `ARCANUM_DEV_TELEPORT_CLIENT_FAR=N` (host side) moves the slot-1 PC N
  tiles east 15 s after it joins, then back 45 s later.

**Esc menu (PT11 #3).** The host lost Save/Load because the dialog flag was left set.
The Virgil intro dialog had started, and then the dev teleport moved the host away.
- In coop, `dialog_ui_is_local_pc_in_dialog` clears the flag when the UI is not in
  dialog or barter mode.
- The host no longer runs dialog for remote PCs.
- On a client, Save exports only the character, and Load is refused.

**Virgil (PT11 #5).** Virgil's intro dialog starts in both runs, with and without the
dev start. With `ARCANUM_DEV_START_LOC` the teleport fires right after it and leaves
Virgil behind. The dev hook causes this; it is not a coop bug.

**Dev hooks (PT11 #4).**
- `ARCANUM_DEV_SPAWN_HOSTILE_NEAR_CLIENT`: 1 or 2 spawns a sewer rat, 3 a wolf.
- `ARCANUM_DEV_GRANT_RESURRECT=1`, set on both sides, teaches each PC Resurrect
  once and raises WIL/CON to 18 so it can afford the cast.
- `ARCANUM_DEV_CLIENT_BARTER=1`, on the client together with host
  `ARCANUM_DEV_START_LOC=merchant`, runs a scripted buy and sell.

**Review #2 fixes.**
- H1: Packet100/13 (set HP/fatigue) is ignored on the host.
- H2: the host accepts only whitelisted (HOST_RESOLVE/BOTH) types from clients.
  121, 118, 94, 106, 54 and 71 are CLIENT_APPLY, and `sub_4EFB50` returns on the
  host.
- M1: a stale client ENet host is reset, so reconnecting works. Losing the host
  shows a notice.
- M2: PC_POS carries the map id. Poses for another map are dropped, and the client
  sends no pose during a carry.
- M3: the host checks ownership before pinning sectors. A client pose is rejected
  when it jumps more than 12 tiles or changes the art type, and the client is
  force-corrected.
- M5: Packet 54 is handled as a no-op on clients, because magictech ids are
  allocated per peer and freeing by the host's id is unsafe (known limitation).
  Other unhandled types are logged once per type.
- M6: on an adjacent target, the attacker's client plays a cosmetic swing after
  sending the attack intent. The host still resolves the hit.
- M7: poses (PC_POS, NPC_POS) go on ENet channel 2 as unreliable+sequenced, with a
  reliable 1 s keyframe. FORCE corrections stay reliable.
- L1: on the host, Packet98 must name the sender's own slot.
- L2: the deferred Packet70 queue is cleared on session reset.
- L3: each send creates one ENet packet, and it is destroyed if no peer took it.
- L4: the SP turn-based preference is saved at coop connect and restored before
  settings are saved or the session ends.
- L5: Packet57's source must be the PC or an item the PC holds.
- L6: `coop_reload_raise` is cleared on reset and on a failed reload.
- L7: Packet29 (junk-pile collapse) must be within 5 tiles of the sender's PC.

## 26. Strategy recon P0/P1 and Playtest 12

- **P0-1 character bundle.** `coop_lite_export_local_character` no longer goes through `save_char`. `save_char` stripped the live PC's inventory (`sub_463730`), fully healed it and reset its flags (`sub_4A6010`), and queued a PRIORITY_HIGHEST anim (`sub_424070`, the anim.c:4096 assert). In PT12 that dropped the host from 385 gold to 0. The export now writes `Players\<oid>.coopchar` with magic `CCB1`, laid out as PC blob + item blobs + level scheme + name. Nothing is destroyed. Separately, `save_char` itself had two bugs, also fixed: it wrote `&data`, and it used `OPCF_USE_ALT_DATA` as a field id.
- **`obj_data_write_mem`** was a stub, so every memory blob was header-only. It is implemented now as the inverse of `obj_data_read_mem`.
- **P0-2 pose ordering.** `PacketCoopPcPos` carries an epoch and a seq. The host bumps a slot's epoch on every correction and every join. A map generation (`coop_map_gen`, sent in FOLLOW and SESSION_SYNC) keys the map identity. Stale unreliable poses therefore can't undo host corrections.
- **P1-1 roster.** The host resends the full roster every 5 s and whenever the map generation changes. Clients reconcile against it and remove ghost PCs.
- **P1-2 items.**
  - Only the host generates starting kits.
  - Every client gets the other PCs' inventories, at most once a second when something changed and every 10 s regardless.
  - A missing item arrives as a full object blob (`OBJ_REQ` 139 / `OBJ_BLOB` 140). It is created from the proto only as a fallback.
- **P1-3 item ops.** Packets 26, 27, 28 and 125 are resolved by the host. The host checks reach, ownership, locks, merchant and cost, then resyncs. A drop is announced on the ground first, so the owner moves its own object instead of destroying and recreating it. Worn and wielded slots follow the host.
- **PT12.**
  - The host's Esc menu lacked Save/Load because the host was still in dialog mode: the dev-start teleport happened while the Virgil dialog was open. The dialog is now ended first.
  - Client combat mode is sent to the host as an intent (Packet11), and the host mirrors it back.
  - Clicking an NPC in combat mode attacks it. That is vanilla behaviour.
- **Dev hooks:**
  - `ARCANUM_DEV_EXPORT_CHAR=1`: export twice and re-parse.
  - `ARCANUM_DEV_CLIENT_COMBAT=1`: combat-mode toggles.
  - `ARCANUM_DEV_CLIENT_ITEMS=1`: drop, pickup and equip round trip. On the host it gives the client a dagger.

## 27. PT13/PT14 follow-ups: v2 offer, parked saves, world state (§7)

- **v2 character offer.** Before CHAR_OFFER the client sends its PC and items as a chunked bundle (`CHAR_BUNDLE` 141, ≤ 64 KB). The host validates every blob plus `coop_lite_char_plausible`, then builds the client PC from it, so a custom character (stats, skills, spells, inventory) carries over. Kill switch: `ARCANUM_COOP_NO_CHAR_BUNDLE=1`, which falls back to the proto plus the host's starting kit.
- **Parked PCs in the host save.** After the multiplayer block, a `CPK1` appendix stores token hash → PC OID for parked PCs and connected slots. On load those PCs are restored as OF_OFF and parked. A rejoining client with the same token gets its PC back, items included. Old saves have no appendix and still load.
- **Party reload hang fixed.** An open dialog that ended during `map_clear_objects` led to a validator failure, and the host sat at the quit prompt. The dialog is now ended before the reload.
- **Bags.** Vanilla Arcanum has no carryable containers, so there is nothing to sync. Known gap: a key added to an existing keyring is not resynced, because INVEN_SYNC does not refresh the fields of items the client already has.
- **§7 world state (subset).**
  - When it asks: the client sends `WORLD_REQ` 142 1.5 s after each map load (key = map + map gen) and every 30 s after that.
  - What the host sends back: `WORLD_STATE` 143, in 64-entry chunks, covering everything within 24 tiles of that client's PC (at most one answer per 3 s per slot):
    - Doors and windows: art frame, see/shoot-through and no-block flags, lock/jam/bust flags.
    - Containers: frame, lock flags, and contents. Contents go as one INVEN_SYNC per container; unknown items arrive as blobs.
    - NPCs: dead ones get HP damage plus the corpse frame; hidden ones get OF_OFF.
    - Ground items: unknown ones are fetched as blobs, moved ones are moved.
  - Pruning: on the last chunk, the client removes ground items within 18 tiles that the host no longer has.
  - Not covered yet: objects outside the radius until the next refresh, revived NPCs, and stackable ground piles the client has never seen.
  - Kill switch: `ARCANUM_COOP_NO_WORLD_STATE=1`.
  - Dev hooks: `ARCANUM_DEV_WORLD_MUTATE=1` (host) opens a door, locks one, toggles a container lock and puts a dagger in it, kills an NPC, removes a ground item and drops a dagger, all silently 10 s in. `ARCANUM_COOP_WORLD_TRACE=1` logs both sides.

## 28. PT16 and review #3 (Argon)

PT16: a GUI ground pickup on a client (AG_PICKUP_ITEM, `sub_42AA70`) is
queued (`CoopPickupQueue`, 8 entries, map-key tagged, deduped) and sent to
the host as Packet28 after a fresh reliable pose; the host does the transfer
and resyncs. Dev hooks: host `ARCANUM_DEV_KILL_CLIENT=N` (kill each client
PC once, N s after it binds), `ARCANUM_DEV_GRANT_RESURRECT=1` (also puts
Resurrect on the hotbar), client `ARCANUM_DEV_CLIENT_ITEMS=2` (pickup as a
real click goal).

Review #3, verified against the code:

- P0-1 (bundle rollback): partly rejected. `object_destroy` of a PC does
  not destroy its inventory (sub_4639E0 only drops invulnerable items) and
  destroying a destroyed object returns early, so there was no double free.
  Confirmed: the PC was destroyed before items still parented to it, and
  objects that were never postprocessed went through object_destroy. Now
  `coop_lite_bundle_rollback` (items first; read-only objects deallocated).
  Half-received bundles are freed on session reset.
- P0-2 (single pickup slot): confirmed, fixed by the queue above.
- P0-3 (suspend): no early return inside the brackets (rejected), but
  scripts did run while packets were suspended (confirmed). Host state is
  applied in `coop_lite_apply_begin/end`: suspended, scripts muted
  (`coop_lite_scripts_muted`, checked in object_script_execute), depth
  restored exactly. `coop_world_seen` was a one-time allocation, not a
  growing leak; it is static now.
- P1-1 (barter lifecycle): sessions end on disconnect, map change and a 1 s
  partner check (alive, merchant, <= 12 tiles).
- P1-2: NPC_POS writes OBJ_F_HP_DAMAGE; host-alive / client-corpse NPCs are
  revived; ground stacks are fetched as blobs.
- P1-3: INVEN_SYNC entries carry HP_DAMAGE, mana stores and a key-ring hash
  (entry 0x38, 64 per chunk).
- P1-4: every network object blob (OBJ_BLOB, rejoin PC, v2 bundle) passes
  `obj_mem_layout` (bounded header + the field list from the bitmap) and
  `coop_lite_blob_walk` (bounded presence/length walk, exact size, element
  sizes, string terminators, bitset <= count) before obj_read_mem.
- P1-5: when the host holds the player's PC (parked, save-bound, leftover)
  it wins: the client's bundle is discarded and SESSION_SYNC carries
  REJOINED, so the client adopts the host's copy even with the same OID
  (party-wipe reload). Limit: a save from before the player joined has no
  copy of them, so their bundle is used.

## 29. PT17: Resurrect crash, re-entry, ground review (Argon #2)

- Crash: Packet57/58 carried the sender's raw object handles; the client
  dereferenced the host's handle in sub_454700. Both paths now go through
  `mp_magictech_resolve_invocation`: spell range check, then every
  FollowerInfo (source/parent/target required, attacker optional) is
  resolved by OID via `sub_443F80` and checked valid / not destroyed; on
  failure the cast is logged and dropped. Applies to every spell.
- "Process function is NOT Re-Entrant" (host): not coop code; it is an
  identical second cast (same caster, spell, target) arriving while the
  first is still in BEGIN. magictech now logs the asked/running run and,
  with coop enabled on host/SP, drops such duplicates.
- Client resurrect: when the party watch sees a PC rise it re-sets hp
  damage unlocked so Packet100/13 reaches the client; the client stands
  its PC up when it stops being dead.
- Dev hooks: `ARCANUM_DEV_AUTO_RESURRECT=N[f][d]` (host casts Resurrect
  on a dead client PC after N s; f = free, d = double cast),
  `ARCANUM_DEV_GOLD_PILE` (host =N creates a 37-gold pile at the client PC;
  client =1 picks it up then drops 10).
- Argon review #2 (ground items), checked against current code:
  1. invisible gold/ammo piles: already fixed (4c15738e).
  2. pose vs Packet28 ordering: already fixed (f898ea2a; fresh PcPos on
     the app channel right before Packet28, no timer).
  3. ghost pile for the picker: confirmed, fixed (Packet72 to all for
     stacks).
  4. gold/ammo drop blocked in Packet81: confirmed, fixed (host split +
     client stack OID mapping).
  5. gold-pile dev check: added.
- Limit: if a ground stack's blob request times out, the fallback pile
  uses the proto quantity; pickup still goes through the host, so totals
  stay right.

## 30. PT17b: ground pickups in the GUI, Packet9 echo, real-click dev hooks

- Ground items vanished before a client could pick them back up. Cause:
  vanilla NPC AI (`ai_look_for_item_func`, 1 in 100 per AI tick, any item
  within perception) takes items lying around. In a shop the merchant took
  the client's dropped lockpicks and gold on the host. The client's copy
  stayed until the next world state (up to 10 s, then "1 pruned"), and a
  click on it asked the host for an item an NPC now carried. Reproduced
  headless (NPC picked up the dropped dagger and gold). Fixes:
  - On the host, an item a PC drops (item_drop_ex, inven_ui gold/ammo
    drop, coop Packet81 drop) gets `OIF_NO_NPC_PICKUP`, which vanilla AI
    already honours. A PC pickup clears it again.
  - When an item leaves the ground at the host through a pickup goal (an
    NPC, the host's PC), Packet72 goes out at once: no clickable ghost.
  - Host log: "ground item … left the ground: destroyed / now carried by
    NPC|PC …" when a listed item drops out of a slot's world state, and
    "Anim: coop: NPC … picked up ground item …".
- The real click path itself was fine: a click on a ground item goes
  intgame_process_event → sub_54ED30 → AG_PICKUP_ITEM (non-auto-attack
  path) → client-local goal → sub_42AA70 → pickup request. Verified with
  queued mouse messages. Coop sessions log one "Intgame: click: …" line per
  object click (type, combat mode, chosen goal, why refused).
- "Packet9 from slot 1 … does not own — dropped" (host log): the client,
  applying the host's Resurrect (Packet58), interrupted the caster (host
  PC) locally and asked the host to do the same. A coop client now never
  sends Packet9 for a PC it does not drive.
- inven_ui: the gold/ammo OID is mapped to the host's only on the copy
  that is sent (the local part of the drop used the unmapped one; the log
  named the item "Walls").
- Dev hooks (real UI input: mouse down/up queued like SDL input):
  - client `ARCANUM_DEV_GOLD_PILE=2` (host `=Noi`: pile 2 tiles off and a
    dagger): click the pile, drop 10 gold, click the dagger, drop it, walk
    6 tiles, click both back.
  - host `ARCANUM_DEV_AUTO_RESURRECT=Nh`: click the Resurrect hotbar slot,
    then the corpse. Spell mode armed (mode 1); one cast, no duplicate.
- Hotbar quirk (slot click looted the corpse): not reproduced. A mouse
  click on the slot and the "1" key both call intgame_hotkey_activate.
  The dev click logs whether the button under the slot centre is the slot.

## 31. PT17c results and PT18: corpse looting, pickpocketing, victim resync (Argon review, relayed 2026-10-21)

The review text is in `/workspace/review/argon-review-3-pt18.md`, outside the repo. Every claim was checked against the code before anything was changed.

### PT17c findings
- **Hotbar.** The Resurrect icon *is* drawn on slot 0. It is the blue tile right of the round button (headless screenshots with and without `ARCANUM_DEV_GRANT_RESURRECT`; the user's `bar.png` shows it too). Slots are left buttons: TIG `RELEASED` calls `intgame_hotkey_activate`, and dragging uses the right button. A mouse click on slot 0 arms spell mode in every headless run. The GUI log has no hotkey line, so the failed GUI click cannot be explained from the data yet. New coop-only trace: `HotkeyUI: click: slot N (type T, data D) activate|empty (consumed)|ends a drag` and `Intgame: click: hotkey: ...`, including why an activation was ignored (dead, stunned, dragged). The dev grant re-asserts the hotbar entry every tick, which is a no-op while it is there; `HotkeyUI: spell 64 put on hotbar slot 0` is logged when it is (re)placed. An empty slot is already consumed: `hotkey_ui_process_event` returns true, and world clicks need the iso window (`sub_5517A0`). Argon's "empty slot falls through" is **rejected**.
- **Missing lockpicks.** These were not a desync. The host looted the client's corpse, which is vanilla `inven_ui` loot on the host. The items moved to the host PC, and the client saw them leave through the host PC's party sync, which moves items by OID. What was missing was the **victim's own resync**: gold, and anything not re-homed, stayed on the client. Reproduced headless with `ARCANUM_DEV_AUTO_RESURRECT=5lh`: after the host's Take All the host had slot 1 at gold 0 while the client kept gold 400. Fixed by the victim watch below.
- **"slot 1 transfer done (weapon now '')".** That line is the gold-pile *pickup* (op 28, same OID as the client's `pickup queued`). `weapon now` is the wielded weapon after the op, and it was empty because the host had already looted the dagger. The drag of a worn item to the drop box never reached the host: there is no `UI: InvenItemTransfer` in the client log.
- **"0 ground items".** That world-state line came just before the drop. The next one says `1 ground items`, and the client listed 1. Timing only.
- **No "Intgame: click" lines on the client.** They are there (client log lines 606–610). The trace runs on both sides.

### Argon #3 verdicts
1. Equipped-weapon drop: **rejected** (both parts). `item_drop` already does `item_remove` (unwield) + `sub_466E50`. `mp_handle_packet70` already moves an existing inventory copy to the ground (`item_remove` + `object_drop` under lock/suspend) instead of skipping it. Headless `ARCANUM_DEV_OPEN_INVEN=3` (new: drop a worn item through the drop-box Packet81) dropped the worn suit (loc 1006): host `drop done`, client inventory without it, `1 ground items listed`, nothing destroyed. New guard: a dead PC's transfers are refused (a corpse could drop its things).
2. Gold drop missing from world state: **rejected.** `item_gold_create` → `mp_object_create` sends Packet70 regardless of `multiplayer_lock` (the lock does not gate it). `pile != item` therefore skips nothing. The GUI client log shows `obj blob: ground item loaded` right after the drop, and the next world state lists it.
3. NPC pickup: **done as proposed**, and this replaces the earlier "revert". `coop_lite_mark_player_drop` acts only with `ARCANUM_DEV_GOLD_PILE` / `ARCANUM_DEV_CLIENT_ITEMS`, so NPCs take player drops as in vanilla. The immediate Packet72 when anything takes a ground item (`coop_lite_host_ground_item_taken` in `sub_42AA70`) stays.
4. Looting / pickpocket: **implemented, with corrections**:
   - Pickpocket is `INVEN_UI_MODE_STEAL` = 3; mode 4 is IDENTIFY.
   - Packet81 0x04 is "take to cursor/use" and 0x10 is gamble, so pickpocket gets its own bits: `COOP_XFER_STEAL` 0x40 / `COOP_XFER_PLANT` 0x80, only as 0x41 / 0x81.
   - Client: `coop_lite_client_opens_inven_locally` accepts modes 0–3. The client's own use-container goal (anim.c, container and critter branches) and the pickpocket skill open the window locally. `inven_ui_open` sends `COOP_BARTER_OPEN_LOOT` / `_OPEN_STEAL` with the target. In a pickpocket window the drag (`sub_575180` / `sub_575100`) asks the host (`coop_lite_client_request_steal`) instead of running the skill locally.
   - Host: `coop_lite_target_kind_of` → `coop_lite_target_kind`, which is pure and unit-tested. Within `COOP_TARGET_REACH` 4, the target can be an unlocked container or a dead critter (NPC corpse, host or client corpse) = LOOT, or a living critter other than yourself (NPC or PC) = STEAL. The session lives in `coop_barter[slot]` (`target_valid/kind/target`). It ends like barter (close, disconnect, map change, watch: gone / died / locked / too far), and `coop_lite_host_sync_slot_inven` sends [pc, target].
   - Transfers inside a session go to `coop_lite_host_target_transfer`, which checks `coop_lite_target_transfer_ok` (unit-tested). Loot: no steal bits, the window's own `sub_578EA0` at price 0. Steal: only 0x41 out of the target and never a worn item (vanilla refuses those drags); plant: 0x81 into it.
   - NPC victim: the vanilla `skill_invocation_run(SKILL_PICK_POCKET)` (transfer, alignment, OCF2_ITEM_STOLEN, detection with `ai_attack`). PC victim: the same odds (`basic_skill_effectiveness` vs `skill_invocation_difficulty` + d100; gold ≤ 100, ammo ≤ 20). A failure gives both players a red "Caught pickpocketing!" and nothing moves. A thief whose attempt failed unnoticed against an NPC gets "Your pickpocket attempt failed."
   - Victim resync (4c): the host keeps a signature (gold, ammo, item OIDs + locations) of what it last sent each slot about its own PC. `coop_lite_host_victim_watch` (every 250 ms) resyncs the slot as soon as the host's copy differs: host loot of a client corpse, host or other-client pickpocket, scripts, NPC theft. Log: `inven: slot N PC's inventory changed on the host (loot / steal / script) - resync, gold G`. This is a signature check, not a hook in `item_transfer`, so every path is covered, including stacks.
5. Hotbar: see above. The icon and arming work in-process, a click on an empty slot is already consumed, and a trace was added for the next GUI run.

### Headless evidence (`logs/pt17c-dev/`)
- `loot2`: host Take All on the client corpse moved the items. Before the fix the client kept gold 400 while the host had 0.
- `loot3`: with the fix, `changed on the host ... resync, gold 0`. The client ends with 0 items and gold 0. Hotbar trace `HotkeyUI: click: slot 0 (type 3, data 64) activate` → spell mode → resurrect. Worn suit dropped.
- `steal1`: the client pickpockets the host PC without skill → `caught`, notice on both sides, nothing moved.
- `steal-h` / `steal-n` / `steal-c` (`ARCANUM_DEV_STEAL_FORCE=1`): steal from the host PC (ring moves), from the Blacksmith (key moves), and loot of a rubbish bin (Take All, 7 items).

### New dev switches
- Host `ARCANUM_DEV_AUTO_RESURRECT=Nlh`: `l` first loots the corpse the GUI way (click, Take All, close).
- `ARCANUM_DEV_INVEN_DUMP=N` (both sides): every N s, the PC inventories as the UI sees them, with flags and a parent check.
- Client `ARCANUM_DEV_CLIENT_LOOT=c|n|h`: 25 s in, walk to the nearest container / living NPC / the host PC, open the window through `inven_ui_open`, Take All or one pickpocket drag, close.
- Client `ARCANUM_DEV_OPEN_INVEN=3`: drop a worn item (the weapon if any) through the drop-box Packet81.
- Host `ARCANUM_DEV_STEAL_FORCE=1`: pickpocket always succeeds (tests the success path).

Known noise: `ASSERTION anim.c:4105 priority_level < PRIORITY_HIGHEST` on a client when its PC falls. Vanilla callers (`multiplayer.c`) also pass PRIORITY_HIGHEST, the original check was `<=`, and the CE assertion is stricter. Harmless.

### PT18 GUI run 1 (`logs/pt18-gui1/`)
Part 1 passed: the host looted the client corpse (`changed on the host ... gold 0`), and the hotbar resurrect worked (`HotkeyUI: click: slot 0 ... activate`, then `resurrected`). Part 2 failed. Diagnosis:
- **Container click.** Both client clicks were `Intgame: click: on own PC, ignored`. The grey can next to the PC is scenery. The client PC stood over it, so its sprite took the click. The real Rubbish Bin (wire basket, 7 items) is at 90246,84155. A real xdotool click opens it: `goal 81` → `opened loot window on 'Rubbish Bin'`.
- **Pickpocket.** The skill rows in the character sheet only show the skill. Vanilla uses skills through the in-game Skills button, which opens the rotating panel (Prowling / Pick Pocket / Disarm / Repair): pick the hand icon, then click the target (`skill_ui_activate` → `sub_57A710` → `inven_ui_open(STEAL)`). On a client this already goes to the host (`OPEN_STEAL`). Clicking the Blacksmith normally is dialog, which is correct.
- **"Snap back."** There is no `host correction` (client) and no `implausible` (host) line. Both censuses agree. The client walked to 84158, and the click on the Blacksmith (`goal 21`, talk) walked it back next to him. That is not a correction.

Fixed:
- **A peer PC that dies stayed standing on the client.** Only hit points arrive, and the poses of dead PCs are not interpolated. `coop_lite_client_watch_vitals` now lays down / stands up other slots' PCs (`slot N PC (peer) fell — corpse pose here` / `stood up here`).
- **Looting the gold of a remote PC** (host corpse, another client) sent the client's local stack OID: `takes '' from target: refused`. INVEN_SYNC already carries each critter owner's gold/ammo OIDs. The client now remembers them (`coop_stack_map`), and `coop_lite_client_map_stack_oid` maps any critter's stack, not only its own.
- **Closing a loot / pickpocket window** now ends the host session (`window ended (closed by the client)`). Before, it ran on until the watch ended it.

New dev switches (both sides unless noted):
- `ARCANUM_DEV_GRANT_PICKPOCKET=N`: N Pick Pocket points (DEX raised to 20), once per PC.
- `ARCANUM_DEV_LIST_NEAR=S`: S s in, log containers and critters within 15 tiles: name, tile, lock / dead, items, and the game-window spot where a click picks it (`intgame_dev_find_spot`).
- `ARCANUM_DEV_TRIGGER=<file>`: polled every 0.5 s; the first word is run and the file deleted. `list`, and on the host `kill-host` / `kill-client`. The host's death does not wipe while a client lives. The client can loot the corpse and Resurrect it from its hotbar.

Headless with real xdotool clicks (`pt18-gui-flow.sh`, `logs/pt18-h3/`): bin loot (boots), pickpocket of the Blacksmith (key) and of the host (ring) with skill 20, kill-host, client loot of the host corpse (385 gold, client 785), and client hotbar Resurrect of the host (`host PC resurrected`). Each window close was logged.

### PT18 user session 1 (`logs/pt18-user1/`): checks
- **"Invisible" drop.** The log shows the host dropped the Worn Ladies Boots (`going from Merwin Tumblebrook to nobody`), not the client. Reproduced headless (`logs/pt18-d2`, `pt18-d3`; new trigger `items` dumps ground items with flags and screen spot). The item lies on the dropper's own tile, flags 0x434 (FLAT | SEE_THROUGH | SHOOT_THROUGH | NO_BLOCK, no OFF / INVENTORY / DONTDRAW), and is clickable. It is drawn under the dropping PC's sprite, so it only shows once the PC steps away (`host-walk2.png`: the boots by the bin). The Human Guard (vanilla ONF_LOOK_FOR_ARMOR wanderer) picks such drops up within about 10 s. This is vanilla rendering and AI, not a sync bug.
- **Plant logged as steal.** The client sent only one request in that session, `asked the host to steal`, and the host stole the host PC's own Crude Lockpicks (the Merwin pregen carries some; 6 + 7 from the bin − 1 dropped = 12 items). Headless plants are sent and logged as plants (`plants 'Bandages' into target: succeeded`).
- **Host steal from a client.** Headless host pickpocket of the client PC (100 gold, then a ring planted and stolen back) logs `inventory changed on the host ... resync` every time, and the client applies it (`1 removed`). The live host log has no host steal at all, so nothing moved on the host there.
- **PT18 GUI session 3 (12:1x): "client pickpocket on Merwin opens nothing", "no logs".** The windows were the other way round this time: the host window at (891,29), the client's at (5,29). The actions ran in the HOST window. The host log has the bin click (`Intgame: click: object type 2 ... goal 81`, `Worn Boots ... to Merwin`) and `Intgame: click: on own PC, ignored` for the click on Merwin. The host's pickpocket of the Blacksmith is vanilla and local, so it logs no `loot:` line, only vanilla "You succeed.". The client log has no clicks at all. Logs are not buffered. Headless (`logs/pt18-p2`): the client pickpocket on Merwin opens (`opened pickpocket window on 'Merwin Tumblebrook'`). After `kill-host` the corpse hovers as "Merwin Tumblebrook / Dead" on the client and a click loots it. Fix for the confusion: coop windows are now titled "Arcanum CE - CO-OP HOST" / "Arcanum CE - CO-OP CLIENT".

### PT18 GUI run 3 (build 33f2e1dc): "direction inverted for a PC target"

- **BUG A — not a code bug, a panel mix-up.** The client's own engine log
  (`UI: InvenItemTransfer - Bandages(G_38A9…) going from Godfrey Castleburger
  to Merwin Tumblebrook`) shows the drag really went from the client's PC to
  Merwin: those Bandages were Godfrey's starting kit (Merwin never had any).
  In the loot / pickpocket window the **left** panel (portrait at the top) is
  the target, the **right** panel (gold / stat column) is your own PC. The
  "gold at 0 → refused" was the client's own gold with the quantity box left
  at 0; the host correctly refuses a 0 transfer and resyncs. Headless real
  drags (pt18-pc1/pc2) in both directions against Merwin dead (loot) and
  alive (steal/plant) and against the Blacksmith all log the right verb, and
  `inven` dumps on both sides agree on every item's owner.
- **BUG B — same mix-up**: after planting its Bandages the client's own grid
  holds only gold (the suit is worn) — that was the "Merwin panel with only a
  gold icon". Merwin's panel shows gold, ring, lockpicks headless.
- **BUG C — fixed**: the host now sends the thief `COOP_PARTY_STEAL_OK`
  ("You succeed.", green) or `COOP_PARTY_STEAL_FAILED` for every pickpocket
  roll (NPC and PC victims); "Caught pickpocketing!" is unchanged.
- The client log line now names the item and direction:
  `loot: asked the host to plant an item ('Bandages' from my PC into 'Merwin Tumblebrook')`.
- New trigger command `inven` (`ARCANUM_DEV_TRIGGER`): dumps every slot PC's
  inventory with owner checks on that side.

### Core-systems test (combat, corpses, loot, map change), 2026-10-21

Headless runs `logs/core1`–`core10` with real UI-click hooks. Fixes:

- **Client attack clicks never reached the host.** `intgame` builds AG_ATTACK
  with `anim_goal_add`, not `anim_goal_attack`, so no attack intent was sent
  and the client's PC swung at nothing (rat HP unchanged after 3 client
  clicks). A coop client's attack click is now sent as
  `PACKET_COOP_ATTACK_INTENT` (`anim_goal_attack`), and the host logs
  `slot N attacks '<target>' (dist d): goal started`.
- **"There is nothing to loot." on the client** for a corpse whose items were
  gained on the host (the client's copy was empty). A coop client now opens
  the loot window anyway; the host's loot-session sync fills it.
- New `ARCANUM_DEV_TRIGGER` commands: `hp` (HP, death and combat focus of
  slot PCs and NPCs within 15 tiles), `attack` / `attack-npc <name>` (a real
  click in combat mode; the host uses `anim_goal_attack` directly), `loot
  <name>` (nearest corpse, NPC or PC), and on the host `spawn-rat|spawn-wolf
  [host]` (25 gold in its inventory), `kill-npc <name>`, `maps`, `map <n>`
  (teleport_do to map n's start, as a stair or door does).

Known, not fixed: the host PC's stat changes (dev CON grant, and so likely
level-ups) don't reach clients. The client shows Merwin at 24 max HP while
the host has 34; current damage matches. The NPC blood pool decal is
host-only (the corpse pose matches).

## 32. PT19: PC build sync and client blood pools (Argon review #4)

- **`PACKET_COOP_PC_STATS` (144, two-way; type limit now 145).** The payload is raw copies of the
  base stats (all 28, including level, XP, alignment, fate and unspent points), 12 basic skills,
  4 tech skills, `OBJ_F_CRITTER_SPELL_TECH_IDX` (16 college levels, which are the known spells, the
  mastery slot and 8 tech degrees) and up to 32 found schematics. Argon said 16 basic skills and a
  separate known-spell bitmask. Neither exists: there are 12 basic skills, and the college levels
  are the known spells.
  - **Host to clients:** each slot PC has a signature, checked every 500 ms. When it changes, the
    host broadcasts the build. When the roster changes, it resends every slot.
    - A non-owner client applies the full build. The client now shows the host PC at 34/34 max HP,
      not 24.
    - The owner takes only level, XP, alignment and fate. It also takes the unspent count, but only
      once the host has every spend of its own, or on a level-up.
  - **Owner to host:** when the local build signature changes (a charedit spend), the owner sends
    it. Vanilla charedit sends nothing in MP, so nothing is applied twice.
    - The host checks the sender is the slot owner.
    - It then runs `coop_lite_pc_build_spend_ok`. Only rises are allowed, each costing one point,
      with caps of 20 for stats and skills, 5 for colleges and 7 for degrees.
    - On success it applies the spend and the 500 ms watch rebroadcasts. On failure it logs
      `spend REJECTED` and sends its own copy to the owner with `COOP_PC_STATS_F_FORCE`.
  - **Dev triggers:** host `give-points N`; client `spend` (+1 STR, -1 point) and `inflate`
    (+5 STR, no points).
- **Blood pools:** Argon's diagnosis was wrong. `sub_432D90` returns early on a client. The host
  already sends Packet70 subtype 5, which the client handles; nothing filters it. The client's
  pool sat on frame 0 while the host's grows through AG_ANIMATE, so the client now sets the last
  frame. Log line: `MP: blood pool at x,y (4 frames)`.

## 33. PT20: Gemini review fixes and party XP

Review fixes (verified against the code; full list in the review notes):

- `TIG_NET_MAX_PACKET` 4096 → 16384. A full world-state chunk
  (0x10 + 64 × 0x40 = 4112 bytes) and any object blob over ~4 KB were
  silently dropped by `tig_net_send_raw_ex`. ENet fragments large packets;
  static asserts now check that the world chunk, obj blob and PC stats fit.
- Co-op: `critter_kill` no longer schedules body decay for PCs. Before,
  a fallen player lying for a game day (world-map travel, resting) would
  have been destroyed by `critter_decay_timeevent_process`.
- `coop_lite_pc_build_spend_ok`: skills compare points (low 6 bits) with
  the training bits held equal, so a trained skill can take a point (it
  used to be refused as over the cap of 20). Training and the mastery
  college (index 16, never checked before) can't change through a spend.
- Identify service (packet 125): the host charges
  `spell_money(SPELL_DIVINE_MAGICK)` as `inven_ui` does and ignores the
  client's `cost`, which let a client identify for free.

Party XP (host-authoritative):

- `critter_give_xp` (kill bonus, per-hit combat XP, quest, dialog and
  script XP) applies the difficulty first. In co-op, clients then return
  (XP is the host's), and the host calls `coop_lite_host_share_xp`, which
  splits the XP among living, connected slot PCs. Coop-lite keeps every PC
  on the host's map. Each PC gets `(xp + carry) / n` and keeps the rest
  as a per-slot carry, so tiny per-hit awards still even out. The award
  goes through the vanilla tail (`critter_award_xp`:
  `effect_adjust_xp_gain`, `stat_base_set` → `level_recalc`). Dead PCs
  get nothing. A PC that is alone, or an earner who is not a party PC,
  uses the vanilla path.
- Level-ups run on the host and reach the owner through packet 144. If
  the owner sees a higher level, it takes the whole build, because the
  host may have run an auto-level scheme (pregens have one). It also shows
  the vanilla level-up message and lights the character button.
- Dev: the `give-xp N` trigger (host) calls `critter_give_xp(host PC, N)`.
  The `hp` trigger now also prints lvl, xp and unspent points.
- Headless runs (logs/pt20, pt20b, pt20c) checked the following. 20000 XP
  gave each PC 10000, and both reached level 4. The client logged
  `level up 1 -> 4` and its build signature matched the host's
  (336f9bea). With the client dead, 100 XP went to the host alone and the
  corpse got 0. The combat per-hit path was not hit: the host's attacks
  on the dev rat all missed.

## 34. PT21: DeepSeek review fixes

- `coop_lite_write_char_bundle`: the buffer starts zeroed. A failed build
  (bad inventory count) used to free an uninitialised pointer.
- `tig_net_send_ctrl_to_peer`: if `enet_peer_send` fails, the packet is
  destroyed instead of leaked.
- `coop_lite_client_map_stack_oid`: the 24-byte wire OIDs are read with
  memcpy (`coop_lite_oid_bytes_set`), not an unaligned `ObjectID*` cast.
- Protocol check: `TIG_NET_PROTOCOL` (21) travels in WELCOME (host to
  client) and NAME (client to host). A missing or different value is
  logged ("protocol X != ours Y ... refusing") and the peer is
  disconnected, so mismatched builds no longer corrupt each other. Bump it
  whenever a packet layout changes.

## 35. PT22: protocol refusal hardening (DeepSeek follow-up)

- Host: a refused peer (NAME too short or with the wrong protocol) is
  marked `refused`. Its app packets, such as a CHAR_OFFER queued behind
  the NAME, are dropped until the disconnect lands.
- Client: a refused host sets a sticky `host_refused`. From then on the
  client sends nothing to that host and drops everything it receives.
  `tig_net_start_client` won't rejoin it, which matters because the
  automatic New Game rejoin sends its CHAR_OFFER before WELCOME; a live
  run of an old host against the new client showed exactly that (logs/pt22
  vs pt22b). Only `tig_net_configure_join` clears the flag.
- The refusal logs tell an old peer (packet too short, no protocol field)
  apart from a real version mismatch.
- Char bundles: level must be 1..51 (`COOP_CHAR_MAX_LEVEL`, the STAT_LEVEL
  max) and XP 0..2e9. The PC must also have at least the XP its level
  needs, per the new helper `level_xp_required` (from xp_level.mes).
- Unit test: an old NAME followed by a CHAR_OFFER leaves the offer
  undelivered and the peer disconnected.

## PT25 behaviour notes

- **Game time** belongs to the host. A client cannot open the sleep/wait UI
  ("Only the party leader can rest or wait."). When the host rests, clients
  fade out with it (PARTY_STATE REST_BEGIN/END), every co-op PC and its
  followers get the engine's hourly resting heal on the host, and the new
  time is pushed (Packet3). Clients follow the host's clock both ways.
- **Party peace:** co-op PCs and their followers never become each other's
  enemies (`ai_attack`/shitlist gate); a caught pickpocket between players
  only shows the notice. The host never auto-counter-attacks with a client's
  PC (`sub_4A9AD0`); its owner drives it.
- **Client melee:** the host runs the client PC's AG_ATTACK; its pose stream
  no longer overwrites the attack art, and every strike is echoed to the
  owner (PACKET_COOP_SWING) so the swing animates there. When the target
  steps away the owner walks after it itself (SWING hit=2), never the host.
- **Auto-attack / always-run** are switched on once per run on a client;
  peers' PCs on the host always run and keep attacking.
- **Ground items:** no junk piles in co-op (clients never learned the new
  pile containers); a PC drop on the host asks clients to refresh the
  world state within ~4 s.
- **NPCs** attacking in co-op run (vanilla did so only offline); their aggro
  growl / attack cry is forwarded to clients near them (Packet106).

## PT26 behaviour notes

- **Container stacks** (gold, ammo in chests, barrels, piles) are listed in
  INVEN_SYNC as items (OID + quantity, `COOP_INVEN_E_STACK`) instead of a
  per-proto total; the client drops unlisted stale stacks and loads unknown
  ones as blobs (the host now serves container stacks). Critters keep the
  total-based stack sync. A junk pile the host destroyed when emptied is
  pruned on clients by the world state.
- **Missing NPCs:** an NPC_POS for an unknown OID (in a loaded sector) makes
  the client ask the host (PACKET_COOP_NPC_REQ 146, at most every 5 s per
  OID); the host re-sends Packet70 so the NPC is created from its proto, and
  NPC_POS then sets pose, art and HP. An NPC that NPC_POS lists but the client
  has OF_OFF (switched on by a script) is switched on.
- **Party-shared hostility:** in the AI target search an NPC that is KOS by
  reaction toward one co-op PC (within 20 tiles) or already fights one treats
  the other co-op PCs as KOS too (dialog keeps the per-PC reaction).
- **Client melee:** an AG_ATTACK refused on the host because of a busy
  animation is retried for 1.5 s instead of being dropped; refusals and swing
  animation failures are logged (`attack refused ... retrying`, `swing anim
  refused`, CoopDiag `anim=ok/fail`).
- **Vitals:** the host sends each client PC's HP/fatigue after every rest hour
  and on wake-up, and every 3 s when they changed (the setters stay silent
  under the MP lock).
- **World map:** while the host has the world map open, clients get a dark
  overlay and a notice (PARTY_STATE 14/15, refreshed every 3 s, lapses after
  8 s without a refresh).

## PT27 behaviour notes

Protocol stays 24 (no packet layout changed).

- **Anim-end snapshots on clients:** a co-op client ignores vanilla Packet10
  (anim-end pose: location, offset, stand frame 0) for critters, and Packet9
  (interrupt + location/art) for PCs. Those snapshots reset the client's own
  PC to the stand frame after each host-side swing (the "swing plays once"
  bug) and snapped the host PC ahead of the interpolation ring (smear /
  rubber band). Positions keep coming from PC_POS / NPC_POS. CoopDiag
  `p10=own/peer/npc` counts the dropped snapshots; `peer_anim` and
  `interp_lag` watch the peer PC interpolation.
- **Client swing:** the echo of a client's own swing is skipped once (700 ms)
  instead of every swing in that window; a busy AG_ANIMATE is restarted.
- **Container / corpse use from a distance:** AG_USE_CONTAINER is a
  client-local goal, so the client walks to it (AG_MOVE_NEAR_OBJ) and opens
  the loot window as in single player.
- **Far loot:** a dropped item the client had destroyed earlier (an owner's
  INVEN_SYNC arrived first) left an OF_DESTROYED handle that blocked the
  refetch. Such handles are freed (`world_reclaimed`) before a ground item or
  a container entry is applied, so the blob is requested again. Ground items
  in unloaded client sectors are not fetched until the sector loads.
- **Rest until healed:** the host keeps sleeping while any living co-op PC
  is hurt (cap 7 days).
- **Party:** an NPC follower that heals (AI heal) also considers the other
  co-op PCs and their followers within 20 tiles. Party-shared hostility now
  covers KOS by reaction, faction and alignment, up to 120 tiles.
- **Note:** an NPC whose dialog with a PC never ended keeps its AI frozen
  (vanilla behaviour).

## PT28 behaviour notes

Protocol 25: PACKET_COOP_PC_POS grew to 0x60 (visual state: look flags,
critter flags, spell flags, blit flags/colour/alpha/scale).

- Own-PC equip slot <-> backpack moves (Packet81) are allowed without a
  barter/loot session; the victim watch takes the result as known state.
- Client swing: an attack anim already playing gets its frame reset instead
  of an interrupt; the first host echo never cuts the local swing.
- Weapon hit/miss/critical sounds are forwarded (Packet106) for PC and NPC
  attackers.
- Remote-PC interpolation delay 55 ms; a stand pose snaps.
- Clients take the host's look of every PC; a base-art change (polymorph
  ends) clears stale overlays.
- Spell invocations: the parent is optional. Packet77 spell eye candy is
  streamed host -> clients again (no double when already shown).
- The build id (git short sha) is the first log line and in the window title.

## PT31 behaviour notes

- **Stuck follow.** A host that loaded a save from the menu never armed the
  follow (only the party-reload path did), so its first real map change was
  swallowed as the "initial spawn" and the client stayed behind forever (both
  sides drop PC_POS of another map key). The host now arms the follow after
  1.5 s settled in the world (`follow armed (host settled in the world)`).
  Client self-heal, no packet: a host PC_POS whose map key differs from ours
  for 3 s (slot 0) makes the client request the teleport itself, from the key
  (map = key & 0xFFFF, gen = key >> 16) and the packet's loc; retried every
  5 s (`map mismatch ... self-follow`).
- **Paralysis / stun.** `coop_lite_client_clear_stuck_flags` clears
  OCF_PARALYZED / OCF_STUNNED on the client's own PC on follow and sync (the
  host's state is echoed to the owner, which is what sets them there);
  without it the vanilla anim start refuses to run and the PC is frozen.
- **Rejoin, stale live mobs.** The world-state answer now lists every dead /
  OF_OFF NPC of the host's loaded map (not only those within reach), and the
  client prunes unlisted NPCs inside COOP_WORLD_PRUNE_RADIUS. Limit: the host
  only knows sectors it has loaded.
- **Crash log.** Windows builds install an exception filter
  (`src/game/crash_log.c`): `CRASH: exception ... exe+0x...` and a stack as
  exe offsets in the log. Resolve with `nm -n` of the same build.
- **Remote PC motion.** The engine advances the art frame and the sub-tile
  offset together in one discrete step (`sub_4305D0`). The old client lerped
  the offset per render frame while showing the newer sample's frame.
  Default is now stepwise: the newest due sample is shown whole.
  `ARCANUM_DEV=1 ARCANUM_DEV_INTERP=lerp` restores the lerp for comparison.
  PC_POS interval 50 -> 33 ms, interpolation ring 3 -> 6 (120 ms delay kept).
  Stand-pose snap and the over-1-tile snap are unchanged.

## PT32 behaviour notes (Argon review of PT31)

- World prune is measured from the client PC's location when it SENT the
  WORLD_REQ (not where it has run to by the last chunk). The host samples 24
  tiles around its copy of that PC; the prune radius is 18, so 6 tiles cover
  the PC_POS lag. No packet change, protocol stays 25.
- `coop_interp_push`: a new pose more than 2x COOP_INTERP_DELAY_MS after the
  newest sample restarts the ring (stepwise mode no longer shows a stale
  sample when walking resumes after a stand). Keyframe duplicates still return
  early and keep their time.
- Follow-arming and self-follow statics are file scope and cleared in
  `coop_lite_session_reset`; a matching slot-0 map key clears the mismatch timer.
- Crash log prints raw addresses and shows `exe+0x` only for addresses inside
  the exe image (NULL jumps and DLL frames stay readable).

## PT33 behaviour notes

- **Host save load → clients follow.** A save loaded from the menu goes through
  `sub_5432B0`; it now calls `coop_lite_host_note_save_loaded()`, which arms the
  existing party-reload resync (`COOP_PARTY_RESYNC` once the host is back in the
  world: clients reset the session and re-offer their PC; PCs come back on their
  feet as after a wipe reload). No packet change.
- **Dead PC level-up.** The XP split still pays only the living. The host's
  500 ms PC-stats tick now runs `level_recalc` (auto-level scheme included) for
  any connected PC, dead or alive, whose XP already reaches the next level; the
  normal build sync carries it to the owner.
- **Worn items vs inventory sync.** When the host's copy lists an item in the
  backpack that the client's PC still wears, the client keeps it worn and
  re-asserts the slot with the usual own-move Packet81 (rate-limited per item).
- **Auto-generated loot.** `sub_463E20` returns on a suspended coop client (the
  window's local open-time prep used to roll loot with the client's RNG, which
  the host's empty container sync then deleted). The host rolls SPAWN_ONCE stock
  on the first OPEN_LOOT before it syncs.
- **Main menu.** "Host Co-op" / "Join Co-op (addr)" under Exit (plain launches
  only). They relaunch the exe with `-coop -host` / `-coop -join <addr>`, keeping
  other options, then close this copy. Join address: a valid host[:port] on the
  clipboard, else `coop_join.txt` (last used; written by `-join` too), else
  127.0.0.1. CLI flags unchanged.

## PT35: story sync (protocol 26)

The client's journal (logbook) reads the PC's quest / rumor / reputation
arrays and the global quest and quelled-rumor tables. The host owns them:

- `PACKET_COOP_STORY_SYNC` (147, host -> client). Every 500 ms the host builds
  each client's journal (story PC record merged with the client PC's own record:
  the further quest state wins, rumors and reputations are unioned, plus the
  global quest states and quelled bits) and sends it when its FNV signature
  changed, on join / PC change / host save load, and every 30 s as insurance.
- A snapshot is a sparse blob (`coop_story_encode`, strict `coop_story_decode`)
  cut into 2048-byte chunks (at most 32; worst case about 58 KB). The client
  reassembles (any order, duplicates ignored, signature checked) and applies it
  to its own PC and the global tables with net sends and scripts suppressed.
  What the host does not list is cleared locally. An open logbook reloads.
- The client never writes this state: quest_state_set(_internal), quest_unbotch,
  quest_global_state_set, rumor_known_set, rumor_qstate_set and
  reputation_add / remove return early on a co-op client
  (`coop_lite_story_write_blocked`), except inside the apply scope.
- Not synced: kill / injury statistics and the background text (separate
  logbook pages), global variables / flags (the logbook does not read them).
- Test: `coop_story_test` (codec round trip, strict decode, merge, worst-case
  chunking and reassembly, packet policy).

## PT36: dialog viewer + voting (protocol 27)

When the host talks to an NPC, the other co-op clients see a read-only panel
(NPC name, current NPC line, the PC reply options). Only the host's own
dialog is shown; a client's own merchant dialog stays local (not broadcast).

- `PACKET_COOP_DIALOG_STATE` (148, `TYPE_LIMIT` 149), role BOTH, with a
  `kind` field: STATE (host -> client) or VOTE (client -> host). Each side
  drops the other's kind (`coop_dialog_packet_valid(pkt, size, from_host)`).
- STATE is sent over the reliable channel by the host tick to every active
  client whose last seen `seq` differs: on open, every new NPC line / option
  list (`dialog_ui_display`), every vote-count change, and on close
  (`dialog_ui_end_dialog`, notify-ended, reset; header-only close packet). A
  client that joins mid-dialog gets the current state. Texts are capped
  (name 63, line 999, option 383 bytes, at most 5 options): the host truncates,
  so one packet is always 3008 bytes (< 4096) and nothing is chunked.
- Validation: exact sizes (open = whole struct, close / vote = 24-byte
  header), kind / action / option-count bounds, every string NUL-terminated
  inside its field, vote counts <= 7 and zero past `num_options`, stale or
  repeated `seq` dropped. Control characters are blanked for display.
- Client viewer (`dialog_ui.c`, PT39 layout): a SOLID opaque tig window
  (fill 20,16,12, border 160,130,70, no color key; PT38's interlaced
  see-through rows were an unreadable mesh) in the top-right corner (2/5 of the
  screen width, min 360 px, height capped at 1/3 screen, recomputed from the
  screen size whenever it is shown). It shows the NPC name, the last 6 wrapped
  lines of the NPC line ("..." when cut; fewer on small screens), and up to 5
  one-line options with "[n]"
  votes. Mouse buttons are swallowed only inside its rectangle.
  It is a plain tig window with a message filter.
  It has no path into the dialog engine; its only output is a vote packet.
  Vote: click an option or press V (cycles); voting for the same option
  again clears it. Your vote is marked "<" (and green), counts as "[n]".
- VOTE (header only): option index or -1. The host checks the slot, that a
  dialog is open, the line id (stale votes dropped), the index against the
  line's option count, and a 250 ms per-slot rate limit. Votes reset on every
  new line; a slot that leaves loses its vote. The host shows the counts as
  " [n]" after each option (display string only; the option text the game
  uses is untouched) and republishes them to the clients. The host alone
  picks the answer; the vote has no effect on the dialog.
- Not done: initials instead of counts; a client's own dialog is not shown to
  others; the viewer is not repositioned on a resolution change; the panel
  stays up while the client is itself in a dialog.
- Test: `coop_dialog_test` (codec, truncation, strict validation, vote
  aggregation / rate limit / tally, packet policy). The panel and the host
  markers are compiled but not playtested (no game data on this box).

## PT37 (protocol 28)

Verified against code before changing; Argon was right on several symptoms,
wrong on some causes.

1. **NPC fatigue** (fixed, protocol 27 → 28). `PacketCoopNpcPosEntry` had a
   `reserved` int at 0x34 (always 0); it is now `fatigue_damage` (size
   unchanged, the host sends `critter_fatigue_damage_get`, the client applies
   it). The logged "TimeEvent" error was a generic map-load message, not the
   cause. Bumped because an old host would send 0 and zero the client's fatigue.
2. **Ghost NPCs after a host reload** (fixed, runtime untested). The client
   sync teleport now carries `TELEPORT_FORCE_RELOAD`: same-map reload of the
   cleared `Save\Current\maps\<map>` (the clear is unconditional) without the
   `map_flush` that would write the stale state back. The host resets the NPC
   cache on resync / client spawn so the full state is resent. An AG_ATTACK
   intent beyond range+3 is refused and the host chases to the owner instead of
   dragging the client PC (FORCE slot 1).
3. **Doors** (fixed, untested in play). New `PACKET_COOP_DOOR_USE` 149
   (client → host, 32 B, `TYPE_LIMIT` 150). A client's own AG_USE_OBJECT on a
   portal is client-local (walk), `sub_428A10` queues the use; PC_POS goes
   first, then the door packet. The host checks the sender's PC, that the
   portal exists and is ≤ 8 tiles away, runs the door's own SAP_USE script
   (locks / keys apply), then WORLD_DIRTY at once. Doors opened by the host,
   NPCs (`portal_toggle`, the two NPC `portal_open` sites) call
   `coop_lite_host_portal_changed()`: a 400 ms trailing debounce → WORLD_DIRTY.
4. **Sprite trail / "outside the limits" spam**. Argon's cause was only half
   right: `sub_43E770` and `object_set_current_aid` already invalidate the old
   and new rects. The real gap is `sub_4423E0` (offset-only move, used between
   frames): it read the old rect and never invalidated it; now it does. The
   spam is not missing invalidation: a position from the host's NEW map reaches
   the client while it is still on the old map (log: x=90220 vs limit 192, right
   after "follow apply"). PC_POS, NPC_POS and Packet99 now drop out-of-limit
   locations (`coop_lite_loc_in_map`).
5. **Dense zones** (revised): `COOP_NPC_CACHE` 2048, a miss replaces the oldest
   `sent_ms` among 8 probes; per-tick set is the 81×81 rect around every PC;
   idle unchanged NPCs refresh every 5 s (others 2 s) and keyframes go after the
   changed NPCs; `world_req` lists dead / OF_OFF NPCs only within
   `COOP_WORLD_DEAD_RADIUS` (72) of the client, so the 1024-entry seen set no
   longer overflows (the prune only looks 18 tiles; farther corpses arrive in
   the 100 ms NPC_POS DEAD flag or the next refresh). Deferred: per-slot interest
   lists (needs a per-slot sent cache and an addressed pose send; with the shared
   cache a newcomer's first copy comes from the reset on spawn).
6. **Menu**. The Host / Join buttons were only skipped when launched with co-op
   flags; a plain launch shows them (code read, not seen on screen). A flagged
   launch now shows "Co-op Mode: Hosting on port N" / "Connected to addr:port".

Tests: `coop_lite_policy_test` (fatigue offset, door packet size / role /
validity). Everything else is compiled only.

## PT39 (playtest of PT38, review by Argon; protocol stays 28)

1. **Dialog viewer**: solid dark panel + border, 2/5 width (min 360), 6 text
   lines (see the viewer paragraph above). Hit-test and voting unchanged.
2. **Throw dupe + inventory freeze**. Symptom right, cause half right. Flag 0x04
   is "item to the cursor" (the item leaves the pack, held parentless); the host
   rightly refuses it (`transfer refused (flags 0x4)`), so the item stays in the
   host's pack, but the client still ran `combat_throw`: `object_drop` put the
   item on the ground while it was still in the client's inventory list (and it
   was then destroyed/decayed there): a dangling list entry, NUM != list length
   (`!VS ... doesn't match count`), and the next INVEN_SYNC found it "already
   there" without OF_INVENTORY. Fixes (smallest): clients may not throw
   (`coop_lite_throw_allowed` / `coop_lite_client_throw_denied`: guards in
   `anim_goal_throw_item`, the cursor-throw clicks and the default-throw weapon
   path in `intgame.c`; the item never leaves the pack); `apply_inven_batch`
   re-picks an owned item that lies on the ground (`object_pickup`) and ends by
   forcing NUM == list length per owner; the `obj.c` validator really repairs
   NUM (co-op only; it used to store the same value back); the item-delete loop
   in `item.c` can no longer spin when `item_force_remove` fails. The exact
   freeze site was not reproduced (no GUI): the unbounded candidate is that
   delete loop on a stale NUM. Deferred: host-authoritative throw (host removes
   from stack, resolves the hit, creates the dropped item; needs a throw intent
   packet + projectile replay, protocol bump). Allowing 0x04 was rejected: the
   host would hold a parentless item forever if the client never throws.
3. **.dif**: when a client's bad `.dif` is removed, `sector_lock` now resets the
   half-loaded sector and reloads it from the map in the same call (no failed
   lock). `Couldn't match ObjID to HANDLE` is a benign Note (an object field
   names a PC that is created after `map_open`, census follows); not changed.
   "Don't write .dif on clients" is not done: clients also keep Save\Current for
   their own saves and reloads, and skipping the write changes sector_save_game
   for every sector; risky for little gain (the truncated .dif now self-heals).

Tests: `coop_lite_policy_test` (throw policy; 0x04 still refused). Dialog panel,
item/sector/obj changes are compiled only.

## PT40: Stage 2, skills and economy (protocol 29)

1. **Starting gear (fixed, compiled only).** A coop client skipped nothing in
   the new-game shop, but the shop ran as a coop client: `sub_463E20` only sent
   a request (empty merchant), `inven_ui_open` took the coop path (barter
   request to a host that has no session yet) and every purchase was a
   Packet81 the host refused. Now `mainmenu_ui_shop_refresh` starts a
   pre-session "preshop" (`coop_lite_client_preshop_begin`: local suspension,
   `coop_lite_is_client_role()` false, `sub_463E20` stocks locally); it ends in
   `sub_5412E0` (before CHAR_OFFER) and on shop destroy. The bought kit is in
   the PC's inventory and rides to the host in the v2 character bundle (the
   host skips its own kit when the PC already has items). Pregens have no shop
   (vanilla) and keep the host kit.
2. **In-game merchant trade (reviewed, no change).** The barter path (client
   window, `COOP_BARTER_OPEN`, Packet81 flags 0x01/0x21, host price, gold,
   identify, INVEN_SYNC of PC/merchant/store) was re-read end to end; no defect
   was found without a log. Next playtest: look for `CoopLite: barter:` /
   `inven: ... refused` lines in the host log.
3. **Pick Locks / Disarm Traps (new, compiled only).** `skill_invocation_run`
   on a coop client for its own PC and these two skills sends
   `PACKET_COOP_SKILL_USE` (150, 72 B, client → host; flags must be 0,
   modifier in [-100, 100]) and returns false locally. The host checks the
   slot PC, the target (≤ 4 tiles), the lockpicks (dropped if not the PC's),
   runs the vanilla invocation (lock / trap state, XP through party XP,
   lockpick use), then WORLD_DIRTY, the slot's INVEN_SYNC and a notice
   ("You succeed." / "You failed.", new kind 16).
4. **Schematics (new, compiled only).** The Combine button on a client sends
   `PACKET_COOP_CRAFT` (151, 8 B). The host runs `schematic_ui_process` on the
   client PC: ingredients from its pack, expertise check (now also inside
   `schematic_ui_process`), product created with a host OID, INVEN_SYNC.
5. **Fate points (new).** Dialog action `fp` and script "grant one fate point"
   call `coop_lite_host_party_fate_point`: every other present PC of an active
   slot (and the host's) gets one; PC_STATS carries it to the owners.
6. **Party-skill dialogs (deferred).** Design: allow `coop_lite_can_talk` for
   story NPCs when the host PC is within 18 tiles; the client sends
   DIALOG_STATE kind START (npc OID); the host opens the dialog with the
   client's PC as `pc_obj` (checks use its INT/CHA/Persuasion), publishes it
   through the existing viewer to everyone including the speaker, and the
   speaker's panel gets clickable options sent as kind CHOOSE (line id +
   index, validated like VOTE). Needs the host dialog engine to run for a
   non-local PC (dialog_ui entries are keyed by pc_obj, intgame windows are
   local-only), so it is a round of its own.

Tests: `coop_lite_policy_test` (packet sizes, roles, TYPE_LIMIT 152, skill /
craft validity, fate recipients).

## PT41: Stage 2, party dialogs (protocol 30)

- **Party dialog (E).** A client talking to a story NPC (bark policy) sends DIALOG kind START (header + NPC OID).
  - The host checks the slot, a living PC, and an NPC within 5 tiles of the speaker and 18 tiles of the host PC. It also checks that no other dialog is running and the host is not in combat, with a 1 s rate limit.
  - The host then runs SAP_DIALOG with the client's PC, so checks use that PC's stats.
  - The state goes out through the PT36 viewer to everyone, the host included. STATE `votes[7]` holds the speaking slot.
  - The speaker's panel clicks send CHOOSE (line + index; -1/Esc leaves). The host accepts them only from the speaker, for the current line and a valid index, at most one per 250 ms.
  - Others vote as before. Barter, charedit, map and schematic results close the party dialog.
  - Merchants are still talked to locally. If the speaker disconnects, the dialog ends.
- **Traps.** A WORLD_STATE portal/container entry carries `reserved[0]` = COOP_WORLD_TRAP_ARMED. A client drops a trap the host has disarmed (`trap_remove`). Locks already travel in `kind_flags`.
- **Barter diagnosis.** The `CoopLite: barter:` lines cover the client request (sent / not sent), each failed barter check, sell/buy refusal, low gold, and every completed buy/sell with prices and gold.

## PT42: floor traps (protocol 31)

- **Sync.** WORLD_STATE has a new entry kind, COOP_WORLD_TRAP (5), for OBJ_TYPE_TRAP objects.
  - Each entry carries OF_TRAP_SPOTTED, OF_DONTDRAW and OF_TRAP_PC, plus an armed bit.
  - A spot from the host is shared with clients. It is only ever added, so a client's own spot is never cleared.
  - When the host reports a trap as disarmed or spent, the client destroys its copy. Unlisted traps within the prune radius are destroyed too.
- **Triggering.** Clients never spring floor traps; anim's walk skips them on a coop client. On each accepted client PC_POS, the host walks the line from the old tile to the new one (up to 16 tiles) and springs the first armed trap it finds with the client's PC (SAP_USE). It then refreshes the world, and damage reaches clients through the normal state sync.
- **Disarm.** This uses the existing SKILL_USE (150).
  - The host refuses targets that have no armed trap.
  - For a trap the host has no spot for, the host makes its own Spot Trap roll for that PC first. On success the spot is shared.

## PT43 — party portraits and town-map markers (Stage 3 UX)

- Left party column (follower_ui) appends other players' PCs after NPC followers
  (`coop_lite_party_teammates`: active slots only, valid live PC handles, no
  local PC / duplicates, max 8). Live HP/Fatigue bars; a 300 ms coop tick
  rebuilds on join/leave/death and redraws on HP/fatigue change.
- Left-click on a teammate centers the camera; in spell/skill/item targeting the
  click goes through the normal path (host-authoritative cast). No right-click
  menu for teammates. Bars guard max==0.
- Town Map: teammates on the same town map get a blue/white square, the local
  PC keeps the cross. Protocol unchanged (31).

## PT44 — damage floats, client consumables, town map

- Floating damage: death writes hp damage 32000 (critter_kill / death path),
  which NPC_POS and the vitals watch turned into "-31988"-style hits that
  looked like one-shots. `coop_lite_float_delta()` clamps the float to the
  remaining hp pool (0 when already at/over max).
- Client consumables (potions, food, healing kits): used locally, the effect
  and destroy were undone by host vitals/inventory sync. Now
  `item_use_on_obj` on a client sends `PACKET_COOP_SKILL_USE` with
  `skill = COOP_SKILL_USE_ITEM` (item OID, optional target OID); the host
  checks ownership, uses the item and resyncs the slot inventory. No layout
  change, protocol stays 31.
- Town map: clients may open the map button when their sector has a town
  map; world/continent modes and spell-select remain host-only.

## PT44b — looted corpse items back on revisit, loot crash

- A client revisiting a sector loads the corpse's original items from its
  own map data, with the same OIDs as the items that now live in a PC's
  pack (looted on the host). The load re-pointed the OID table at the copy;
  the host's inventory sync then destroyed the copy ("loot vanished") and
  `obj_deallocate` removed the OID entry outright, leaving the live item
  unresolvable / handles dangling — the next loot crashed.
- `obj_deallocate` now unmaps an OID only if it still names that object
  (`coop_lite_oid_unmap_ok`); a coop client remembers duplicate loads and
  restores the original's mapping when the copy goes.
- The inventory batch closes an open loot window before destroying items of
  its target.

## PT45 — trainers for clients, gifts between players

- Trainers: a client's talk to a non-merchant NPC already runs as the PT41
  host party dialog with the client's PC as speaker, so `dialog_perform_training`
  trains that PC on the host and pays from its gold. New
  `coop_lite_host_pc_changed(pc, true)` then forces the PC build to its owner
  (PC_STATS, COOP_PC_STATS_F_FORCE) and resyncs its inventory. A client's
  local merchant dialog refuses the training menu (it would train a copy).
- Gifts: in the pickpocket window on another living player's PC, putting an
  item into their pack is a gift (`coop_lite_party_gift`): no roll, always
  succeeds — host-side for client requests, and in `skill_invocation_run`
  for the host's own PC; both packs are resynced. Taking still rolls
  (pickpocket). No new packet, protocol stays 31.

## PT46 — loot items the client could not take

- Host log: "takes 'Old Revolver' / 'Wool Jacket' from target: failed" with
  the same OIDs on both sides (host took G_92E2F532… itself later), so not
  an OID mismatch. The client's Packet81 names the inventory cell of its
  own copy of the pack; on the host that cell can be taken (packs drift),
  so `item_transfer_ex` fails. Now the host retries with `item_transfer`
  (its own placement) — `coop_lite_loot_retry_auto_slot`.

## PT47

- Trainer levels: the owner-mode PC_STATS apply now also takes the host's training bits (>= bit 6) of basic/tech skills on every tick, not only the PT45 FORCE send (`coop_lite_skill_merge_training`). Gold comes with the inventory sync.
- Portrait: OBJ_F_CRITTER_PORTRAIT rides in the high 16 bits of PC_STATS `flags` (no layout change; protocol stays 31). Applied to non-local PCs; party UI refreshed. Host includes it in the build signature; clients send once after joining.
- Auto-follow/guard: `F` toggles following the nearest other player PC (moves via normal anim goals, host-authoritative). Within 4 tiles it attacks an NPC whose combat focus is that PC. Any left click, death, map change or >25 tiles cancels.

## PT48

- Client "forgot" inventory items after a rejoin (build 538878cd logs): the parked-PC restore left null/duplicate handles in the PC's inventory list ("Inventory entry is null", "duplicate handle"), while the items still named the PC as parent. INVEN_SYNC saw `item_parent == owner` and counted them as present, so they stayed invisible. Now each batch repairs the owner's list first (drops null/destroyed/foreign/duplicate handles, `coop_lite_inv_compact_handles`) and re-lists any item whose parent is the owner but is missing from the list. The host's periodic resync then restores the client's view.

## PT49

- Party dialog (PT41) no longer needs the NPC near the host PC: the host-distance cap went from 18 to 1000 tiles. The host still needs the NPC object loaded (resolved by OID; clients' surroundings are kept loaded) and it must be within 5 tiles of the speaking client PC, alive, with no other dialog/combat running. Barter/trainers already only check the client PC ↔ NPC range.

## PT50

Protocol 32. New packet 152 `PACKET_COOP_PARTY_CTRL` (16 bytes, `op`-checked, both directions); type limit 153.

- Travel tether / ready check. `teleport_do` of the host PC (area/map transition, stairs, world-map arrival) goes through `coop_lite_host_travel_gate`. If any living client exists, the teleport is held and the host sends `TRAVEL_START` with a bit mask of clients more than 25 tiles away. Each client reports `BUSY` (inventory/barter, its own dialog, or the party-dialog viewer open) or `IDLE`, and sends the change again when it changes. A client counts as ready if it pressed Y or clicked the notice (`READY`), or if it is `IDLE` and within 25 tiles. Clients that are far or busy see a small solid panel in the top-left corner (PT39 colours): "Лидер группы собирается в путь… Y — готов". The host sees "Готовы N/M. Ждём: names… Y — отправиться сразу" (no countdown). There is no timeout: the hold lasts until every living client is ready, or until the host forces it by pressing Y or repeating the travel action. A client that never confirms cannot block the party, because the host can always force the departure. The saved teleport then runs with a bypass, and the normal FOLLOW_HOST teleports everyone. Dead and disconnected clients never hold the party.
- Maintained spells (audit): the cast, maintain ticks, fatigue drain and stat/visual effects run only on the host (`magictech_process` returns early on clients; effects reach clients through the object/stat sync and Packet77 eye candy), so clients never apply effects twice. Packet60 adds the owner's maintain icon, and the END path sends Packet61. Gap fixed: when a client clicked its maintain icon, it ended only the client's local shadow copy while the host kept the spell and kept draining fatigue. Now it sends `COOP_CTRL_SPELL_END` with the host mt_id. The host checks that the spell is the sender PC's own and in MAINTAIN, then calls `magictech_interrupt`, and Packet61 clears the icon.
- Deferred/unverified: Packet60 carries the host's mt_id, and the client builds the icon from its local run-info with that id. If the client's magictech id pool drifts from the host's, the icon may be missing or show the wrong spell; a fix needs the spell id on the wire. NPC-held spells have no icon by design. Not playtested in GUI.

## PT51

No new packet. Protocol stays 32: WORLD_STATE gains flag bit `COOP_WORLD_F_FULL` (0x4), and the layout is unchanged.

- **Full-map catch-up on join, rejoin and map/area entry.** The host answers a slot's first `WORLD_REQ` for a map key (the client sends one 1.5 s after each map load) with a full snapshot of every object the host has loaded on that map, not just the 24-tile radius. That covers doors (open, locked, jammed, broken), containers (flags and contents through INVEN_SYNC), traps, dead and switched-off NPCs, living NPCs, and ground items. It is paged from the host tick at one 64-entry page per slot per 100 ms (`COOP_WORLD_FULL_PAGE_MS`), so ENet is not flooded. Each page is built at send time, so it never carries state older than the live door, loot and death packets already sent on the same ordered channel. Radius replies to that slot are held while a full stream runs. The stream is cancelled on map change or PC change, and a disconnect resets it, so the next join or rejoin gets a full snapshot again. Session reset clears everything.
- **Client prune in full mode.** The client removes ground items, junk piles, traps and NPCs the host did not list, but only in sectors where the host listed at least one object, meaning the host had that sector loaded. Up to 256 sectors are tracked; if there are more, pruning is skipped. The client seen-set grew from 1024 to 4096 OIDs; if it overflows, pruning is also skipped, never guessed.
- **Gaps.**
  - Only sectors loaded on the host are covered; far sectors the host never loaded are still in its .dif files and are not sent.
  - A sector where the host has nothing listable left is not pruned.
  - Containers are capped at 64 per page.
  - A snapshot of everything, not a changed-only delta: "changed vs. pristine" is not tracked.
- **UPnP (optional, `ARCANUM_UPNP` CMake option, ON by default).** miniupnpc 2.2.8 (BSD-3-Clause) is vendored in `third_party/miniupnpc`. It builds as a static library and links ws2_32 and iphlpapi on mingw-w64. `tig_net_start_server` maps UDP `listen_port` (default 7777) on a worker thread: 1.5 s discovery, then AddPortMapping and GetExternalIPAddress. `tig_net_reset_connection` and atexit remove the mapping. The PT33 menu line reads "Co-op Mode: Hosting on port N - UPnP: UDP N open, external IP x.x.x.x", or "failed (…), forward UDP N by hand". The log shows "CoopUPnP: …". Kill switch: `ARCANUM_COOP_NO_UPNP=1`. The menu line is drawn when the menu opens, so a mapping that finishes later shows on the next menu redraw.
- **Deferred:** Steam Networking Sockets / relay, ReForged integration.

## PT52 — pre-release review

- **Full snapshot paging (PT51 fix):** the FULL dump now takes the ObjectID
  list in one pass when it starts and pages over it (64 per page); each oid
  is re-resolved with `obj_pool_perm_lookup` and skipped if gone, destroyed
  or picked up. Creating/destroying objects mid-dump no longer shifts the
  cursor (objects were skipped and then pruned on the client). The last page
  (LAST + FULL, FIRST when it is also the first) is always sent, even empty.
- **Travel gate (PT50 fix):** clients report BUSY/IDLE (inventory, barter,
  dialog) whenever it changes, not only during a check; the host keeps it per
  slot and measures distance itself. Everyone alive within 25 tiles and free
  → travel immediately, no panel. A running check is dropped (TRAVEL_END
  sent) on session reset, save load, host death, host entering combat, or the
  host walking more than 3 tiles from where it started (clicked elsewhere).
- **Item use (PT44 fix):** scrolls and items with a `SAP_USE` script are used
  on the host too; written items and keys stay local. A targeted use whose
  target is gone, not a critter or farther than 4 tiles is refused (inventory
  resynced) instead of being redirected onto the user.
- **Follow guard (PT47 fix):** the guard uses `anim_goal_attack` on host and
  client (client: walk-in through `coop_lite_client_attack_approach`, then the
  intent); `coop_follow_on` is cleared on session reset.
- Protocol unchanged (32).

### PT52b

- **Full snapshot containers:** a FULL page now also ends after 8 containers
  (`COOP_WORLD_FULL_CONTAINERS`), since each sends its own reliable INVEN_SYNC
  batch; the snapshot index continues next tick. INVEN_SYNC between FIRST and
  LAST is fine on the client: contents become OF_INVENTORY, which the prune
  never touches, and the container itself is already in the seen set.
- **UPnP restart:** `coop_upnp_urls`/`data` are zeroed before
  `UPNP_GetValidIGD`; `have_urls` is set only for igd > 0 (freed in stop).
  The ext/err strings are only rewritten while the state is BUSY/OFF and are
  only read by describe for MAPPED/FAILED (set after the write, atomic), so
  there is no read/write overlap.
- Protocol unchanged (32).

### PT52c

- FULL snapshot: the FIRST flag is kept per slot (`coop_world_full_first`,
  set by `coop_world_full_snapshot`) and cleared only when a page is really
  sent, so an empty early tick cannot lose it; an all-empty snapshot still
  ends with one FIRST+LAST+FULL page.
- Travel: the client clears `pressed` and `client_sent` on TRAVEL_END (and
  on session reset), so proactive BUSY/IDLE resumes after pressing Y. Host
  client-disconnect clears that slot's busy flag and frees its snapshot list.
- Protocol unchanged (32).

### PT52d (second review)

- Slot disconnect clears its travel state (-1, never a stale READY), the
  TRAVEL_START mask bit and the whole full-snapshot state.
- A client that joins during a running travel check gets TRAVEL_START
  (host tick, per-slot mask) and is waited for like the others.
- A client that turns BUSY during a check sees the panel ("leader waits").
- `obj_dup_oids`: 1024 entries, free/stale entries reused, overflow logged.
- Full snapshot: allocation failure falls back to the radius reply; the
  list (16384 × 24 B ≈ 384 KB) exists only while a slot's snapshot runs.
- WORLD_REQ for a new map key / PC bypasses the 3 s rate limit.
- `coop_lite_follow_toggle` requires coop mode.
- Kept: UPnP permanent lease (IGDv1 routers often refuse finite leases;
  removed on stop/atexit). String invariant documented in coop_upnp.c.
- Protocol unchanged (32).

## PT52e — third review pass

- NULL checks on net temp allocations (char chunk, inven batch, Packet81 copies, obj blob, world state page); failure logs and resyncs/aborts.
- Removed unused locals in `coop_lite_handle_world_req`.
- Travel: a dead client PC's travel state is reset to -1 (no stale READY after resurrect); `teleport_do` failure is logged.
- Inventory list repair: dynamic buffer (no 512 cap) and `coop_lite_fix_inven_num` after a length change.
- Client world rx: during a full stream, non-FULL non-FIRST pages are ignored.
- Maintained spell end: host replies `SPELL_END` on reject; the client ends its local shadow.
- `F` key is consumed only when follow toggled on/off.
- Known (not fixed): a .dif copy with a live original's OID owns the OID mapping until the copy is destroyed (PT44b); objects created on the host after a full snapshot starts may be pruned on LAST and come back with the next radius snapshot; destroyed non-junk containers/doors are not pruned (vanilla does not destroy them); full snapshot may burst unknown-prototype requests (batched by obj req flush).

## PT52f — review follow-up

- Full snapshot alloc failure already logs on the host and falls back to a radius reply (no empty full snapshot is sent).
- `coop_lite_host_send_inven_batch` skips owners that are NULL, invalid or `OF_DESTROYED` at send time.
- Client: before a `FOLLOW_HOST` teleport, dialog_ui closes inventory/loot/barter/pickpocket, the local dialog, the coop dialog view and the char view (`coop_lite_set_travel_close_hook`).

## PT52g — review follow-up

- PC_STATS build signature (host and client) is never 0 (0 = none yet).
- Inventory list repair drops invalid handles before reading their flags.
- `inven_ui_destroy` clears the dragged item (a loot window closed under a drag).
- Client local-dialog training refusal text is Russian, like the other coop UI strings.
- `dialog_perform_training` checks the PC handle before `coop_lite_host_pc_changed`.
- Notes: portrait is set only at character creation (no change-send needed); PC_STATS flags high 16 bits carry the portrait (no room for more flags there); party talk to an NPC not loaded on the host is refused (code 5), not hung.
- (PT52g, follow-up commit) Travel panel redraws in place when its size is unchanged; `coop_travel_state` is set to -1 on session reset (0 is BUSY). Panel click policy kept: clicks over the panel are swallowed, only left-up means ready/go.
- UPnP: empty control URL / service type is a clean failure; a failed AddPortMapping (stale mapping after a crash, 718) is retried once after DeletePortMapping; `coop_upnp_stop` is called before `tig_exit` in main (atexit stays as an idempotent guard). Stop/join is bounded by the discovery (1.5 s) and miniupnpc HTTP timeouts.

## PT52h — bilingual RU/EN coop strings

- All player-visible Coop Lite strings are shown as "Русский / English": party notices (fall, resurrect, wipe/reload, pickpocket, skill, talk refusal, rest, world map), host-left notice, "X has fallen", travel panel texts (RU line, then EN line), coop dialog hint, training refusal, main menu buttons and co-op mode line, UPnP status.
- Notice buffers grew 160 -> 512, travel texts 200/256 -> 512, menu labels 160 -> 512, UPnP status 128 -> 256. Travel panel is 460 px wide and up to 8 lines; the coop panel wrapper honours '\n' as a line break.
- Debug logs and vanilla game strings are unchanged.

## PT52i (DeepSeek review)

- Travel vote: the `told_mask` is built from slots that are actually connected (it used to be 0xFE).
- WIPE notice: the format string has two `%u`, so the seconds argument is now passed twice (caught by `-Wformat`).
- `coop_world_host_emit`: returns false on allocation failure, so the full tick retries the page instead of treating the snapshot as done.
- `handle_world_req`: the allocation-fail fallback resets `full_first[slot]`.
- `inv_list_repair`: guards against SIZE_MAX overflow before growing the buffer.
- Follow toggle: resolves `pc_obj` first and calls the travel close hook only when `pc_obj` is valid.
- Rejected / OK by audit: radius rate-limit after a full snapshot (full_key/full_pc persist), UPnP stop idempotency, cdv_wrap '\n' handling, close_all recursion (send-only).

## PT52j (deep analysis: static, sanitizers, fuzz, 8 players)

- Tools: cppcheck 2.22, gcc -fanalyzer, gcc -Wall -Wextra -Wformat=2 -Wshadow, ASan+UBSan build (`/workspace/nbuild-asan`, Debug, SDL console build).
- New test `coop_lite_fuzz8` checks three things. It fuzzes every pure packet validator and decoder: WORLD_STATE, INVEN_SYNC, OBJ_BLOB/REQ, STORY chunk and rx, CHAR chunk rx, the char bundle, story decode, DIALOG_STATE and PARTY_CTRL. It checks the party notice texts: RU / EN, valid UTF-8, no truncation, no stray `%`. It checks the 8-player pure logic: roster slots 1..7, 7 dialog votes, travel ready and the 0xFE mask, per-slot pose order, batch limits, OID set overflow.
- `coop_lite_party_notice_text` moved out of the stub ifdef and exported, so tests can reach it.
- `coop_story_decode`: no pointer arithmetic on NULL. This was a UB finding from cppcheck.
- Travel panel: the waiting-names list cuts each name on a UTF-8 boundary, because Cyrillic names were split mid-character. The name buffer is now 128 bytes.
- tig `tig_find_first_file`: frees the empty SDL_GlobDirectory result. LSan flagged the leak.
- 8 players, audit: per-slot arrays are [8], loops `slot < 8`, masks are int (bit 7 OK). Party dialog is one-at-a-time (`coop_party_slot`). WORLD_REQ is rate-limited per slot (3 s). Full snapshot is paged per slot: 7 x 16384 x 24 B = 2.7 MB host RAM and about 7 x 41 KB/s while streaming.

## PT52k — нагрузка и лимиты / load and rate limits

- Полные снимки мира: максимум один обход объектов за тик (`coop_world_full_need_walk`), при ошибке — ответ по радиусу.
- `CoopOidSet`: хеш-сет (FNV-1a, линейное пробирование), ёмкость 16384, прежняя семантика переполнения.
- Лимиты хоста на слот (token bucket `coop_lite_rate_take`): OBJ_REQ 256/128 в с, NPC_REQ 64/32, door/skill/craft 20/10; сброс при отключении, лог отладки при отбросе.
- Подсчёт готовности к переходу вынесен в чистые `coop_lite_travel_tally`/`coop_lite_travel_untold`; обрезка имени UTF-8-безопасна.
- Тесты: `coop_lite_fuzz8` (oid_hash_set 16384, rate_bucket, travel_soak 400k), новый `coop_lite_net8` (7 ENet-клиентов, шторм валидных/битых пакетов, переподключение слотов).
- Нет лимитов пока для ATTACK_INTENT и PC_STATS.

## PT53

Playtest pt53 (client log1 / host log2). Protocol **33** (new packet 153).

1. **Ring pickup / potion re-equipped the old sword (items 1, 7).** The client's own-pack moves
   (equip, grid rearrange, hotkey bar) were local only: the client log has no `own move` line
   at all. Each host resync (ring pickup at host 171.7 `item op: ... (weapon now 'Short Sword')`,
   potion at 207.5) carried the host's stale layout, and worn slots follow the host, so the
   old sword came back. Fix: `PACKET_COOP_INVEN_LAYOUT` (client → host intent). After every
   INVEN_SYNC the client notes the host's layout hash for its PC (order-independent sum of
   oid+loc terms). When its local layout differs, it sends the oid/location of every non-stack
   item at once; the same layout is repeated at most every 3 s, and nothing is sent mid-drag.
   The host checks that the sender owns the PC, that each item is already in that PC's pack,
   and that each location is legal (grid 0..119, wear 1000..1008, hotkey 2000..2009).
   A worn item that `item_check_remove` refuses stays put. The host then removes the moved
   items and re-inserts each one at the requested place if it is legal (wear: free slot and
   wieldable; hotkey: free; grid: room). Otherwise the item goes back to its old place, or to
   any free cell. The host then echoes an INVEN_SYNC.
2. **The host doesn't know the quickbar (item 3b).** Same root cause: hotkey locations now
   reach the host through the layout.
3. **Corpse armour/weapon loot fails (item 2).** The host logs `takes 'Robes' ... failed`, and
   the PT46 auto-place retry fails too, so this is not the client's cell choice. The reason is
   not in the logs. The likely cause is the host's stale copy of the pack (the shovel still
   on the grid there, so no room) or weight. Item 1's fix removes the stale copy. A new
   `loot: slot N why: remove R, insert I, src loc L` line names the `ITEM_CANNOT_*` code next
   time.
4. **The window closes when the host gives an item (item 3a).** On the host, a plant on a
   teammate PC went through `skill_plant_item` (animation), which closes the window every
   time. It is now a direct host `item_transfer`, and the window stays open. On the client,
   PT44b closed the loot/pickpocket window whenever the host removed any of the target's
   items (`loot window closed (host removed its items)` ×4). It now closes only if the
   removed item is the one being dragged; the slot grids are rebuilt by `ui_update_inven`.
5. **Cyrillic garbage (item 4).** All bilingual co-op strings go through `coop_lite_lang()`:
   English by default, and `ARCANUM_COOP_LANG=ru` restores the "RU / EN" text. This covers
   notices, party notices, the travel panel, the vote hint, the training reply and the main
   menu labels.
6. **The travel panel is empty or at the edge (item 5).** Its text was written into
   `cdv_window` (the vote panel, normally closed). The host shows `tig_window_text_write:
   Empty WinID` ×12 at 552.1. Text now goes to the panel's own window, and the panel is
   centred in the iso viewport at any resolution (`coop_lite_center_rect`).
7. **Rat crits / double damage (item 6).** Not confirmable: there are no damage or crit lines
   in either log. Host combat used the host's stale equipment for the client's PC (old
   sword and armour), and the layout fix makes that copy current. Crits are rolled on the
   host only; no double-apply path was found.

## PT53a

Regression fix for the PT53 layout intent (client crash after a host save load and rejoin).

- **Cause:** the host looked items up by OID. After a reload, stale objects share OIDs with the
  real ones and name the PC as parent but are not in its list. `item_force_remove` failed
  ("Couldn't match object in parent!"), then `item_insert` appended the handle anyway, which
  left duplicates in the inventory list (`redraw_inven_fix_bad_inven_obj` ×20). The client
  listed items the host does not have (17 refused), its hash never matched, and it resent the
  layout every 3 s forever. A corrupt echo could then insert an OID twice on the client.
- **Host:** handles now come only from the PC's real list, after `coop_lite_inv_list_repair`.
  The plan is pure: `coop_lite_inven_layout_plan`. It refuses unknown OIDs, an item asked for
  twice, an OID held by two objects, and illegal locations. An item already in place is a
  no-op, so a converged layout reports 0 moved. The list is repaired again after the moves,
  and the host always echoes an INVEN_SYNC. Gold and ammo are never touched (stackables are
  excluded).
- **Client:** the host's echo is the truth. The acked hash is set to the local layout at each
  echo and at each send, so a refused layout is never resent; only a new local move is sent.
  The inventory sync apply skips a duplicate OID entry and repairs every owner's list at the
  end.

## PT53b: client inventory crash / layout churn

- Crash: `redraw_inven_fix_bad_inven_obj` looped forever on a non-item
  (type 0, dangling) handle in the PC's list: `item_parent` failed,
  `item_force_remove` removed nothing, the loop did `i--` again. The loop is
  now bounded and only retries when the list really shrank.
- Opening the inventory in co-op first runs `coop_lite_inv_sanitize`: drops
  invalid / non-item / destroyed / foreign-parent / duplicate handles, fixes
  NUM, moves illegal locations to a free cell. Also run after each obj blob.
- Sync never inserts a handle whose OID maps to a non-item; it asks for the
  blob instead. Key rings are removed from every list before deallocation.
- Layout fallback: grid cells are not synced (hash and plan ignore grid
  order); only wear/hotkey moves apply. A sort sends nothing; after the echo
  the client converges. Each distinct refusal is logged once with OID+reason.
- Log file: identical consecutive lines collapse to "last message repeated N
  times"; above 20 MB only CoopLite/tig_net lines are written (hard stop 40 MB).

## PT53c: static-analysis fixes (no protocol change)

From the gcc -fanalyzer / cppcheck / clang-tidy review of PT53b (protocol unchanged, no wire format change):

- Client inventory batch: a key ring that is re-fetched (freed) is cleared from the doomed list first, so the doomed loop never touches a freed handle; a ring listed by two owners is queued (and freed) only once (`coop_lite_handle_list_contains/clear`).
- The doomed list grows on demand (was a silent 256 cap).
- The dragged-item guard closes the inventory window whatever pane the doomed dragged item came from (was the loot target only).
- ObjectID comparisons in coop code use `coop_lite_oid_raw_equal` (by type/payload) instead of `memcmp` over padding and union tails (PC_POS ownership, follower table, inventory listing, rejoin slot, finish sync).
- `tig_debug_printf`: an overflowing partial line no longer reads past the 1024-byte buffer or stalls all later logging; the line buffer and the log-file repeat collapse are guarded by a spinlock (net thread).
- `dialog_ui_option_label` checks the option index before indexing `options[5]`.
- Character bundle build frees its buffer when the header write fails; obj-blob ground defer checks the pending entry.

Tests: `coop_lite_policy` (PT53c helpers), new `tig_debug_long_line` (fails under ASan on PT53b).

## PT53d: receive-path fuzz harness (no protocol change)

- New `tests/coop_lite_rx_fuzz_test.c` (ctest `coop_lite_rx_fuzz`, 3 s;
  `COOP_RX_FUZZ_SECONDS` for longer runs). Host phase: a raw ENet peer
  completes the tig_net handshake and sends random typed coop packets
  (truncated / exact / padded / oversized) through `tig_net_dispatch_app`
  and the validator to the stub-available host handlers. Client phase:
  handlers called on exact-size heap copies so ASan catches any over-read.
- Game-only handlers (obj_blob, char_chunk, dialog_state, story_sync,
  obj_req, npc_req, party_state, followers, npc_pos) are not linked in the
  stub build; their batch validators stay covered by `coop_lite_fuzz8`.
- A 180 s ASan/UBSan run (2.78M packets sent, 399k delivered, 3.6M client
  iterations) was clean. No code changes to the game were required.

## PT53e: host-authoritative doors, coop portraits (protocol 34)

**Doors — root causes.**
1. Client → host: the client's AG_ATTEMPT_OPEN_DOOR sent `DOOR_USE` and then
   kept animating the door open locally. The host only ran the door's
   SAP_USE script — vanilla opens the door in the anim goal *after* the
   script, so the host door never moved (log: `slot 1 used a door (open now
   0)`) and the next WORLD_STATE reverted the client's local open
   ("changed portals 2").
2. Closing on a client (AG_ATTEMPT_CLOSE_DOOR) was never sent at all.
3. Host → client: door changes only raised WORLD_DIRTY (400 ms debounce +
   3.5 s client delay + 3 s host per-slot limit) and the radius reply only
   lists doors within 24 tiles of the client, so a door was late or never
   shown.

**Fix.**
- Client: open *and* close of the own PC's door goal end locally and queue
  `DOOR_USE` (anim.c `sub_428A10` portal case returns false; `sub_428620`
  queues the close). No local prediction.
- Host `coop_lite_handle_door_use`: open door → close (unless magically
  held); closed → `critter_can_open_portals` + `ai_attempt_open_portal`
  (lock / key / jam) → SAP_USE script → `portal_toggle`. Refusals log why.
- New `PACKET_COOP_DOOR_STATE` 154 (host → all clients, 0x50 bytes: map,
  slot, one `CoopWorldEntry`): `portal_open`/`portal_close` frame steps,
  `portal_bust`, `object_locked_set` on a portal and the pick-lock intent
  call `coop_lite_host_portal_touch`; the host flushes the pending set
  (by OID, max 32) every 100 ms to everyone, any distance. The client
  applies it through the WORLD_STATE portal apply (art frame, block/see
  flags, portal flags incl. lock/jam/bust, trap). Late join / reconnect
  keep using the full-map WORLD_STATE snapshot, which already carries
  every door.
- Logs (once per state change per door):
  `CoopLite: door: <oid> open=1 locked=0 jammed=0 busted=0 from slot N (host sent)`
  on the host, `(... client applied)` on clients; refusals:
  `door: <oid> open from slot N refused: locked or jammed`, `... stopped by its script`,
  `... refused: N tiles away`, `close ... refused: held open`.

**Portrait — root cause.** A multiplayer PC has `OPCF_USE_ALT_DATA`, so
`portrait_draw_func` draws `Players\<oid>.bmp`, a file that only exists on
the machine that created the PC (host and client run from separate
folders). The synced portrait number (PT47, PC_STATS high bits) was
applied but never used for drawing, so other players' PCs drew blank.
Now in coop an engine portrait (id >= 1000) is drawn from its number, and a
missing alt file falls back to the number too (all 8 slots, both ways —
PC_STATS already carries every slot's portrait on join/rejoin/change).
Log: `CoopLite: portrait: slot N id P (have Q)` once per slot per new id.

Tests: `coop_dialog_test` `test_pt53e` (packet id/role/size, validator,
pending-set dedupe/cap, portrait choice).

## PT54: regression fixes after PT53e (protocol 35)

Triggered by a play log where inventory, loot display, doors and the host's
alive state broke after a host save load.

- **Host alive/dead + HP.** The vitals tick only covered client slots 1-7,
  so the host PC's HP/dead flag was never sent: after the host died and
  loaded a save, clients kept seeing a corpse. The tick now covers slot 0
  too and is forced for 3 rounds (full resend) after a host save load /
  rejoin. Log: `CoopLite: host PC vitals sent (hp damage N, alive|dead)`.
- **OID compares by value.** Every remaining raw `memcmp` of 24-byte OIDs
  now uses `coop_lite_oid_raw_equal` (type + payload, padding ignored); the
  layout hash hashes `coop_lite_oid_canon`. Host log showed all 9 layout
  entries refused ("item not (uniquely) in host list").
- **Backpack grid order is synced.** `PacketCoopInvenLayout` carries a
  `seq`; grid->grid is a move. Host applies the wanted cell, or the
  nearest valid free cell (`coop_lite_grid_cell_dist`), then echoes the
  seq in the high 16 bits of `PacketCoopInvenSync.flags`. The client holds
  its own layout until the echo (5 s timeout, logged once), then takes the
  host copy without resending -> no loops. Client inventory batch now also
  moves its own items to the host's grid cell.
- **Rejoin never finishing.** If the parked-PC blob arrived before the
  client world was ready it was dropped and `rejoin_pc_wanted` stayed true,
  silently blocking door/pickup requests. The client now re-asks every 3 s
  (4 tries), then keeps its PC and asks for a pack resync. Logs:
  `rejoin: parked PC not here yet - asking again (N)`,
  `rejoin: no parked PC blob after 4 tries - keeping ours, pack resync`.
- **Floating item icon in the inventory window.** A drag whose item was
  replaced by a sync (e.g. re-totalled arrows) left its art on the cursor.
  The drag is dropped and the cursor refreshed (`inven_ui_drag_cancel_if_stale`,
  also on window destroy). Log: `inven: held item was replaced by a sync - drag dropped`.
- Owner missing for a batch: `inven sync: owner X not here - its N items skipped` (once).

Tests: `coop_dialog_test` `test_pt54` (padding-insensitive OID equality and
layout plan, grid distance, echo seq incl. wrap, validator accepting echo
bits), updated `test_pt53a_plan`/`test_pt53b` for grid sync.

## PT54a: no quit when far apart, main menu text, portrait/body art (protocol 36)

- **"Too far apart" kick (session log, client).** Not a distance check: when the
  host ran off, the client loaded new sectors and `obj_validate_system` found
  an NPC inventory entry whose item handle a co-op sync had destroyed. The
  vanilla answer is `tig_message_post_quit` -> quit prompt; the client froze in
  it (`max gap 106430 ms`), the host timed it out (`disconnect slot 1`), and a
  later load crashed. In co-op the dead entry is now dropped
  (`CoopLite: validate: dead inventory entry dropped (owner type T, N left)`)
  and validate failures never quit (`CoopLite: validate failed in X - co-op
  keeps running (no quit)`, once). Same for map_open / multiplayer map open.
- **Main menu text.** The co-op status line (`Кооп: подключение / Co-op: joining
  host:port`, PT52h) was never routed through the PT53 language pick and was
  wider than the menu window, so `tig_window_text_write` refused the rect
  ("text_write2 failed"). Lines are now language-picked and trimmed to fit
  (`MainMenu: co-op line "..." (W px, window N)`).
- **Portraits / body art.** PC_STATS sent at join reached the client before its
  world was ready and was only resent on a build change (log: client applied
  slot 0's portrait 5 minutes in). Host now resends every slot 4x every 3 s
  after join / rejoin / load; the client resends its own 4x too. PC_STATS now
  carries `base_aid` (OBJ_F_AID) both ways. Logs: `portrait: slot N id P
  applied`, `PC stats: slot N base art -> XXXXXXXX applied`.

## PT54b: gold / ammo stacks keep their grid cell (no protocol change)

Cause: the PT53/PT54 layout path skipped stackables everywhere - the client
left gold/ammo out of its layout hash and packet, and the host left them out
of its list (`coop_lite_is_stackable` filters). A client's stacks are also
local objects with their own OIDs (the host's travel as totals + head OIDs).
Fix: the client lists its stacks under the host's stack OID
(`coop_lite_client_map_stack_oid`), and the host places them like any item.
The host's echo still carries stacks as totals only, so the client's cell for
a stack stays (no revert, no loop: the layout is resent only when the local
hash changes). Other players' stacks seen on a client are not placed.
Logs: `inven: layout sent (N items)` now counts stacks; `inven: slot N layout
applied (M moved, R refused)` on the host; a stack refused before the host's
stack OID is known logs `layout refused ... item not (uniquely) in host list`.

### PT54c — client stuck after a stair/door transition (Tarant ring-masters dungeon)

Logs (client time): `client follow apply map=3 ...` is followed by
`Inventory array count does not equal associated num field on teleport. Array: 19, Field: 18`.
Vanilla `schedule_teleport_obj_recursively` refuses a critter whose inventory NUM
differs from the list length, so the client's PC was never carried to the new map:
the map loads, but the client is "not in world" (every later follow is
`queued follow ... (client not in world yet)`, the resync is skipped, it sends
nothing) and the host times the peer out (`tig_net: disconnect slot 1`) and parks its PC.

Fix:
- the client repairs its PC's inventory list (`coop_lite_inv_list_repair`) before the follow teleport;
- `teleport.c` calls `coop_lite_teleport_inv_fix` in co-op and repairs instead of refusing;
- `coop_lite_follow_loc_for_slot` no longer falls back to the host's own tile
  (spiral starts at radius 1), and after the carry the client moves its own PC off the
  host tile or a blocked tile to a free neighbour (the destination had been chosen on the old map).

Log lines to check: `PT54c teleport: inventory NUM/list mismatch repaired`,
`PT54c own PC moved off`, `resync skipped - no local PC`.
No wire change; protocol stays 36. Verified by unit tests and a build only; not yet tested in play.

## PT54d: inventory list drift — root cause

Object array fields are **sparse keyed arrays** (`sa.c`):
`obj_arrayfield_length_get()` returns the number of *present keys*, and
`obj_arrayfield_length_set(obj, fld, k)` only **removes key k** (vanilla calls
it only with the last index after shifting). Coop code used it as "truncate to
k", which left keys k+1.. alive:

- `coop_lite_inv_list_repair` / `coop_lite_inv_list_drop`: after compaction,
  stale duplicate keys survived whenever 2+ entries were dropped; with holes
  the repair read keys 0..count-1, saw a null and never saw the last key, so
  every sync logged "list repaired (1 bad entries dropped)" + "NUM x != list
  length y" + `!VS Inventory entry is null` again.
- rejoin empty-inventory reset: `length_set(pc, LIST, 0)` removed only key 0
  (the parked PC kept 16+ dangling handles).
- reputation truncation in the story sync (same misuse).
- orphan re-list appended at index `length_get`, overwriting a live key in a
  holey list.

Fix: `coop_lite_arr_read_keys` scans keys 0..count+256, `coop_lite_arr_rewrite`
removes every scanned key and writes the list densely, NUM is set to the result.
Self-check `coop_lite_inv_selfcheck` (pure core `coop_lite_inv_keys_check`,
unit-tested) runs after inven sync apply, list drop, orphan re-list, layout
apply and world-state prune; it logs once per mutation:
`CoopLite: PT54d inven self-check FAILED after '<name>': <reason> (NUM n, count c)`.
No wire change (protocol 36).

## PT55: 8-slot stress / edge cases, release polish (protocol stays 36)

New ctest `coop_lite_stress8` (`tests/coop_lite_stress8_test.c`): one process,
a tig_net host and up to 9 raw-ENet clients on loopback. The host log is
captured, so refusals are checked by their log lines.

| # | Scenario | Result |
|---|----------|--------|
| S1 | 7 clients connect in the same frame, all named; a 9th is refused (`no free slots`), slots unchanged | pass |
| S5 | client with protocol 35 (`refusing slot N: protocol 35 != ours 36`) and an old build's short NAME (`refusing old client`) are refused; the slot is freed and reused | pass |
| S6 | 16384-byte app packets both ways; 16385+ dropped; a 46-char name is cut to 31 | pass |
| S2 | 64 x 16 KB reliable data queued to all, 3 clients hard-reset mid-stream, timeout detection, rejoin into freed slots | pass |
| S7 | one client stops servicing ENet while the host broadcasts poses and data: ENet times it out (about 6 s) and the slot is freed; RSS stays flat (non-ASan 3964 kB) | pass |
| S8 | 20000-tick soak with pose storms, broadcasts and churn: RSS flat outside ASan; LSan clean at exit | pass |
| S3 | ready-check tally with 7 slots (busy, near idle, far idle, dead READY, far READY, no reply, disconnect mid-check, revive) | pass |
| S4 | pose seq and epoch wrap (70000 bumps), layout echo seq wrap 65535 -> 1, rate bucket and interval across the 2^32 ms wrap, the same door used twice in one tick, door set full | pass |

`coop_lite_rx_fuzz` now fuzzes with `COOP_RX_FUZZ_CLIENTS` rogues (default 7).
A 10-minute ASan run with 7 rogues was OOM-killed at 14 GB. The trace
(`COOP_RX_FUZZ_TRACE=1`) showed the growth was in the rogues' own unsent
ENet queues (harness), while host RSS stayed at 10 MB. The harness now
keeps each rogue's backlog under 512 commands.

Not reachable natively (they need the game object layer): two clients
looting the same item in one tick, give-vs-host-move, max-size real
inventories and world snapshots, door queues, and per-slot coop state on
disconnect (parked PCs, travel state, layout seq).

Polish:
- **Main menu line.** The UPnP part (`UPnP: UDP 7777 открыт / open, ...`)
  was embedded in the "RU / EN" co-op line, so the language pick split at
  the first " / " and English users still saw Cyrillic. `tig_net_describe_coop_mode`
  and `coop_upnp_format` now build English text unless `ARCANUM_COOP_LANG=ru`,
  in which case the line stays bilingual. Tested in stress8 (ASCII only, < 64 chars).
- **Log rate limit.** `tig_net_log_budget()`: the first 20, then every 1000th,
  with a running total. Used for `validation failed`, `dropping
  oversized/undersized` and the CHAR_OFFER slot/proto complaints. A bad
  peer wrote 400k lines in 6 minutes.
- -Wall -Wextra is clean on the coop files; there are no TODO/FIXME left.
- README-RU.txt (release staging, not in the repo) was still at PT28 and
  protocol 25. An updated copy is in `/workspace/release/README-RU.txt`.

## PT56 — antidotes, healing others, breakable containers, world-map right-click

- **Antidotes (client):** the host cures poison, but `STAT_POISON_LEVEL` was
  not one of the host-authoritative stats in the owner PC_STATS apply. The
  client's copy stayed poisoned, so it looked like the antidote did nothing.
  The poison level now comes from the host (`CoopLite: PC stats: poison A -> B (host)`).
- **Item cursor → host:** when an item needs a target (antidote on self,
  healing kit on another NPC or PC), the item cursor queued
  `anim_goal_use_item_on_obj`. That goal never runs for a client PC and is
  never forwarded. `item_ui` now sends the use intent to the host
  (`CoopLite: item use on target (item cursor) sent to host`). The host
  accepts a critter target within `COOP_USE_ITEM_TARGET_RANGE` (10 tiles,
  was 4) and logs `slot N used an item (host) on another critter`, or
  `item use: bad target (unknown here|not a critter|too far)`.
- **Breaking containers:** the host already resolved the client's attack.
  On break, `object_bust` calls `object_destroy` on the container. The
  client's world-state prune, however, skipped every container except junk
  piles, so the broken crate stayed on the client at negative HP. The prune
  now removes any container the host no longer lists
  (`CoopLite: world: container gone on the host (broken/destroyed) - removed here`).
- **World map right-click (host crash):** there was no stack trace. The log
  ends about 0.5 s after `host opened the world map`. Hardening: the
  selected waypoint is clamped to the route length on right-click and in
  `sub_564940`, and the event is ignored when the map is not created. In
  network games, diagnostics now log `WMapUI: right-click: …`,
  `Intgame: right-click in world map mode …`,
  `WMapUI: travel stopped … teleport` and `… teleport done`. The root cause
  is not confirmed yet.
- No wire change; protocol stays 36.

## PT56a: crash logs + world-map stop review

**Crash handler** (`src/crash_handler.c`, Windows only, installed at the top of `main`):
`SetUnhandledExceptionFilter` (chains to any previous filter) plus a `SIGABRT` handler.
On a crash it writes, to the game log (flushed, lock-free) and to `crash-YYYYmmdd-HHMMSS.txt`
next to the exe: exception code/address, `exe+0xOFFSET` per address, exe base, access-violation
read/write address, thread id, coop active/host/slot, the last `CoopLite` log line, and an
x64 stack walk (`RtlVirtualUnwind`, no PDB needed). A minidump `crash-YYYYmmdd-HHMMSS.dmp`
(MiniDumpNormal, dbghelp loaded at runtime) is written next to it.
Resolve offsets: `nm address = 0x140000000 + offset`, look it up in
`/workspace/release/syms-<sha>/nm.txt`. Files to send: the game log, `crash-*.txt`, `crash-*.dmp`.
Test: `ARCANUM_CRASH_TEST=1` (access violation) / `=2` (abort). The old PT31 `crash_log.c`
(non-chaining filter installed later) was removed.

**World-map stop review:** `wmap_ui_teleport` calls `sector_flush(0)` before the teleport.
`sector_flush` (upstream code) moved `sector_cache_size - 1` indexes instead of
`size - index - 1`, writing `index` ints past the end of the cache index array once the cache is
full — which co-op makes likely (sectors locked for every client PC). Heap corruption on a
world-map right-click stop: fixed. The follow path itself (carry list, `after_party_carry`,
FOLLOW_HOST) only touches existing PCs and locks sectors via `sector_lock`; no NULL issues found.
Known (not changed): if the ready check defers the stop teleport, the host stands at the old
spot until the check finishes or the stop is repeated (repeat forces travel). Protocol unchanged (36).

## PT57: inventory requests, stack layout, host loss, reconnect, travel requests (protocol 37)

- **Editing another player's pack.** The client now records the host layout of every other party PC it receives in INVEN_SYNC. When its local layout of that pack differs, it sends INVEN_LAYOUT with that PC's oid (seq 0). The host only accepts this if the target is a live party PC that the sender has targeted or is within 12 tiles of. It applies the layout host-side and resyncs the owner, the sender and every viewer. Before PT57 the host accepted layouts only for the sender's own PC.
- **Gold, ammo and arrow grid cells.** The host now lists critter stackables as E_STACK entries carrying their cell. The client maps each one to its local stack object by prototype (gold field / OBJ_F_AMMO_TYPE field) and moves it to the host's cell, or the nearest free cell. Totals still ride in the per-owner header. The client's own stacks are left alone while its layout hold is active.
- **Host loss.** The client peer timeout is now 4–10 s (it was ENet's ~30 s). `coop_lite_client_host_left` also fires outside an active session. After 2.5 s the client goes back to the main menu with "Lost connection to the host… press R there to reconnect."
- **Reconnect.** In the main menu with coop and join configured, "R - Reconnect" is shown. Pressing R starts the pregen/rejoin flow to the last ARCANUM_COOP_JOIN address, port and token, and the rejoin token restores the parked PC.
- **Travel requests.** On the client, `teleport_do` for the local PC to a different valid map is intercepted. It sends PACKET_COOP_TRAVEL_REQ (155) ASK instead, rate-limited to one every 3 s. The host sees "<name> wants to travel to <map>. Y = go, N = refuse." Y sends ACCEPTED and runs the host's own teleport through the PT50 ready check, so the whole party moves. N sends REFUSED, nobody moves, and the client sees a message. Teleports that coop itself applies are allowed through by an auth counter. Same-map teleports are not intercepted. World-map walking by clients stays blocked; since PT77z a client may open the world map (when the host is not already travelling) and propose a destination as a Y/N travel request.

## PT58: launcher + transport abstraction + Steam transport

**Launcher (`arcanum-coop-launcher.exe`, tools/launcher/launcher.c, Win32, Windows builds only).**
- Fields: player name, Host / Join, transport (Direct+UPnP or Steam), the port (host) or host IP:port (join, default 7777), the host's SteamID64 (Steam join), and Windowed.
- Shows the local IP and the public IP. The public IP comes from the router via UPnP (miniupnpc) and stays blank without it. **Copy** puts `ip:port` on the clipboard.
- **Start** runs `arcanum-ce.exe [-window] -coop -host <port> | -join <ip:port|SteamID64> -log coop_host.log|coop_client.log` from `coop-host\` / `coop-client\` (Coop-Setup layout). It refreshes the instance exe from a different root build, as Start-*.bat does. If those folders don't exist it uses the exe next to the launcher. (PT68: the root exe is now preferred; see below.)
- Environment it sets: `ARCANUM_COOP_NAME` (replaces the default "Player"), `ARCANUM_COOP_TRANSPORT=direct|steam`, `ARCANUM_COOP_STEAM_HOST=<SteamID64>` (Steam join).
- Settings are saved in `arcanum-coop-launcher.ini` next to it.
- Joining a host that is already playing uses the normal late-join flow.

**Transport abstraction (src/net/tig_net_transport.h).**
- tig_net keeps all co-op logic: slots, HELLO/WELCOME, the protocol check, dispatch, stats.
- A backend only does listen/connect/service (connect, receive, disconnect events)/send (channel, reliable flag)/flush/disconnect/timeout.
- Backends:
  - `tig_net_enet.c`: the previous ENet calls, moved unchanged. It is the default.
  - `tig_net_steam.c`.
  - A loopback fake in `tests/tig_net_transport_test.c`.
- Selection: `ARCANUM_COOP_TRANSPORT` (unset / `direct` / `enet` → ENet, `steam` → Steam).
- UPnP runs only with ENet.

**Steam backend.**
- Uses ISteamNetworkingMessages: P2P through Valve's SDR relays, with NAT traversal handled by Steam.
- `steam_api64.dll` is loaded with LoadLibrary at runtime. The game never links it, so GOG and non-Steam builds behave exactly as before.
- Only flat C exports are used, and their declarations live in `src/net/tig_net_steam.h`. No Steamworks SDK files are in the repo, and static asserts pin the struct layouts.
- Connection model: a small control protocol on channel 7. The client sends CONNECT (resent every 500 ms, up to 20 s); the host answers ACCEPT or FULL. BYE ends a session, PING keeps it alive once a second, and a peer silent for 10 s is dropped. App channels 0..2 map one-to-one, and reliable messages keep their order per channel.
- The host's main-menu status line shows `Co-op: Steam host, ID <SteamID64>`. The client joins with that ID.
- AppID: `SteamAppId` defaults to **500810** (Arcanum on Steam). Override it with `ARCANUM_COOP_STEAM_APPID`, e.g. 480 (Spacewar, Valve's test app) if Steam refuses the init. The launcher also writes `steam_appid.txt`.
- Without the DLL, without a running Steam, or with a too-old DLL, the log says `tig_net: Steam transport unavailable: <reason>` and starting co-op fails cleanly.
- **What the user provides:** a 64-bit `steam_api64.dll` from Steamworks SDK 1.51 or newer, or from any recent 64-bit Steam game's folder, placed next to `arcanum-ce.exe` in every instance folder. Steam must be running and logged in on both PCs, and the account needs the AppID (owning Arcanum on Steam, or use 480).

## PT59: launcher port check

The launcher only (no in-game UI) answers "can friends reach my UDP port?" without any server of ours.
It runs on a worker thread, triggered by **Check port**, by selecting **Host**, and after **Start** as host.
It never blocks the start.

Checks:
1. **Local bind** of UDP <port>: free / in use (game already hosting).
2. **STUN** (stun.l.google.com:19302, stun1, stun.cloudflare.com:3478) from the bound port: public IP and mapped port; reports whether the NAT preserved the port.
3. **UPnP**: existing mapping of UDP <port> (and whether it points at this PC). If there is none and the port is free, a test mapping is added and deleted again; refusal is reported.
4. **CGNAT**: the router WAN IP is private or 100.64/10, or differs from the STUN IP.
5. **Firewall**: `netsh advfirewall firewall show rule name="Arcanum Coop UDP <port>"` (exit code, so it is language-independent). **Add firewall rule** runs netsh elevated (`runas`) only on click.

After Start the check does not bind the port or add mappings (the game owns them).

Verdicts:
- `OPEN (likely)`: UPnP maps the port to this PC and there is no CGNAT.
- `CLOSED`: CGNAT detected; use the Steam transport or ask the provider for a public IP.
- `CLOSED (likely)`: the port is not mapped; enable UPnP, or forward UDP <port> to <local IP> and allow it in the firewall.
- `UNKNOWN`: neither UPnP nor STUN answered.

Limits: a true inbound UDP test needs an outside peer; only a friend joining proves it. A manual forward cannot be seen (hence "likely"). Third-party firewalls and other Windows rules are not detected.

## PT60: broken doors reach the clients (protocol 38)

**Cause.** A bashed door goes through `object_bust` → `portal_bust`.
- A window gets `OPF_BUSTED` and the busted art, which PT53e already synced.
- A real door is `object_destroy`ed. That path never called `coop_lite_host_portal_touch`. Even when the door was queued, the PT53e flush skipped `OF_DESTROYED` portals.
- The world-state snapshot does not list destroyed objects, and the client prune (PT26/PT31/PT42/PT56) only removed items, traps, containers and NPCs. So the door stayed whole on every client, live and on entry, rejoin and late join.
- Door HP was not sent either.

**Fix.**
- `object_destroy` and `object_hp_damage_set` touch portals, host side.
- The door flush sends a destroyed (or already freed) door as `DOOR_STATE` with `e.reserved[1] = COOP_WORLD_PORTAL_GONE`. The client `object_destroy`s it and plays the destroy sound.
- Portal entries (live and snapshot) carry `hp_damage`, and the client applies it.
- The world prune now also removes portals in scope that the host no longer lists. This covers location entry, rejoin and late join.
- A client's attack or spell on a door was already resolved by the host (same path as the PT56 containers), so the host's result now reaches everybody.

Logs:
- host: `CoopLite: door: <oid> destroyed on the host - sent to clients (slot N)`
- client, live: `CoopLite: door: <oid> destroyed on the host - removed here (slot N)`
- client, snapshot: `CoopLite: world: door gone on the host (bashed/destroyed) - removed here`
- windows: `CoopLite: door: ... busted=1 ...` (host sent / client applied)

Wire: new `COOP_WORLD_PORTAL_GONE` flag and portal `hp_damage`; protocol 37 → 38.

## PT60b: broken / destroyed scenery and other objects (protocol 39)

Reported case: an engine/machine broken by damage (the Silent Hills temple engine, Bates' engine in Tarant) stayed intact on the client.

**Cause.** Only doors (PT53e/PT60), containers (PT56 prune), NPCs, traps and ground items were replicated. `OBJ_TYPE_SCENERY` was in neither the live door flush nor the world snapshot. Its `object_bust` result was never sent: busted art, `OSCF_BUSTED`, `OF_INVULNERABLE`, HP, or `object_destroy` (second bust, or a script destroy). The same was true for a respawnable scenery's `OF_OFF`.

**Fix.** The PT53e door queue is now an object-state queue (`coop_lite_host_object_touch`), host side.
- **Hooks:** `object_destroy` (any cause: damage, spell, script) for portals, scenery and containers; `object_hp_damage_set` for portals and scenery; `object_flags_set/unset` for scenery when OFF / INVULNERABLE / passability bits change.
- **Queue:** objects with a null OID (temporary objects) or in an inventory are ignored.
- **Live:** `DOOR_STATE` now carries `COOP_WORLD_SCENERY` entries (art, flags, `OBJ_F_SCENERY_FLAGS`, HP). A destroyed portal, scenery or container is sent with `COOP_WORLD_OBJ_GONE` and its kind; the client destroys it.
- **Snapshot (entry / rejoin / late join, radius and full map):** lists breakable scenery. That is scenery that is not invulnerable, plus anything the host busted, damaged or turned off. The client applies it. The prune removes in-scope scenery that is still breakable locally and that the host no longer lists.
- **Excluded:**
  - items: unchanged ground/inventory paths, no double removal;
  - critters/PCs;
  - walls: there are too many to list;
  - projectiles, effects and animation objects: no portal, scenery or container type.
- **Client attacks:** they already go to the host through the generic `ATTACK_INTENT` (no target type filter), so a client breaking scenery is resolved by the host and reaches all players.

**Not covered:**
- A script that only changes scenery art via `object_set_current_aid` without bust, flags, HP or destroy. Hooking art changes would fire on every animation frame.
- Scenery that is invulnerable on the client but destroyed on the host is only removed live, not by the entry prune.

Logs:
- host: `CoopLite: object: <oid> (door|scenery|container) destroyed on the host - sent to clients (slot N)`
- host: `CoopLite: object: scenery <oid> busted=1 off=0 hp dmg N - sent to clients`
- client: `CoopLite: object: <oid> (scenery) destroyed on the host - removed here (slot N)`
- client: `CoopLite: object: scenery busted on the host - applied here`
- client: `CoopLite: world: scenery gone on the host (broken/destroyed) - removed here`

Wire: new world kind 6 (`COOP_WORLD_SCENERY`); `DOOR_STATE` accepts scenery, and containers when gone. Protocol 38 → 39.

## PT61 (protocol 40): host-side saves/loads for clients, trap visuals, script wall sync, resync reset

- **Client save (F7):** the client sends `PACKET_COOP_SAVE_REQ` (156) and the host saves into its own slot range `Slot9700`–`Slot9789`, description `coop-<nickname>`. It reuses the player's slot if there is one, otherwise it takes the first free slot. Host slots are never overwritten. Rate limits: 3 s on the client, 10 s per slot on the host. The client is told "Saved on the host as …". `gamelib_last_save_name` skips this range.
- **Client load (F8, load menu):** the request is sent to the host, which asks "<name> asks to load <slot>. Y = load, N = refuse". On Y the host loads the requester's coop slot (or its quicksave), and every client then resyncs. A client cannot load locally (F8, in-game menu or main menu) while it is in a co-op session.
- **Map lock:** the client dim/lock is now applied only for the global world map. The town/location map is free.
- **Trap visuals:** `PACKET_COOP_TRAP_FX` (157) is sent by the host to clients when a trap fires, whoever set it off. It carries the trigger, victim and spotted-fx removal eye candy. Clients play the visuals only and apply no damage.
- **Script-driven wall sync:** this covers `COOP_WORLD_WALL` = 7 and only while `script_is_running()`. A wall destroyed by a script, or a script change to OFF/DONTDRAW/see/shoot/no-block or to its art, is queued on the host. It goes out with the 100 ms DOOR_STATE flush. The host keeps a list of changed walls per map (max 256, cleared on `gamelib_load`). On a world request it sends a slot only that map's changed walls, never all walls. The "gone" state is recorded at destroy time, so a failed lookup is never treated as destroyed. Walls are never pruned on clients, and this path never changes walls on the host.
- **"Host lost walls" under Tarant (d31eb4f7):** this was not a wall bug. The host log shows `slot 1 attacks 'Heavy Wood Door'` followed 1 s later by `target gone/dead` (6574 s, 6635 s). The full-map portal count on that trap map drops 4→3→2. The client bashed two doors and the host destroyed them as vanilla does (`portal_bust` → `object_destroy`), which leaves door-shaped dark openings. d31eb4f7 did not sync door destruction, so the client still showed the doors. PT60 (85395727) syncs it.
- **Client crash `magictech_id_new_lock: ran out of id's`:** the rejoin after a host reload never ran map_reset's magictech clear, so the 512 spell locks leaked with each reload. `coop_lite_client_resync` now interrupts all anims and frees every magictech lock, including its time events, as a real load does. In co-op, running out of ids is no longer fatal: stale locks (or all of them) are freed and logged, and the allocation is retried.

## PT62: launcher crash handling, host auto-restart, clients wait for the host

- **Process watch:** the launcher stays open and keeps the game's process handle. A watcher thread waits on it and posts the exit code. A run counts as a crash if the exit code is non-zero or a `crash-*.txt` was written in the game folder after the start. A normal exit only says "Game closed.". Start is disabled while the game runs.
- **Failure panel:** "Game closed unexpectedly (exit 0x…)" with the buttons:
  - **Reconnect:** relaunch with the same settings. A client rejoins through the normal late join and rejoin token. A host resumes from its last save.
  - **Create bug report:** writes `bugreport-YYYYMMDD-HHMMSS.zip` next to the launcher and opens Explorer on it. The zip holds `info.txt` (build sha, protocol, role, exit code, time), `coop_host.log` and `coop_client.log` from the game folder, the 2 newest `crash-*.txt` and `crash-*.dmp`, and `arcanum-coop-launcher.ini` (not redacted; it holds the nick and the host address/SteamID). The zip is written by a small built-in "stored" (uncompressed) zip writer with crc32, so there are no dependencies. Files over 512 MB are skipped.
  - **Close:** hide the panel.
- **Host crash:** the launcher restarts the host at once with `ARCANUM_COOP_AUTOLOAD=latest`. A crash-loop guard allows at most 3 automatic restarts per 2 minutes; after that only the panel shows.
- **`ARCANUM_COOP_AUTOLOAD=latest` (or `1`):** about 30 frames after the main menu opens, a non-joining game loads `gamelib_last_save_name()`, which skips client slots Slot9700–9789, via `mainmenu_ui_coop_reload_save`. This skips the menu. With no save the game stays in the menu.
- **Clients wait in the lobby:** when the host is lost (the PT57 `coop_lite_client_host_left` path), the client goes back to the main menu as before, and the R hint line now reads "Waiting for host... (Esc to cancel)". Every 3 s the menu calls `coop_lite_client_reconnect()`. When it connects, it runs the same steps as R (`mainmenu_ui_start_pregen(1)`), so the parked PC comes back. Esc or R ends the wait. `ARCANUM_COOP_WAIT_HOST=0` turns it off.
- **Why the wait is in the game and not a launcher relaunch loop:** the client is already in its menu with the join address, token and character bundle, and the existing R path does the rejoin. A relaunch loop would restart the whole game every few seconds and flash windows. The cost: one connect attempt blocks the menu for up to ~5 s (ENet's 50×100 ms connect wait) while the host is down.
- No protocol change (still 40).

## PT63 — bashed doors, client stutter, hotbar, reload crash, autojoin (protocol 41)

- **Bashed doors (Bates tunnel)**: vanilla `portal_bust` destroys a door with `object_destroy`. PT60 only queued the oid for a 100 ms flush, and the flush checked `tig_net_is_host()` *before* lifting a local suspend, so it could drop the whole queue silently. The host log had no "destroyed on the host" line; the client only lost the door ~30 s later through the snapshot prune. Now `object_destroy` (after the SAP_DESTROY script allows it) sends GONE right away with the tile (`CoopLite: object: … destroyed on the host at x,y - sent to clients now`). The flush lifts the suspend before its checks and logs any drop. If the client can't resolve the id, it matches by tile + type (+ art) instead (`PT63 world object matched by location`), or logs `not found here`. Native test: `coop_lite_world_pick_candidate` in stress8.
- **Stutter**: tig's "No sound handle available" dump (~60 lines, several times a second in both logs) now prints at most once per 10 s, with a count of the skipped dumps. The object.c flag/HP/destroy hooks are behind an O(1) flag `coop_lite_world_hooks_on`, which is true on the coop host only and refreshed each ping. Before, clients paid for type reads and net checks on every `object_flags_set`.
- **Hotbar**: no earlier PT ever synced the 10 quick slots. The client sends its layout (`PACKET_COOP_HOTBAR` 158) whenever it changes, polled every 0.5 s. The host keeps it under the player's rejoin token and writes it into the save (block `CHB1` after the parked PCs; older saves load without it). On every join/rejoin/load the host sends the kept layout back once. The client restores it, matching items and item-spells by host oid, and retries for up to 30 s while the inventory sync arrives. It only sends its own bar after the host's copy has arrived (or after 10 s).
- **Client crash 07239681** (`obj_find_remove` NULL ← `map_open` during the host-reload resync): the client emptied `Save\Current\maps\<map>` of the map that was **open**. `map_open_in_game` then flushed into it, and the sector cache read from it, which left dead inventory handles ("validate: dead inventory entry dropped") and a freed find node. For the open map, the wipe now happens inside `map_open` between `map_close` and the reload (`coop_lite_map_open_after_close`). `obj_find_remove` logs and skips a NULL node.
- **Minidump**: it is now written from a helper thread with exception pointers, with dbghelp loaded from System32 and read/write file access. The log line has the error code.
- **Autojoin**: launcher Reconnect starts a client with `ARCANUM_COOP_AUTOJOIN=1`. The menu then joins at once with the rejoin token (same path as R / auto-wait), and falls back to auto-wait if the host isn't up yet.

## PT64: client travel requests that actually reach the host

**Symptom.** A client walking onto an exit, jump point, dungeon entrance or
scripted transition never produced the PT57 "Travel request" prompt on the host
(the PT63 client log has no PT57 line at all).

**Cause.** PT57 only intercepted `teleport_do` on the *client*. But a client
PC's moves, tile scripts, portal/stairs scripts and jump points (`object.c`
`sub_444270` -> `map_process_jumppoint`, `script.c`, `magictech.c`) run on the
**host**, with `td->obj` = the client's PC. There `coop_lite_client_travel_intercept`
is off (host) and `coop_lite_host_travel_gate` only looks at the host's own PC,
so the request was never asked: the transition simply went through the
vanilla path for a remote PC (or nothing visible happened).

**Fix.** `teleport_do` now also calls `coop_lite_host_remote_travel_intercept`:
on the host, a teleport of a remote co-op PC (slot 1..7) to another map, outside
co-op's own authorised teleports (`coop_client_tp_auth`, follow, travel bypass),
is blocked and turned into a travel request for that slot. Both paths (client
packet and host-side transition) go through `coop_treq_host_ask`, which:
- dedupes a repeat of the same ask (jump points re-fire on every step);
- shows the prompt on the travel panel **and** always in the message line
  ("<name> wants to travel to <map>. Y = the party goes, N = refuse.");
- logs `CoopLite: PT64 Travel request: slot S asks to travel to map M (via)`.
Y -> the host travels via its own path (ready check + party carry), the client
gets "The host agreed". N -> nobody moves, the client gets "The host refused the
trip." Client strings are English unless `ARCANUM_COOP_LANG=ru`.
No wire change: protocol stays 41.

## PT65: launcher update check (optional) and one folder per PC

**Folders.** On separate PCs (the normal case) one folder is enough: unzip the
package next to the game data and run `arcanum-coop-launcher.exe`; it starts the
`arcanum-ce.exe` next to it for both Host and Join. `Coop-Setup` (the
`coop-host\` / `coop-client\` instances) is only for testing host + client on
the same PC. The launcher keeps using those folders only when they exist.

**Update check (off by default).** `arcanum-coop-launcher.ini`:

```
[update]
enabled=1
releases_url=https://api.github.com/repos/Destroider12/arcanum-ce/releases
tag_prefix=coop-lite-
```

With `enabled=0` (or no `[update]` section) the launcher makes no network
request and shows "Updates off". When on, it checks at start and on
**Check for updates** (WinINet): the newest release whose tag starts with
`tag_prefix` (prereleases included, API order = newest first) and has a `.zip`
asset. If the tag's sha is not this build's (`COOP_BUILD_SHA`) it shows
"Update available: <tag>" and **Update**, which:
1. downloads the zip to `%TEMP%\arcanum-coop-update-*`;
2. requires a sha256 from a `sha256.txt` asset or a 64-hex line in the release
   body, and refuses on mismatch / missing (BCrypt SHA-256);
3. unpacks with PowerShell `Expand-Archive`, finds `arcanum-ce.exe` in it;
4. replaces **only** `arcanum-ce.exe` next to the launcher (the old one is
   kept as `arcanum-ce.exe.old`, restored if the copy fails, deleted on the
   next launcher start). It never touches `coop-host\` / `coop-client\`, the
   launcher, scripts, game data, saves or the ini.
Check / Update are disabled while the game runs, and Start waits for an update.
No release publishing tooling is part of this (builds are handed out by hand).

## PT66: companion and player corpses never disappear

**Cause.** `critter_kill` (critter.c) schedules `TIMEEVENT_TYPE_DECAY_DEAD_BODIE`
for every dead critter not flagged `OCF2_NO_DECAY` (co-op only exempted PCs).
One game day later `critter_decay_timeevent_process` drops the inventory and
`object_destroy`s the body. A dead follower is disbanded first, so it is just
another NPC body and decayed too, whether or not you left the map; on return,
or after resting / travel time, the body was gone. Nothing else removes bodies
(sector unload / map leave keep objects in the map diff).

**Fix.** `critter_corpse_is_kept`: a PC (any, incl. co-op client PCs on the
host), an NPC whose leader chain reaches a PC (`OBJ_F_NPC_LEADER` is restored
right after the death disband), or a body already marked `OCF2_NO_DECAY`.
Such a body gets `OCF2_NO_DECAY` set at death (a vanilla, saved flag, so the
decision survives map changes and saves) and no decay event. The decay handler
also skips kept bodies, so decay events already queued in older saves do not
destroy them. The body stays where it died with its inventory; it syncs to
clients as any other object, and resurrection works as for any existing body.
Decision is the pure `coop_lite_corpse_keep` (policy test). Protocol unchanged (41).

## PT67: dialog softlock after a companion craft menu

Asking a companion (Magnus) to craft opens the schematic UI from dialog, which
ends the dialog first. All PCs share one dialog entry on the host, so it could
already be marked ended while the local dialog window and its input filter
stayed installed; the end call then returned early and the empty dialog ate
all input, even across loads (reset only closed it when a flag that had been
stale-cleared was still set). Now ending a dialog, any save load/reset, and the
Esc-menu stale check always remove an orphaned dialog window/filter
(`coop_lite_dialog_ui_orphaned`, tested in coop_lite_policy_test).

## PT68: one game folder, separate host/client saves and logs

**Problem.** Two game instances in one folder shared `Modules\Arcanum\Save`
(incl. `Save\Current`, the live map state), `Players\`, `arcanum.cfg` and the
log, so Coop-Setup had to build `coop-host\` / `coop-client\` copies.

**Fix.** The coop role (`-coop` with `-host` / `-join`, `src/coop_paths.c`)
gets a writable folder `coop\host\` or `coop\client\` in the game folder.
After `tig_init` it is mounted as a tig *overlay* (`tig_file_set_overlay`,
first_party/tig/src/file.c): a directory repository that is kept on top of the
search list (re-pinned after every `tig_file_repository_add`), so every
relative write (saves, `Save\Current`, `Players\`, tile cache, ...) lands in it,
while reads of game data fall through to the module / data folders. The
prefixes `Save` and `Players` are private to the overlay: other directory
repositories are skipped for them in open / exists / list / remove / rename /
extract, so a role never sees, loads, empties or deletes single-player or the
other role's saves. Also per role: `arcanum.cfg` (`coop\<role>\arcanum.cfg`,
seeded once from the root file), the default log `coop\<role>\coop_host.log` /
`coop_client.log` (when no `-log` / `ARCANUM_LOG_FILE`), and crash
`crash-*.txt/.dmp` (`crash_handler_set_subdir`). Without a role nothing
changes. New saves made in coop get `[Host] ` / `[Client] ` in front of the
description (`ARCANUM_COOP_LANG=ru`: `[Хост / Host] ` / `[Клиент / Client] `);
a description already starting with `[` is left as is (re-saving a slot).

**Launcher.** Uses the root `arcanum-ce.exe` when present (no `-log`: the game
picks the role log); `coop-host\` / `coop-client\` only when the root exe is
missing. On Start in root mode, if the old instance folder exists, its
`modules\Arcanum\Save\*` slot files, `Players\` (module and `data\`) and
`arcanum.cfg` are copied once into `coop\<role>\` (`CopyFile` without
overwrite; `migrated-from-coop-*.txt` marks it done; the old folder is never
changed). Crash detection and the bug report read `coop\<role>\`.

Not covered: saves are not split per module inside a role folder; a client
character made in single player (root `Players\`) is not visible to the role
(the host sends the PC anyway). Protocol unchanged. Test: `tig_file_overlay_test`.

## PT71: client main menu Join Host, coop hotkeys rebound

- **Why R did nothing:** PT69 gated the hint, the click, R and AUTOJOIN on
  `!tig_net_is_active()`. A `-join` launch is already connected at start-up
  ("Co-op: connected ..."), so the gate was false and only the status line
  showed. The gate is now "co-op client launch, not in (or joining) the
  host's session" (`coop_lite_client_is_in_session`).
- **Keys:** R is the game's combat key and F its Fate key. Join Host is now
  **J** or **Enter** (or a click on the "[ Join Host ]" line) in the main
  menu; follow/guard is **G** in game.
- **Client menu:** in a co-op client launch Single Player and Credits are
  greyed out and inactive; Options, Join Host and Exit Game stay active.

## PT72: client stays out of the world after a host reload

Cause: when the host reloads, the client's SESSION_SYNC teleport could finish
before its PC was bound. `coop_lite_client_finish_sync` logged "finish sync:
no local PC" and dropped the pending sync, so the client never finished
joining. After that, every host follow was only queued ("client not in world
yet"), and the client seemed to vanish.

Fix: a missing local PC is now adopted by the OID in the sync packet when that
object exists. Otherwise the sync is kept and only the finish step is retried
every 250 ms from `coop_lite_ping`, with no new teleport. It gives up after 40
tries (about 10 s; `coop_lite_sync_retry_allowed`, tested). The earlier "Magnus
vanished" report was a different problem: the teammate portrait had taken the
follower slot (see PT74).

## PT73: client sees host world-map travel (protocol 43)

While the host has the world map open, `coop_lite_host_wmap_tick` sends a
new packet, `PACKET_COOP_WMAP_POS` (159), with the party marker's world-map
coordinates (x, y, moving flag, seq). It sends when the marker moves, at most
every 150 ms (about 6–7 Hz), plus a 1 s keepalive
(`coop_lite_wmap_pos_should_send`, tested).

On the client, `coop_lite_handle_wmap_pos` opens the normal world map in
spectator mode (`wmap_ui_spectate_open`) and moves the marker there, centring
the view. The old screen dim is lifted while the map is shown. Spectator mode
is read-only:
- `wmap_ui_message_filter` swallows all input, including Esc.
- `wmap_ui_open`/`select` are ignored.

The view closes on WMAP_CLOSE, on the 8 s overlay timeout, or when the host's
follow (arrival) packet comes in. If the client already has its own town map
open, it is left alone (dim only). Route lines are not sent; only the marker
moves.

## PT74: co-op player portraits on the right with a blue frame

Teammate PCs (PT43) used to be appended to the follower list, so they took
NPC follower slots on the left. That is why Magnus looked "lost". They now
have their own column of up to 7 slots, anchored top-right
(`follower_ui_team_*`), with a light-blue tinted frame. The left bar shows
only NPC followers again.

Clicks work as before:
- Hover shows the name.
- A plain click centres the camera.
- A click in spell/skill/item targeting targets the PC.
- There is still no right-click order menu for teammates.

The team column is shown even when the follower bar is collapsed.

## PT77: in-game chat + launcher lobby (protocol 45)

Vanilla Enter/broadcast already floats a text bubble over the local PC, but
other co-op players never saw it and there was no shared log.

**Wire:** `PACKET_COOP_CHAT` (160), protocol **45**, type limit raised to 168.
Client → host → all (host stamps slot + network name). Text is sanitized
(control chars → space, trimmed, max 120). ~400 ms per-slot rate limit.

**In game:**
- Enter (broadcast UI) still shows the bubble locally; Coop Lite then relays
  the line so every peer floats it over the sender's PC.
- A small bottom-left chat log (`[Name]: text`, colour-keyed so it does not
  steal clicks) shows the last few lines globally, fades after ~12 s of
  inactivity, and reappears on a new line or while typing.
- Lines are also written to `coop_host.log` / `coop_client.log` as
  `[CHAT] [Name]: text` for the launcher.

**Launcher lobby:** while the game runs, a pane lists players
(`lobby_players.txt` in the role folder) and mirrors `[CHAT]` lines from the
log. An input box writes `lobby_chat_in.txt`; the game polls it and submits
as normal chat (late join unchanged).

Keys: **Enter** opens the vanilla broadcast field (same as before); lobby
**Send** / Enter sends into the same chat from the launcher.


## PT77z: restore client-initiated travel votes

The Y/N travel-request feature (PT57 packet, PT64 host-side intercept) was
never deleted, but from a client's point of view it had disappeared.

**Cause.** Jump points, portal/stairs scripts and other map exits still go
through `coop_lite_host_remote_travel_intercept` / `PACKET_COOP_TRAVEL_REQ`
and were not removed by later work (PT77r only skips the host's own PT50
ready check while the world-map UI is open). Most overland travel, though,
is the world map: `coop_lite_can_use_worldmap` has been host-only since
before PT57, PT44 locked clients to the town map, and PT73 only lets a
client *spectate* the host's trip. A client pressing W, or leaving a town
onto the world map, saw "The World Map is not available" and could not
propose a destination. Same-map wilderness landings also slipped past
`client_travel_intercept` (it ignores same-map teleports).

**Fix.** A coop client may open the world/continent map when the host is
not already travelling (PT73 spectator view still takes over then). Starting
a world-map route, or landing via `wmap_ui_teleport`, sends a TRAVEL_REQ ASK
with the destination and `MAP_TYPE_START_MAP` instead of walking the route
alone (no local time/encounters). The host still sees Y/N; refusal means
nobody moves; acceptance runs the host's own teleport (ready check + party
carry). Map exits and teleport doors keep using the existing PT64 host-side
intercept; a missing map name no longer auto-refuses (the prompt uses
"another area"). `coop_lite_can_use_worldmap` stays host-only, so the client
still cannot actually drive the world map. No protocol change (stays 47).


## PT77z: host cannot drop a merchant's items for free

PT77m refused a *client* Packet28 pickup of store-chest stock after the
barter drop-slot trick. The host (and any local coop inventory path) still
ran `item_drop` when a merchant's item was dragged to the "throw away"
slot, so equipped gear, ammo and store stock landed on the ground for
free pickup.

**Fix.** In coop, `coop_lite_refuse_merchant_stock_drop` blocks a drop when
the item's parent is a merchant's substitute-inventory chest or a living
merchant who is not a companion. The inven UI drop slot (`sub_578EA0`
flags 0x02) refuses before any gold/ammo transfer, and `item_drop_ex` on
the host is the backstop. Vanilla single-player is unchanged. Own-item
drops, companion inventory, loot and steal still work. No protocol change.


## PT77aa: client vanishes after the host reloads a save

After a host save load, the already-connected client got `COOP_PARTY_RESYNC`,
sent a CHAR_OFFER, and immediately received SESSION_SYNC for the same map it
was standing on. The client force-reloads that map so stale NPCs/objects do
not linger (PT37), and PT63 empties `Save\\Current\\maps\\<map>` *after*
`map_close` (wiping it while the map is still open crashed the client).

Teleport writes the carried local PC into `dest\\mobile.mdy` *before*
`map_close`. The PT63 wipe then deleted that file, so `map_open` loaded a
pristine map with no PC. `PT72 finish sync` retried "no local PC" and either
gave up or the player Escaped to the main menu (which tears the session down).
A manual Join Host from ShopMap worked, because that is a *different* map:
the destination folder is emptied *before* teleport writes `mobile.mdy`.

**Fix.** While a post-close wipe of the destination folder is pending,
teleport writes the carried objects to `Save\\Current\\coop_lite_carry.mdy`
(outside the wiped directory). After the wipe, that file is moved into
`dest\\mobile.mdy` and the local PC is loaded as usual. `finish_sync` can
then apply SESSION_SYNC and, when `COOP_SYNC_F_REJOINED` is set, request the
host's parked copy of the PC. No protocol change (stays 47).


## PT77ab: stop dumping every animation on a repeated assertion

`ASSERT` in the anim code always called `anim_stats()`, which prints every
active animation slot (goal stack, path, object names). In a client log from
build dd68d994 this fired 269 times at `anim.c:4480` (`parent_obj !=
OBJ_HANDLE_NULL` in `sub_424BC0`, already handled by returning false) and
wrote 6.4 MB of a 9.1 MB log — mostly looping torch/fire candy. The one-line
`ASSERTION:` is still logged every time; a full dump is written only on the
first failure at that file:line, and at most once a minute after that, with
a short "repeated N times" reminder. No protocol change.


## PT77ac: host crash leaving BMC (stale sector-light object handles)

The host crashed with 0xC0000005 on a map-changing exit from the Black
Mountain Clan mines (and the same sequence appears to have crashed an
earlier attempt whose log the launcher did not keep). The stack was
`teleport_ping` → `map_open_in_game` → `map_flush` → `sector_flush` →
`sector_cache_evict` → `sector_light_list_reset` →
`light_unset_flags_internal` → `obj_arrayfield_uint32_set` (read of
0x37c07).

A same-map portal just before the crash did not flush the map and
survived. The map-changing jumppoint evicts unlocked sectors and, for
every light still tagged as object-attached, stops animation and writes
flags back through `light->obj`. That handle can be stale: it was saved
in the sector light list and is not a live object-pool handle after the
object was destroyed or the game was reloaded. `obj_lock` does not check
validity.

This is not the same bug as e399f4e4 (duplicate list nodes / half-loaded
slots / `sector_lock` during `cache_reset` on F8) or 4d22c6a2 (drop an
unreadable .dif). Those remain; this path was still open.

**Fix.** `light_obj_is_live` checks `obj_handle_is_valid` and clears a
stale handle. `sub_4DD110` (static light) treats a stale handle as
unattached, so evict frees it instead of writing through it. Flag/art/color
writers, `light_read`, and `light_write` use the same check. No protocol
change (stays 47).


## PT77ad: full pack in one INVEN_LAYOUT (protocol 48)

Review (Argon) 1b. The client's layout hash covers every item it carries,
but `coop_lite_local_layout_hash` filled only the first 64 entries
(`COOP_INVEN_LAYOUT_MAX 64`). With more than 64 items the host applied a
truncated list: items past #64 kept their old cells (holes / overlaps on
the client after the echo) and the hashes never matched. The cap is now
256 entries (0x28 + 256·0x20 = 8232 bytes, still sent with its real
length; TIG_NET_MAX_PACKET is 16384). A pack over 256 items logs once.
The host's planner arrays are sized by the same constant.

Review 1a/1c were checked and not changed: the host apply is already
two-phase (remove every moved item, then insert at the wanted or nearest
cell) with one repair/self-check before and one after; the client's own
grid moves are deferred and applied remove-all/insert-all, repair runs
before and after, and `coop_lite_inv_list_repair` returns at once on a
clean list (the in-loop call is only the rare orphan re-list).

Protocol 47 → 48: an old host would refuse a layout over 64 entries.

## PT77ad: a client whose PC does not come back after a host load rejoins

Review (Argon) 2. When the PT72 retries (40 × 250 ms) found no local PC
after a host reload, the client logged "no local PC (gave up)" and stayed
connected with no character until the player quit to the menu and joined
again. That manual quit-and-rejoin is the path that always worked (the
host parks the PC on leave, the rejoin-only offer brings it back). It is
now automatic: on give-up the client shows a notice, arms the existing
host-left return to the main menu (the game reset leaves the session) and
switches on the wait-host loop, which rejoins within ~3 s. Nothing new is
built in the world (no PC rebuilt from a proto in place).

Checked and not changed: a busy sync teleport is retried every 250 ms
until it queues (`coop_lite_client_request_sync_teleport`); a teleport
that cannot be scheduled never closes the map, so it cannot lose the PC.
The PC-loss case in the load-vanish log was the PT63 wipe (PT77aa). If the
Esc menu is open when the give-up fires, the return is skipped (the menu
is already up) but the wait-host loop still rejoins after the player
quits. No protocol change.

## PT77ad: world state - faster corpses, lighter pages

Review (Argon) 3 and 5. A client applies a dead NPC only to an NPC it has
loaded when the entry arrives. A sector it loads later from its own map
files shows the host's dead NPCs alive until the next radius reply (every
30 s), so revisits showed living corpses.

- **3c.** The client now sends a WORLD_REQ as soon as its PC enters another
  sector of the same map, at most once per 3 s (the host's per-slot reply
  limit, so no request is wasted). The radius reply lists dead NPCs within
  `COOP_WORLD_DEAD_RADIUS` (72 tiles) and costs the host about 2.5 ms
  (median of 547 radius replies in review logs; max 3.7 ms).
- **3a.** The first request after a map load waits 500 ms instead of
  1500 ms (entries for sectors not loaded yet are skipped and come again
  with the sector request). Full-snapshot pages go every 50 ms instead of
  100 ms: one 4 KB page = ~80 KB/s per joining slot, only while its
  snapshot runs. 25 ms was not taken: 160 KB/s per slot, ~1.1 MB/s host
  upload with 7 joining clients.
- **3b.** The full snapshot lists NPCs and portals first, then the rest in
  walk order (sector-scope entries stay on the first page).
- **5a.** WORLD_STATE sends container contents (one reliable INVEN_SYNC
  batch each) only for containers within 6 tiles of the client PC, in both
  the full and the radius reply. Opening any container sends its contents
  (`COOP_BARTER_OPEN_LOOT` → `coop_lite_host_sync_slot_inven` includes the
  loot target). The 8-containers-per-page cap now counts only those.

Not changed:
- 3c "reset coop_npc_cache": the NPC_POS cache is host-side and shared by
  all slots; the radius reply already carries dead state.
- 4: the drop path is not WORLD_DIRTY-bound. The host answers a client
  drop with the owner's INVEN_SYNC, then a Packet70 to everyone; a client
  that lacks the item asks for its blob at once (`want_ground_blob` →
  OBJ_REQ flush), and only if the tile's sector is loaded. Pushing the blob
  to every client would send it to clients far away too.
- 5b: the 3x3 keep is PT77y (081eefc6). The radius reply looks 24 tiles out,
  dead NPCs 72, and NPC_POS 40, so a 1..4-sector window with a 20-tile
  margin would unload sectors those scans need (the reason PT77y dropped the
  old 2x2-near-edge keep).
- 5c: the co-op sector cache has been 64 since PT77y (engine cap 128). The
  radius reply walk is cheap (above), and the dead-NPC window spans
  several sectors, so an `object_list_rect` rewrite gains little.

No protocol change.

## PT77ae: host drops reach nearby clients at once (protocol 49)

User playtest: items the host drops show up on clients late. The client
drop path was already immediate (INVEN_SYNC, Packet70, OBJ_REQ → OBJ_BLOB:
one extra round trip). The host's own drops were not:

- dragging an item to the ground in the host's inventory
  (`inven_ui.c` → `sub_466E50`) and gold/ammo splits announced nothing
  and did not raise WORLD_DIRTY. Clients saw the item at their next
  WORLD_STATE (30 s refresh, or sooner only if something else made the
  world dirty);
- `item_drop_ex` (drop slot, hotkeys) raised WORLD_DIRTY: the client asks
  ~3.5 s later, then fetches the blob (another round trip).

**Fix.** `sub_466E50` on the host (every drop: host UI, splits, NPC
drops, client drops) calls `coop_lite_host_item_landed`. It serialises
the item once and pushes it as OBJ_BLOB with `COOP_OBJ_BLOB_F_GROUND` in
the same tick, only to clients whose PC is within
`COOP_DROP_PUSH_RADIUS` (72 tiles). A ground item has no owner, so the
first 8 bytes of `owner_oid` carry its location. The client loads it
when the tile's sector is loaded here (else WORLD_STATE lists it later).
If the client already has that object in an inventory (its copy of the
dropper's pack), it moves it to the ground keeping its state, like a
Packet70. Packet70 / WORLD_DIRTY stay; they become no-ops for clients
that already have the item. Several items = one push each.

Protocol 48 → 49: an old client drops the unasked blob.

## PT77ae: restore our PC in the world after a host load

Review (Argon) 2, reconsidered. When SESSION_SYNC finds no local PC after
the host's save load (neither `player_get_local_pc_obj` nor the sync's
`client_pc_oid` resolves), the client now restores the PC in place and
does not wait out 40 retries before going to the menu:

- from the 4th PT72 retry (~1 s), and again every 4th retry, it asks the
  host for the blob of the PC the host keeps for its slot. This is the
  existing rejoin request (`coop_lite_client_request_own_pc` → OBJ_REQ;
  the host answers its slot PC as a blob);
- the blob is taken before the world-ready check (that check needs a
  local PC). It is loaded at the sync's `client_loc` and set up like the
  rejoin adopt: empty pack until the next INVEN_SYNC, OF_OFF cleared, set
  as the local PC, slot bound, view refreshed, pack sync requested. The
  next PT72 retry then runs the normal finish-sync binding;
- if the place is not loaded or the blob is refused, the next ask
  retries. The PT77ad menu rejoin (cbbef416) runs only after retry 40
  (~10 s) as the last resort;
- if the PC turns up by OID meanwhile, the blob request is dropped.

Review 2a/2b, preemptive measures:
- done: before the sync teleport the client closes the world-map
  spectate and inventory / loot / barter / dialog / character view. It
  reuses the forced-follow closer (PT52f / PT73).
- not done: force-clearing `teleport_pending`. It cannot get stuck:
  `teleport_ping` clears it on every call. A reset by hand would drop a
  scheduled teleport that has already taken the render lock (frozen
  screen).
- not done: closing the Esc menu / death screen from game code. They are
  the main-menu window with its own input loop; the retries keep running
  under them, and the menu-return fallback already skips when the menu
  is up.

No protocol change (49).
