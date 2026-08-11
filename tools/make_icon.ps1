# Convert icon.png into a multi-size icon.ico for the Windows resource
# compiler. Embeds each resolution as a PNG-in-ICO (Vista+ format).
param(
    [string]$Png = "icon.png",
    [string]$Ico = "icon.ico"
)

Add-Type -AssemblyName System.Drawing

$pngPath = (Resolve-Path $Png).Path
$icoPath = Join-Path (Split-Path $pngPath -Parent) $Ico

$src = [System.Drawing.Image]::FromFile($pngPath)
try {
    # Centre-square crop so the icon is always round-safe.
    $sz = [Math]::Min($src.Width, $src.Height)
    $x  = [int](($src.Width  - $sz) / 2)
    $y  = [int](($src.Height - $sz) / 2)
    $cropRect = New-Object System.Drawing.Rectangle $x, $y, $sz, $sz

    $sizes = @(256, 128, 64, 48, 32, 16)
    $pngs  = New-Object System.Collections.Generic.List[byte[]]

    foreach ($s in $sizes) {
        $bmp = New-Object System.Drawing.Bitmap $s, $s
        try {
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            try {
                $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
                $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                $dst = New-Object System.Drawing.Rectangle 0, 0, $s, $s
                $g.DrawImage($src, $dst, $cropRect, [System.Drawing.GraphicsUnit]::Pixel)
            } finally { $g.Dispose() }
            $ms = New-Object System.IO.MemoryStream
            try {
                $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
                [void]$pngs.Add([byte[]]$ms.ToArray())
            } finally { $ms.Dispose() }
        } finally { $bmp.Dispose() }
    }

    $count = $pngs.Count
    $headerBytes = 6 + 16 * $count
    $dataBytes = 0
    foreach ($p in $pngs) { $dataBytes += $p.Length }
    $buf = New-Object byte[] ($headerBytes + $dataBytes)

    # ICONDIR
    $buf[0] = 0; $buf[1] = 0
    $buf[2] = 1; $buf[3] = 0   # type = ICO
    $buf[4] = $count -band 0xff; $buf[5] = ($count -shr 8) -band 0xff

    $offset = $headerBytes
    for ($i = 0; $i -lt $count; $i++) {
        $s = $sizes[$i]
        $e = 6 + 16 * $i
        $buf[$e + 0] = if ($s -eq 256) { 0 } else { $s }
        $buf[$e + 1] = if ($s -eq 256) { 0 } else { $s }
        $buf[$e + 2] = 0        # colors in palette
        $buf[$e + 3] = 0        # reserved
        $buf[$e + 4] = 1; $buf[$e + 5] = 0   # planes
        $buf[$e + 6] = 32; $buf[$e + 7] = 0  # bit count
        $sz32 = [System.BitConverter]::GetBytes([uint32]$pngs[$i].Length)
        for ($k = 0; $k -lt 4; $k++) { $buf[$e + 8 + $k] = $sz32[$k] }
        $off32 = [System.BitConverter]::GetBytes([uint32]$offset)
        for ($k = 0; $k -lt 4; $k++) { $buf[$e + 12 + $k] = $off32[$k] }
        [System.Array]::Copy($pngs[$i], 0, $buf, $offset, $pngs[$i].Length)
        $offset += $pngs[$i].Length
    }

    [System.IO.File]::WriteAllBytes($icoPath, $buf)
    Write-Host "wrote $icoPath ($($buf.Length) bytes, $count sizes)"
} finally {
    $src.Dispose()
}
