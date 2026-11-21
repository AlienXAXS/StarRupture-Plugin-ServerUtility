# ServerUtility — StarRupture Server Plugin

Adds remote administration and server management features to a [StarRupture](https://store.steampowered.com/app/1631270/StarRupture/) dedicated server.

**Target:** Dedicated server only

---

## Features

### Command-Line Server Settings
Pass server configuration directly on the command line, bypassing `DSSettings.txt` entirely. The save game name is always `AutoSave0.sav`, and `StartNewGame` / `LoadSavedGame` are set automatically based on whether an existing save is found.

| Parameter | Required | Description |
|---|---|---|
| `-SessionName=<name>` | Yes, for command-line settings | Session/server name. Without it the game reads `DSSettings.txt` as usual |
| `-SaveGameInterval=<seconds>` | No | Autosave interval (default: `300`) |
| `-RconPort=<port>` | Only for RCON | TCP/UDP port for RCON and Steam Query. No default: RCON does not start without it |
| `-RconPassword=<password>` | Only for RCON | Password for RCON authentication |

**Example:**
```
StarRuptureGameSteam-Win64-Shipping.exe -SessionName="My Server" -SaveGameInterval=600 -RconPort=27015 -RconPassword=secret
```

### Source RCON
Authenticated remote command execution over TCP, compatible with any standard RCON client. RCON is disabled unless both `-RconPort=` and `-RconPassword=` are provided.

See [RCON_README.md](src/RCON_README.md) for supported commands and client compatibility.

### Steam A2S Query
UDP server browser integration exposing player counts, server name, and map info to tools that support the Steam query protocol.

### Remote Vulnerability Patch
Blocks a known exploit in the game's built-in HTTP server that allows unauthenticated remote code execution. Enabled by default.

See the [vulnerability announcement](https://wiki.starrupture-utilities.com/en/dedicated-server/Vulnerability-Announcement) for details.

### Removing players on an old mod loader

The host can set a minimum mod loader version (`hostrules minversion`) and a minimum version for one mod (`hostrules modmin <plugin> <version>`). A player whose mod loader or mod is older is removed: they go to the main menu with a short written reason, for example "This server needs ModLoader ... or newer". Each player is removed at most once per join. **Nothing is removed while both minimums are off**, which is the default.

Exempt: a player with `ExemptVersion` or `Admin` in the player list (below) is kept, and the log says `Player 'X' is below a minimum version but exempt - kept`. Never removed: a player without the mod loader (vanilla), a player on a development build, and a player who does not have the named mod, because the mod loader reports none of them. A removal is logged as `[HostRules] Player 'X' removed: <reason>`.

The minimums are sent to the server's mod loader when the server starts and after every `hostrules` change. `hostrules` shows `version minimums: sent`, or `not sent` with the reason (an old mod loader without host rules support, or a minimum the loader refused). `loader` cannot be used as a mod name in `hostrules modmin`; use `hostrules minversion` for the mod loader itself.

### Player list (admins and exemptions)

The host keeps a list of players by their ID in the `[Players]` section of `Plugins\config\HostRules.ini`. One line per player:

```
[Players]
76561198000000001=Alice|Admin
76561198000000002=Bob|ExemptVersion Sections=wave;save
```

The text before `=` is the player's ID, the text after it is a label for you, and the flags follow `|`, separated by spaces:

| Flag | Meaning |
|---|---|
| `Admin` | The player is an admin. Admin also exempts them from the version removal and from the rules. |
| `ExemptVersion` | Never removed for an old mod loader. |
| `ExemptRules` | Exempt from the host's rules. |
| `Sections=<key>;<key>` | Rule sections that apply to this player. |

The server reads each player's ID from the game; the client does not send it. Whether a client could change it has not been tested, so do not treat the list as a security guarantee. Other sections and comments in `HostRules.ini` are kept when the plugin saves the file. After editing the file by hand, run `player reload`.

Easiest is RCON. `player ids` shows who is connected and the exact ID to use; `admin add 1 Alice` makes the first player in that list an admin. See [RCON_README.md](src/RCON_README.md) for the `player` and `admin` commands.

#### Claiming admin with a code

A new server has no admin. While the player list has none, ServerUtility makes a one-time claim code of 8 letters and digits and shows it to the host only in the server log, once at start and again whenever a new code is made:

```
[Admin] No admins yet. Claim code: <code> (shown only while the list is empty; RCON: admin code)
```

RCON `admin code` prints the same code. The code is kept in memory only and is never written to a file. Nothing else shows it, and the typed code is never logged.

What a player sees: when a player joins with the mod loader and the list has no admin, their mod loader asks them for the claim code. The host reads it from the log and tells them. They type it (upper or lower case, spaces around it are fine), or choose **Not me**.

- **Right code:** the player is added to `[Players]` as `Admin` with their name as the label (the same as `admin add`), the rules are sent again, and any other open prompt closes with "Another player already claimed this server."
- **Wrong code:** "That is not the code. N tries left." One player gets 5 wrong tries; after that they are not asked again until the server restarts. If 5 wrong tries pile up across all players, the code is replaced by a new one in the log and the old one stops working.
- **Not me:** closes the prompt. That player is not asked again until the server restarts.
- **When it ends:** the code is gone once the list has an admin, however that happened (claim, `admin add`, a hand edit and `player reload`). A restart makes a new code if the list is still empty.

Every player with the mod loader is asked once per server run while there is no admin, so the host should be ready to tell the first player the code. A player without the mod loader, or with an older one, is never asked. **The prompt needs a mod loader with plugin interface 79 or newer on both the server and the player's side.** On an older server loader the claim is off and `admin code` says to use `admin add`.

To turn asking off, set `AdminPrompt=0` in `[Policy]` (`off`, `false` and `no` also work). The code is still shown to the host and `admin add` still works.

```
[Policy]
AdminPrompt=0
```

The admin panel has matching endpoints (`/rules/players`, below), but the panel is off in this build, so RCON is the way to manage the list for now. `Admin` and `ExemptVersion` players are skipped by the version removal.

### Host rules: switch mod features off for players

The host can tell players' mod loaders to switch a mod's feature off. ServerUtility keeps the rules in `Plugins\config\HostRules.ini` and sends them to the server's mod loader when the server starts and each time you change them. The mod loader passes them to the players' mod loaders, which switch off what a rule denies and show your message. A player without the mod loader is not affected.

**Needs mod loader with plugin interface 79 or newer; older loaders refuse to load it and say a newer loader is needed.** On a mod loader that has no host rules support the rules are still saved, but they are not sent (the log says so, and `hostrules` shows "not sent").

A feature is named `Plugin.feature`; `Plugin.*` means every feature of one mod. `Default` decides what a feature with no rule gets.

```
[Policy]
Default=allow
Message=Some mods are off here
MinLoaderVersion=

[Rules]
BetterCheats.movement=deny
BetterDrone.*=deny

[MinVersions]
RuptureTimer=0.11.0
```

Easiest is RCON (see [RCON_README.md](src/RCON_README.md) for every verb):

```
hostrules deny BetterCheats.movement
hostrules message Some mods are off here
hostrules default allow
hostrules unset BetterCheats.movement
```

After editing the file by hand, run `hostrules reload`. A line the plugin does not understand is logged, left in the file and ignored.

What works today and what does not:

- **Rules (`allow`, `deny`, `default`, `message`)** are saved and sent to the mod loader. This is the part that takes effect, on a mod loader that supports it.
- **`minversion` and `modmin`** are saved, sent to the mod loader and enforced: a player below a minimum is removed (see "Removing players on an old mod loader" above).
- **`who <id|number>`** shows one connected player: the player's ID, how many times their mod loader reported its features and when it last did, the mod loader version and the mods with each feature on or off. A player whose mod loader sent nothing shows `no report (vanilla or older loader)`.
- **Admins and `ExemptRules` players** are exempt: the host's rules are replaced for them by a set with nothing denied (the server message still shows). A player with `Sections=` gets the host's rules with those keys set to allow. Each player is sent their own set, and a change to the rules or to the player list is applied to players already connected.

### Admin API

An HTTP JSON API for remote server administration.

> **The admin panel is currently disabled in this build.** The code that starts it is commented out in `src/plugin.cpp`, so setting the keys below does nothing yet. The rest of this section describes how it is meant to work once it is switched on.

To enable it, set `Enabled=1` and `ApiKey=<your-key>` in the `[AdminPanel]` section of `ServerUtility.ini`. `SessionExpiry` (seconds, default `3600`, between `60` and `86400`) sets how long a session token lasts. All endpoints are `POST`, all bodies are JSON, and all protected endpoints require a `session_token` obtained from `/auth/login`.

Base URL: `http://<server-ip>:<game-http-port>/ServerUtility/api`

#### Authentication

##### `POST /auth/login`

Exchange your API key for a session token.

```bash
curl -s -X POST http://localhost:7777/ServerUtility/api/auth/login \
  -H "Content-Type: application/json" \
  -d '{"api_key": "your-secret-key"}'
```

Response:
```json
{"ok": true, "session_token": "abc123..."}
```

---

#### Players

##### `POST /players/list`

List all currently connected players with their index, name, ping, IP and player list status. `id` is the player's ID; `label`, `admin`, `exempt_version`, `exempt_rules` and `sections` come from the player list (see above) and are empty or `false` for a player who has no entry.

```bash
curl -s -X POST http://localhost:7777/ServerUtility/api/players/list \
  -H "Content-Type: application/json" \
  -d '{"session_token": "abc123..."}'
```

Response:
```json
{"ok": true, "players": [{"index": 0, "name": "Alice", "ping": 42, "ip": "192.168.1.10", "id": "76561198000000001", "label": "Alice", "admin": true, "exempt_version": true, "exempt_rules": true, "sections": []}]}
```

##### `POST /players/kick`

Kick a player by their index from `/players/list`. Send `player_name` (the name the list showed) as well, and the request fails with "player not found" if that index no longer names that player. `/players/ban` and `/inventory/give` take `player_name` the same way.

```bash
curl -s -X POST http://localhost:7777/ServerUtility/api/players/kick \
  -H "Content-Type: application/json" \
  -d '{"session_token": "abc123...", "player_index": 0}'
```

Response:
```json
{"ok": true}
```

##### `POST /players/ban`

Ban a player by index. Kicks them immediately and appends their IP and name to `Plugins\ServerUtility\bans.txt`. **Nothing reads `bans.txt`, so a ban is only a kick plus a log line: the player can rejoin.** There is no unban.

```bash
curl -s -X POST http://localhost:7777/ServerUtility/api/players/ban \
  -H "Content-Type: application/json" \
  -d '{"session_token": "abc123...", "player_index": 0}'
```

Response:
```json
{"ok": true}
```

---

#### Wave Control

##### `POST /wave/status`

Get the current wave state.

```bash
curl -s -X POST http://localhost:7777/ServerUtility/api/wave/status \
  -H "Content-Type: application/json" \
  -d '{"session_token": "abc123..."}'
```

Response:
```json
{"ok": true, "in_progress": true, "paused": false, "type": 1, "stage": 2, "progress": 0.4500}
```

##### `POST /wave/start`

Start a wave of the given type.

```bash
curl -s -X POST http://localhost:7777/ServerUtility/api/wave/start \
  -H "Content-Type: application/json" \
  -d '{"session_token": "abc123...", "wave_type": 1}'
```

Response:
```json
{"ok": true}
```

##### `POST /wave/cancel`

Cancel the currently active wave.

```bash
curl -s -X POST http://localhost:7777/ServerUtility/api/wave/cancel \
  -H "Content-Type: application/json" \
  -d '{"session_token": "abc123..."}'
```

Response:
```json
{"ok": true}
```

##### `POST /wave/pause`

Pause the currently active wave.

```bash
curl -s -X POST http://localhost:7777/ServerUtility/api/wave/pause \
  -H "Content-Type: application/json" \
  -d '{"session_token": "abc123..."}'
```

Response:
```json
{"ok": true}
```

##### `POST /wave/resume`

Resume a paused wave.

```bash
curl -s -X POST http://localhost:7777/ServerUtility/api/wave/resume \
  -H "Content-Type: application/json" \
  -d '{"session_token": "abc123..."}'
```

Response:
```json
{"ok": true}
```

---

#### Inventory

##### `POST /inventory/give`

Give an item to a player. `count` defaults to `1` if omitted.

```bash
curl -s -X POST http://localhost:7777/ServerUtility/api/inventory/give \
  -H "Content-Type: application/json" \
  -d '{"session_token": "abc123...", "player_index": 0, "asset_path": "AuItemDataBase_Rifle", "count": 1}'
```

Response:
```json
{"ok": true}
```

---

#### Rules

##### `POST /rules/players`

Read or change the player list. `action` is `get`, `set` or `remove`. `set` and `remove` need `id`. `flags` (for `set`) is the same text as the `player set` flags, for example `admin label=Alice` or `-exemptversion`. The answer always lists every entry after the action.

```bash
curl -s -X POST http://localhost:7777/ServerUtility/api/rules/players \
  -H "Content-Type: application/json" \
  -d '{"session_token": "abc123...", "action": "set", "id": "76561198000000001", "flags": "admin label=Alice"}'
```

Response:
```json
{"ok": true, "entries": [{"id": "76561198000000001", "label": "Alice", "admin": true, "exempt_version": true, "exempt_rules": true, "sections": []}]}
```

---

#### Error responses

All endpoints return `{"ok": false, "error": "<reason>"}` on failure, with an appropriate HTTP status code (`400` for bad input, `401` for missing/invalid session token, `404` for an unknown `players`, `wave` or `rules` route, `403` for a non-`POST` request).

---

## Installation

1. Download the latest release ZIP from the [Releases](../../releases) page:
   - `ServerUtility-Server-*.zip`

2. Extract into your game's `Binaries\Win64\` folder. The ZIP contains a `Plugins\` folder — it will sit alongside your existing `dwmapi.dll`.

3. After the first launch, edit `Plugins\config\ServerUtility.ini` and set `Enabled=1` in the `[General]` section.

> **Requires [StarRupture-ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader)** to be installed first.

> **Host rules need a mod loader with plugin interface 79 or newer.** Older loaders refuse to load this build of ServerUtility and say a newer loader is needed.

ServerUtility takes its game addresses and member offsets from the mod loader's version object map (the version map) first, so the same DLL runs on every server build the version map covers. It also carries its own values for CL 127004, the current 0.3.5 server, so it runs there even when the version map has no entries for that build. On a newer server build it needs the version map's entries for that build. After a game update, update the mod loader to get them.

On a dedicated server, keep the mod loader up to date. It carries the version map rows for each server build.

---

## Troubleshooting

| Problem | Solution |
|---|---|
| RCON won't start | Both `-RconPort=` and `-RconPassword=` must be provided on the command line. |
| Server settings not applying | Ensure `-SessionName=` is present — this is what activates command-line mode. |
| Plugin not loading | Check `ModLoader.log` in `Binaries\Win64\ModLoader\Logs\` for errors. |
| Plugin refused after a game update | On a server build other than CL 127004, if the loader's version map has no entry for a game address or offset the plugin needs, the plugin is refused. `ModLoader.log` then has `[HookScan] 'ServerUtility' will NOT be loaded -- PluginInit will not be called.`, with the missing entry and the reason listed above it. Update the mod loader. |
| Auto-profession patch is off | Only applies when `MaxPlayers` is set in `ServerUtility.ini`. The log has an `[AutoProfession]` line; if it says `EProfessionType::Soldier` was not found, this game build has no such entry and the patch stays off. |

---

## Building from Source

Requires Visual Studio 2022 and the [StarRupture-Plugin-SDK](https://github.com/AlienXAXS/StarRupture-Plugin-SDK).

Clone the repo, open `ServerUtility.sln`, and build the `Server Release|x64` configuration. The output DLL will be placed in `build\Server Release\Plugins\`.

Only the Server configurations build. The plain `Debug` and `Release` configurations stop with an error that names the Server ones.

---

## Disclaimer

Use at your own risk. The authors are not responsible for any damage caused by using this software.

---

## Credits

Original mod by [@AlienXAXS](https://github.com/AlienXAXS).

Maintained by [@msantoro12](https://github.com/msantoro12), creator of [StarRupture Planner](https://starrupture.goodstuff.software).
