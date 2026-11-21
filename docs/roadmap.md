# Roadmap, complexity and run estimate

## How hard is this?

**Overall: hard but well understood.** It's not research: every piece has prior art. But it has three hard parts and a long reliability tail.

Complexity scale: 1 = routine · 3 = real engineering · 5 = hard, high risk of surprises.

| Subsystem | Complexity | Why |
|-----------|-----------|-----|
| usbmux client + device detection | 2 | Small protocol with known gotchas. Risk sits in AMDS behaviour (trust, Listen), which spike R1 settles |
| Wire protocol | 2 | Our own design, simple framing |
| iPhone capture + encode + send | 3 | Standard AVFoundation/VideoToolbox, but tuning for low latency and iOS lifecycle edge cases |
| iPhone dev loop | 3 | The owner has a Mac (Xcode, debugger), so this was cut from 4. Still left: the phone can only be cabled to one computer at a time, the free account's 7-day/3-app/10-ID limits, tests done by hand |
| Decode (FFmpeg D3D11VA) | 3 | Well-trodden, but zero-copy setup and device-loss handling take care |
| GPU pipeline (crop/scale/colour/convert/readback) | 3 | Shaders are simple. Correct colour handling and stall-free readback are the work |
| **MF virtual camera (Windows 11)** | **5** | COM source inside Frame Server (session 0, LOCAL SERVICE), cross-session IPC, opaque failures, per-app quirks |
| DirectShow filter (Windows 10) | 3 | Old but documented, has to be built for x86 + x64 |
| Capability negotiation | 4 | Combinations of phone formats × lenses × consumers × link × thermal. Easy to get subtly wrong |
| Camera controls | 3 | Many controls, lens-dependent support, virtual-device quirks |
| Auto-framing | 4 | The maths is easy. Making it *feel* good (no jitter, no seasickness) is iterative tuning |
| WinUI 3 UI | 3 | Lots of screens. SwapChainPanel interop |
| Installer + prerequisites + onboarding | 3 | Elevation, COM, virtual camera registration, detecting AMDS |
| **Reliability tail** (reconnect, interruptions, thermal, sleep, TDR, multiple consumers) | **4** | Many small cases, and only real-device time finds some of them |

## What a "run" means here
One focused Claude Code session on one milestone, ending in a **buildable, tested increment** pushed to the repo, plus a short checklist for you to test on the device. A run is typically a few hours of agent work. Between runs, you test with the iPhone and report back. That human loop is the real bottleneck, not the coding.

## Plan

### Progress
| Run | Status | Page |
|-----|--------|------|
| R1 | Built and tested on the Windows side. Waiting for the device test | [runs/R1.md](runs/R1.md) |

