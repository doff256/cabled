# Requirements

This is the owner's product spec, cut down and numbered so other docs can refer to it. If the spec and the docs ever disagree, the spec wins and the docs get fixed.

## Product

- **R-01** A lightweight Windows app that turns an iPhone into a high-quality *wired* webcam.
- **R-02** Priorities, in order: reliability → image quality → low latency → simplicity → feature count.
- **R-03** Philosophy: connect a cable, open the app, and the camera works. No configuration needed for basic use.
- **R-04** Positioning: a webcam utility. Not a streaming suite, camera editor or production studio.

## Platforms

- **R-10** Windows 10 and 11. (Whether Windows 10 is needed at launch is open: see open-questions Q6.)
- **R-11** iPhone, connected by USB only.
- **R-12** Output is a Windows virtual webcam that works in Discord, Zoom, Teams, OBS, browsers, and any other app that uses webcams.

## iPhone companion

- **R-20** Not on the App Store. No paid Apple Developer Program. Signed with a free Apple ID and sideloaded with AltStore or SideStore.
- **R-21** No accounts, ads, in-app purchases or subscriptions.
- **R-22** Free signing expires after 7 days, so the app must be re-signed. The app should make that easy to notice.
- **R-23** The companion only does these things: access the camera, capture, send over USB, take control commands, report status, handle permissions, and manage the camera session lifecycle.
- **R-24** It must run in the foreground with a minimal status screen. iOS doesn't allow capturing in the background.

## Windows app (the main product)

- **R-30** Detects the device automatically, handles the connection, reconnects automatically.
- **R-31** Main screen: live preview, connection status, lens choice, zoom, auto-framing, resolution, FPS, virtual camera status, settings.
- **R-32** Advanced settings: exposure, ISO, shutter, focus, white balance, brightness, contrast, saturation, sharpness, crop, rotation, mirroring, presets.

## Camera

- **R-40** Lens choice: every lens iOS exposes (front, main, ultra-wide, telephoto). Lenses the phone doesn't have stay hidden.
- **R-41** Zoom: digital zoom, optical lens switching, a slider, a numeric readout, quick presets, smooth transitions.
- **R-42** Auto-framing is on by default. It includes face detection and tracking, centring, dynamic crop, adjustable headroom, smooth movement, tracking sensitivity, a maximum zoom, and an optional multi-person mode.
- **R-43** Auto-framing crops from a higher-resolution source with as little resampling as possible.

## Quality

- **R-50** The best quality that the phone, the cable, the PC and the receiving app together can deliver.
- **R-51** No arbitrary caps on resolution or FPS. Every compatible resolution/FPS combination is available.
- **R-52** As little compression and generation loss as possible, at the highest practical bitrate. One encode only.
- **R-53** Accurate colour space handling. HDR "where supported" (see D-12).
- **R-54** As little latency and buffering as possible.
- **R-55** Hardware encode and decode.
- **R-56** No unnecessary rescaling, sharpening or processing. Keep the original feed when possible.
- **R-57** Automatic capability negotiation: camera formats and FPS, pixel formats, USB throughput, what the virtual camera can output, and what the receiving app picks. The best compatible configuration is chosen automatically.
- **R-58** Manual override of resolution, FPS and quality.

## Explicitly excluded

- **R-60** Wi-Fi or wireless of any kind, cloud, accounts, scenes, streaming integrations, chat, recording, overlays, emojis, filters, LUTs, background effects, spotlight, beauty/retouch, social features, mobile production tools.

## Priorities

| P0 | P1 | P2 |
|----|----|----|
| Reliable USB communication | Lens selection | Advanced image adjustments |
| iPhone camera capture | Zoom | Camera presets |
| Windows virtual webcam | Auto-framing | Connection diagnostics |
| Automatic device detection | Resolution / FPS controls | |
| | Focus and exposure | |

## Implied requirements (not in the spec, but needed)

- **R-70** Audio is **out**. A virtual microphone on Windows needs a signed kernel driver, which conflicts with R-20's spirit and costs money. Confirm: Q14.
- **R-71** The PC makes no network calls beyond localhost: no telemetry, no automatic update checks.
- **R-72** Multiple apps can use the virtual camera at once (for example Teams and OBS).
- **R-73** When no phone is connected, the virtual camera shows a clear placeholder ("Connect your iPhone"). It never shows a frozen or black frame without explanation.
