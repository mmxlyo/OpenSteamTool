<div align="center">
  <img src="docs/logo-animated.svg" width="180" alt="OpenSteamTool logo">

  <h1>OpenSteamTool</h1>

  <p>
    <strong>Open-Source Steam Unlock Tool</strong>
  </p>

  <p>
    <img src="https://img.shields.io/badge/C%2B%2B-20%2B-2ea44f?logo=cplusplus&logoColor=white" alt="C++ 20+">
    <img src="https://img.shields.io/badge/CMake-3.20%2B-2ea44f?logo=cmake&logoColor=white" alt="CMake 3.20+">
    <img src="https://img.shields.io/badge/Windows-only-d73a49?logo=windows&logoColor=white" alt="Windows only">
    <a href="https://deepwiki.com/OpenSteam001/OpenSteamTool">
      <img src="https://deepwiki.com/badge.svg" alt="Ask DeepWiki">
    </a>
  </p>

  <p>
    <a href="README.md">
      <img src="https://flagcdn.com/w40/us.png" width="22" alt="United States flag">
      English
    </a>
    &nbsp;|&nbsp;
    <a href="README_ES.md">
      <img src="https://flagcdn.com/w40/es.png" width="22" alt="Spain flag">
      Español
    </a>
    &nbsp;|&nbsp;
    <a href="README_ZH.md">
      <img src="https://flagcdn.com/w40/cn.png" width="22" alt="China flag">
      中文
    </a>
  </p>
</div>

## Features

### Core Unlocks & Manifest Management
- Unlock unowned games and DLCs without limits
- Auto-load depot decryption keys and PICS access tokens (`addtoken`) from Lua configs
- Auto-download manifests via `manifestdex` (default), `opensteamtool`, `steamrun`, `wudrm`, or custom Lua endpoints
- Lock manifest versions to prevent game updates
- Recursive multi-level subdirectories in `config/lua/` with automatic `.manifest` mirroring to `Steam/depotcache/`

### Hot Reload
- Watched `.lua` directories reload additions and modifications instantly without restarting Steam

