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

### Admin API

An HTTP JSON API for remote server administration.

> **The admin panel is currently disabled in this build.** The code that starts it is commented out in `src/plugin.cpp`, so setting the keys below does nothing yet. The rest of this section describes how it is meant to work once it is switched on.

To enable it, set `Enabled=1` and `ApiKey=<your-key>` in the `[AdminPanel]` section of `ServerUtility.ini`. The key must be at least 16 characters, or the panel does not start and the log says so. `SessionExpiry` (seconds, default `3600`, between `60` and `86400`) sets how long a session token lasts. All endpoints are `POST`, all bodies are JSON, and all protected endpoints require a `session_token` obtained from `/auth/login`.

Base URL: `http://<server-ip>:<game-http-port>/ServerUtility/api`

#### Authentication

##### `POST /auth/login`

Exchange your API key for a session token. After 5 wrong keys within 60 seconds, every login is refused with `429` until the 60 seconds are up. The count is shared by everyone who calls the panel, because the request does not say where it came from.

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

List all currently connected players with their index, name, ping, IP (empty in this build) and player ID. `id` is the player's ID.

```bash
curl -s -X POST http://localhost:7777/ServerUtility/api/players/list \
  -H "Content-Type: application/json" \
  -d '{"session_token": "abc123..."}'
```

Response:
```json
{"ok": true, "players": [{"index": 0, "name": "Alice", "ping": 42, "ip": "", "id": "76561198000000001"}]}
```

##### `POST /players/kick`

Kick a player. Send `player_id` (the `id` from `/players/list`) to name the player by ID. Without it, send `player_index` and `player_name` from the list: the name must belong to exactly one connected player, at that index, or the request fails ("player not found", or "more than one connected player has that name, send player_id"). `/players/ban` and `/inventory/give` name the player the same way. A `player_index` without a name is refused.

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

Ban a player (named as for kick). Kicks them immediately and appends their player ID and name to `Plugins\ServerUtility\bans.txt` (and the IP, when it is known). **Nothing reads `bans.txt`, so a ban is only a kick plus a record: the player can rejoin.** If the record cannot be written the player is still kicked and the reply is `{"ok": false, "error": "kicked, but the ban could not be recorded"}`. There is no unban.

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
{"ok": true, "added": 1}
```

`added` is the number of items put into the player's inventory. A give that adds nothing (for example, a full inventory) returns `{"ok": false, "error": "..."}` instead.

---

#### Error responses

All endpoints return `{"ok": false, "error": "<reason>"}` on failure, with an appropriate HTTP status code (`400` for bad input, `401` for missing/invalid session token, `404` for an unknown `players` or `wave` route, `403` for a non-`POST` request, `429` after too many wrong keys).

---

## Installation

1. Download the latest release ZIP from the [Releases](../../releases) page:
   - `ServerUtility-Server-*.zip`

2. Extract into your game's `Binaries\Win64\` folder. The ZIP contains a `Plugins\` folder — it will sit alongside your existing `dwmapi.dll`.

3. After the first launch, edit `Plugins\config\ServerUtility.ini` and set `Enabled=1` in the `[General]` section.

> **Requires [StarRupture-ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader)** to be installed first.

> **Needs ModLoader 1.27.0 or newer (plugin interface 79).** Older loaders refuse to load this build of ServerUtility and say a newer loader is needed.

ServerUtility takes its game addresses and member offsets from the mod loader's version object map (the version map), so the same DLL runs on every server build the version map covers. The plugin has no values of its own. If the version map has no row for one of the engine members behind a single feature (the player list, admin player actions, the gatherable relevancy fix, the RCON `save` command or player lookup), that feature is switched off and the log says so. If it has no row for anything else the plugin needs, the plugin is refused. After a game update, update the mod loader to get the new rows.

On a dedicated server, keep the mod loader up to date. It carries the version map rows for each server build.

---

## Troubleshooting

| Problem | Solution |
|---|---|
| RCON won't start | Both `-RconPort=` and `-RconPassword=` must be provided on the command line. |
| Server settings not applying | Ensure `-SessionName=` is present — this is what activates command-line mode. |
| Plugin not loading | Check `ModLoader.log` in `Binaries\Win64\ModLoader\Logs\` for errors. |
| A feature is off after a game update | If the loader's version map has no row for one of a feature's engine members, `ModLoader.log` has `The loader has no row for one of its engine members on this game build - <feature> is off` and only that feature is off. Update the mod loader. |
| Plugin refused after a game update | If the loader's version map has no entry for any other game address or offset the plugin needs, the plugin is refused. `ModLoader.log` then has `[HookScan] 'ServerUtility' will NOT be loaded -- PluginInit will not be called.`, with the missing entry and the reason listed above it. Update the mod loader. |
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
