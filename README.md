# Arcanum Coop Lite (on Arcanum Community Edition)

[Русская версия](README.ru.md) · [Design & dev log](docs/coop-lite.md) · [Coop Middle](docs/coop-middle.md) · [License](LICENSE.md)

> [!IMPORTANT]
> Unofficial fan project, pre-release. It is not affiliated with or endorsed by Troika Games, Activision, or any other rights holder of *Arcanum: Of Steamworks and Magick Obscura*. **No game data is included.** You need your own legal copy of the game.

## What this is

- **arcanum-ce** ([alexbatalov/arcanum-ce](https://github.com/alexbatalov/arcanum-ce)) is an open-source reimplementation of the *Arcanum* engine by Alexander Batalov and contributors. This repository is a fork of it.
- **Arcanum Coop Lite** is a co-op mod built on top of it (branch `coop-lite-session-sync`). It lets **up to 8 players** play the regular single-player campaign together. One player hosts, and the host's game is the authority.

**Goal:** play the whole single-player campaign (roughly 50–80 hours) together, with friends able to join a game in progress (late join) and to come back after a disconnect or crash (reconnect).

Without `-coop` the game runs as plain single-player arcanum-ce.

## Requirements

- Your own legal copy of *Arcanum* (Steam or GOG, version 1.0.7.4). The game's `.dat` files, `modules/`, movies and music come from your install (see the upstream install notes below).
- Windows, 64-bit. Builds are cross-compiled with mingw. Other platforms are inherited from upstream but are **not tested** for co-op. The launcher is Windows-only.
- For Steam transport only: Steam running and logged in on every PC, plus a 64-bit `steam_api64.dll` next to `arcanum-ce.exe` (not shipped, see below).

## Technology

- **C** with **CMake** (presets in `CMakePresets.json`), **SDL3** + SDL3_mixer (from upstream), mingw-w64 cross-builds for Windows x64.
- **ENet 1.3.18** (vendored, MIT) as the default UDP transport.
- **miniupnpc** for UPnP port mapping on the host.
- **Steam Networking Sockets** (ISteamNetworkingMessages, P2P via Valve's relays). `steam_api64.dll` is loaded at runtime with `LoadLibrary`. It is never linked, so GOG and non-Steam builds don't need it. No Steamworks SDK files are in the repository.
- **Tests:** a native `ctest` suite under `tests/` (policy, dialog filter, net hardening, 8-player net/stress, session sync, transport loopback, and more), run with AddressSanitizer, plus receive-path fuzz harnesses (`coop_lite_rx_fuzz_test`, `coop_lite_fuzz8_test`).

Main code: `src/game/coop_lite.c/.h` (co-op game logic), `src/net/` (`tig_net.c`, transport backends `tig_net_enet.c` / `tig_net_steam.c`, `coop_upnp.c`), `src/crash_handler.c`, `tools/launcher/launcher.c`.

## How Coop Lite works

### Architecture

- **Host-authoritative.** The host runs the only real simulation: game time, AI, combat, scripts, loot, and saves.
- **Clients send intents** (move, attack, cast, use item, inventory changes, dialog choices, travel requests, and so on). The host checks them (bounds, ownership, range, rate limits), applies them, and **broadcasts** the result.
- Clients don't advance game time on their own. They apply what the host sends.
- **Protocol version: `TIG_NET_PROTOCOL 60`** on this branch (`coop-middle`, Coop Middle after M7; M7 was 59, M1 was 53, M2 54, M3 55, M4 56, M5 57, M6 58); Coop Lite 1.0 (tag `coop-lite-v1.0`, branch `coop-lite`) stays on **52**, so Lite and Middle builds refuse each other (`src/net/tig_net.h`). Host and client must run the same build. A client with a different protocol is refused at handshake (HELLO/WELCOME).
- **Trust model:** this is built for a group of friends, not public servers. The checks stop desyncs, stale packets and mismatched builds from damaging the host's world. They don't stop a deliberately patched client. **Don't expose a host to strangers.**

### What gets synced

- **Player characters:** PCs, stats, vitals (HP/fatigue/poison), levels, portraits and body art.
- **Inventory:** contents, **grid layout**, and stackables (gold, ammo, arrows keep their cell). You can rearrange the pack of another nearby party member.
- **Hotbar:** the 10 quick slots, kept by the host per player and stored in the host save.
- **World:** doors (open/locked/bashed or destroyed), scenery and breakable containers, script-driven wall changes, and destroyed objects.
- **Traps:** trap visual effects (clients show the effects; damage is the host's).
- **Spells:** casting, FX and **maintained spells**.
- **Dialogs:** NPC dialogs run on the host, with restrictions on which player may talk.
- **Chat:** Enter opens the broadcast field; messages float over the sender and appear in a small fading log for everyone. The launcher mirrors chat while the game runs.
- **Loot, merchants, and pickpocketing**, resolved by the host.
- **Follower item exchange:** a client can open the barter/exchange window with their own follower (from the follower's dialog); the host validates every transfer.
- **Known world-map locations:** areas discovered by any player are shared with the whole party (sent on join and when new ones are found).
- Animation and combat state of PCs and NPCs.

### Travel

- **Ready check:** when the host travels (world map or area change), the party goes through a ready check. Players press **Y** to confirm, and the host can force the trip with **Y**. Party PCs are carried along.
- **Client travel requests:** when a client steps onto an exit, jump point, stairs or scripted transition, the host is asked "<name> wants to travel to <map>. Y = the party goes, N = refuse." Clients can't travel the world map by themselves.
- **G – follow:** follow or guard the nearest player.

### Saving and loading

- The host saves and loads normally. Parked PCs of absent players and their hotbars are saved with the world.
- **F7 (client):** asks the host to save. The host writes into a reserved client slot range (`Slot9700`–`Slot9789`, named `coop-<nickname>`) and never overwrites the host's own slots.
- **F8 (client):** asks the host to load. The host confirms with Y/N and all clients resync. A client can't load locally during a session.

### Late join and reconnect

- Each player has a **rejoin token** (`Players/coop_token.txt`). When a player leaves, the host parks their PC. A player who joins again with the same token gets the same character back, also into a game that is already running.
- If the host is lost, the client returns to the main menu. Use **Join Host** there (J, Enter or click; Single Player and Credits are inactive in a client launch), or the client keeps trying every 3 s ("Waiting for host…").

### Crash handling (Windows)

- `src/crash_handler.c` writes `crash-*.txt` (exception, offsets, stack walk, co-op state, last co-op log line) and a minidump `crash-*.dmp` next to the exe.
- The launcher watches the game process. After a crash it shows **Reconnect / Create bug report / Close**. The bug report zip has the logs, crash files and the launcher ini. **That ini is not redacted:** it holds your nickname and the host address/SteamID, so check it before sharing the zip publicly.
- After a host crash, the launcher restarts the host from its latest save (`ARCANUM_COOP_AUTOLOAD=latest`), at most 3 times per 2 minutes.

### Launcher (`arcanum-coop-launcher.exe`)

- Name, Host/Join, transport **Direct + UPnP** or **Steam**, port (default 7777) or host address / SteamID64, Windowed mode.
- **Rules (host, Coop Middle):** Split gold / Loot feed / Rest vote / Direct trade / Client pause / No friendly fire / Shared quest items / Shared followers / Unified sleep checkboxes (the free world map is always on unless **Coop Lite mode**) and Enemy HP % / XP % lists, saved in `[rules]` of `arcanum-coop-launcher.ini` and passed to the game as `ARCANUM_COOP_RULES`.
- **Lobby pane** while the game runs: connected players and a chat mirror with Send.
- Shows your local IP and, through UPnP, your public IP, with a **Copy** button.
- **Port check:** local bind, STUN (public IP and port mapping), UPnP mapping test, CGNAT detection, Windows firewall rule check, and an optional "Add firewall rule" (runs elevated, only when you click it).
- Crash recovery as described above.
- **Update check:** optional and **off by default** (`[update] enabled=1` in `arcanum-coop-launcher.ini`). It checks this fork's GitHub releases (tags `coop-middle-*` on this branch, `coop-lite-*` on Coop Lite), requires a matching SHA-256, and replaces only `arcanum-ce.exe`. *This feature is new (PT65) and not well tested yet.*

### Controls

| Key | Where | Action |
| --- | --- | --- |
| **G** | in game | follow / guard the nearest player |
| **Y** | in game | ready for travel; host: go now (force) / accept a request (travel, load, rest) |
| **N** | in game | host: refuse a travel, save, load or rest request |
| **F7** | client | save on the host |
| **F8** | client | ask the host to load |
| **Ctrl+LMB** on a player / **RMB** on a teammate portrait | in game | direct trade (Coop Middle M2, within 6 tiles) |
| **Ctrl+LMB** on a teammate's follower | in game | follower exchange (Coop Middle M4, shared followers, within 6 tiles) |
| **Alt** (hold) | in game | loot labels (Coop Middle M2) |
| **F9** | in game | net HUD on / off (Coop Middle M2) |
| **Pause** | in game | shared tactical pause (Coop Middle M3; clients only if the host allows it) |
| **MMB** / **Alt+RMB** on the map | in game | ping a spot for the team (Coop Middle M3) |
| `/tp`, `/stuck` / `/gather` in chat | client / host | regroup to the host / pull the party (Coop Middle M3, 30 s cooldown) |
| **J** / **Enter** | client main menu | Join Host (rejoin the last host, no New Game) |
| **Enter** | in game | open chat / broadcast bubble (relayed in co-op) |

### Command line and environment variables

Command line: `arcanum-ce.exe -coop -host <port>` or `-coop -join <ip:port | SteamID64>`, plus optional `-window` and `-log <file>`. The launcher fills these in for you.

| Variable | Meaning |
| --- | --- |
| `ARCANUM_COOP_NAME` | player nickname (default "Player") |
| `ARCANUM_COOP_TRANSPORT` | `direct` / `enet` (default) or `steam` |
| `ARCANUM_COOP_STEAM_HOST` | host SteamID64 when joining over Steam |
| `ARCANUM_COOP_STEAM_APPID` | Steam AppID (default 500810; 480 for testing) |
| `ARCANUM_COOP_NO_UPNP` | disable UPnP on the host |
| `ARCANUM_COOP_AUTOLOAD` | `latest`/`1`: load the latest host save from the menu |
| `ARCANUM_COOP_AUTOJOIN` | `1`: join right away with the rejoin token (used by the launcher's Reconnect) |
| `ARCANUM_COOP_WAIT_HOST` | `0`: turn off the client's automatic waiting for the host |
| `ARCANUM_COOP_LANG` | `ru`: Russian co-op messages (default English) |
| `ARCANUM_COOP_RULES` | host session rules (Coop Middle), e.g. `gold=0,loot=1,rest=1,trade=1,pause=0,ff=1,qitems=1,sfol=1,ow=1,usleep=1,flvl=1,plvl=1,hp=100,xp=100`; see [docs/coop-middle.md](docs/coop-middle.md) |
| `ARCANUM_COOP_NO_CHAR_BUNDLE`, `ARCANUM_COOP_NO_OBJ_BLOB`, `ARCANUM_COOP_NO_WORLD_STATE`, `ARCANUM_COOP_WORLD_TRACE` | debugging switches (turn off parts of the sync / add tracing). Not for normal play. |

Developer switches `ARCANUM_DEV_*` only work with `ARCANUM_DEV=1`. `ARCANUM_LOG_FILE` sets the log file.

## Coop Middle (branch `coop-middle`, protocol 60)

Coop Middle is the next development line on top of the Coop Lite 1.0 release (`74a7076f`, tag `coop-lite-v1.0`). Coop Lite is maintained separately on branch `coop-lite` (protocol 52). All Middle code lives in its own files (`src/game/coop_rules.*`, `src/game/coop_middle.*`, `src/ui/coop_middle_ui.*`). Lite files only get small hook calls, so Lite fixes merge cleanly.

Stage **M1 (foundation)**:

- **Session rules** (`CoopRules`): the host sets them in the launcher (or `ARCANUM_COOP_RULES`) and sends them to every client when it joins or rejoins (`PACKET_COOP_RULES`).
- **Gold split** (off by default): gold a player picks up from the ground, a container or a corpse is shared equally among the living players. The picker keeps the remainder, and gold a player dropped or stashed is never split.
- **Loot feed** (on): `[Loot] <Name> picked up: <Item>` chat lines for keys, scrolls, notes and schematics, quest or unique items, and gear worth 250+. At most 5 lines a second; a "Take all" burst collapses into "... and N more".
- **Rest vote** (on): a client's rest button or bed sends a request to the host (Y = rest, N = refuse) instead of the old "Only the party leader can rest" refusal.

Stage **M2 (QoL and team UX)**:

- **Direct trade** (on): Ctrl+LMB on another player's PC, or RMB on a teammate portrait, opens a trade window (within 6 tiles). Giving from your own pack is free. Nothing can be taken from the other player. `[Trade] <From> gave <Item> to <To>` goes to both players, or to everyone when the loot feed is on.
- **Alt loot labels**: hold Alt to see ground items, containers and corpses with loot nearby (local only, up to 32).
- **Status badges** on teammate portraits: X dead, Z unconscious, P poisoned, D dialog, I inventory, C combat.
- **F9 net HUD**: players, RTT, packet loss and sync state (local only).

Stage **M3 (tactics and difficulty)**:

- **Shared pause** (Pause key): game time, AI, combat and animations stop for everyone, while chat, inventory, the character sheet and the camera keep working. The host can always pause; clients only when **Client pause** is on (off by default). A banner shows who paused, and the pause lifts if that player leaves.
- **Difficulty** (host): **Enemy HP %** (100-300, by dividing damage to enemies) and **XP %** (100-250). **No friendly fire** (on): party members never damage each other, AoE included.
- **Pings**: MMB or Alt+RMB marks a tile for everyone on the map for 4.5 s, red over an enemy or trap. It also shows on the town map.
- **`/tp`, `/stuck`** (client) and **`/gather`** (host): regroup next to the host when nobody is in combat or being attacked. 30 s cooldown.

Stage **M4 (shared party)**:

- **Shared quest items** (on): a quest check or hand-in also finds the item in a nearby player's pack (30 tiles, same map). `[Quest] <Item> handed over from <Name>`.
- **Town map fog**: tiles one player uncovers on a town map open for everyone.
- **Parallel dialogs**: several players can talk to different NPCs at once. Clicking an NPC someone else is talking to shows that dialog, and you can vote.
- **Shared followers** (on): any player within 6 tiles can trade with or re-equip any party follower; follower skills are synced.

Stage **M5 (open world)**:

- **Interest management**: each player gets NPC updates only within 44 tiles; on the world map, doors, traps, projectiles, sounds and spell effects only reach players nearby. Larger sector cache (128).
- **Free world map** (always on unless Coop Lite mode; no launcher box): on the world map each player travels on their own; the clock does not run while travelling. Allies show as coloured markers; an ambush shows a red marker for 15 s.
- **Unified sleep** (on): picking a rest is a vote; the others press Y to join or N to cancel. When everyone alive is ready, the whole party sleeps and heals together.

M5 fixes: the Teleport spell no longer throws the party to the edge of the world (the place is picked on the world map, as in single player); a client's world map trip follows the plotted route instead of a straight line over mountains and sea.

Stage **M6 (solo instances)**:

- **Solo interiors/dungeons** (on, `subinst`): a client can enter another map alone. The host hands it that map (a CMAP1 archive), the client plays it locally, and story changes, XP and items go back to the host (checkpoints every 45 s and before a host save, a final commit on exit). One player per map; a busy map says "<Name> is already inside <map>. Wait or ask the host for group travel.". Badge **M** on the portrait, an entrance marker on the world map, a chat line.

Stage **M7 (final polish)**:

- **Fallen teammates**: `[Party] <Name> collapsed from exhaustion!` in chat; a teammate who is dead (red **X**) or unconscious (lilac **Z**) off screen gets an arrow plate at the screen edge with the distance in tiles; a dead teammate is a red square on the town map.
- **Host autosave** (`asave`, launcher **Auto-save** Off / 5 / 10 / 15 / 30 min, default 10): the host saves the world into `[Host] AutoSave 1`..`5` (Slot9690-9694, round robin) at a safe moment (no pause, combat, dialog, world map, travel or solo-instance commit; retried every 15 s). Manual saves and client slots are never touched. `[AutoSave] World saved (AutoSave N).`
- **Solo instances join the party's sleep**: they vote, and after a unified sleep they heal inside too. The pause also freezes world map trips, checkpoints and the autosave clock; an open trade window closes by itself when the partner travels, enters an instance, fights, dies or walks away.
- The launcher lobby shows the session rules under the player list.

After M7:

- **Coop Lite mode** (`lite`, launcher checkbox **Coop Lite mode (as release 1.0)**, off by default): one switch that plays the session as the Coop Lite 1.0 release: no free world map (the host drives the party), no solo instances, one party dialog at a time with the leader nearby, no shared quest items / followers, no unified sleep or rest request, no gold split, trade, pause, `/tp` `/stuck` `/gather`, no friendly-fire protection, HP / XP at 100 %. The other rule boxes grey out. What stays: HUD, loot labels and feed, badges, pings, arrows, town map marks, autosave.
- **Auto-levelling** (both on by default): **Auto-level followers** (`flvl`) raises a follower to its leader's level the vanilla way (e.g. a dog hired at level 35 becomes level 35); **Party level** (`plvl`) keeps every player at the party's highest XP, so when one player levels (even alone in a dungeon) everyone does, and a late joiner gets all the levels at once with free points to spend. Protocol 60.
- Fixes: a host's random encounter no longer pulls the clients to the host (free world map); a client can talk to NPCs anywhere on the shared map, not only near the host. In Middle a host's stairs / teleport inside the same map (dungeons) no longer pull the clients (a map change still moves the party). Host followers now travel with the party on clients too, and a client crash on the host's teleport (stale follower handle) is fixed. Host saves made after a client found items in a solo dungeon (for example after it died there) load again: such items were saved the wrong way and the whole save fell back to the start merchant; old saves of that kind load now, minus those items.

Hotkeys: Alt (loot labels), F9 (net HUD), Pause (shared pause), MMB / Alt+RMB (ping), Ctrl+LMB on a teammate or RMB on a portrait (trade), Y / N (rest / sleep). Chat: `/tp`, `/stuck`, `/gather`.

Not tested in play yet. Details, all `ARCANUM_COOP_RULES` keys and log lines are in [docs/coop-middle.md](docs/coop-middle.md).

## Installing and playing

1. Install *Arcanum* (Steam or GOG, 1.0.7.4).
2. Unzip the Coop Lite release (`arcanum-ce.exe`, `arcanum-coop-launcher.exe`) into the game folder, next to `arcanum1.dat`…`arcanum4.dat` and `modules/`. Every player needs **the same build**.
3. Start `arcanum-coop-launcher.exe` and enter your name.
4. **Host:** pick Host and a transport, use **Check port**, press **Start**, and load or start a game. Give friends your `ip:port` (Copy button) or your SteamID64. With Direct, the UDP port must be reachable (UPnP or a manual port forward). Behind CGNAT, use Steam.
5. **Join:** pick Join, enter the host's address or SteamID64, and press **Start**. You can join while the host is already playing.
6. For Steam: put a 64-bit `steam_api64.dll` (Steamworks SDK 1.51+ or taken from a recent 64-bit Steam game) next to `arcanum-ce.exe` on every PC.

One game folder is enough, even for a host and a client on the same PC: in coop the game keeps each role's saves, `Players\`, `arcanum.cfg`, log and crash files in `coop\host\` or `coop\client\` of that folder (game data stays shared; single player is unchanged). Coop saves are tagged `[Host]` / `[Client]`. The old `Coop-Setup` folders (`coop-host\`, `coop-client\`) are only used when the root `arcanum-ce.exe` is missing; otherwise the launcher copies their saves once into the new folders (nothing is moved or deleted).

### Building

See the upstream build notes below. Use the CMake presets, or a mingw-w64 cross-build for Windows x64. The tests are in `tests/` and run with `ctest`.

## Known limitations

- **Pre-release, limited testing.** The full campaign has **not** been played through in co-op yet. There have been only a few short playtests (on one PC and over the internet between two PCs), and many recent fixes are **not tested in play** yet. Expect bugs and desyncs.
- Windows x64 only for co-op. Other platforms are untested.
- The **Steam transport** has not been tested at all yet.
- **Cyrillic (non-Latin) text input is not supported** (chat, names).
- Known open bug: a client could sometimes no longer move after dialogs; the cause is not found yet. If it happens, please send both logs (host and client).
- The world map is driven by the host. Clients can't travel on their own.
- When the ready check delays a world-map stop, the host can stand at the old spot until the check finishes.
- While the host is down, each reconnect attempt can block the client's menu for up to about 5 s.
- No protection against deliberately cheating clients (see the trust model).
- The launcher update check (PT65) is new and off by default.
- **Arcanum: ReForged** is **not supported**: compatibility is a goal, but it is not done or verified, so expect it not to work.

## Roadmap

- Compatibility with [Arcanum: ReForged](https://github.com/kurkijar/Arcanum-ReForged) (modern world map, HighRes).
- Polish for the Steam relay transport (connection UX, testing).
- Testing between separate PCs over a real network, then a complete campaign playthrough in co-op.
- More sync coverage and fewer desyncs found by playtests, fuzzing and 8-player stress tests.
- A release and update workflow for the launcher.

The full engineering log (every PT step, protocol changes and reviews) is in [docs/coop-lite.md](docs/coop-lite.md).

## Credits

- **Coop Lite code:** written with **Grok Bot**.
- **Code review:** with the help of **Gemini Argon**, **DeepSeek 4.1** and **Qwen 3.8 Max**.
- **Project author, lead and playtesting:** **Destroider12**.
- **Base engine:** [arcanum-ce](https://github.com/alexbatalov/arcanum-ce) by **Alexander Batalov (alexbatalov)** and its contributors.
- ***Arcanum: Of Steamworks and Magick Obscura*** by **Troika Games**.
- Third-party libraries: ENet (MIT), miniupnpc (BSD), SDL3 (zlib), and the others listed under `third_party/`.

## License

arcanum-ce is distributed under the **Sustainable Use License 1.0** ([LICENSE.md](LICENSE.md)). It allows non-commercial use and free distribution only. The Coop Lite additions are released under **the same license**. This is a modified version of arcanum-ce, as the license's notice requirement asks. Third-party components keep their own licenses.

## Disclaimer

*Arcanum: Of Steamworks and Magick Obscura* and its trademarks belong to their respective owners. This is an unofficial, non-commercial fan project, with no affiliation to or endorsement from the rights holders. No original game assets are distributed here.

---

# Upstream: Arcanum Community Edition README


> [!IMPORTANT]
> This is a beta, technology preview, or whatever other label you have in mind for a pre-release software.

## Status

The game is ready, but I haven't tested everything - just the speedrun - and that was about three months ago, so things might have changed since then.

Regarding the source code, about half of the modules (not half of the code) are in good shape. All APIs have meaningful names, along with brief documentation, annotations, and explanations (see `skill.c` and `quest.c` as examples). The other half may have cryptic names, little to no symbols , and no documentation at all (`anim.c` is extremely large and hard to understand).

## Installation

You must own the game to play. Purchase your copy on [GOG](https://www.gog.com/game/arcanum_of_steamworks_and_magick_obscura) or [Steam](https://store.steampowered.com/app/500810).

<details>
    <summary>Minimum installation</summary>

    ```
    .
    ├── arcanum1.dat
    ├── arcanum2.dat
    ├── arcanum3.dat
    ├── arcanum4.dat
    ├── modules
    │   ├── Arcanum
    │   │   ├── movies
    │   │   │   ├── 00069.bik
    │   │   │   ├── 01138.bik
    │   │   │   ├── 02112.bik
    │   │   │   ├── 50000.bik
    │   │   │   ├── 51169.bik
    │   │   │   ├── 91568.bik
    │   │   │   ├── A0021.bik
    │   │   │   ├── G0021.bik
    │   │   │   └── movies.mes
    │   │   └── sound
    │   │       └── music
    │   │           ├── Arcanum.mp3
    │   │           ├── Caladon.mp3
    │   │           ├── Caladon_Catacombs.mp3
    │   │           ├── Cities.mp3
    │   │           ├── Combat 1.mp3
    │   │           ├── Combat 2.mp3
    │   │           ├── Combat 3.mp3
    │   │           ├── Combat 4.mp3
    │   │           ├── Combat 5.mp3
    │   │           ├── Combat 6.mp3
    │   │           ├── CombatMusic.mp3
    │   │           ├── Dungeons.mp3
    │   │           ├── DwarvenMusic.mp3
    │   │           ├── Interlude.mp3
    │   │           ├── Isle_of_Despair.mp3
    │   │           ├── Kerghan.mp3
    │   │           ├── Mines.mp3
    │   │           ├── Qintara.mp3
    │   │           ├── Tarant.mp3
    │   │           ├── Tarant_Sewers.mp3
    │   │           ├── Towns.mp3
    │   │           ├── Tulla.mp3
    │   │           ├── Vendegoth.mp3
    │   │           ├── Villages.mp3
    │   │           ├── Void.mp3
    │   │           └── Wilderness.mp3
    │   ├── Arcanum.PATCH0
    │   ├── Arcanum.dat
    │   └── Vormantown.dat
    └── tig.dat
    ```
</details>

### Windows

Download and copy `arcanum-ce.exe` to your `Arcanum` folder. It serves as a drop-in replacement for `arcanum.exe`.

### Linux

- Use the Windows installation as a base - it contains the data assets needed to play. Copy the `Arcanum` folder somewhere, for example `/home/john/Desktop/Arcanum`.

- Alternatively, you can extract the required files from the GOG installer:

```console
$ sudo apt install innoextract
$ innoextract ~/Downloads/setup_arcanum.exe -I app
$ mv app Arcanum
```

- Download and copy `arcanum-ce` to this folder.

- Run `./arcanum-ce`.

### macOS

> [!NOTE]
> macOS 10.13 (High Sierra) or higher is required. Runs natively on Intel-based Macs and Apple Silicon.

- Use the Windows installation as a base - it contains the data assets needed to play. Copy the `Arcanum` folder somewhere, for example `/Applications/Arcanum`.

- Alternatively, if you have Homebrew installed, you can extract the required files from the GOG installer:

```console
$ brew install innoextract
$ innoextract ~/Downloads/setup_arcanum.exe -I app
$ mv app /Applications/Arcanum
```

- Download and copy `Arcanum Community Edition.app` to this folder.

- Run `Arcanum Community Edition.app`.

### Android & iOS

These ports are not currently intended for players. Touch controls are not yet implemented, and window management is subpar. No further instructions will be provided until these issues are resolved (but you can easily figure it out, it's not rocket science).

## Building from source

Check [`ci-build.yml`](.github/workflows/ci-build.yml) for details on how the project is compiled.

## Configuration

Several configuration options are available as command-line switches (admittedly not very user-friendly):

- `-4637`: Enable cheat level 3

- `-window`: Run in windowed mode (default is fullscreen)

- `-geometry=1280x720`: Set window size (default is 800x600)

When `HighRes/config.ini` from the Unofficial Arcanum Patch is present in the game directory, Community Edition also imports `Width`, `Height`, `Windowed`, `ShowFPS`, `ScrollFPS`, `ScrollDist`, `Logos`, and `Intro` at startup.

## Contributing

Play the game and file bugs if any (there are likely many). Attach a save game for investigation. Suggestions for quality of life improvements are also welcome. The major objective for 25H2 is to clarify remaining functions.

## License

The source code is this repository is available under the [Sustainable Use License](LICENSE.md).
