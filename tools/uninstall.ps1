# Removes JARVIS installed by tools\install.ps1. Safe to run again.
#   powershell -ExecutionPolicy Bypass -File tools\uninstall.ps1

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$installDir = Join-Path $env:LOCALAPPDATA 'Programs\JARVIS'
$dataDir    = Join-Path $env:LOCALAPPDATA 'JARVIS'
$shortcut   = Join-Path ([Environment]::GetFolderPath('Programs')) 'JARVIS.lnk'

function Write-Step([string]$msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

# JARVIS must not be running (its exe would be locked)
while ($true) {
    $running = @(Get-Process -Name 'jarvis' -ErrorAction SilentlyContinue)
    if ($running.Count -eq 0) { break }
    Write-Host 'JARVIS is currently running. Please close it (Esc in its window, or end jarvis.exe in Task Manager).' -ForegroundColor Yellow
    $ans = Read-Host 'Press Enter when it is closed, or type q to quit'
    if ($ans -match '^\s*q') { Write-Host 'Uninstall cancelled.'; exit 1 }
}

if (Test-Path -LiteralPath $shortcut) {
    Write-Step "Removing Start Menu shortcut $shortcut"
    Remove-Item -LiteralPath $shortcut -Force
}
else {
    Write-Host 'Start Menu shortcut not found (already removed).'
}

if (Test-Path -LiteralPath $installDir) {
    Write-Step "Removing $installDir (including jarvis.ini)"
    Remove-Item -LiteralPath $installDir -Recurse -Force
}
else {
    Write-Host "$installDir not found (already removed)."
}

if (Test-Path -LiteralPath $dataDir) {
    $ans = Read-Host "Also delete JARVIS data in $dataDir (file index + log)? [y/N]"
    if ($ans -match '^\s*y') {
        Write-Step "Removing $dataDir"
        Remove-Item -LiteralPath $dataDir -Recurse -Force
    }
    else {
        Write-Host "Kept $dataDir."
    }
}

Write-Host ''
Write-Host 'JARVIS has been uninstalled.' -ForegroundColor Green
Write-Host 'Remember to remove the Copilot-key mapping in PowerToys:' -ForegroundColor Yellow
Write-Host '  PowerToys Settings -> Keyboard Manager -> Remap a shortcut -> delete the Win+Shift+F23 -> jarvis.exe entry.'
