# iPhone companion

## Scope
It does what R-23 lists and nothing more. One screen, no settings worth mentioning. Everything a user would adjust lives on Windows.

## Stack
- Swift 6, SwiftUI, AVFoundation, VideoToolbox, Network.framework (or BSD sockets), CoreMotion for orientation.
- **No third-party dependencies.**
- **No app extensions, no App Groups, no iCloud, no push.** With a free Apple ID, each extension uses up a scarce App ID (10 per 7 days) and adds signing failure points. One target, one bundle.
- Minimum **iOS 17** (proposed, Q11). That covers iPhone XS/XR and newer. It gives `videoRotationAngle`, current AVCapture APIs, and stable behaviour with Developer Mode and SideStore.

## Modules

| Module | Job |
|--------|-----|
| `SessionCoordinator` | The phone's state machine: Idle → Listening → Connected → Streaming ⇄ Interrupted. The only owner of the other modules |
| `CameraController` | Finds devices, lists formats for `CAPS`, configures `AVCaptureSession`, applies controls, reports live values |
| `Encoder` | `VTCompressionSession` with real-time settings, bitrate changes on the fly, forced IDR |
| `MetadataSource` | `AVCaptureMetadataOutput` for `.face` (+ `.humanBody`). Matches boxes to frames by timestamp |
| `Transport` | Listener bound to **127.0.0.1:47800 only**, one client, message framing, send queue capped at 2 frames |
| `StatusView` | The one screen |
| `SigningInfo` | Reads `embedded.mobileprovision` from the app bundle to get `ExpirationDate`, which goes in `HELLO` |

Binding to loopback only matters: connections forwarded by usbmux arrive on the phone's loopback interface. Never listening on Wi-Fi interfaces keeps R-60 true and should avoid iOS's Local Network permission prompt. **Verify in spike R1.**

## Camera session
- Devices found with `AVCaptureDevice.DiscoverySession` across `builtInWideAngleCamera`, `builtInUltraWideCamera`, `builtInTelephotoCamera`, `builtInTrueDepthCamera` (front), plus the **virtual** devices `builtInTripleCamera`, `builtInDualWideCamera`, `builtInDualCamera`. The virtual devices switch lenses seamlessly while you zoom (see camera-controls-and-framing.md).
- Output: `AVCaptureVideoDataOutput`, pixel format `420v` (8-bit, BT.709 video range), `alwaysDiscardsLateVideoFrames = true`.
- `activeFormat` + `activeVideoMin/MaxFrameDuration` are set exactly from `CONFIGURE`.
- Stabilisation **off** by default. It adds latency and crops the image. The Windows app can switch it on.
- HDR video **off** (SDR only, D-12).
- Rotation is applied at capture via `AVCaptureConnection.videoRotationAngle`. The camera hardware rotates the image for free.

## Encoder settings (starting values; tuning happens in R5)
- H.264 High (default) or HEVC Main, chosen by `CONFIGURE`.
- `RealTime = true`, `AllowFrameReordering = false` (no B-frames), `MaxFrameDelayCount = 0`.
- Low-latency rate control (`EnableLowLatencyRateControl`) where the codec and iOS version support it. It's documented for H.264; HEVC support is **unverified**.
- `AverageBitRate` from `CONFIGURE`, `DataRateLimits` at about 1.5× over 1 s, `MaxKeyFrameIntervalDuration = 2 s`, IDR on demand.
- Hardware encoder required. If it isn't available, report an error rather than quietly falling back to software.

