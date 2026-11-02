# Generates res\jarvis.ico: an "arc reactor" style icon (navy disc, cyan rings,
# three arc segments, glowing core) at 16..256 px, packed as a PNG-in-ICO file.
# Usage: powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1 [-PreviewDir <dir>]
param(
    [string]$OutFile = (Join-Path (Split-Path -Parent $PSScriptRoot) 'res\jarvis.ico'),
    [string]$PreviewDir = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function New-Color([string]$hex, [int]$alpha = 255) {
    $c = [System.Drawing.ColorTranslator]::FromHtml($hex)
    return [System.Drawing.Color]::FromArgb($alpha, $c.R, $c.G, $c.B)
}

function Get-Rect([double]$cx, [double]$cy, [double]$r) {
    return New-Object System.Drawing.RectangleF ([single]($cx - $r)), ([single]($cy - $r)), ([single](2 * $r)), ([single](2 * $r))
}

function Draw-Ring($g, [double]$c, [double]$r, [double]$w, $color) {
    $pen = New-Object System.Drawing.Pen $color, ([single]$w)
    $g.DrawEllipse($pen, (Get-Rect $c $c $r))
    $pen.Dispose()
}

function Draw-Arc($g, [double]$c, [double]$r, [double]$w, $color, [double]$start, [double]$sweep) {
    $pen = New-Object System.Drawing.Pen $color, ([single]$w)
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawArc($pen, (Get-Rect $c $c $r), [single]$start, [single]$sweep)
    $pen.Dispose()
}

# Radial glow: opaque-ish color at the center fading to transparent at radius r.
function Fill-Glow($g, [double]$c, [double]$r, $centerColor, $edgeColor) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddEllipse((Get-Rect $c $c $r))
    $brush = New-Object System.Drawing.Drawing2D.PathGradientBrush $path
    $brush.CenterColor = $centerColor
    $brush.SurroundColors = [System.Drawing.Color[]]@($edgeColor)
    $brush.CenterPoint = New-Object System.Drawing.PointF ([single]$c), ([single]$c)
    $g.FillPath($brush, $path)
    $brush.Dispose()
    $path.Dispose()
}

function Render-Icon([int]$size) {
    # Draw at 4x and downscale with a high-quality filter: smoother than GDI+ AA alone.
    $ss = 4
    $S = $size * $ss
    $big = New-Object System.Drawing.Bitmap $S, $S, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($big)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    $c = $S / 2.0
    $R = $S / 2.0 - $S * 0.02          # disc radius (tiny margin)
    # Stroke unit: 1 px at 32x32; larger sizes get relatively thinner strokes
    # (scale ~ size^0.6) so the big renders look refined rather than chunky.
    if ($size -le 32) { $u = $ss * $size / 32.0 } else { $u = $ss * [Math]::Pow($size / 32.0, 0.6) }

    $navy  = New-Color '#0B1220'
    $cyan  = New-Color '#22D3EE'
    $sky   = New-Color '#38BDF8'

    # Background disc + faint inner vignette glow
    $bg = New-Object System.Drawing.SolidBrush $navy
    $g.FillEllipse($bg, (Get-Rect $c $c $R))
    $bg.Dispose()
    Fill-Glow $g $c ($R * 0.95) (New-Color '#0E7490' 110) (New-Color '#0B1220' 0)

    if ($size -le 24) {
        # Simplified: outer ring, 3 bold arcs, big core.
        Draw-Ring $g $c ($R - 1.0 * $u) (2.0 * $u) $sky
        foreach ($a in 0, 120, 240) {
            Draw-Arc $g $c ($R * 0.56) (3.4 * $u) $cyan ($a - 90 + 20) 80
        }
        Fill-Glow $g $c ($R * 0.42) (New-Color '#22D3EE' 255) (New-Color '#22D3EE' 0)
        $core = New-Object System.Drawing.SolidBrush (New-Color '#ECFEFF')
        $g.FillEllipse($core, (Get-Rect $c $c ($R * 0.20)))
        $core.Dispose()
    }
    else {
        $detail = $size -ge 64
        # Outer rim
        Draw-Ring $g $c ($R - 0.9 * $u) (1.4 * $u) $sky
        if ($detail) {
            # Thin tick ring just inside the rim
            $tick = New-Object System.Drawing.Pen (New-Color '#38BDF8' 150), ([single](0.9 * $u))
            $tick.DashStyle = [System.Drawing.Drawing2D.DashStyle]::Custom
            $tick.DashPattern = [single[]]@(0.6, 2.4)
            $g.DrawEllipse($tick, (Get-Rect $c $c ($R * 0.86)))
            $tick.Dispose()
        }
        # Three arc segments with a soft glow underneath
        $aw = if ($detail) { 3.6 } else { 2.8 }
        foreach ($a in 0, 120, 240) {
            $st = $a - 90 + 14
            Draw-Arc $g $c ($R * 0.66) (($aw + 2.4) * $u) (New-Color '#22D3EE' 60) $st 92
            Draw-Arc $g $c ($R * 0.66) ($aw * $u) $cyan $st 92
        }
        # Inner ring
        Draw-Ring $g $c ($R * 0.44) (1.1 * $u) $sky
        # Core glow
        Fill-Glow $g $c ($R * 0.40) (New-Color '#67E8F9' 255) (New-Color '#22D3EE' 0)
        Fill-Glow $g $c ($R * 0.24) (New-Color '#FFFFFF' 255) (New-Color '#A5F3FC' 120)
        $core = New-Object System.Drawing.SolidBrush (New-Color '#F0FDFF')
        $g.FillEllipse($core, (Get-Rect $c $c ($R * 0.13)))
        $core.Dispose()
    }
    $g.Dispose()

    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g2 = [System.Drawing.Graphics]::FromImage($bmp)
    $g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g2.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g2.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g2.Clear([System.Drawing.Color]::Transparent)
    $g2.DrawImage($big, 0, 0, $size, $size)
    $g2.Dispose()
    $big.Dispose()
    return $bmp
}

$sizes = 16, 24, 32, 48, 64, 128, 256
$pngs = New-Object System.Collections.Generic.List[byte[]]
foreach ($s in $sizes) {
    $bmp = Render-Icon $s
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    if ($PreviewDir) {
        if (-not (Test-Path $PreviewDir)) { New-Item -ItemType Directory -Path $PreviewDir | Out-Null }
        $bmp.Save((Join-Path $PreviewDir "icon_$s.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    }
    $bmp.Dispose()
    $pngs.Add($ms.ToArray())
    $ms.Dispose()
}

# ICO container: ICONDIR (6 bytes) + ICONDIRENTRY (16 bytes each) + PNG blobs.
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([uint16]0)               # reserved
$w.Write([uint16]1)               # type: icon
$w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $dim = if ($s -ge 256) { 0 } else { $s }   # 0 means 256
    $w.Write([byte]$dim)          # width
    $w.Write([byte]$dim)          # height
    $w.Write([byte]0)             # palette colors
    $w.Write([byte]0)             # reserved
    $w.Write([uint16]1)           # color planes
    $w.Write([uint16]32)          # bits per pixel
    $w.Write([uint32]$pngs[$i].Length)
    $w.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $w.Write($p) }
$w.Flush()

$dir = Split-Path -Parent $OutFile
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
$bytes = $out.ToArray()
$w.Dispose()
[System.IO.File]::WriteAllBytes($OutFile, $bytes)
Write-Host "Wrote $OutFile ($($bytes.Length) bytes, sizes: $($sizes -join ', '))"
