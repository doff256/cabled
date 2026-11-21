# Virtual camera

This is the hardest Windows piece. The DLL runs inside processes we don't own. Bugs there show up as "the camera doesn't appear in Teams" with no error anywhere.

## Options

| Option | Windows 10 | Windows 11 | Visible to | Verdict |
|--------|-----------|-----------|-----------|---------|
| **Media Foundation virtual camera** (`MFCreateVirtualCamera`) | ❌ | ✅ build 22000+ | MF and WinRT apps (Windows Camera, Teams, Chromium/Edge/Discord, Zoom). DirectShow apps through Frame Server's DirectShow bridge (**to be confirmed in spike R2**, e.g. OBS) | ✅ **Primary on Windows 11** |
| **DirectShow source filter** (COM DLL, x64 + x86) | ✅ | ✅ | DirectShow apps (Zoom, OBS, Chromium, Discord, Skype…). **Not** apps that only use MF (Windows Camera app, some WinRT apps) | ✅ **Fallback for Windows 10** |
| Kernel AVStream driver | ✅ | ✅ | everyone | ❌ needs EV certificate + Microsoft attestation signing |

## Settled by spike R2 (2026-11-21, details in [runs/R2.md](runs/R2.md))
- **It works:** an unsigned media source in Frame Server delivers 1080p30/60 and 4K30 to Media Foundation apps, Chromium and DirectShow apps. Engine → app adds about 0.5 ms at 1080p and 2.3 ms at 4K.
- **IPC (D-09):** a single `Global\` section + event, created by the media source (Frame Server has the privilege) and opened by the engine. **No named pipe, no broker service.** The section exists only while an app has the camera, so the engine also knows when to wake the phone.
- **DirectShow on Windows 11 comes free** through Frame Server's bridge, which also adds YUY2 variants. No DirectShow filter needed on 11.
- **Admin:** only for COM registration in HKLM and installing the DLL somewhere LOCAL SERVICE can read. Creating the camera (`MFCreateVirtualCamera`, System lifetime, CurrentUser) needs no admin.
- Windows shows the device as **"Cabled Camera (Windows Virtual Camera)"**.
- **Colour:** Media Foundation consumers (Chromium) honour our BT.709 tags. DirectShow consumers get no matrix, and FFmpeg falls back to BT.601. → R12.
- With no constraints, Chromium opens **640×480**. → R5 decides the advertised list.

## Windows 11: Media Foundation virtual camera

### How it works
- We ship `cabled_vcam_mf.dll`, an in-process COM server implementing a **custom media source** (`IMFMediaSourceEx`, `IMFMediaStream2`, `IKsControl` stub, `IMFGetService`). It's based on Microsoft's open-source Windows-Camera *VirtualCamera / SimpleMediaSource* sample (MIT).
- The installer (elevated) registers the CLSID under HKLM. Then `MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System, MFVirtualCameraAccess_AllUsers, L"Cabled Camera", CLSID…)` + `Start()` registers the device **for good** (D-08). It appears in every app's camera list even when Cabled isn't running.
- When an app opens the camera, **Frame Server** (`svchost`, runs as **LOCAL SERVICE**, **session 0**) loads our DLL and pulls samples from it. Frame Server lets several apps share one camera (R-72).

### What we advertise (initial list, R-51)
Formats: **NV12** (main), **YUY2** (older apps), **RGB32** (rare apps). Sizes × FPS:

- 16:9 at 640×360, 960×540, 1280×720, 1600×900, 1920×1080, 2560×1440, 3840×2160 × {24, 25, 30, 50, 60}
- 4:3 at 640×480, 1440×1080 × {30, 60}
- We only list a size × FPS combination if the connected phone can deliver it, either natively or by cropping/scaling from a format it supports. Before any phone has connected, we use the list cached from the last phone seen, or the full default list.

Apps tend to pick the first or "best" type in the list, so **order matters**. 1080p30 NV12 comes first as the safe default that works everywhere, and the rest follow. When we tune output formats against real apps, we decide whether 1080p60 can go first without breaking anything.

### IPC between the engine and the Frame Server process (R2: simplified, see "Settled by spike R2" above; the pipe below turned out unnecessary)
The problem: the engine runs in the user's session without privileges. The DLL runs in session 0 as LOCAL SERVICE. `Local\` names don't cross sessions, and creating `Global\` objects needs `SeCreateGlobalPrivilege`, which normal user processes don't have.

**Proposed design:**
1. **Control pipe:** the engine creates `\\.\pipe\Cabled.VCam` with a DACL that allows `LOCAL SERVICE` and the current user. Named pipes cross sessions and need no special privilege.
2. **Frame ring:** the **DLL side** (which has the privilege) creates `Global\Cabled.VCam.Ring.<n>` with a DACL that grants the interactive user read/write access, and sends its name over the pipe. The engine opens it. The ring has 4 slots, each with a header (seq, pts, format, size) and the pixels, plus a `Global\` event for "new frame".
3. When no engine is connected to the pipe, the DLL draws the **placeholder** itself, a "Connect your iPhone / open Cabled" card. A consuming app therefore always gets frames, and its preview never freezes.
4. The DLL reports back over the pipe: the chosen media type, how many apps are consuming, start/stop events. That's input #5 for the Negotiator.

**Fallback if Frame Server can't be made to cooperate:** a tiny Cabled Windows service (LocalSystem) as a broker that creates the global objects. It's more to install, but a well-trodden path.

### Development gotchas (it's in the docs so future runs don't rediscover it)
- To debug, attach to the right `svchost` (the one running "Windows Camera Frame Server") or log through ETW/`OutputDebugString`. Frame Server caches loaded DLLs, so after replacing the DLL, **restart the FrameServer service** or the old code stays loaded.
- Code signing for the DLL is **believed not to be required** (Microsoft's sample works unsigned). Confirm in spike R2.
- Camera privacy settings in Windows apply. If "Let desktop apps access your camera" is off, our camera is blocked too, and the UI should say so.
- Windows Studio Effects don't apply to virtual cameras, which is good: no surprise processing.

## Windows 10: DirectShow source filter
- `cabled_vcam_ds.dll`, built **for x64 and x86** (32-bit apps like some Zoom builds or old OBS load the 32-bit one). Registered under `CLSID_VideoInputDeviceCategory` with the friendly name "Cabled Camera".
- Implements `IBaseFilter` + output pin + `IAMStreamConfig` (lists formats) + `IKsPropertySet` (pin category). References: *softcam* (MIT, OK to borrow ideas from) and the DirectShow base classes (Windows SDK samples, MIT). **Don't copy** from OBS's virtualcam module (GPLv2), unless the project ends up GPL (Q8).
- Each consuming app loads the filter into **its own process in the user's session**, so the IPC is simpler: a `Local\` section + event created by the engine. The ring layout is the same as on Windows 11.
- Several consumers is natural: each one reads the same ring.

### On Windows 11, should the DirectShow filter be installed as well?
Only if spike R2 shows that important DirectShow-only apps (OBS is the one to check) *don't* see the MF camera through Frame Server. Installing both risks two "Cabled Camera" entries. If both are needed, the DirectShow one gets a different name or is hidden when the MF camera is visible.

## Media type changes
- Within a consumer's session, the type is fixed once it starts streaming. When a second app joins while sharing, it gets the type already running.
- When the active type changes (an app restarts with a different resolution), the engine gets a pipe message and the Negotiator may reconfigure the phone.
