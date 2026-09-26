<div align="center">
  <img src="docs/logo-animated.svg" width="180" alt="OpenSteamTool logo">

  <h1>OpenSteamTool</h1>

  <p>
    <strong>Herramienta de desbloqueo de Steam de código abierto</strong>
  </p>

  <p>
    <img src="https://img.shields.io/badge/C%2B%2B-20%2B-2ea44f?logo=cplusplus&logoColor=white" alt="C++ 20+">
    <img src="https://img.shields.io/badge/CMake-3.20%2B-2ea44f?logo=cmake&logoColor=white" alt="CMake 3.20+">
    <img src="https://img.shields.io/badge/Windows-only-d73a49?logo=windows&logoColor=white" alt="Solo Windows">
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

## Características

### Desbloqueos principales y gestión de manifiestos
- Desbloquea juegos no adquiridos y todos sus DLC sin límite alguno
- Carga automática de claves de descifrado de depósitos y tokens de acceso PICS (`addtoken`) desde Lua
- Descarga automática de manifiestos a través de las API de `manifestdex` (predeterminado), `opensteamtool`, `steamrun`, `wudrm` o puntos de enlace Lua personalizados
- Bloqueo de versiones de manifiesto para impedir actualizaciones automáticas del juego
- Compatibilidad con subdirectorios anidados en `config/lua/` y sincronización automática de archivos `*.manifest` en `Steam/depotcache/`

### Recarga en caliente (Hot Reload)
- Detección inmediata de adiciones y modificaciones en archivos `.lua` sin necesidad de reiniciar Steam

