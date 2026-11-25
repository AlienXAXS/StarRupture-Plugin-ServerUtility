# ServerUtility — RCON & Steam Query Protocol

## Overview

The **ServerUtility** plugin adds remote administration capabilities to a Star Rupture dedicated server via two standard protocols:

| Protocol | Transport | Default Port | Purpose |
|---|---|---|---|
| **Source RCON** | TCP | `-RconPort=` | Authenticated remote command execution |
| **Steam A2S Query** | UDP | Same port | Server browser / status queries |

Both servers bind to the same port number (TCP and UDP respectively).

---

## Command-Line Parameters

| Parameter | Required | Description |
|---|---|---|
| `-RconPort=<port>` | Yes | TCP/UDP port for RCON and A2S query (1–65535) |
| `-RconPassword=<password>` | Yes | Password for RCON authentication |
| `-SessionName=<name>` | No for RCON | Server name shown in A2S info responses (default: `StarRupture Server`). It is required for the plugin's command-line server settings, not for RCON |

Both `-RconPort=` and `-RconPassword=` must be set for the RCON subsystem to start. If either is missing, the plugin logs an informational message and skips initialisation.

**Example launch:**
```
StarRupture-Server.exe -SessionName="My Server" -RconPort=27015 -RconPassword=mysecretpass
```

---

## Source RCON Protocol

