<#
.SYNOPSIS
  Dev-only: install (or remove) the Cabled virtual camera media source so Frame Server can load it.
  Asks for admin once (UAC). The real installer replaces this in R11.

  - Copies cabled_vcam_mf.dll and cabled-vcam.exe from build\bin\Release to
    C:\Program Files\Cabled\Dev (Frame Server runs as LOCAL SERVICE and can't read user profile folders).
  - Stops Frame Server first so a loaded old DLL doesn't block the copy (it restarts on demand).
  - Registers the COM class under HKLM (regsvr32).
  - Creates C:\ProgramData\Cabled\logs, writable by LOCAL SERVICE, for the DLL's log.
  Writes a transcript to build\vcam-install.log.

.EXAMPLE
  .\windows\scripts\vcam-dev-install.ps1
  .\windows\scripts\vcam-dev-install.ps1 -Uninstall
#>
param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    $argsList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($Uninstall) { $argsList += '-Uninstall' }
    $p = Start-Process powershell.exe -Verb RunAs -ArgumentList $argsList -Wait -PassThru
    Get-Content (Join-Path $repo 'build\vcam-install.log') -ErrorAction SilentlyContinue
    exit $p.ExitCode
}

Start-Transcript -Path (Join-Path $repo 'build\vcam-install.log') -Force | Out-Null
try {
    $dest = Join-Path $env:ProgramFiles 'Cabled\Dev'

    # Try to stop Frame Server so it drops old DLLs; not required (see versioned copies below).
    # It fails while an app has a camera open, which is fine.
    foreach ($svc in 'FrameServer', 'FrameServerMonitor') {
        $s = Get-Service $svc -ErrorAction SilentlyContinue
        if ($s -and $s.Status -ne 'Stopped') {
            try { Stop-Service $svc -Force -ErrorAction Stop; Write-Host "Stopped $svc" }
            catch { Write-Host "Couldn't stop $svc (a camera is probably in use) - continuing" }
        }
    }

    if ($Uninstall) {
        $clsidKey = 'HKLM:\SOFTWARE\Classes\CLSID\{5A7CE6A3-D22D-4EF6-BC88-E7775C0315E6}'
        Remove-Item $clsidKey -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "Unregistered COM class"
        Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "Removed $dest (files still in use are deleted after a restart of Frame Server)"
        exit 0
    }

    # Each install gets its own file name, so a DLL that Frame Server still has loaded never blocks
    # the update: new camera sessions load the newly registered file.
    $src = Join-Path $repo 'build\bin\Release'
    New-Item -ItemType Directory -Force $dest | Out-Null
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $dll = Join-Path $dest "cabled_vcam_mf-$stamp.dll"
    Copy-Item (Join-Path $src 'cabled_vcam_mf.dll') $dll -Force
    Copy-Item (Join-Path $src 'cabled-vcam.exe') $dest -Force -ErrorAction SilentlyContinue
    Write-Host "Copied to $dll"

    # Old copies: delete the ones nothing has loaded any more.
    Get-ChildItem $dest -Filter 'cabled_vcam_mf*.dll' | Where-Object { $_.FullName -ne $dll } | ForEach-Object {
        $old = $_  # inside catch, $_ is the error, not the file
        try { Remove-Item $old.FullName -Force -ErrorAction Stop; Write-Host "Removed old $($old.Name)" }
        catch { Write-Host "Keeping $($old.Name) for now (still loaded)" }
    }

    $logs = Join-Path $env:ProgramData 'Cabled\logs'
    New-Item -ItemType Directory -Force $logs | Out-Null
    & icacls.exe $logs /grant '*S-1-5-19:(OI)(CI)M' '*S-1-5-32-545:(OI)(CI)M' | Out-Null  # LOCAL SERVICE, Users
    Write-Host "Log folder $logs"

    & regsvr32.exe /s $dll
    if ($LASTEXITCODE -ne 0) { throw "regsvr32 failed with exit code $LASTEXITCODE" }
    Write-Host "Registered COM class {5A7CE6A3-D22D-4EF6-BC88-E7775C0315E6} -> $dll"
    exit 0
}
catch {
    Write-Host "ERROR: $_"
    exit 1
}
finally {
    Stop-Transcript | Out-Null
}
