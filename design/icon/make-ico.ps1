# Renders design/icon/out/flat.svg (from make-icon.mjs) with headless Edge at each size and packs app/Cabled.App/Assets/Cabled.ico.
#   powershell -NoProfile -ExecutionPolicy Bypass -File design\icon\make-ico.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$here = $PSScriptRoot; $repo = Resolve-Path "$here\..\.."
$out = Join-Path $here 'out'
$edge = "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
$svgUri = ([Uri](Join-Path $out 'flat.svg')).AbsoluteUri
$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$pngs = @()
foreach ($s in $sizes) {
  # Draw the SVG at its native size in a larger transparent page, then crop (Edge clamps tiny windows).
  $html = Join-Path $out "r$s.html"
  [IO.File]::WriteAllText($html, "<html><body style='margin:0;background:transparent'><img src='$svgUri' width='$s' height='$s' style='display:block'></body></html>")
  $shot = Join-Path $out "shot$s.png"
  $edgeArgs = @("--headless=new","--disable-gpu","--hide-scrollbars","--force-device-scale-factor=1","--default-background-color=00000000",
    "--window-size=400,400","--screenshot=$shot",([Uri]$html).AbsoluteUri)
  Start-Process $edge -ArgumentList $edgeArgs -Wait -WindowStyle Hidden -RedirectStandardError (Join-Path $out "edge.log")
  for ($i = 0; $i -lt 50 -and -not (Test-Path $shot); $i++) { Start-Sleep -Milliseconds 100 }
  $src = [Drawing.Bitmap]::FromFile($shot)
  $bmp = $src.Clone([Drawing.Rectangle]::new(0, 0, $s, $s), [Drawing.Imaging.PixelFormat]::Format32bppArgb); $src.Dispose()
  $png = Join-Path $out "Cabled-$s.png"; $bmp.Save($png, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
  Remove-Item $html, $shot
  $pngs += $png
}
# ICO with PNG-compressed entries (supported since Vista).
$ms = New-Object IO.MemoryStream; $w = New-Object IO.BinaryWriter $ms
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$data = $pngs | ForEach-Object { ,[IO.File]::ReadAllBytes($_) }
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
  $s = $sizes[$i]; $b = if ($s -ge 256) { 0 } else { $s }
  $w.Write([byte]$b); $w.Write([byte]$b); $w.Write([byte]0); $w.Write([byte]0)
  $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$data[$i].Length); $w.Write([uint32]$offset)
  $offset += $data[$i].Length
}
foreach ($d in $data) { $w.Write($d) }
$assets = Join-Path $repo 'app\Cabled.App\Assets'
[IO.File]::WriteAllBytes((Join-Path $assets 'Cabled.ico'), $ms.ToArray())
Copy-Item (Join-Path $out 'Cabled-256.png') (Join-Path $assets 'Cabled-256.png') -Force
"wrote Cabled.ico ($($sizes -join ', ') px) and Cabled-256.png"
