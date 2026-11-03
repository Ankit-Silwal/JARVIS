# Installs Whisper speech recognition for JARVIS (offline, runs on your PC).
#
#   powershell -ExecutionPolicy Bypass -File tools\get_whisper.ps1            # small.en (best for accents, ~190 MB)
#   powershell -ExecutionPolicy Bypass -File tools\get_whisper.ps1 -Model base.en   # faster, a bit less accurate (~60 MB)
#
# Downloads whisper.cpp (ggml-org, release b5454, Windows x64) and an OpenAI Whisper
# model (converted by ggml-org on Hugging Face) into %LOCALAPPDATA%\JARVIS\whisper.
# Every file is checked against a pinned SHA-256 before it is used.
# JARVIS picks Whisper up the next time it starts ([voice] engine=auto in jarvis.ini).
param(
    [ValidateSet("small.en", "base.en")] [string]$Model = "small.en",
    [string]$Dir = "$env:LOCALAPPDATA\JARVIS\whisper"
)
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$zipUrl  = "https://github.com/ggml-org/whisper.cpp/releases/download/b5454/whisper-bin-x64.zip"
$zipSha  = "6ba69e3482d7826214f90a6a9c84ca07782aec1e1d0c6a7c30c994fd5d816ccb"
$models = @{
    "small.en" = @{ file = "ggml-small.en-q5_1.bin"; sha = "bfdff4894dcb76bbf647d56263ea2a96645423f1669176f4844a1bf8e478ad30" }
    "base.en"  = @{ file = "ggml-base.en-q5_1.bin";  sha = "4baf70dd0d7c4247ba2b81fafd9c01005ac77c2f9ef064e00dcf195d0e2fdd2f" }
}
$m = $models[$Model]
$modelUrl = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/$($m.file)"

function Get-Checked([string]$url, [string]$out, [string]$sha) {
    if (Test-Path $out) {
        if ((Get-FileHash $out -Algorithm SHA256).Hash.ToLower() -eq $sha) { Write-Host "  already here: $(Split-Path $out -Leaf)"; return }
        Remove-Item $out -Force
    }
    Write-Host "  downloading $url"
    $tmp = "$out.part"
    Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing
    $got = (Get-FileHash $tmp -Algorithm SHA256).Hash.ToLower()
    if ($got -ne $sha) { Remove-Item $tmp -Force; throw "checksum mismatch for $url`n  expected $sha`n  got      $got" }
    Move-Item $tmp $out -Force
}

New-Item -ItemType Directory -Force "$Dir\bin" | Out-Null
$dl = "$Dir\download"
New-Item -ItemType Directory -Force $dl | Out-Null

Write-Host "whisper.cpp:"
Get-Checked $zipUrl "$dl\whisper-bin-x64.zip" $zipSha
$ex = "$dl\unzip"
if (Test-Path $ex) { Remove-Item $ex -Recurse -Force }
Expand-Archive "$dl\whisper-bin-x64.zip" -DestinationPath $ex -Force
$cli = Get-ChildItem $ex -Recurse -Filter whisper-cli.exe | Select-Object -First 1
if (-not $cli) { throw "whisper-cli.exe not found in the zip" }
Copy-Item $cli.FullName "$Dir\bin\" -Force
Get-ChildItem $cli.DirectoryName -Filter "*.dll" | Where-Object { $_.Name -like "whisper*" -or $_.Name -like "ggml*" } |
    Copy-Item -Destination "$Dir\bin\" -Force
Remove-Item $ex -Recurse -Force

Write-Host "model ($Model):"
Get-Checked $modelUrl "$Dir\$($m.file)" $m.sha

Write-Host ""
Write-Host "Installed in $Dir"
Write-Host "  bin\whisper-cli.exe + $((Get-ChildItem "$Dir\bin" -Filter *.dll).Count) DLLs"
Write-Host "  $($m.file)"
Write-Host "Start JARVIS: it now uses Whisper (log line 'voice: Whisper' in %LOCALAPPDATA%\JARVIS\jarvis.log)."