The RCON server implements the [Source RCON protocol](https://developer.valvesoftware.com/wiki/Source_RCON_Protocol) (Valve specification). Any standard RCON client (e.g. `mcrcon`, `rcon-cli`, game server panels) should work out of the box.

### Packet Format
| Field | Size | Description |
|---|---|---|
| **Size** | 4 bytes | Number of bytes in the rest of the packet (`ID` + `Type` + `Body` + 2 null bytes). Maximum 4096. |
| **ID** | 4 bytes | Client-chosen request ID, echoed back in the response. Set to `-1` on auth failure. |
| **Type** | 4 bytes | Packet type (see below) |
| **Body** | Variable | Null-terminated ASCII string (command text or response text) |
| **Pad** | 1 byte | Always `0x00` |

### Packet Types

| Value | Direction | Name | Description |
|---|---|---|---|
| `0` | Server → Client | `SERVERDATA_RESPONSE_VALUE` | Response to a command |
| `2` | Server → Client | `SERVERDATA_AUTH_RESPONSE` | Authentication result |
| `2` | Client → Server | `SERVERDATA_EXECCOMMAND` | Execute a command |
| `3` | Client → Server | `SERVERDATA_AUTH` | Authenticate with password |

> **Note:** `SERVERDATA_EXECCOMMAND` and `SERVERDATA_AUTH_RESPONSE` share type value `2`. They are distinguished by direction (client→server vs server→client).

### Authentication Flow

1. Client connects via TCP.
2. Client sends a `SERVERDATA_AUTH` (type `3`) packet with the password in the body.
3. On success:
   - Server sends an empty `SERVERDATA_RESPONSE_VALUE` (type `0`) with the client's request ID.
- Server sends a `SERVERDATA_AUTH_RESPONSE` (type `2`) with the client's request ID.
4. On failure:
   - Server sends a `SERVERDATA_AUTH_RESPONSE` (type `2`) with ID set to `-1`.
   - Server closes the connection.

The empty `RESPONSE_VALUE` before the `AUTH_RESPONSE` follows the Source RCON specification — many clients require it and will hang without it.

### Command Execution Flow

1. Client sends a `SERVERDATA_EXECCOMMAND` (type `2`) packet with the command string in the body.
2. Server hands the line to the mod loader (`IPluginConsole::ExecuteWithEngine`), which runs it on the **game thread** exactly as if it had been typed into the server's `-console` window. The network thread blocks until the output is complete (30-second timeout).
3. Server sends a `SERVERDATA_RESPONSE_VALUE` (type `0`) packet with the command output.

### Connection Behaviour

| Setting | Value | Notes |
|---|---|---|
| Receive timeout | 5 minutes | Accommodates idle admin sessions |
| TCP keepalive | Enabled | Probes start after 60s idle, every 10s, 3 retries |
| Max packet size | 4096 bytes | Per Source RCON specification |
| Concurrency | 1 thread per client | Suitable for low-concurrency admin use |

---

## Game-Thread Dispatch

RCON has no command table of its own. Every line is passed to the mod loader, which resolves it the same way its `-console` window does:

1. **Mod loader and plugin commands** -- `help`, `plugins`, `clients`, `stop`, ..., plus anything any plugin registered (including this plugin's `players` and `save`).
2. **Engine commands** for anything else -- console variables (`CrRepGraph.SomeCVar 1`), `log <category> <verbosity>`, and the rest of what `UGameEngine::Exec` accepts.
3. A leading `!` skips step 1 and goes straight to the engine.

Every command, whichever step answers it, runs on the **game thread** during the next engine tick. The RCON client thread blocks until the loader reports the command complete, then sends back every line it wrote. If the game thread does not get to it within 30 seconds the reply ends with a timeout error (the command still runs when the tick arrives).

Engine commands return whatever they write to their output device. A command that only writes to the engine log sends back `(no output)`.

---

## Available Commands

Sending an empty command runs `help`, which lists every mod loader and plugin command. An unrecognised command is passed to the engine; if the engine does not know it either, the reply says so.

The commands below are the ones this plugin adds. They also work from the server's `-console` window. For everything else, run `help`.

### `players`

List all connected players with their ping. Reads the world when it runs, so it is accurate whether or not RCON is enabled.

```
> players
Players (2 connected):
  Player Name      Time On Server  IP Address      Latency
  ----------------------------------------------------------------------
  [1] Alice        2h 15m 30s      N/A             42 ms
  [2] Bob          0m 45s          N/A             18 ms
```

(`list` and `who` used to be aliases. They now belong to the mod loader's `plugins` and `clients` commands.)

### `player [list|ids|set|remove|reload]`

Show and edit the host's player list, which is the `[Players]` section of `Plugins\config\HostRules.ini`. It marks admins and players exempt from the version removal (`admin` and `exemptversion` players are never removed for an old mod loader or mod).

| Command | What it does |
|---|---|
| `player` or `player list` | Entries in the list, and which are connected now |
| `player ids` | Connected players with their numbers and exact IDs |
| `player set <id\|number> <flag>...` | Add an entry or change one |
| `player remove <id\|number>` | Delete an entry |
| `player reload` | Re-read the file after a hand edit |

Flags for `set`: `admin`, `exemptversion`, `exemptrules`, `sections=<key>;<key>`, `label=<text>` (the label runs to the end of the line). A leading `-` takes a flag off, for example `-admin`. A section key may not hold control characters, quotes or `\| = ; [ ]`, or start with `#`. For a mod name with a space, put the whole value in quotes: `player set <id|number> sections="Best Builder.some_feature;Other"`. The reply names the player the number or ID reached. After editing `HostRules.ini` by hand, run `player reload` (or `hostrules reload`) before changing players with RCON or the panel.

A 1-3 digit argument is a list number, the number `player ids` shows. Anything else is an ID.

```
> player ids
Players (2 connected):
  [1] Alice  76561198000000001
  [2] Bob    76561198000000002
> player set 2 exemptversion label=Bob
Saved for Bob (76561198000000002): 76561198000000002=Bob|ExemptVersion
> player remove 2
Removed the entry for Bob (76561198000000002).
```

### `admin add|remove|clear|code`

Shortcuts for the Admin flag, and the claim code for the first admin.

```
> admin add 1 Alice
Saved for Alice (76561198000000001): 76561198000000001=Alice|Admin
> admin remove 76561198000000001
> admin clear
Took Admin off 0 entries.
```

- `admin add <id|number> [label]` adds the player as an admin, with an optional label.
- `admin remove <id|number>` takes Admin off an existing entry; it errors if the ID has no entry.
- `admin clear` takes Admin off every entry.
- `admin code` shows the claim code while the player list has no admin. Players whose mod loader supports it (interface 79 or newer, server and player) are asked for this code when they join and become admin if they type it right. With an admin in the list it says the server already has one and there is no code. On a server loader without the prompt it says to use `admin add`.

```
> admin code
No admins yet. Claim code: <code>
```

A player gets 5 wrong tries, then is not asked again until restart; 5 wrong tries in total replace the code. "Not me" also stops the asking until restart. `[Policy] AdminPrompt=0` in `HostRules.ini` turns the asking off (the code still shows here). See the README for the full flow.

- `admin pair <id|number>` makes a one-time code (8 characters, 10 minutes, one use) for that admin's in-game admin tool and prints it in the reply only; it is never logged. The player must already have the Admin flag. 5 wrong tries on one connection, or 5 in total, void the code. `[Policy] InGameAdmin=0` turns the tool off. A paired admin's tool is told to refresh whenever the rules or player list change over RCON, so a pairing code or an RCON change shows up in the open window at once.
- `admin unpair <id|number>` forgets that admin's pairing; the tool must be paired again. Taking the Admin flag away does the same.

### `hostrules [list|allow|deny|unset|default|message|minversion|modmin|reload|known|who]`

Show and edit the host's rules for mod features: the `[Policy]`, `[Rules]` and `[MinVersions]` sections of `Plugins\config\HostRules.ini`. Each change is written to the file and sent to the mod loader straight away. The rules are sent only to a mod loader with plugin interface 79 or newer; on any other they are saved and `hostrules` says "not sent".

| Verb | What it does |
|---|---|
| `hostrules` or `hostrules list` | Show the rules, whether the mod loader received them, and the version minimums. Built-in default deny rows no rule names are listed marked `(default)` |
| `hostrules allow <Plugin.feature>` | Allow one feature (`Plugin.*` = every feature of that mod) |
| `hostrules deny <Plugin.feature>` | Switch one feature off for players |
| `hostrules unset <Plugin.feature>` | Take that rule away, so the default applies |
| `hostrules default allow\|deny` | What a feature with no rule gets |
| `hostrules message [text\|off]` | The line players see when a feature is off; with no text, shows the rules. `off` clears it |
| `hostrules minversion [version\|off]` | Minimum mod loader version. A player below it is removed. With no argument, shows the current value; `off` clears it |
| `hostrules modmin [<plugin> <version\|wire\|off>]` | Minimum version of one mod (`wire`: the copy must pass the mod loader's wire check). A player whose copy is older is removed. With no argument, lists the minimums; `off` clears one. `loader` is refused: use `minversion` |
| `hostrules reload` | Re-read the file after a hand edit |
| `hostrules known` | The mod features the server has seen players' mod loaders report (up to 30 shown), and any rule that matches none of them |
| `hostrules who <id\|number>` | What a connected player's mod loader reported: its version and each mod's features, on or off. A 1-3 digit argument is the number from `player ids`; anything else is an ID |

A mod name with a space goes in double quotes: `hostrules deny "Best Builder.some_feature"`, `hostrules modmin "Best Builder" 1.2.0`. Feature names never hold spaces. In `HostRules.ini` the name is written without quotes.

A version is `0.11.0`, `v0.11.0` or `1.27.0-dev.1`: at least major.minor, up to four numbers. A player below a minimum is sent to the main menu with the mod loader's reason (for example "This server needs ModLoader ... or newer") and the log shows `[HostRules] Player 'X' removed: <reason>`. Nothing is removed while both minimums are off. Players with `ExemptVersion` or `Admin` in the player list are kept; players without the mod loader, on a development build, or without the named mod are never reported, so never removed. The minimums are sent to the mod loader after every change; the last line of `hostrules` is `version minimums: sent`, or `not sent: <why>`.

`who` shows the player's ID, how many times their mod loader reported its features and when it last did, then the mod loader version and each mod's features. For a player whose mod loader sent nothing it shows `no report (vanilla or older loader)`; for a player who is not connected, `not connected`.

```
> hostrules deny BetterCheats.movement
Saved.
Host rules (sent):
  default: allow
  message: (none)
  BetterCheats.movement = deny
  minimum loader version: off
  minimum mod versions: none
  version minimums: sent
> hostrules message Some mods are off here
> hostrules default deny
> hostrules unset BetterCheats.movement
> hostrules minversion 1.27.0
Saved.
(then the list above, with "minimum loader version: v1.27.0" and "version minimums: sent")
> hostrules modmin RuptureTimer 0.11.0
Saved.
(the list shows "minimum RuptureTimer: 0.11.0")
> hostrules modmin RuptureTimer off
> hostrules who 1
Player 76561198000000001(Alice): the mod loader reported its features 2 time(s), last 41 s ago.
  mod loader: loader=v1.26.1 interface=79
  mods:
    BetterCheats.movement=on
    RuptureTimer.panel=off
> hostrules who 2
Player 76561198000000002(Bob): no report (vanilla or older loader)
```

### Admin panel routes for the rules

The admin panel (off in this build) makes the same changes as `hostrules`, `player` and `admin`, through the same code, so a change made by RCON, the panel or an in-game admin is checked, saved and logged the same way. The routes under `/ServerUtility/api/rules/` are `catalog`, `get`, `set` (`key`, `value`), `policy` (`default`, `message`), `minversion` (`version`), `modmin` (`plugin`, `version`) and `players`. Every answer has `revision` and `state`. A change may send `base_revision`; if the rules changed since then, the answer is `409` with `Changed elsewhere, refreshed` and the current `state`. See the Admin API section of the README for the details.

### `save` / `savegame` / `forcesave`

Force an immediate world save by calling `UCrSaveSubsystem::SaveNextSaveGame`. The save runs synchronously on the game thread; the RCON response is returned after it completes.

```
> save
World saved successfully.
```

Possible errors:
- `Error: save function not found (pattern not matched).` -- The byte-pattern scan for `SaveNextSaveGame` failed (game update may have changed the binary).
- `Error: save subsystem not available (world may not be loaded yet).` -- No `UCrSaveSubsystem` instance found (too early in startup).
- `Error: exception occurred during save.` -- SEH exception caught during the save call.
- `Error: command timed out waiting for the game thread.` -- The game thread didn't process the request within 30 seconds.

### Shutting the server down

This plugin no longer ships its own `stop`: the mod loader's `stop` (alias `shutdown`) is used instead. Without a console attached it goes through the engine's `exit` command, which runs the normal graceful save-and-shutdown path. `stop force` exits immediately and may lose the save.

The engine's own `exit` and `quit` commands also reach the engine over RCON and shut the server down.

---

## Steam A2S Query Protocol

The UDP server implements the [Steam Server Query](https://developer.valvesoftware.com/wiki/Server_queries) protocol for server browser integration.

### Supported Query Types

| Query | Request Header | Response Header | Description |
|---|---|---|---|
| **A2S_INFO** | `0x54` (`T`) | `0x49` (`I`) | Server name, map, player count, game info |
| **A2S_PLAYER** | `0x55` (`U`) | `0x44` (`D`) | Per-player name, score, and duration |
| **A2S_RULES** | `0x56` (`V`) | `0x45` (`E`) | Server rules / key-value pairs |

### A2S_INFO Response Fields

| Field | Value |
|---|---|
| Protocol | `17` |
| Server Name | From `-SessionName=` or `"StarRupture Server"` |
| Map | Current world name (e.g. `ChimeraMain`) |
| Folder | `StarRupture` |
| Game | `Star Rupture` |
| App ID | `0` |
| Players | Current connected count |
| Max Players | Server max (currently hardcoded) |
| Bots | `0` |
| Server Type | `d` (dedicated) |
| OS | `w` (Windows) |
| Visibility | `0` (public) |
| VAC | `0` (unsecured) |

### A2S_PLAYER Challenge-Response

The player query uses a challenge-response to prevent spoofed requests:

1. Client sends `A2S_PLAYER` with challenge `0xFFFFFFFF`.
2. Server responds with `S2C_CHALLENGE` (`0x41`) containing a unique 32-bit challenge number.
3. Client re-sends `A2S_PLAYER` with the received challenge.
4. Server validates and responds with the player list.

### A2S_RULES Response

Returns server key-value pairs:

| Key | Value |
|---|---|
| `world` | Current world/map name |
| `players` | Current player count |
| `maxplayers` | Maximum player count |

---

## Security — Remote Vulnerability Patch

A vulnerability in the game's built-in HTTP server allows unauthenticated remote callers to invoke arbitrary Unreal Engine remote object calls via the `/remote/object/call` endpoint. See the [vulnerability announcement](https://wiki.starrupture-utilities.com/en/dedicated-server/Vulnerability-Announcement) for full details.

ServerUtility includes a patch that intercepts this endpoint and blocks any call whose `objectPath` does not target the legitimate `DedicatedServerSettingsComp` object. Blocked requests receive an HTTP `403 Forbidden` response and are logged as warnings.

### Configuration

The patch is controlled by `RemoteVulnerabilityPatch` in the `[PluginSettings]` section of `ServerUtility.ini`:

```ini
[PluginSettings]
RemoteVulnerabilityPatch=true
```

| Value | Behaviour |
|---|---|
| `true` *(default)* | Unauthorized `/remote/object/call` requests are blocked and logged |
| `false` | No interception — the game's HTTP server handles all requests as normal |

> **It is strongly recommended to leave this enabled.** Only disable it if you have a specific reason and your server is not exposed to untrusted networks.

### What gets logged

When a request is blocked, two `WARN` lines are written to `ModLoader.log` in `Binaries\Win64\ModLoader\Logs\`:

```
[WARN] [RemoteVulnerabilityPatcher] Blocked unauthorized /remote/object/call
[WARN] [RemoteVulnerabilityPatcher]   objectPath:   '/Game/...'
[WARN] [RemoteVulnerabilityPatcher]   functionName: '...'
```

Legitimate requests (those targeting the allowed object path) pass through silently.

---

## Troubleshooting

### RCON won't start
- Check that both `-RconPort=` and `-RconPassword=` are on the command line.
- Check the mod loader log for `[Rcon]` messages — they report exactly which parameter is missing.
- Ensure the port isn't already in use by another process.

### Client can't authenticate
- Verify the password matches exactly (case-sensitive).
- Some clients send the password with trailing whitespace — check client settings.

### Commands time out
- The game thread must be running for commands to execute. If the server is frozen or in a loading screen, commands will time out after 30 seconds.
- Check the mod loader log for `[EngineTick]` messages confirming the tick hook is installed.

### Pattern scan failures
- Game updates may change function byte patterns. Check the log for `pattern not found` errors.
- Update the patterns in `scan_patterns.h` (mod loader) or the command source files (plugin) to match the new binary.

### Remote Vulnerability Patch not activating
- Check the log at startup for `[RemoteVulnerabilityPatcher] Hook installed successfully` — if absent, the pattern scan failed (game update may have changed the binary).
- If `FHttpServerResponse::Error not located` appears, the patch is still active but blocked requests will drop the connection rather than receiving a 403 response. The exploit is still blocked.

### Firewall / port forwarding
- The RCON port needs both **TCP** (RCON) and **UDP** (A2S query) forwarded.
- Windows Firewall may block the server binary — add an inbound rule for the port.

---

## Compatible RCON Clients

Any client implementing the Source RCON protocol should work, including:

- [mcrcon](https://github.com/Tiiffi/mcrcon) (CLI)
- [rcon-cli](https://github.com/gorcon/rcon-cli) (CLI)
- [RustAdmin](https://www.rustadmin.com/) (GUI, originally for Rust servers)
- [RCON Web Admin](https://github.com/rcon-web-admin/rcon-web-admin) (Web-based)
- Custom scripts using any Source RCON library (Python, Node.js, C#, etc.)
