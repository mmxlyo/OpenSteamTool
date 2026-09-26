<div align="center">
  <img src="docs/logo-animated.svg" width="180" alt="OpenSteamTool logo">

  <h1>OpenSteamTool</h1>

  <p>
    <strong>开源 Steam 解锁工具</strong>
  </p>

  <p>
    <img src="https://img.shields.io/badge/C%2B%2B-20%2B-2ea44f?logo=cplusplus&logoColor=white" alt="C++ 20+">
    <img src="https://img.shields.io/badge/CMake-3.20%2B-2ea44f?logo=cmake&logoColor=white" alt="CMake 3.20+">
    <img src="https://img.shields.io/badge/Windows-only-d73a49?logo=windows&logoColor=white" alt="仅 Windows">
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

## 功能特性

### 核心解锁与清单管理
- 解锁任意数量未拥有的游戏及全部 DLC
- 支持从 Lua 配置自动加载仓库（Depot）解密密钥与 PICS 访问令牌（`addtoken`）
- 支持通过 `manifestdex`（默认）、`opensteamtool`、`steamrun`、`wudrm` API 或自定义 Lua 端点自动下载 Manifest
- 支持固定 Manifest 版本以阻止游戏更新
- `config/lua/` 支持多层级嵌套子目录，且其中的 `*.manifest` 文件会自动镜像同步至 `Steam/depotcache/`

### 热重载
- 自动监视配置目录中的 `.lua` 文件，新增或修改即刻生效，无需重启 Steam

