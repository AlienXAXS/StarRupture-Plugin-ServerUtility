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

The host can set a minimum mod loader version (`hostrules minversion`) and a minimum version for one mod (`hostrules modmin <plugin> <version|wire>`). A player whose mod loader or mod is older is removed: they go to the main menu with a short written reason, for example "This server needs ModLoader ... or newer". Each player is removed at most once per join. **Nothing is removed while both minimums are off**, which is the default.

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
| `Sections=<key>;<key>` | Rule sections that apply to this player. A key may not hold control characters, quotes or `\| = ; [ ]`, or start with `#`; such a key is refused. A mod name with a space goes in quotes around the whole value: `Sections="Best Builder.some_feature;Other"`. |

Host rules are applied by each player's own game, so they keep honest players in line but are not protection: a modified client can ignore them, and it can also claim another player's ID, so anyone who knows a player's Steam ID can use the exemptions on that player's `[Players]` entry. Other sections and comments in `HostRules.ini` are kept when the plugin saves the file. After editing the file by hand, run `player reload`.

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

Every player with the mod loader is asked once per server run while there is no admin, so the host should be ready to tell the first player the code. A player without the mod loader, or with an older one, is never asked. **The prompt needs a mod loader with plugin interface 79 or newer on both the server and the player's side.** On a mod loader without the prompt the claim is off and `admin code` says to use `admin add`.

To turn asking off, set `AdminPrompt=0` in `[Policy]` (`off`, `false` and `no` also work). The code is still shown to the host and `admin add` still works.

```
[Policy]
AdminPrompt=0
```

### In-game admin

The in-game admin is a window in the admin's own game. It shows the server's rules, version minimums, mods and players, and changes them: allow or deny a mod feature, set the default and the message, set the minimums, flag or remove a player, and kick a player with a reason. RCON keeps working as before. Players without the client, and players who are not admins, are unaffected.

Setup, once per admin:

1. Build the client configuration (`Client Release`) and put its `ServerUtility.dll` in the Plugins folder of the admin's game, like any other client plugin. It needs a mod loader with plugin interface 80 or newer <!-- update to the loader release number at release -->. The client declares plugin interface 80, so on an older loader it does not load, and the loader's log says why. The server DLL is unchanged and still loads on older loaders. Feature titles and mod versions in the window come from the admin's own loader; the client sends them to the server once per proven connection, and the server remembers them.
2. Join the server. The host runs `admin pair <id|number>` in RCON, which prints an 8-character code (one use, valid 10 minutes, 5 wrong tries per connection and 5 in total, then the code is void). The code is shown only in the RCON reply and is never written to the log.
3. Press F8 to open the window and type the code. The client remembers the pairing for that server. (A player who has just claimed admin with the claim code above is paired on that same connection with no code.)

The pairing is tied to the server address the admin's game connected to, which the client reads from the loader. On a game build where the loader cannot read that address, pairing is refused. If the server's address changes, or the admin joins by a different route (server list versus direct IP), the admin pairs again. `ToggleKey` in the `[Window]` section of the client's config changes the key.

The client keeps its secret in `Plugins\ServerUtility\admin-pairing.txt`, which is as sensitive as the RCON password and should be protected like the server's config folder. `admin unpair <id|number>` forgets a pairing, and so does taking the Admin flag away. The server answers only players who have the Admin flag in the player list; anyone else gets one "no access" answer and then silence. To turn it off, set `InGameAdmin=0` in `[Policy]` of `HostRules.ini` (default on).

The admin panel has matching endpoints (`/rules/players`, below), but the panel is off in this build, so RCON is the way to manage the list for now.

How it works. Once an admin is paired and proven, the server sends the current state (rules, minimums, the mods it knows, the players) in small pieces, and sends it again when the client asks. A change the admin makes goes to the server as a batch and is applied as one save. If someone else (another admin, RCON or the panel) changed things first, the batch is refused with "Changed elsewhere, refreshed" and the admin gets the new state to apply again. After any change, from any front end, every paired admin is told to refresh. A connection may send 20 change requests a second; more are refused. The server is the authority: it reads each player's ID itself, checks the Admin flag on every message, and applies the same checks as RCON, including that the last Admin cannot be removed and an Admin cannot be kicked from the game. The client is only a window onto that. `Admin` and `ExemptVersion` players are skipped by the version removal.

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

#### Built-in default rows

ServerUtility ships with rows that deny the cheat-style features of these mods, so a server that never writes a rule still has them off for players:

| Mod | Sections |
|---|---|
| BetterCheats | `stack-size`, `player-stats`, `movement`, `weapons`, `grenade-probe`, `mining-tools`, `inventory`, `item-spawner`, `skills`, `build-free`, `build-unlock`, `build-anywhere`, `deconstruct-windows`, `enemies`, `machine-power`, `corporations`, `rupture-wave` |
| Best Builder | `build-anywhere`, `deconstruct-windows`, `no-build-cost`, `no-stability-check`, `unlock-all-recipes`, `unlock-all-buildings` |
| CameraControls | `vitals` |
| WaveOptimiser | `heat-tools` |

They are built into the plugin, so an existing `HostRules.ini` gets them too, and they apply on a server only (to remote players): single player and a listen host's own player see no change. `hostrules` lists them marked `(default)`. Admins, and any player with `ExemptRules`, are exempt like they are from every rule.

