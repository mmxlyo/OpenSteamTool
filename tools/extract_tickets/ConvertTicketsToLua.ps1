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

foreach ($f in $files) {
    Write-Host ("[Processing] " + $f.FullName) -ForegroundColor Cyan
    $lines = Get-Content $f.FullName
    $appId = ""
    $appTicket = ""
    $eTicket = ""

    foreach ($line in $lines) {
        $l = $line.Trim()
        if ($l -match '^appid\s*:\s*(\d+)') {
            $appId = $matches[1]
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

    $outDir = $f.DirectoryName
    $outLua = Join-Path $outDir ($appId + ".lua")

    $luaContent = @()
    $luaContent += ("-- Auto-generated Lua config for AppID: " + $appId)
    $luaContent += ("addappid(" + $appId + ")")
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