### 游戏进程注入
- 通过 `opensteamtool.toml` 中的 `[[inject]]` 将第三方 DLL 注入到游戏进程
- 支持配置多个 DLL、启动参数过滤及 AppID 限制（参见 [第三方 DLL 注入](#第三方-dll-注入)）

### 家庭共享支持
- 自动解锁家庭共享限制，互不影响，无需配置

### 云存档重定向 (CloudRedirect)
- 集成 [CloudRedirect](https://github.com/Selectively11/CloudRedirect)，支持 OST 托管的游戏使用 CloudRedirect 获得存档、游戏时长与成就数据云同步的能力（参见 [配置](#配置可选) 中的 `[cloud]`）

### Denuvo 和 SteamStub 兼容
- **SteamStub**：无需配置票据，自动通过本地 ConfigStore 伪造 AppID
- **显式票据**：在 Lua 中通过 `setAppTicket` / `setETicket` 注入，直接在内存中管理，无需生成磁盘 bin 文件
- **切号离线授权**：正版账号运行后自动将凭据同步至 `<AppId>.lua`（自动写入 `setAppTicket`），切换无游戏账号即可离线游玩
- **清单锁定与更新控制**：默认写入活跃的 `setManifestid` 锁定已安装版本；若需允许正版更新，可在配置中设 `lock_manifest = false`，或在启动项中加入 `-nodenuvo`（Lua 中配置 `nodenuvo(appid)`）
- **辅助参数**：
  - `-d+`：正版账号启动项参数，一键自动生成该游戏的 `<AppId>.lua` 并锁定清单版本
  - `-forcedenuvo`：强制将游戏按 Denuvo 保护处理（Lua 中配置 `forcedenuvo(appid)`）

### 使用 `extract_tickets` 提取凭据与配置
用于在拥有游戏的账号上提取 AppTicket、ETicket、DLC 列表、Depot 密钥、Manifest 并生成开箱即用的 `<appid>.lua`。

* **获取方式**：[GitHub Actions Tools 页面](https://github.com/mmxlyo/OpenSteamTool/actions/workflows/tools.yml) 或本地 `build.bat` 编译（产物位于 `build/tools/<Config>/extract_tickets.exe`）
* **使用方法**：
  ```powershell
  # 标准提取（本地已安装游戏）
  extract_tickets.exe 1361510

  # 未安装游戏强制提取 ETicket（若客户端按钮卡住，重启 Steam 即可恢复）
  extract_tickets.exe 1361510 --force-eticket
  ```

### 统计和成就
- 为未拥有游戏启用成就与统计
- 优先级：Lua `setStat(appid, "steamid")` > stats API（`https://stats.opensteamtool.com/{appid}`）> 默认 SteamID（`76561198028121353`）

### 联机修复 (Online Fix)
- 在 Steam 启动选项中添加 `-onlinefix` 启用基于 480 (Spacewar) 的在线联机，自动保护真实存档与 AppID（同一时间仅运行一个此类游戏）
- 极少数需要匹配 480 证书的游戏可使用 `-onlinefix -p2pflip`（可能存在兼容性问题，非必要不建议使用）

---

## 使用方法

### 方式一：便携模式（推荐）
无需向 Steam 目录复制任何 DLL，完全独立运行：
1. 解压发布包（含 `ost-Injector.exe`、`OpenSteamTool.dll` 等）到任意独立目录（如 `D:\OpenSteamTool_Portable`）
2. 在该目录下创建 `config/lua/` 文件夹并放入 `.lua` 解锁脚本；若需自定义配置，可将 `opensteamtool.toml` 直接放在此目录下
3. 启动方式：
   - **手动启动**：直接运行 `ost-Injector.exe`，自动检测或拉起 Steam 并注入
   - **开机自启**：右键以管理员身份运行 `CreateAutoInjectTask.bat`（卸载运行 `DeleteAutoInjectTask.bat`）
   - **命令行**：支持 `-watch`（后台监听）与 `-silent`（单次静默注入）

### 方式二：标准模式（DLL 劫持）
1. 将 `dwmapi.dll`、`xinput1_4.dll` 和 `OpenSteamTool.dll` 复制到 Steam 安装根目录
2. 在 Steam 根目录下创建 `config/lua/` 目录并放入 Lua 脚本

---

## Lua 配置示例

```lua
addappid(1361510) -- 解锁游戏
addappid(1361511, 0, "5954562e7f5260400040a818bc29b60b335bb690066ff767e20d145a3b6b4af0") -- 解锁 depot 并配置密钥
addtoken(1361510, "2764735786934684318") -- 添加 PICS 访问令牌

setManifestid(1361511, "5656605350306673283") -- 锁定 depot 清单版本
setManifestid(1361511, "5656605350306673283", 12345678) -- 锁定清单并指定大小

setAppTicket(1361510, "0100000000000000...") -- 存入内存 AppTicket
setETicket(1361510, "0100000000000000...")   -- 存入内存 ETicket
setStat(1361510, "76561197960287930")        -- 指定成就数据拉取源 SteamID

addprocess(1361510, "CustomGame.exe")         -- 为无 SteamAppId 环境变量的进程映射 AppID
seteticketurl("https://example.com/eticket")  -- 在线动态获取 ETicket 端点（可选）

nodenuvo(1361510)    -- 跳过 Denuvo 处理（等价于启动项 -nodenuvo，别名: disallowdenuvo）
forcedenuvo(1361510) -- 强制标记为 Denuvo（等价于启动项 -forcedenuvo）
```
所有函数名**不区分大小写**。

---

## 配置（可选）

将 `opensteamtool.example.toml` 重命名为 `opensteamtool.toml`，放置于便携目录根目录或 Steam 根目录。修改后自动热重载。

```toml
[log]
# 仅调试构建有效：trace, debug, info, warn, error
level = "info"

[manifest]
# 上游 API："manifestdex"（默认）、"opensteamtool"、"steamrun"、"wudrm"
url = "manifestdex"
timeout_resolve_ms = 5000
timeout_connect_ms = 5000
timeout_send_ms    = 10000
timeout_recv_ms    = 10000

[stats]
# 未配置 setStat 时查询 https://stats.opensteamtool.com/{appid}
enable_api = true

[lua]
# 额外加载的 Lua 配置目录列表（可选）
paths = []

[cloud]
# 启用基于 CloudRedirect 的云存档透明重定向（需配合其伴侣客户端使用）
enabled = false
# library = "cloud_redirect.dll"

# 可选游戏进程第三方 DLL 注入（表数组，可配置多个）
[[inject]]
path = "OnlineFix.dll"
when_cmdline = "-onlinefix"
when_appids = [1361510]
all_games = false

[remote]
# 可选特征码元数据镜像（默认优先 GitHub，自动回退 jsDelivr）
# url_template = "https://your.server/{channel}/{component}/{sha256}.toml"

[denuvo]
# 切号授权或 -d+ 时是否锁定清单（默认 true）
lock_manifest = true
```

### 第三方 DLL 注入说明
| 字段 | 说明 |
| :--- | :--- |
| `path` | DLL 路径。裸文件名优先在 toml 目录、DLL 目录及 Steam 根目录解析，支持绝对路径 |
| `when_cmdline` | 可选。启动命令行中必须包含的子串 |
| `when_appids` | 可选。限定目标 AppID 列表 |
| `all_games` | 可选。`false`（默认）仅对 Lua 解锁游戏生效；`true` 对所有游戏生效 |

### 通过 Lua 获取 Manifest
若在 `config/lua/` 中定义了以下函数，将优先于配置中的远程 API 调用：

- `fetch_manifest_code_ex(app_id, depot_id, gid)` *（推荐）*：扩展函数，接收 `app_id`、`depot_id` 和 `gid`，允许构造需要应用识别的 API 端点
- `fetch_manifest_code(gid)`：基础函数，只接收 manifest GID

C++ 运行时提供两个辅助网络函数：
| 函数 | 签名 | 返回值 |
| :--- | :--- | :--- |
| `http_get` | `http_get(url [, headers])` | `body, status_code` |
| `http_post` | `http_post(url, body [, headers])` | `body, status_code` |

`headers` 为可选表：`{["Key"]="Value", ...}`

---

## 调试日志

调试构建在 `<Steam或便携目录>/opensteamtool/` 下输出模块日志：

| 文件 | 来源 | 内容 |
| :--- | :--- | :--- |
| `main.log` | 通用 | 初始化、配置加载、Lua 解析 |
| `ipc.log` | `LOG_IPC_*` | IPC 命令、接口分发与伪造 |
| `netpacket.log` | `LOG_NETPACKET_*` | 网络包拦截、eMsg 调度 |
| `manifest.log` | `LOG_MANIFEST_*` | 清单下载与自动镜像同步 |
| `decryptionkey.log` | `LOG_DECRYPTIONKEY_*` | Depot 解密密钥注入 |
| `keyvalue.log` | `LOG_KEYVALUE_*` | KeyValues 清单锁定修补 |
| `misc.log` | `LOG_MISC_*` | 引擎指针捕获与 AppID 映射 |
| `achievement.log` | `LOG_ACHIEVEMENT_*` | 统计与成就数据处理 |
| `pics.log` | `LOG_PICS_*` | PICS 访问令牌注入 |
| `package.log` | `LOG_PACKAGE_*` | Package 0 授权与热重载撤销 |
| `onlinefix.log` | `LOG_ONLINEFIX_*` | 480 联机修复与 AppID 保护 |
| `richpresence.log` | `LOG_RICHPRESENCE_*` | 丰富状态 (Rich Presence) 注入 |
| `steamui.log` | `LOG_STEAMUI_*` | SteamUI 界面状态同步 |
| `inject.log` | `LOG_INJECT_*` | 第三方 DLL 注入匹配与结果 |
| `pipe.log` | `LOG_PIPE_*` | 管道握手、Denuvo 认证调度 |
| `platform.log` | `LOG_PLATFORM_*` | 平台底层与远程注入诊断 |

---

## 构建

### 环境要求
- Windows 10/11
- CMake 3.20+
- Visual Studio 2022（MSVC x64 工具链）

### 构建命令
```powershell
build.bat
```

### 输出产物
* **核心组件**（位于 `build/Release/` 或 `build/Debug/`）：
  - `OpenSteamTool.dll`、`dwmapi.dll`、`xinput1_4.dll`、`ost-Injector.exe` 及辅助脚本
* **独立提取工具**（位于 `build/tools/Release/` 或 `build/tools/Debug/`）：
  - `extract_tickets.exe`（主发布包默认排除，由 `build.bat` 显式编译）

---

## 鸣谢
感谢以下开源项目与贡献者支持：
- [OpenSteam001/OpenSteamTool](https://github.com/OpenSteam001/OpenSteamTool) — 上游项目基石
- [Selectively11/CloudRedirect](https://github.com/Selectively11/CloudRedirect) — Steam 云存档重定向引擎
- [Berkecann](https://github.com/Berkecann) — 贡献 ManifestDeX 默认清单提供源 ([PR #200](https://github.com/OpenSteam001/OpenSteamTool/pull/200))
- [microsoft/Detours](https://github.com/microsoft/Detours) — 二进制 API 拦截库
- [marzer/tomlplusplus](https://github.com/marzer/tomlplusplus) — C++ TOML 解析器
- [gabime/spdlog](https://github.com/gabime/spdlog) — 高性能快速日志库

---

## 免责声明
本项目仅供研究和教育目的使用。使用者须自行遵守当地法律及相关平台服务条款。
