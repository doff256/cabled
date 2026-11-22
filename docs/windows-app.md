# Windows application

## Modules (mapped to the spec's architecture tree)

| Spec module | Component | Language | Lives in |
|-------------|-----------|----------|----------|
| Device Manager: USB detection, identification, connection, reconnection | `usbmux::Client`, `DeviceManager` (state machine, see architecture.md) | C++ | cabled_core.dll |
| Video Receiver: USB comms, decode, buffering | `Transport`, `Protocol`, `Decoder`, mailboxes | C++ | cabled_core.dll |
| Camera Control | `CameraControl` (commands, coalescing, live state), `Negotiator` | C++ | cabled_core.dll |
| Image Processing: framing, crop, colour, scaling | `Framing`, `GpuPipeline` (D3D11 shaders) | C++ / HLSL | cabled_core.dll |
| Virtual Camera: registration, output, format management | `VCamPublisher` (engine side), `cabled_vcam_mf.dll`, `cabled_vcam_ds.dll` | C++ | separate DLLs |
| User Interface | WinUI 3 app: preview `SwapChainPanel`, view models | C# | Cabled.exe |

The engine has **no UI dependency** and can run headless. That's what the test harness and the fake-device tests drive.

## UI

### Main window
```
┌──────────────────────────────────────────────────────────────┐
│ ● Connected · iPhone 16 Pro · USB 3 · 3840×2160→1920×1080 @60 · 34 ms   [⚙] │
│ ┌──────────────────────────────────────────────────────────┐ │
│ │                                                          │ │
│ │                    live preview                          │ │
│ │          (exactly what the virtual camera sends)         │ │
│ │                                                          │ │
│ └──────────────────────────────────────────────────────────┘ │
│  Lens: [0.5×][1×][2×][5×] [Front]    Zoom ━━━●━━━━ 1.8×      │
│  [✓] Auto-framing  (headroom · sensitivity in ⚙)             │
│  Resolution [Auto ▾]   FPS [Auto ▾]                          │
│  Virtual camera: in use by Teams, OBS                         │
└──────────────────────────────────────────────────────────────┘
```
- The status line always says what is happening and **what to do next** when something is wrong. For example: "Plug in your iPhone", "Tap Trust on your iPhone", "Open Cabled on your iPhone", "Companion expires in 1 day — refresh in SideStore", "iPhone is hot — reduced to 1080p30".
- Clicking the preview sets the focus/exposure point (P1).

### Settings pages
- **Camera**: exposure (auto / EV / manual ISO + shutter), focus, white balance, stabilisation.
- **Image**: brightness, contrast, saturation, sharpness, crop, rotation, mirroring. A "Reset to neutral" button.
- **Auto-framing**: headroom, sensitivity, max zoom, multi-person, speed.
- **Quality** (advanced): manual source format, FPS, codec, bitrate ceiling, output size pin.
- **Presets** (P2): save, apply, rename, delete.
- **Diagnostics** (P2): link throughput, bitrate, FPS at each stage, latency at each stage, dropped frames, phone thermal state and battery, companion log viewer, "Copy report" (copies text to the clipboard; nothing is sent anywhere).
- **App**: start at login (on by default), tray behaviour, about/licences (MIT + FFmpeg LGPL notice).

### Window and tray behaviour (D-20)
- Closing the window → keep running in the tray (R9: an App setting, on by default, so a phone plugged in later still works). The stream keeps going. Quit is in the tray menu. Only one Cabled runs per user; launching it again shows the running one.
- **Start at login, in the tray:** offered in onboarding and **ticked by default**, and can be changed under App settings. It's registered per user (HKCU Run key), not as a service. The tray agent uses almost no resources while idle (only the usbmux `Listen` socket). Combined with the iPhone's Shortcuts auto-launch, that gets the full R-03 experience: plug in, and Zoom has a camera, with no clicks.

## Settings storage
- `%APPDATA%\Cabled\settings.json` for app settings, plus `devices\<UDID>.json` for per-iPhone camera settings and presets (different phones have different lenses).
- Settings are versioned, with migration on load. A corrupt file is renamed to `.bad` and defaults are used.

## Install and prerequisites
- **Installer: Inno Setup** (D-17). It asks for admin once, because it has to register COM classes in HKLM and register the persistent virtual camera.
  - Installs: Cabled.exe + .NET runtime (self-contained), cabled_core.dll, FFmpeg DLLs, cabled_vcam_mf.dll (Windows 11), cabled_vcam_ds.dll x64 + x86 (Windows 10, and Windows 11 only if spike R2 says so).
  - Uninstall removes the virtual camera, the COM registration and `%APPDATA%\Cabled` (asks first).
- **Prerequisite: Apple Mobile Device Service.** Detect it **by connecting to 127.0.0.1:27015**, not by looking up a service name. The Store "Apple Devices" app installs it as a *packaged* service that doesn't appear under the classic "Apple Mobile Device Service" name in the Services list (seen on the dev PC in R1). On first launch, if the port doesn't answer:
  - Store available → button opens the **Apple Devices** Store page.
  - No Store (LTSC / Store removed) → link to **iTunes for Windows** from apple.com (the non-Store installer, which includes AMDS).
  - We **don't bundle** Apple's driver or service MSI. Redistributing it isn't permitted as far as we know, and it ties us to one old version.
- **First-run onboarding** (one screen per step, each step checked live): Apple service ✓ → iPhone plugged in ✓ → Trusted ✓ → Companion installed? (link to the guide) → Developer Mode → Companion open ✓ → done. Plus the optional Shortcuts auto-launch tip.

## Code signing and SmartScreen (D-25: never signed)
- The installer and all binaries are **unsigned**, permanently.
- Users see SmartScreen "Windows protected your PC" → **More info → Run anyway**, then UAC "Unknown publisher" for the one elevation. The README and download page show this with screenshots.
- Each release publishes SHA-256 hashes, and builds come only from public CI.

## Updates
No automatic network checks (R-71). About → "Check for updates" opens the releases page in the browser. The companion version check happens over USB only (`HELLO`).