### Inyección en procesos de juego
- Carga de DLL de terceros en los procesos de los juegos mediante `[[inject]]` en `opensteamtool.toml`
- Admite múltiples reglas de DLL, filtrado por línea de comandos y restricción por AppID (ver [Inyección de DLL de terceros](#inyección-de-dll-de-terceros))

### Soporte de préstamo familiar
- Desbloquea automáticamente las restricciones del préstamo familiar, sin conflictos ni configuración

### Redirección de guardado en la nube (CloudRedirect)
- Integra [CloudRedirect](https://github.com/Selectively11/CloudRedirect) para habilitar la sincronización en la nube de partidas guardadas, tiempo de juego y logros en juegos gestionados por OST (ver `[cloud]` en [Configuración](#configuración-opcional))

### Compatibilidad con Denuvo y SteamStub
- **SteamStub**: Suplantación de AppID mediante tickets locales de ConfigStore sin inyectar en el juego
- **Tickets explícitos**: Gestión exclusivamente en memoria mediante `setAppTicket` / `setETicket` en Lua; no genera archivos `.bin` en disco
- **Autorización offline por cambio de cuenta**: Sincroniza automáticamente las credenciales directamente en `<AppId>.lua` (mediante `setAppTicket`) tras ejecutar en una cuenta poseedora; cambia a cuentas sin el juego para jugar offline
- **Bloqueo de manifiestos y actualizaciones**: Por defecto activa `setManifestid` para proteger la autorización offline; establece `lock_manifest = false` o pasa `-nodenuvo` (o `nodenuvo(appid)`) para permitir parches oficiales
- **Parámetros auxiliares**:
  - `-d+`: Parámetro de lanzamiento en cuentas propietarias para generar automáticamente `<AppId>.lua` y bloquear manifiestos
  - `-forcedenuvo`: Fuerza el tratamiento de un juego como protegido por Denuvo (o `forcedenuvo(appid)`)

### Extracción de credenciales y configuración con `extract_tickets`
Extrae AppTicket, ETicket, DLC, claves de depósitos, manifiestos y genera un `<appid>.lua` listo para usar en cuentas propietarias.

* **Descarga**: [Flujo de trabajo Tools en GitHub Actions](https://github.com/mmxlyo/OpenSteamTool/actions/workflows/tools.yml) o compilación local mediante `build.bat` (`build/tools/<Config>/extract_tickets.exe`)
* **Uso**:
  ```powershell
  # Extracción estándar (juego instalado localmente)
  extract_tickets.exe 1361510

  # Forzar extracción de ETicket en juegos no instalados (reiniciar Steam si el botón se queda bloqueado)
  extract_tickets.exe 1361510 --force-eticket
  ```

### Estadísticas y logros
- Habilita estadísticas y logros para juegos no adquiridos
- Prioridad: Lua `setStat(appid, "steamid")` > API de estadísticas (`https://stats.opensteamtool.com/{appid}`) > SteamID predeterminado (`76561198028121353`)

### Reparación en línea (Online Fix)
- Añade `-onlinefix` a los parámetros de lanzamiento para juego online basado en 480 (Spacewar) con preservación automática de guardados y AppID reales (un solo juego activo a la vez)
- Para títulos excepcionales que requieran validar el certificado 480 se puede usar `-onlinefix -p2pflip` (usar con precaución debido a incompatibilidades)

---

## Uso

### Método 1: Modo portátil (Recomendado)
No requiere copiar DLL en la carpeta de Steam y funciona de manera totalmente independiente:
1. Extrae el paquete (con `ost-Injector.exe`, `OpenSteamTool.dll`, etc.) en cualquier carpeta independiente (ej. `D:\OpenSteamTool_Portable`)
2. Crea `config/lua/` y coloca tus scripts de desbloqueo; `opensteamtool.toml` se puede ubicar directamente en este directorio portátil
3. Métodos de inicio:
   - **Manual**: Ejecuta `ost-Injector.exe` para detectar o abrir Steam e inyectar
   - **Inicio automático**: Ejecuta `CreateAutoInjectTask.bat` como administrador (desinstalar con `DeleteAutoInjectTask.bat`)
   - **Línea de comandos**: Admite `-watch` (servicio en segundo plano) y `-silent` (inyección única silenciosa)

### Método 2: Modo estándar (Secuestro de DLL)
1. Copia `dwmapi.dll`, `xinput1_4.dll` y `OpenSteamTool.dll` al directorio raíz de Steam
2. Crea la carpeta `config/lua/` en el directorio de Steam y coloca allí tus scripts de Lua

---

## Ejemplo de configuración Lua

```lua
addappid(1361510) -- desbloquea el juego
addappid(1361511, 0, "5954562e7f5260400040a818bc29b60b335bb690066ff767e20d145a3b6b4af0") -- depósito con clave
addtoken(1361510, "2764735786934684318") -- token de acceso PICS

setManifestid(1361511, "5656605350306673283") -- fijar versión de manifiesto
setManifestid(1361511, "5656605350306673283", 12345678) -- fijar manifiesto con tamaño

setAppTicket(1361510, "0100000000000000...") -- AppTicket en memoria
setETicket(1361510, "0100000000000000...")   -- ETicket en memoria
setStat(1361510, "76561197960287930")        -- SteamID para obtención de logros

addprocess(1361510, "JuegoPersonalizado.exe") -- mapear AppID para procesos sin variables de entorno
seteticketurl("https://ejemplo.com/eticket")  -- punto de enlace para generar ETicket (opcional)

nodenuvo(1361510)    -- omitir procesamiento de Denuvo (equivalente a -nodenuvo, alias: disallowdenuvo)
forcedenuvo(1361510) -- forzar procesamiento como Denuvo (equivalente a -forcedenuvo)
```
Los nombres de todas las funciones **no distinguen entre mayúsculas y minúsculas**.

---

## Configuración (opcional)

Renombra `opensteamtool.example.toml` a `opensteamtool.toml` y colócalo en el directorio portátil o en la raíz de Steam. Se recarga en caliente tras guardar.

```toml
[log]
# Solo en compilaciones Debug: trace, debug, info, warn, error
level = "info"

[manifest]
# API ascendente: "manifestdex" (predeterminado), "opensteamtool", "steamrun", "wudrm"
url = "manifestdex"
timeout_resolve_ms = 5000
timeout_connect_ms = 5000
timeout_send_ms    = 10000
timeout_recv_ms    = 10000

[stats]
# Consulta la API cuando no haya setStat en Lua
enable_api = true

[lua]
# Directorios adicionales para buscar scripts Lua (opcional)
paths = []

[cloud]
# Redirección de Steam Cloud mediante la aplicación CloudRedirect
enabled = false
# library = "cloud_redirect.dll"

# Inyección opcional de DLL en juegos (tabla de matrices, admite múltiples reglas)
[[inject]]
path = "OnlineFix.dll"
when_cmdline = "-onlinefix"
when_appids = [1361510]
all_games = false

[remote]
# Espejo opcional de patrones (por defecto GitHub con respaldo en jsDelivr)
# url_template = "https://tu.servidor/{channel}/{component}/{sha256}.toml"

[denuvo]
# Bloquear manifiestos al cambiar de cuenta o con -d+ (predeterminado true)
lock_manifest = true
```

### Inyección de DLL de terceros
| Clave | Descripción |
| :--- | :--- |
| `path` | DLL a cargar. Los nombres simples se resuelven junto a toml, DLL o steam.exe; admite rutas absolutas |
| `when_cmdline` | Opcional. Subcadena requerida en la línea de comandos de inicio |
| `when_appids` | Opcional. Lista de AppIDs a los que restringir la inyección |
| `all_games` | Opcional. `false` (predeterminado) solo inyecta en juegos Lua; `true` inyecta en todos los juegos |

### Manifest mediante Lua
Si se definen en `config/lua/`, estas funciones tienen prioridad sobre las API HTTP remotas configuradas:

- `fetch_manifest_code_ex(app_id, depot_id, gid)` *(Recomendado)*: Variante extendida que recibe `app_id`, `depot_id` y `gid` para construir puntos finales con identificación de app
- `fetch_manifest_code(gid)`: Variante base que solo recibe el GID del manifiesto

Funciones auxiliares HTTP proporcionadas por el runtime de C++:
| Función | Firma | Valor de retorno |
| :--- | :--- | :--- |
| `http_get` | `http_get(url [, headers])` | `body, status_code` |
| `http_post` | `http_post(url, body [, headers])` | `body, status_code` |

`headers` es una tabla opcional: `{["Key"]="Value", ...}`

---

## Registros de depuración (Logs)

Las versiones Debug generan registros bajo `<Directorio Steam o Portátil>/opensteamtool/`:

| Archivo | Origen | Contenido |
| :--- | :--- | :--- |
| `main.log` | General | Inicialización, carga de configuración, Lua |
| `ipc.log` | `LOG_IPC_*` | Comandos IPC, llamadas a interfaces y suplantación |
| `netpacket.log` | `LOG_NETPACKET_*` | Intercepción de paquetes de red y eMsg |
| `manifest.log` | `LOG_MANIFEST_*` | Descarga y sincronización en depotcache |
| `decryptionkey.log` | `LOG_DECRYPTIONKEY_*` | Inyección de claves de depósitos |
| `keyvalue.log` | `LOG_KEYVALUE_*` | Parches de KeyValues para manifiestos |
| `misc.log` | `LOG_MISC_*` | Captura de punteros y mapeo de AppID |
| `achievement.log` | `LOG_ACHIEVEMENT_*` | Procesamiento de estadísticas y logros |
| `pics.log` | `LOG_PICS_*` | Inyección de tokens de acceso PICS |
| `package.log` | `LOG_PACKAGE_*` | Licencias de Package 0 y revocación dinámica |
| `onlinefix.log` | `LOG_ONLINEFIX_*` | Corrección online 480 y protección de AppID |
| `richpresence.log` | `LOG_RICHPRESENCE_*` | Inyección de paquetes Rich Presence |
| `steamui.log` | `LOG_STEAMUI_*` | Sincronización de interfaz SteamUI |
| `inject.log` | `LOG_INJECT_*` | Coincidencias y resultados de inyección de DLL |
| `pipe.log` | `LOG_PIPE_*` | Canales IPC y autorización de Denuvo |
| `platform.log` | `LOG_PLATFORM_*` | Diagnóstico de plataforma e inyección remota |

---

## Compilación

### Requisitos
- Windows 10/11
- CMake 3.20+
- Visual Studio 2022 con MSVC (cadena de herramientas x64)

### Compilación rápida
```powershell
build.bat
```

### Salida
* **Componentes principales** (en `build/Release/` o `build/Debug/`):
  - `OpenSteamTool.dll`, `dwmapi.dll`, `xinput1_4.dll`, `ost-Injector.exe` y scripts auxiliares
* **Herramienta independiente** (en `build/tools/Release/` o `build/tools/Debug/`):
  - `extract_tickets.exe` (excluido del paquete general por defecto, compilado explícitamente por `build.bat`)

---

## Agradecimientos
Un agradecimiento especial a los proyectos upstream, colaboradores y de código abierto:
- [OpenSteam001/OpenSteamTool](https://github.com/OpenSteam001/OpenSteamTool) — Base del proyecto upstream
- [Selectively11/CloudRedirect](https://github.com/Selectively11/CloudRedirect) — Motor de redirección de Steam Cloud
- [Berkecann](https://github.com/Berkecann) — Contribuyó con el proveedor de manifiestos predeterminado ManifestDeX ([PR #200](https://github.com/OpenSteam001/OpenSteamTool/pull/200))
- [microsoft/Detours](https://github.com/microsoft/Detours) — Biblioteca de intercepción de API binaria
- [marzer/tomlplusplus](https://github.com/marzer/tomlplusplus) — Analizador de TOML para C++
- [gabime/spdlog](https://github.com/gabime/spdlog) — Biblioteca de registro rápido en C++

---

## Descargo de responsabilidad
Este proyecto se proporciona únicamente con fines de investigación y educación. El usuario es responsable de cumplir con las leyes locales y los términos de servicio de la plataforma.
