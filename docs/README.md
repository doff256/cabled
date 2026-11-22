# Cabled — design docs

**Cabled** (working name) turns an iPhone into a wired webcam for Windows. You plug in a cable, open the app, and the camera works.

Status: **1.0.0 released (2026-11-22).** Run history and what's next are in [roadmap.md](roadmap.md); each run has a page in [runs/](runs/). The app's look follows the owner's design system, Mangrove Glass (see [windows-app.md](windows-app.md)). Licence: MIT.

## Reading order

| # | Doc | What's in it |
|---|-----|--------------|
| 1 | [requirements.md](requirements.md) | The product spec, cut down and numbered (`R-xx`) so other docs can point at it |
| 2 | [architecture.md](architecture.md) | Processes, data flow, threads, state machine, latency budget |
| 3 | [stack.md](stack.md) | Languages, frameworks, dependencies, licences, toolchain |
| 4 | [decisions.md](decisions.md) | Decision log (`D-xx`): options considered, the choice, and why |
| 5 | [transport-and-protocol.md](transport-and-protocol.md) | How USB works without a driver (usbmux), plus the Cabled wire protocol |
| 6 | [ios-companion.md](ios-companion.md) | iPhone app, free signing, sideloading, building without a Mac |
| 7 | [video-pipeline.md](video-pipeline.md) | Capture → encode → decode → GPU processing → output, quality, colour, negotiation |
| 8 | [virtual-camera.md](virtual-camera.md) | Windows 11 Media Foundation virtual camera, Windows 10 DirectShow fallback |
| 9 | [camera-controls-and-framing.md](camera-controls-and-framing.md) | Where each control lives, and the auto-framing algorithm |
| 10 | [windows-app.md](windows-app.md) | App modules, UI, settings, installer, prerequisites |
| 11 | [reliability-and-testing.md](reliability-and-testing.md) | Failure modes, the fake-device simulator, test matrix |
| 12 | [roadmap.md](roadmap.md) | Milestones, complexity ratings, number of runs |
| 13 | [open-questions.md](open-questions.md) | Things only the owner can decide |

## The short version

- **USB transport:** the Windows app talks to Apple's *Apple Mobile Device Service* (installed with "Apple Devices" or iTunes). That service tunnels a TCP connection over the cable to a port the iPhone app listens on. This is how existing iPhone webcam apps work over USB, and it needs no driver from us. The Wi-Fi paths this service offers are ignored on purpose.
- **Video:** the iPhone hardware-encodes H.264 (HEVC as an option) at about 10× normal streaming bitrate, with no B-frames. Windows decodes on the GPU (D3D11VA). Cropping, scaling and colour work happen on the GPU in one pass. When all adjustments are neutral, that pass does nothing to the image.
- **Virtual webcam:** Windows 11 uses Media Foundation `MFCreateVirtualCamera`, which needs no kernel driver and no paid signing. 1.0 targets Windows 11. A Windows 10 DirectShow fallback comes after 1.0.
- **Auto-framing:** the iPhone finds faces almost for free (the camera chip's face metadata). Windows does the framing: smoothing, headroom, and a crop taken from a higher-resolution source.
- **iPhone app:** Swift/SwiftUI with no third-party dependencies. Developed in Xcode on the owner's Mac. Releases are built by public-repo CI as an unsigned `.ipa`, and users install them with SideStore or AltStore using a free Apple ID.
- **Windows app:** a C++20 engine (transport, decode, GPU, virtual camera) plus a C# WinUI 3 interface.
- **Effort:** 13 runs to 1.0 (realistic range 11–17). Two spike runs come first, because two parts could still sink the plan. See [roadmap.md](roadmap.md).
- **Test phones:** iPhone Air (one rear lens, USB 2 path) and iPhone 17 Pro (three lenses, USB 3).

## Facts about the dev machine (checked 2026-11-21, before R1; the toolchain is now installed, see CLAUDE.md)

- Windows 11 IoT Enterprise LTSC, build 26100. That is new enough for `MFCreateVirtualCamera` (needs build 22000+).
- GPU: AMD Radeon RX 9070 XT. Its hardware decoder handles H.264, HEVC and AV1.
- Installed: git, Microsoft Store.
- **Not installed:** Visual Studio / MSVC, .NET SDK, CMake, Apple Mobile Device Service, GitHub CLI.
- The owner also has a Mac (for iPhone builds) and a public GitHub repo is planned.
