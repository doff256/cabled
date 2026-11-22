<#
.SYNOPSIS
  Builds the Windows installer: build\installer\Cabled-Setup-<version>.exe (R11, D-17).
  Expects the C++ side already built (cmake --build build --config Release). Used by CI's release
  workflow too.

.EXAMPLE
  .\windows\scripts\build-installer.ps1
  .\windows\scripts\build-installer.ps1 -Version 0.2.0
#>
param(
    [string]$Version = '',
    [string]$Iscc = ''
)

$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$native = Join-Path $repo 'build\bin\Release'
$publish = Join-Path $repo 'build\publish'
$csproj = Join-Path $repo 'app\Cabled.App\Cabled.App.csproj'

if (-not $Version) {
    $Version = ([xml](Get-Content $csproj)).Project.PropertyGroup.Version | Where-Object { $_ } | Select-Object -First 1
}
if (-not $Iscc) {
    $Iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
              "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Iscc) { throw 'Inno Setup 6 not found (winget install JRSoftware.InnoSetup)' }
foreach ($f in 'cabled_core.dll', 'cabled_vcam_mf.dll', 'cabled-vcam.exe') {
    if (-not (Test-Path (Join-Path $native $f))) { throw "$f missing in ${native}: build the C++ side first" }
}

Write-Host "Cabled $Version"
Remove-Item $publish -Recurse -Force -ErrorAction SilentlyContinue
# Own bin folder, so a Cabled.exe running from the dev build folder doesn't block this.
& dotnet publish $csproj -c Release -o $publish -p:Version=$Version "-p:BaseOutputPath=$(Join-Path $repo 'build\app-bin')\"
if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed ($LASTEXITCODE)" }

# dotnet publish leaves out an unpackaged WinUI app's compiled XAML (*.xbf) and its resource index
# (Cabled.pri); without them the window can't load and the app crashes at start (seen in R11).
$built = Get-ChildItem (Join-Path $repo 'build\app-bin') -Recurse -Filter 'Cabled.pri' |
    Where-Object { $_.FullName -match '\\Release\\' } | Select-Object -First 1
if (-not $built) { throw 'Cabled.pri not found in build\app-bin' }
# Subfolders too: the theme compiles to Themes\Mangrove.xbf.
Get-ChildItem $built.DirectoryName -File -Recurse | Where-Object { $_.Extension -in '.pri', '.xbf' } |
    ForEach-Object {
        $rel = $_.FullName.Substring($built.DirectoryName.Length + 1)
        $dest = Join-Path $publish $rel
        if (-not (Test-Path $dest)) {
            New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
            Copy-Item $_.FullName $dest; Write-Host "Added $rel"
        }
    }
foreach ($f in 'Cabled.pri', 'MainWindow.xbf', 'App.xbf', 'Themes\Mangrove.xbf', 'Assets\Fonts\Geist.ttf') {
    if (-not (Test-Path (Join-Path $publish $f))) { throw "$f missing from the publish folder" }
}

# The virtual camera and its helper sit next to the app.
Copy-Item (Join-Path $native 'cabled_vcam_mf.dll'), (Join-Path $native 'cabled-vcam.exe') $publish -Force
# Licences: ours (MIT) and FFmpeg's (LGPL 2.1, shipped as replaceable DLLs).
Copy-Item (Join-Path $repo 'LICENSE') (Join-Path $publish 'LICENSE.txt') -Force
$ffmpegLicense = Get-ChildItem (Join-Path $repo 'build') -Recurse -Filter 'copyright' -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\ffmpeg\\' } | Select-Object -First 1
if ($ffmpegLicense) { Copy-Item $ffmpegLicense.FullName (Join-Path $publish 'FFmpeg-LICENSE.txt') -Force }

& $Iscc "/DAppVersion=$Version" "/DSourceDir=$publish" "/O$(Join-Path $repo 'build\installer')" (Join-Path $repo 'installer\Cabled.iss')
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed ($LASTEXITCODE)" }
Get-ChildItem (Join-Path $repo 'build\installer') -Filter '*.exe' | ForEach-Object {
    $hash = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Host "$($_.Name)  $([math]::Round($_.Length / 1MB, 1)) MB  sha256 $hash"
}