Precedence: a `[Rules]` line that names the exact section (`Plugin.section`, `allow` or `deny`) replaces the default. A default beats `Plugin.*` and `[Policy] Default=allow`, so `BetterCheats.*=allow` does not open it: the host names the section. To open one:

```
[Rules]
BetterCheats.stack-size=allow
```

or `hostrules allow BetterCheats.stack-size`. A player's own `Sections=` key opens a default row only when it names the exact section (`BetterCheats.stack-size`); `BetterCheats.*` does not. Mod names are matched without regard to case; section ids must match exactly. In the in-game admin window (F8, Mods tab) a default-denied section reads `Deny (default)`; allowing it there writes the exact `[Rules]` line, and setting it back to Default removes that line so the default applies again. The server tells the window which sections are defaults; against a server without this, the window shows none.

Easiest is RCON (see [RCON_README.md](src/RCON_README.md) for every verb):

```
hostrules deny BetterCheats.movement
hostrules message Some mods are off here
hostrules default allow
hostrules unset BetterCheats.movement
hostrules deny "Best Builder.some_feature"
hostrules modmin "Best Builder" 1.2.0
```

ServerUtility remembers every mod feature a player's mod loader has reported, in `Plugins\ServerUtility\known-features.txt` (plugin, newest version seen, feature ID, title, the game builds that reported it off, the date it was last seen, and where the row came from). The file is read at start and rewritten, at most once every 10 seconds, only when something changed. `hostrules` lists the remembered features with each row's source and warns about a rule that matches none of them, so a mod appears here after one player with it has joined and stays after a restart. The panel's `/rules/catalog` returns the same list as `known`.

A mod name with a space, such as `Best Builder`, goes in double quotes over RCON (`"Best Builder.some_feature"` or `"Best Builder"`). In `HostRules.ini` it is written as it is: `Best Builder.some_feature=deny` under `[Rules]`, `Best Builder=0.2.0` under `[MinVersions]`. A mod name is letters, digits, `_` and `-`, with single spaces between words, up to 64 bytes.

After editing the file by hand, run `hostrules reload`. Run `player reload` (or `hostrules reload`) before changing players with RCON or the panel, or the hand edit to `[Players]` is overwritten. A line the plugin does not understand is logged, left in the file and ignored.

What works today and what does not:

- **Rules (`allow`, `deny`, `default`, `message`)** are saved and sent to the mod loader. This is the part that takes effect, on a mod loader that supports it.
- **`minversion` and `modmin`** (`modmin <plugin> wire` uses the mod loader's wire check instead of a version) are saved, sent to the mod loader and enforced: a player below a minimum is removed (see "Removing players on an old mod loader" above).
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

List all currently connected players with their index, name, ping, IP (empty in this build) and player list status. `id` is the player's ID; `label`, `admin`, `exempt_version`, `exempt_rules` and `sections` come from the player list (see above) and are empty or `false` for a player who has no entry.

```bash
curl -s -X POST http://localhost:7777/ServerUtility/api/players/list \
  -H "Content-Type: application/json" \
  -d '{"session_token": "abc123..."}'
```

Response:
```json
{"ok": true, "players": [{"index": 0, "name": "Alice", "ping": 42, "ip": "", "id": "76561198000000001", "label": "Alice", "admin": true, "exempt_version": true, "exempt_rules": true, "sections": []}]}
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

##### The host rules

These routes make the same changes as the `hostrules` RCON command. Every one is a `POST` with `session_token`.

| Route | Body | What it does |
|---|---|---|
| `/rules/catalog` | none | The mods and features that connected players reported, and how many players listed each |
| `/rules/get` | none | The current rules and minimums |
| `/rules/set` | `key`, `value` | `key` is `Plugin.feature` (or `Plugin.*`); `value` is `allow`, `deny` or `unset` |
| `/rules/policy` | `default` and/or `message` | `default` is `allow` or `deny`; `message` is the line players see (`off` clears it) |
| `/rules/minversion` | `version` | Minimum mod loader version (`off` clears it) |
| `/rules/modmin` | `plugin`, `version` | Minimum version of one mod (`wire` or `off` also work). With no `version` it only reads |

Every answer carries `revision` (a number that goes up with each change made by RCON, the panel or an in-game admin) and `state` (the rules after the call). A change can carry `base_revision`, the `revision` the page last saw. If something else changed the rules since, the call is refused with `409` and `{"ok": false, "error": "Changed elsewhere, refreshed", ...}`, and `state` holds the current rules so the page can redraw. Leave `base_revision` out to skip the check.

```bash
curl -s -X POST http://localhost:7777/ServerUtility/api/rules/set \
  -H "Content-Type: application/json" \
  -d '{"session_token": "abc123...", "key": "BetterCheats.movement", "value": "deny", "base_revision": 12}'
```

Response:
```json
{"ok": true, "text": "Saved.", "revision": 13, "state": {"default": "allow", "message": "", "rules": [{"key": "BetterCheats.movement", "value": "deny"}], "min_loader": "", "mod_mins": [], "loader_receives": true, "push_status": "...", "minimums_status": "..."}}
```

`/rules/players` accepts `base_revision` the same way.

---

#### Error responses

All endpoints return `{"ok": false, "error": "<reason>"}` on failure, with an appropriate HTTP status code (`400` for bad input, `401` for missing/invalid session token, `404` for an unknown `players`, `wave` or `rules` route, `409` when `base_revision` is out of date, `403` for a non-`POST` request).

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
