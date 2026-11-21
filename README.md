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

### Removing players on an old mod loader (not active yet)

The groundwork is in place for a server setting that removes a player whose mod loader is older than a version you choose, and tells them why. A player without the mod loader is never affected. Nothing is removed yet: the setting, and the mod loader version that reports the player's version, come in a later release. Until then the log shows `[HostRules] This mod loader has no host rules support - minimum version removal is off`.

The removal sends the player to the main menu with a short written reason (whether the game displays that text has not been confirmed yet), removes each player at most once per join, and will skip players the host marks as exempt once the host's player list exists.

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

List all currently connected players with their index, name, ping, and IP.

```bash
curl -s -X POST http://localhost:7777/ServerUtility/api/players/list \
  -H "Content-Type: application/json" \
  -d '{"session_token": "abc123..."}'
```

Response:
```json
{"ok": true, "players": [{"index": 0, "name": "Alice", "ping": 42, "ip": "192.168.1.10"}]}
```

##### `POST /players/kick`

Kick a player by their index from `/players/list`.

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

#### Error responses

All endpoints return `{"ok": false, "error": "<reason>"}` on failure, with an appropriate HTTP status code (`400` for bad input, `401` for missing/invalid session token, `404` for an unknown `players` or `wave` route, `403` for a non-`POST` request).

---

## Installation

1. Download the latest release ZIP from the [Releases](../../releases) page:
   - `ServerUtility-Server-*.zip`

2. Extract into your game's `Binaries\Win64\` folder. The ZIP contains a `Plugins\` folder — it will sit alongside your existing `dwmapi.dll`.

3. After the first launch, edit `Plugins\config\ServerUtility.ini` and set `Enabled=1` in the `[General]` section.

> **Requires [StarRupture-ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader) v1.25.0 or newer** to be installed first.

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