### Phase 0: spikes (do these first; they can change the plan)
| Run | Goal | Exit criteria | You do |
|-----|------|---------------|--------|
| **R1** | Repo, toolchain, CI skeleton (Windows build + macOS IPA). Minimal companion that listens on loopback. Windows console tool: list USB devices via AMDS, connect, **measure throughput**, echo test | IPA installs through SideStore. Tool connects over USB, reports Mbit/s. Answers on trust needed? and Local Network prompt? | Install Apple Devices, SideStore, the IPA. Run the tool |
| **R2** | MF virtual camera spike: test-pattern source registered on this PC, visible and streaming in Edge/Chrome, Teams, Zoom, Discord, OBS. Prototype engine→Frame Server IPC | Pattern appears in all of them, or we know which fail and why. D-09 decided. Decided whether the DirectShow filter is also needed on Windows 11 | Install Zoom/Teams/Discord/OBS if not present (I can test once they're installed) |

**If R1 fails** (AMDS doesn't tunnel to app ports on modern iOS without something we can't do), the project needs a rethink before more is spent. That's unlikely, since other webcam apps do this today, but that's why it goes first.

### Phase 1: MVP, "cable in → camera works" (P0)
| Run | Goal |
|-----|------|
| **R3** | Companion: wide camera, fixed 1080p30 H.264, protocol v1 (HELLO/CONFIGURE/VIDEO/PING/LOG/STATUS). Engine: transport, decoder, D3D11, basic preview window. **Fake device** + first integration tests |
| **R4** | Engine → virtual camera end to end with the R2 IPC. DeviceManager state machine, reconnect, placeholder frames, interruption handling. Minimal WinUI 3 shell (preview + status line) |

**Milestone M1: a usable webcam at 1080p30.** Plug in, open both apps, and it appears in Teams.

### Phase 2: camera features (P1)
| Run | Goal |
|-----|------|
| **R5** | CAPS + bandwidth probe + Negotiator. Resolution/FPS selection (auto + manual), consumer format feedback, bitrate adaptation, keyframe recovery, HEVC path |
| **R6** | Lens selection + zoom: physical + virtual devices, ramps, presets, numeric readout, switch-over glitch hiding |
| **R7** | GPU geometry pass (crop/rotate/mirror/high-quality scale, 1:1 bypass) + auto-framing v1 (single person) + metadata recording/replay tool |
| **R8** | Auto-framing tuning + multi-person + settings. Focus, exposure, ISO, shutter, white balance, tap-to-focus. Thermal step-down |

**Milestone M2: feature-complete for P0+P1.**

### Phase 3: product (P1 polish, P2)
| Run | Goal |
|-----|------|
| **R9** | Full UI: main window, all settings pages, image adjustments pass, presets, tray behaviour |
| **R11** | Installer, AMDS detection + onboarding, start at login, SideStore source JSON, release pipeline, diagnostics page |

*R10 (Windows 10 DirectShow filter) is moved to after 1.0 (Q6). It's one run when it happens.*

**Milestone M3: installable beta.**

### Phase 4: hardening
| Run | Goal |
|-----|------|
| **R12** | Device/app matrix fixes from your testing. Colour correctness per consumer |
| **R13** | Soak + thermal + latency tuning. TDR/sleep/Frame Server restart recovery |
| **R14** | Bug bash, docs, release 1.0 |

## Totals
- **Baseline to 1.0: 13 runs** (R1–R9, R11–R14). Windows 11 only.
- **Realistic range: 11–17.**
  - +1–2 if spike R2 needs the broker-service fallback for IPC.
  - +1–3 for auto-framing feel and per-app quirks, which tend to need extra rounds.
  - −1 possible because the Mac speeds up the iPhone side (R1/R3 may merge).
- **After 1.0:** +1 run for Windows 10 (R10), +1 for a later Windows 10 matrix pass.
- **Where runs happen:** iPhone-heavy work (the companion parts of R1, R3, R5, R6, R8) works best as Claude Code sessions on your Mac. Everything else happens on this PC. Some runs touch both sides and can be split into a Mac half and a PC half.
- **Your time:** roughly 30–60 min of device testing after each iPhone-touching run (R1, R3–R8, R11–R13), and a longer session (2–3 h) for the matrix and soak in Phase 4.
- **Calendar:** depends almost entirely on how quickly you can test between runs. With a test session every day or two, M1 in about 1 week and 1.0 in about 4–6 weeks.

## Top risks
| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|-----------|
| Frame Server IPC / MF camera quirks | Medium | High | Spike R2 first, broker-service fallback |
| An iOS update breaks SideStore or free signing | Low–Medium | High (outside our control) | Support both AltStore and SideStore. A Mac with Xcode is the last resort |
| usbmux throughput on Lightning limits 4K60 | Medium | Low | HEVC + negotiation steps down automatically |
| Phone thermals in long 4K sessions | High | Medium | Thermal step-down, dim mode, defaults sized sensibly |
| Apps mishandle colour tags | Medium | Low–Medium | Test bars per app, compatibility option |
| AMDS behaviour changes (Apple Devices app updates) | Low | High | Our usbmux client is small and easy to patch, and the connection layer is isolated |
