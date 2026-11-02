# Installs JARVIS for the current user (no admin needed). Safe to run again.
#   powershell -ExecutionPolicy Bypass -File tools\install.ps1
#
# - builds build\jarvis.exe if it is missing (needs MinGW-w64 gcc + windres)
# - copies jarvis.exe to %LOCALAPPDATA%\Programs\JARVIS\ (an existing jarvis.ini is kept)
# - creates a Start Menu shortcut "JARVIS"
# - prints how to bind the Copilot key with PowerToys
# - optionally builds the file index (jarvis.exe --reindex)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo       = Split-Path -Parent $PSScriptRoot
$builtExe   = Join-Path $repo 'build\jarvis.exe'
$installDir = Join-Path $env:LOCALAPPDATA 'Programs\JARVIS'
$installExe = Join-Path $installDir 'jarvis.exe'
$startMenu  = [Environment]::GetFolderPath('Programs')
$shortcut   = Join-Path $startMenu 'JARVIS.lnk'

function Write-Step([string]$msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

# 1. Build if needed
if (-not (Test-Path -LiteralPath $builtExe)) {
    Write-Step 'build\jarvis.exe not found - building'
    $bat = Join-Path $repo 'build.bat'
    & cmd.exe /c "`"$bat`""
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $builtExe)) {
        throw 'Build failed. Make sure MinGW-w64 (gcc and windres) is on PATH, then run build.bat.'
    }
}
else {
    Write-Step "Using existing $builtExe (run build.bat first to rebuild)"
}

# 2. Make sure JARVIS isn't running (the exe would be locked)
while ($true) {
    $running = @(Get-Process -Name 'jarvis' -ErrorAction SilentlyContinue)
    if ($running.Count -eq 0) { break }
    Write-Host 'JARVIS is currently running. Please close it (Esc in its window, or end jarvis.exe in Task Manager).' -ForegroundColor Yellow
    $ans = Read-Host 'Press Enter when it is closed, or type q to quit'
    if ($ans -match '^\s*q') { Write-Host 'Install cancelled.'; exit 1 }
}

# 3. Copy the exe (jarvis.ini next to it is never overwritten; JARVIS creates it on first run)
Write-Step "Installing to $installDir"
if (-not (Test-Path -LiteralPath $installDir)) {
    New-Item -ItemType Directory -Path $installDir | Out-Null
}
Copy-Item -LiteralPath $builtExe -Destination $installExe -Force
if (Test-Path -LiteralPath (Join-Path $installDir 'jarvis.ini')) {
    Write-Host '    Kept your existing jarvis.ini.'
}

# 4. Start Menu shortcut (recreated every time so it always points at the right exe)
Write-Step "Creating Start Menu shortcut: $shortcut"
$shell = New-Object -ComObject WScript.Shell
try {
    $lnk = $shell.CreateShortcut($shortcut)
    $lnk.TargetPath       = $installExe
    $lnk.WorkingDirectory = $installDir
    $lnk.IconLocation     = "$installExe,0"
    $lnk.Description      = 'JARVIS voice launcher'
    $lnk.Save()
}
finally {
    [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($shell)
}

# 5. Next steps
Write-Host ''
Write-Host 'JARVIS is installed.' -ForegroundColor Green
Write-Host ''
Write-Host 'Next: map the Copilot key to JARVIS with Microsoft PowerToys' -ForegroundColor Cyan
Write-Host '  1. Install PowerToys (winget install Microsoft.PowerToys) and open PowerToys Settings.'
Write-Host '  2. Keyboard Manager -> turn it on -> "Remap a shortcut" -> Add shortcut remapping.'
Write-Host '  3. Shortcut: press the Copilot key (it is sent as Win + Shift + F23).'
Write-Host '  4. Action: "Run program", and set the program path to:'
Write-Host "       $installExe" -ForegroundColor White
Write-Host '     "If running": choose "Start another" (pressing the key again toggles the mic).'
Write-Host '  5. Click OK. Press the Copilot key and say e.g. "open downloads".'
Write-Host ''
Write-Host "Settings: $installDir\jarvis.ini (created on first run)"
Write-Host "Data:     $env:LOCALAPPDATA\JARVIS (file index + log)"
Write-Host ''

# 6. Optional: build the file index now
$ans = Read-Host 'Build the file index now? This can take a minute. [y/N]'
if ($ans -match '^\s*y') {
    Write-Step 'Indexing files (jarvis.exe --reindex)...'
    $p = Start-Process -FilePath $installExe -ArgumentList '--reindex' -WorkingDirectory $installDir -NoNewWindow -Wait -PassThru
    if ($p.ExitCode -ne 0) { Write-Host "Indexing exited with code $($p.ExitCode)." -ForegroundColor Yellow }
}
else {
    Write-Host 'Skipped. JARVIS builds the index on first use (or run: jarvis.exe --reindex).'
}
