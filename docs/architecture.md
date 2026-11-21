# Architecture

## Processes

Five processes take part, on two devices, and we write three of them.

```
 iPHONE                                   WINDOWS
 ┌──────────────────────────┐            ┌──────────────────────────────────────────────┐
 │ Cabled Companion (Swift) │            │ Apple Mobile Device Service (Apple, 3rd-party)│
 │  AVCaptureSession        │   USB      │  usbmux daemon, listens 127.0.0.1:27015       │
 │  VideoToolbox encoder    │◄══════════►│  tunnels TCP ⇄ device port                    │
 │  face metadata           │  (usbmux)  └───────────────▲──────────────────────────────┘
 │  listener 127.0.0.1:N    │                            │ TCP (localhost)
 └──────────────────────────┘            ┌───────────────┴──────────────────────────────┐
                                         │ Cabled.exe  (user session)                    │
                                         │  ┌ UI  (C# / WinUI 3) ────────────────────┐   │
                                         │  │ preview, controls, settings, tray      │   │
                                         │  └──────────────┬─────────────────────────┘   │
                                         │                 │ C ABI                       │
                                         │  ┌ cabled_core.dll (C++20) ───────────────┐   │
                                         │  │ DeviceManager · Transport · Protocol   │   │
                                         │  │ Decoder (FFmpeg D3D11VA)               │   │
                                         │  │ GPU pipeline (D3D11) · Framing         │   │
                                         │  │ CameraControl · Negotiator             │   │
                                         │  │ VCam publisher (shared memory)         │   │
                                         │  └──────────────┬─────────────────────────┘   │
                                         └─────────────────┼────────────────────────────┘
                                                           │ shared memory ring + events + control pipe
                              ┌────────────────────────────┴───────────────────────────┐
                              │ Win 11: cabled_vcam_mf.dll inside Frame Server          │
                              │         (svchost, LOCAL SERVICE, session 0)             │
                              │ Win 10: cabled_vcam_ds.dll (x64 + x86) loaded into     │
                              │         each consuming app (Zoom, OBS, Chrome …)       │
                              └────────────────────────────┬───────────────────────────┘
                                                           ▼
                                         Zoom · Teams · Discord · OBS · browsers
```

### Why this split
- **The phone stays simple** (R-23): it captures, encodes, adds face boxes, sends, and obeys commands. All policy lives on Windows: which format, how to frame, presets, the UI.
- **The engine is native C++.** It's where performance matters, and the virtual camera DLLs must be C++ anyway (they load into processes we don't control).
- **The UI is managed code** to make a polished Fluent UI quicker to build. The UI never touches frames, except through a DXGI swapchain the engine hands it for the preview.
- **The virtual camera DLL is dumb.** It copies frames out of shared memory and reports which format the consuming app picked. All logic stays in the engine, so a bug can't take down Frame Server or someone's Zoom call.

## Data flow per frame

1. iPhone sensor → ISP → `CVPixelBuffer` (NV12, BT.709 video range) at the chosen source resolution/FPS.
2. Face/body metadata for that frame (AVCaptureMetadataOutput) gets the same presentation timestamp.
3. VideoToolbox encodes it (real time, no frame reordering) → NAL units.
4. A VIDEO message (header + face boxes + NALs) is written to the socket. usbmux carries it to Windows.
5. Transport thread on Windows reads the message → **latest-frame mailbox** → decoder.
6. Decoder (D3D11VA) produces an NV12 D3D11 texture with no copy.
7. GPU pipeline: Framing computes the crop rect for this frame → one sampling pass (crop + rotate + mirror + scale) → optional adjustments pass → convert to output format (NV12/YUY2/RGB32) → copy to staging texture (ring of 3).
8. The CPU maps the staging texture and copies it into a slot of the shared memory ring, then signals the frame event.
9. Virtual camera DLL wakes, copies the slot into an MF sample (or DirectShow sample) with the capture timestamp, and delivers it to the consuming app.
10. Preview: the same output texture is drawn into the UI's swapchain, so what you see is exactly what the virtual camera sends.

Every stage boundary uses a **single-slot "latest wins" mailbox**, not a queue. When a later stage falls behind, older frames are dropped and latency doesn't build up. The one exception is compressed frames: a P-frame can't be dropped before the decoder without breaking the picture (see transport-and-protocol.md → flow control).

## Threads (Windows engine)

| Thread | Work | Blocking? |
|--------|------|-----------|
| DeviceWatch | usbmux `Listen` connection: attach/detach events | blocks on socket |
| Transport | reads/writes the tunnelled socket, parses messages | blocks on socket |
| Decode | sends packets to libavcodec, receives textures | GPU sync |
| Render | framing math, GPU passes, staging readback, publish to shared memory, preview present | paced by source frames; repeats the last frame when needed |
| Control | serialises camera commands to the phone, debounces sliders | event-driven |
| UI | WinUI dispatcher (C#), never blocked by the engine | — |

The engine talks to the UI through a C ABI: commands go in, an event callback comes out, and a state snapshot is available on request. The UI marshals events onto its dispatcher.

## Connection state machine (Windows)

```
                ┌──────────────┐  AMDS missing / stopped
   start ──────►│ NoService    │◄──────────────────────────── (any state)
                └──────┬───────┘
                       │ service reachable
                ┌──────▼───────┐
                │ NoDevice     │◄──────── USB detach (any state below)
                └──────┬───────┘
                       │ Attached event, ConnectionType == USB
                ┌──────▼──────────────┐ connect refused → retry every 1s
                │ WaitingForCompanion │ (UI: "Open Cabled on your iPhone")
                └──────┬──────────────┘
                       │ TCP connect OK
                ┌──────▼───────┐ version mismatch → Incompatible (UI: update companion)
                │ Handshaking  │ HELLO ⇄ HELLO, CAPS, probe bandwidth
                └──────┬───────┘
                       │ CONFIGURED
                ┌──────▼───────┐ camera interrupted (call, background, thermal)
                │ Streaming    │──────────► Paused ──► back to Streaming on resume
                └──────────────┘ socket closed → WaitingForCompanion
```

While not in Streaming, the virtual camera shows a **placeholder frame** that reflects the state (R-73). When Streaming briefly glitches (lens switch, reconfigure), the virtual camera **repeats the last good frame** for up to ~1 s before switching to the placeholder.

## Latency budget (target: glass to consuming app)

| Stage | 30 fps | 60 fps |
|-------|--------|--------|
| Exposure + ISP readout | ~20 ms | ~12 ms |
| Encode (real time, no reorder) | 5–10 ms | 4–8 ms |
| USB / usbmux transit (1080p frame ~100 KB) | 1–4 ms | 1–4 ms |
| Decode (D3D11VA) | 2–5 ms | 2–5 ms |
| GPU pipeline + readback | 2–4 ms | 2–4 ms |
| Shared memory → virtual camera → app | 1–5 ms | 1–5 ms |
| **Total, Cabled's share** | **~35–50 ms** | **~25–40 ms** |

The consuming app adds its own buffering and rendering, often 30–100 ms in conferencing apps. Cabled can't control that part. Every run that touches the pipeline should re-measure end-to-end latency (see reliability-and-testing.md).