## Lifecycle and iOS constraints
- **Foreground only** (R-24). When the app goes to the background, iOS interrupts the camera. We stop cleanly and report `STATUS: interrupted(background)`.
- `UIApplication.isIdleTimerDisabled = true` while connected, so the phone doesn't auto-lock. That would kill the camera.
- **Dim mode:** after 20 s without a touch, the screen turns pure black with a tiny status line, and screen brightness drops to about 10%. The original brightness comes back on touch or disconnect. On OLED screens this saves real battery and heat.
- Interruptions handled and reported with their reason (`AVCaptureSession.wasInterruptedNotification`): app in background, camera used by another app (FaceTime), split-screen/multiple foreground apps, **system pressure (thermal)**. Phone calls interrupt the camera too.
- Thermal: watch `ProcessInfo.thermalState` and the device's `systemPressureState`. At *serious*, the phone asks Windows to step down, and Windows picks the lower setting (policy stays on Windows). At *critical*, iOS stops capture itself, so we report it.
- Battery and charging state reported. A PC USB port gives roughly 500–900 mA, and long 4K60 sessions with the screen on can still drain the battery. The Windows UI shows this.

## Status screen (the only UI)
- A large state line: "Waiting for PC" / "Connected — streaming 3840×2160 @ 30" / "Paused: camera in use by another app".
- Camera permission: explains why, links to Settings if denied.
- Signing expiry: "Expires in 2 days — refresh in SideStore". Turns amber under 2 days.
- Version number. Nothing else, and no camera preview by default (the preview is on Windows, and a preview costs power).

## Auto-launch tip (optional, documented in onboarding)
iOS Shortcuts → Automation → **When Charger Is Connected → Open App "Cabled"** (runs immediately, iOS 17+). The companion then opens itself when you plug in the cable. That's as close to "plug in and it works" as iOS allows. The PC can't launch iPhone apps.

## Signing and sideloading with a free Apple ID

| Fact | Consequence |
|------|-------------|
| Free signing lasts **7 days** | The app stops launching after 7 days unless refreshed. We show the expiry on the phone *and* on Windows (from `HELLO`) |
| At most **3 sideloaded apps** active per device with a free ID, and **SideStore/AltStore counts as one** | The user has 2 slots left. Cabled takes 1 |
| **10 App IDs per 7 days** per Apple ID | Never change the bundle ID, never add extensions |
| **Developer Mode** must be on (iOS 16+) | Onboarding explains it: Settings → Privacy & Security → Developer Mode |
| SideStore/AltStore **rewrite the bundle ID** | Nothing may depend on the exact bundle ID |

**Recommended: SideStore.** After the first setup it refreshes on the phone without needing a PC helper running. **Also supported: AltStore Classic + AltServer for Windows** (needs the non-Store iTunes and iCloud, which also gives AMDS).

We publish a **SideStore/AltStore source** (`apps.json` on GitHub Pages or in a release), so users install and update with one tap.

Out of scope: signing the IPA from inside the Windows app with the user's Apple ID. It would mean handling Apple ID passwords, and SideStore already does this well.

## Building and dev loop (owner has a Mac; repo is public)
iOS apps can only be compiled on macOS with Xcode.

- **Development:** on the Mac. Xcode signs with the owner's free Apple ID (Personal Team, also 7-day) and installs straight to the iPhone over the cable. There's a full debugger and Instruments (useful for thermal and energy profiling).
  - Best setup: **run Claude Code on the Mac** for iPhone-side runs, in the same GitHub repo. That session can build with `xcodebuild`, run XCTest and install to the phone. Windows-side runs happen in a Claude Code session on this PC. The two meet through the protocol spec (transport-and-protocol.md) and the repo.
  - Practical problem: the iPhone can only be plugged into one computer at a time. While testing end to end, it needs to be on the Windows PC, so logs reach us through the `LOG` messages streamed to the Windows app, not Xcode's console.
- **Releases:** a **GitHub Actions macOS runner** (free and unlimited, public repo) builds with `xcodebuild archive … CODE_SIGNING_ALLOWED=NO`, packages `Payload/Cabled.app` into `Cabled.ipa`, and attaches it to the release with the SideStore/AltStore source JSON. End users never need a Mac.
- XCTest unit tests run on the simulator in CI for protocol, state machine and format selection, using fake frames. The simulator has no camera.
- Note: an app installed from Xcode and one installed through SideStore have different bundle IDs, so they appear as two separate apps. Remove one before testing the other.