### Game Process Injection
- Inject third-party DLLs into game processes via `[[inject]]` in `opensteamtool.toml`
- Supports multiple DLL rules, command-line filtering, and AppID restrictions (see [Third-party DLL injection](#third-party-dll-injection))

### Family Sharing Support
- Automatically unlocks Family Sharing restrictions with zero configuration and no conflicts

### Cloud Save Redirection (CloudRedirect)
- Integrates [CloudRedirect](https://github.com/Selectively11/CloudRedirect) to enable cloud saves, playtime tracking, and achievement sync for OST-managed games (see `[cloud]` in [Configuration](#configuration-optional))

### Denuvo & SteamStub Compatibility
- **SteamStub**: AppID spoofing via local ConfigStore tickets without game process injection
- **Explicit Tickets**: Memory-only management via `setAppTicket` / `setETicket` in Lua; no disk `.bin` files generated
- **Account Switching Offline Auth**: Auto-syncs credentials directly into `<AppId>.lua` (via `setAppTicket`) after running on a genuine account; switch to unowned accounts for offline play
- **Manifest Locking & Updates**: Defaults to active `setManifestid` to lock versions; set `lock_manifest = false` in config or pass `-nodenuvo` (or `nodenuvo(appid)`) to allow official updates
- **Helper Flags**:
  - `-d+`: Steam launch option on genuine accounts to auto-generate `<AppId>.lua` and lock manifests
  - `-forcedenuvo`: Force treat game as Denuvo-protected (or `forcedenuvo(appid)`)

### Extracting Tickets & Config with `extract_tickets`
Extracts AppTicket, ETicket, DLC lists, depot keys, manifests, and generates a ready-to-use `<appid>.lua` on accounts owning the game.

* **Download**: [GitHub Actions Tools Workflow](https://github.com/mmxlyo/OpenSteamTool/actions/workflows/tools.yml) or build locally via `build.bat` (`build/tools/<Config>/extract_tickets.exe`)
* **Usage**:
  ```powershell
  # Standard extraction (locally installed game)
  extract_tickets.exe 1361510

  # Force ETicket extraction for uninstalled game (restart Steam if button hangs)
  extract_tickets.exe 1361510 --force-eticket
  ```

### Stats and Achievements
- Enable stats and achievements for unowned games
- Priority: Lua `setStat(appid, "steamid")` > stats API (`https://stats.opensteamtool.com/{appid}`) > default SteamID (`76561198028121353`)

### Online Fix
- Add `-onlinefix` to Steam launch options for 480 (Spacewar) multiplayer with automatic real AppID and save protection (one active game at a time)
- Rare titles requiring 480 certificate match can use `-onlinefix -p2pflip` (use only if necessary due to compatibility risks)

---

## Usage

### Method 1: Portable Mode (Recommended)
No DLLs in the Steam directory; runs completely independently:
1. Extract the release package (with `ost-Injector.exe`, `OpenSteamTool.dll`, etc.) to any standalone folder (e.g. `D:\OpenSteamTool_Portable`)
2. Create `config/lua/` and add your `.lua` unlock scripts; `opensteamtool.toml` can be placed directly in this portable folder
3. Launch options:
   - **Manual**: Run `ost-Injector.exe` to detect or launch Steam and inject
   - **Auto-start**: Run `CreateAutoInjectTask.bat` as administrator (uninstall via `DeleteAutoInjectTask.bat`)
   - **CLI**: Supports `-watch` (daemon) and `-silent` (one-shot injection)

### Method 2: Standard Mode (DLL Hijacking)
1. Copy `dwmapi.dll`, `xinput1_4.dll`, and `OpenSteamTool.dll` to the Steam root directory
2. Create `config/lua/` in the Steam root directory and place your Lua scripts there

---

## Lua Configuration Example

```lua
addappid(1361510) -- unlock game
addappid(1361511, 0, "5954562e7f5260400040a818bc29b60b335bb690066ff767e20d145a3b6b4af0") -- unlock depot with key
addtoken(1361510, "2764735786934684318") -- add PICS access token

setManifestid(1361511, "5656605350306673283") -- pin depot manifest
setManifestid(1361511, "5656605350306673283", 12345678) -- pin depot manifest with size

setAppTicket(1361510, "0100000000000000...") -- memory AppTicket
setETicket(1361510, "0100000000000000...")   -- memory ETicket
setStat(1361510, "76561197960287930")        -- achievement source SteamID

addprocess(1361510, "CustomGame.exe")        -- map AppID for processes without SteamAppId env
seteticketurl("https://example.com/eticket") -- online ETicket minting endpoint (optional)

nodenuvo(1361510)    -- bypass Denuvo handling (same as -nodenuvo, alias: disallowdenuvo)
forcedenuvo(1361510) -- force treat as Denuvo (same as -forcedenuvo)
```
All function names are **case-insensitive**.

---

## Configuration (optional)

Rename `opensteamtool.example.toml` to `opensteamtool.toml` and place it in the portable directory or Steam root. Hot-reloaded on changes.

```toml
[log]
# Debug build only: trace, debug, info, warn, error
level = "info"

[manifest]
# Upstream API: "manifestdex" (default), "opensteamtool", "steamrun", "wudrm"
url = "manifestdex"
timeout_resolve_ms = 5000
timeout_connect_ms = 5000
timeout_send_ms    = 10000
timeout_recv_ms    = 10000

[stats]
# Query stats API when setStat is absent
enable_api = true

[lua]
# Extra Lua directories to load (optional)
paths = []

[cloud]
# Steam Cloud redirection via CloudRedirect companion app
enabled = false
# library = "cloud_redirect.dll"

# Optional game DLL injection (array of tables, multiple allowed)
[[inject]]
path = "OnlineFix.dll"
when_cmdline = "-onlinefix"
when_appids = [1361510]
all_games = false

[remote]
# Optional pattern metadata mirror (default GitHub with jsDelivr fallback)
# url_template = "https://your.server/{channel}/{component}/{sha256}.toml"

[denuvo]
# Lock manifests on account switching or -d+ (default true)
lock_manifest = true
```

### Third-party DLL injection
| Key | Explanation |
| :--- | :--- |
| `path` | DLL to load. Bare file names resolve next to toml, DLL, or steam.exe; absolute paths used as-is |
| `when_cmdline` | Optional. Substring required in launch command line |
| `when_appids` | Optional. Restrict to specific AppIDs |
| `all_games` | Optional. `false` (default) only injects Lua games; `true` injects all games |

---

## Debug logging

Debug builds write module logs under `<Steam or Portable Dir>/opensteamtool/`:

| File | Source | Content |
| :--- | :--- | :--- |
| `main.log` | General | Init, config loading, Lua parsing |
| `ipc.log` | `LOG_IPC_*` | IPC commands, interface dispatch, spoofing |
| `netpacket.log` | `LOG_NETPACKET_*` | Network packet interception, eMsg dispatch |
| `manifest.log` | `LOG_MANIFEST_*` | Manifest downloads, depotcache mirroring |
| `decryptionkey.log` | `LOG_DECRYPTIONKEY_*` | Depot decryption key injection |
| `keyvalue.log` | `LOG_KEYVALUE_*` | KeyValues manifest patching |
| `misc.log` | `LOG_MISC_*` | Engine pointer capture, AppID mapping |
| `achievement.log` | `LOG_ACHIEVEMENT_*` | Stats and achievement handling |
| `pics.log` | `LOG_PICS_*` | PICS access token injection |
| `package.log` | `LOG_PACKAGE_*` | Package 0 licenses and dynamic revocation |
| `onlinefix.log` | `LOG_ONLINEFIX_*` | 480 online fix and AppID protection |
| `richpresence.log` | `LOG_RICHPRESENCE_*` | Rich Presence packet injection |
| `steamui.log` | `LOG_STEAMUI_*` | SteamUI state synchronization |
| `inject.log` | `LOG_INJECT_*` | Third-party DLL injection matching & results |
| `pipe.log` | `LOG_PIPE_*` | Pipe handshakes, Denuvo authorization |
| `platform.log` | `LOG_PLATFORM_*` | Platform helpers and remote injection |

---

## Build

### Requirements
- Windows 10/11
- CMake 3.20+
- Visual Studio 2022 with MSVC (x64 toolchain)

### Quick build
```powershell
build.bat
```

### Output
* **Core Components** (in `build/Release/` or `build/Debug/`):
  - `OpenSteamTool.dll`, `dwmapi.dll`, `xinput1_4.dll`, `ost-Injector.exe`, and helper scripts
* **Standalone Tool** (in `build/tools/Release/` or `build/tools/Debug/`):
  - `extract_tickets.exe` (excluded from default release archive, explicitly built by `build.bat`)

---

## Acknowledgments
Special thanks to upstream, contributors, and open-source projects:
- [OpenSteam001/OpenSteamTool](https://github.com/OpenSteam001/OpenSteamTool) — Upstream project foundation
- [Selectively11/CloudRedirect](https://github.com/Selectively11/CloudRedirect) — Steam Cloud redirection engine
- [Berkecann](https://github.com/Berkecann) — Contributed the default ManifestDeX manifest provider ([PR #200](https://github.com/OpenSteam001/OpenSteamTool/pull/200))
- [microsoft/Detours](https://github.com/microsoft/Detours) — Binary API hooking library
- [marzer/tomlplusplus](https://github.com/marzer/tomlplusplus) — Header-only TOML parser
- [gabime/spdlog](https://github.com/gabime/spdlog) — Fast C++ logging library

---

## Disclaimer
This project is provided for research and educational purposes only. You are responsible for complying with local laws, platform terms of service, and software licenses.
