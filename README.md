# ServerUtility — StarRupture Server Plugin

Adds remote administration and server management features to a [StarRupture](https://store.steampowered.com/app/1631270/StarRupture/) dedicated server.

**Target:** Dedicated server only

---

## Features

### Command-Line Server Settings
Pass server configuration directly on the command line, bypassing `DSSettings.txt` entirely. The save game name is always `AutoSave0.sav`, and `StartNewGame` / `LoadSavedGame` are set automatically based on whether an existing save is found.

| Parameter | Required | Description |
|---|---|---|
| `-SessionName=<name>` | Yes | Session/server name |
| `-SaveGameInterval=<seconds>` | No | Autosave interval (default: `300`) |
| `-RconPort=<port>` | No | TCP/UDP port for RCON and Steam Query (default: `27015`) |
| `-RconPassword=<password>` | No | Password for RCON authentication |

**Example:**
```
StarRuptureGameSteam-Win64-Shipping.exe -SessionName="My Server" -SaveGameInterval=600 -RconPort=27015 -RconPassword=secret
```

### Source RCON
Authenticated remote command execution over TCP, compatible with any standard RCON client. RCON is disabled if `-RconPassword=` is not provided.

See [RCON_README.md](src/RCON_README.md) for supported commands and client compatibility.

### Steam A2S Query
UDP server browser integration exposing player counts, server name, and map info to tools that support the Steam query protocol.

### Remote Vulnerability Patch
Blocks a known exploit in the game's built-in HTTP server that allows unauthenticated remote code execution. Enabled by default.

See the [vulnerability announcement](https://wiki.starrupture-utilities.com/en/dedicated-server/Vulnerability-Announcement) for details.

### Admin API

An HTTP JSON API for remote server administration. Enabled by setting `AdminPanelEnabled=1` and `ApiKey=<your-key>` in `ServerUtility.ini`. All endpoints are `POST`, all bodies are JSON, and all protected endpoints require a `session_token` obtained from `/auth/login`.

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

Ban a player by index. Kicks them immediately and appends their IP to `Plugins\ServerUtility\bans.txt`.

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

All endpoints return `{"ok": false, "error": "<reason>"}` on failure, with an appropriate HTTP status code (`400` for bad input, `401` for missing/invalid session token, `404` for unknown route).

---

## Installation

1. Download the latest release ZIP from the [Releases](../../releases) page:
   - `ServerUtility-Server-*.zip`

2. Extract into your game's `Binaries\Win64\` folder. The ZIP contains a `Plugins\` folder — it will sit alongside your existing `dwmapi.dll`.

3. After the first launch, edit `Plugins\config\ServerUtility.ini` and set `Enabled=1`.

> **Requires [StarRupture-ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader)** to be installed first.

---

## Troubleshooting

| Problem | Solution |
|---|---|
| RCON won't start | Both `-RconPort=` and `-RconPassword=` must be provided on the command line. |
| Server settings not applying | Ensure `-SessionName=` is present — this is what activates command-line mode. |
| Plugin not loading | Check `modloader.log` in `Binaries\Win64\` for errors. |

---

## Building from Source

Requires Visual Studio 2022 and the [StarRupture-Plugin-SDK](https://github.com/AlienXAXS/StarRupture-Plugin-SDK).

Clone the repo, open `ServerUtility.sln`, and build the `Server Release|x64` configuration. The output DLL will be placed in `build\Server Release\Plugins\`.

---

## Disclaimer

Use at your own risk. The authors are not responsible for any damage caused by using this software.
