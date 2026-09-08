param(
    [string]$Target = ""
)

if ([string]::IsNullOrWhiteSpace($Target)) {
    $Target = $PSScriptRoot
}

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "  OpenSteamTool - Convert tickets.txt to Lua Script" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

$files = @()
if (Test-Path $Target -PathType Leaf) {
    $files += (Get-Item $Target)
} elseif (Test-Path $Target -PathType Container) {
    $files += (Get-ChildItem -Path $Target -Filter "tickets.txt" -Recurse)
}

if ($files.Count -eq 0) {
    Write-Host "[Warning] No tickets.txt found in target path." -ForegroundColor Yellow
    exit 0
}

# Helper to find Steam install path
function Get-SteamPath {
    try {
        $p = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name 'SteamPath' -ErrorAction Stop).SteamPath
        if (![string]::IsNullOrEmpty($p)) { return $p.Replace('/', '\') }
    } catch {}
    return $null
}

# Helper to query depot decryption keys from config.vdf
function Get-SteamDepotKeys([string]$steamPath) {
    $depotKeys = @{}
    $configPath = Join-Path $steamPath "config\config.vdf"
    if (Test-Path $configPath) {
        $text = [System.IO.File]::ReadAllText($configPath)
        $matches = [regex]::Matches($text, '"(\d{3,10})"\s*\{\s*"DecryptionKey"\s*"([0-9a-fA-F]{64})"')
        foreach ($m in $matches) {
            $depotKeys[$m.Groups[1].Value] = $m.Groups[2].Value
        }
    }
    return $depotKeys
}

# Helper to find installed depots for an AppID
function Get-AppInstalledDepots([string]$steamPath, [string]$appId) {
    $depots = @{}
    $libraries = @($steamPath)
    $libVdf = Join-Path $steamPath "steamapps\libraryfolders.vdf"
    if (Test-Path $libVdf) {
        $libText = [System.IO.File]::ReadAllText($libVdf)
        $libMatches = [regex]::Matches($libText, '"path"\s*"([^"]+)"')
        foreach ($lm in $libMatches) {
            $p = $lm.Groups[1].Value -replace '\\\\', '\'
            if ($libraries -notcontains $p) { $libraries += $p }
        }
    }

    foreach ($lib in $libraries) {
        $acf = Join-Path $lib "steamapps\appmanifest_${appId}.acf"
        if (Test-Path $acf) {
            $acfText = [System.IO.File]::ReadAllText($acf)
            $mDepots = [regex]::Matches($acfText, '"InstalledDepots"\s*\{([\s\S]*?)\n\t\}')
            if ($mDepots.Count -gt 0) {
                $block = $mDepots[0].Groups[1].Value
                $dMatches = [regex]::Matches($block, '"(\d{3,10})"\s*\{([\s\S]*?)\}')
                foreach ($dm in $dMatches) {
                    $dId = $dm.Groups[1].Value
                    $dBody = $dm.Groups[2].Value
                    $manifestId = ""
                    if ($dBody -match '"manifest"\s*"(\d+)"') { $manifestId = $matches[1] }
                    $depots[$dId] = $manifestId
                }
            }
        }
    }
    return $depots
}

$localSteamPath = Get-SteamPath
$cachedDepotKeys = if ($localSteamPath) { Get-SteamDepotKeys $localSteamPath } else { @{} }

foreach ($f in $files) {
    Write-Host ("[Processing] " + $f.FullName) -ForegroundColor Cyan
    $lines = Get-Content $f.FullName
    $appId = ""
    $appTicket = ""
    $eTicket = ""
    $depotKeys = @{}

    foreach ($line in $lines) {
        $l = $line.Trim()
        if ($l -match '^appid\s*:\s*(\d+)') {
            $appId = $matches[1]
        } elseif ($l -match '^depotkey\((\d+)\)\s*:\s*([0-9a-fA-F]{64})') {
            $depotKeys[$matches[1]] = $matches[2]
        } elseif ($l -match '^appticket[^:]*:\s*([0-9a-fA-F]+)') {
            $appTicket = $matches[1]
        } elseif ($l -match '^eticket[^:]*:\s*([0-9a-fA-F]+)') {
            $eTicket = $matches[1]
        }
    }

    if ([string]::IsNullOrEmpty($appId)) {
        Write-Host ("[-] Skip: AppID not found in " + $f.Name) -ForegroundColor Red
        continue
    }

    # If no depot keys in tickets.txt, attempt lookup from local Steam config
    if ($depotKeys.Count -eq 0 -and $localSteamPath -and $cachedDepotKeys.Count -gt 0) {
        $installedDepots = Get-AppInstalledDepots $localSteamPath $appId
        foreach ($dId in $installedDepots.Keys) {
            if ($cachedDepotKeys.ContainsKey($dId)) {
                $depotKeys[$dId] = $cachedDepotKeys[$dId]
            }
        }
        # Check if appId itself has a key
        if ($cachedDepotKeys.ContainsKey($appId)) {
            $depotKeys[$appId] = $cachedDepotKeys[$appId]
        }
        # Check heuristic range
        $numericAppId = [uint32]$appId
        foreach ($k in $cachedDepotKeys.Keys) {
            $numKey = [uint32]$k
            if ($numKey -ge $numericAppId -and $numKey -le ($numericAppId + 50)) {
                if (-not $depotKeys.ContainsKey($k)) {
                    $depotKeys[$k] = $cachedDepotKeys[$k]
                }
            }
        }
    }

    $outDir = $f.DirectoryName
    $outLua = Join-Path $outDir ($appId + ".lua")

    $luaContent = @()
    $luaContent += ("-- Auto-generated Lua config for AppID: " + $appId)

    if (-not $depotKeys.ContainsKey($appId)) {
        $luaContent += ("addappid(" + $appId + ")")
    }

    if ($depotKeys.Count -gt 0) {
        $luaContent += ""
        $luaContent += "-- Depot Decryption Keys"
        foreach ($dId in ($depotKeys.Keys | Sort-Object { [uint32]$_ })) {
            $luaContent += ("addappid(" + $dId + ', 1, "' + $depotKeys[$dId] + '")')
            Write-Host ("  [Key] Depot " + $dId + " -> " + $depotKeys[$dId]) -ForegroundColor Green
        }
    }
    $luaContent += ""

    if (![string]::IsNullOrEmpty($appTicket)) {
        $luaContent += "-- App Ownership Ticket (AppTicket)"
        $luaContent += ("setAppTicket(" + $appId + ', "' + $appTicket + '")')
        $luaContent += ""
    }

    if (![string]::IsNullOrEmpty($eTicket)) {
        $luaContent += "-- Encrypted App Ticket (ETicket)"
        $luaContent += ("setETicket(" + $appId + ', "' + $eTicket + '")')
        $luaContent += ""
    }

    [System.IO.File]::WriteAllLines($outLua, $luaContent, [System.Text.Encoding]::UTF8)
    Write-Host ("[+] Successfully generated: " + $outLua) -ForegroundColor Green
}

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "Done. You can copy the generated .lua file directly" -ForegroundColor Green
Write-Host "to your OpenSteamTool config/lua/ folder." -ForegroundColor Green
Write-Host "=======================================================" -ForegroundColor Cyan
