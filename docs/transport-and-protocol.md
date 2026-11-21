# Transport and wire protocol

## 1. Getting bytes over the cable

### Options considered

| Option | Verdict |
|--------|---------|
| **usbmux via Apple Mobile Device Service (AMDS)** | ✅ **Chosen.** The same mechanism iTunes, Finder sync and existing iPhone webcam apps use. No driver from us, no admin access at runtime, works on every iPhone |
| Our own usbmux over libusb/WinUSB | ❌ Means replacing Apple's USB driver with WinUSB (Zadig-style), which breaks iTunes/Apple Devices. Fragile and hostile to users |
| USB tethering (Personal Hotspot over USB) | ❌ Needs hotspot turned on and depends on the carrier. It's really networking, not a cable connection |
| Custom USB accessory protocol (MFi / External Accessory) | ❌ Needs MFi hardware certification |
| A kernel driver of our own | ❌ Needs EV certificate + Microsoft attestation signing, which costs money |

### How usbmux works on Windows

- AMDS is a Windows service installed by **Apple Devices** (Microsoft Store, Windows 10 22H2+/11) or **iTunes** (installer from apple.com, for machines without the Store). It listens on **TCP 127.0.0.1:27015**.
- Messages: a 16-byte header (`length u32`, `version u32 = 1`, `type u32 = 8 (plist)`, `tag u32`) followed by an XML property list. All fields little-endian.
- `ListDevices` / `Listen`: AMDS returns the attached devices and then pushes `Attached` / `Detached` events. Each has `DeviceID`, `SerialNumber` (UDID), `ProductID`, and **`ConnectionType` (`USB` or `Network`)**.
  - **We ignore every `Network` device.** AMDS also shows phones reachable over Wi-Fi sync. Using them would break R-60 (USB only).
- `Connect {DeviceID, PortNumber}`: on success (`Number = 0`), *that same socket* becomes a raw byte pipe to `127.0.0.1:PortNumber` on the iPhone. `Number = 3` means connection refused, which in practice means the companion isn't running or listening.
  - Known gotcha: `PortNumber` is the port in **network byte order** packed into an integer (port 47800 is sent as the byte-swapped value).
- Trust: the user taps **"Trust This Computer"** once. AMDS stores the pairing record. Whether `Connect` to an app port *needs* trust is **unverified**. Spike R1 checks this, and the UI must handle both cases.
- **We write our own usbmux client** (D-02). It's a few hundred lines: a plist writer/parser plus the three messages above. libimobiledevice's `libusbmuxd` is a reference only, which keeps LGPL and its build chain out.

### Throughput (needs measuring, spike R1)

| iPhone port | Link | Expected usable usbmux throughput |
|-------------|------|-----------------------------------|
| Lightning (all up to iPhone 14) | USB 2.0 High Speed | ~150–300 Mbit/s (estimate) |
| USB-C, non-Pro iPhone 15/16/17 | USB 2.0 | same as above |
| USB-C, Pro iPhone 15+ with a USB 3 cable | USB 3 (10 Gbit/s) | unknown. Possibly much higher, possibly capped by the Apple stack |

Even the low end fits high-bitrate H.264/HEVC at every resolution, including 4K60 at about 100 Mbit/s. Uncompressed video doesn't fit USB 2 (raw 1080p30 NV12 is about 750 Mbit/s), so **compression is unavoidable**. We use one high-bitrate hardware encode (see video-pipeline.md).

## 2. Cabled wire protocol (v1)

### Transport shape
- **One TCP connection** per session, tunnelled through usbmux to the companion's listener on `127.0.0.1:47800` (port is provisional).
- The companion accepts **one client at a time**. A new connection replaces the old one, so a crashed PC app never leaves the phone stuck.
- Why not two connections (separate control and video)? A 4K keyframe is at most about 2 MB, which is roughly 50–80 ms on USB 2. A control command behind it gets delayed that much, which is acceptable. If it turns out not to be, VIDEO payloads can be split into 64 KB chunks with control messages slotted in between, without changing the design.

### Framing
Every message is `u32 length` (of what follows) · `u16 type` · `u16 flags` · payload. Little-endian throughout.

- **Control messages** carry **UTF-8 JSON**. They're rare and small, easy to read in logs, and both sides have a JSON library.
- **VIDEO messages** carry a binary header followed by data (below).

### Message catalogue

| Type | Direction | Purpose |
|------|-----------|---------|
| `HELLO` | both | Protocol version (major.minor), app version, build. Phone also sends model identifier (e.g. `iPhone17,1`), iOS version, **signing expiry date**, thermal and battery state |
| `CAPS` | phone→PC | Every camera device (physical and virtual): lens type, position, field of view, zoom range, switch-over zoom factors, and its formats. Each format lists width×height, FPS ranges, pixel formats, binned or not, max zoom, stabilisation modes, and ranges for ISO, exposure duration, focus and white balance. Also encoder abilities (H.264/HEVC, max level) |
| `PROBE` / `PROBE_RESULT` | phone→PC / PC→phone | Bandwidth probe: phone sends ~1 s of filler, PC reports measured throughput |
| `CONFIGURE` | PC→phone | Device, format, FPS, codec, bitrate, keyframe interval, rotation, stabilisation, metadata types |
| `CONFIGURED` | phone→PC | What was actually applied (may differ), or an error |
| `VIDEO` | phone→PC | One encoded frame (see below) |
| `KEYFRAME_REQUEST` | PC→phone | Decoder lost sync, or a new consumer joined |
| `CONTROL_SET` | PC→phone | Zoom (with ramp rate), focus, exposure, white balance and similar. Each has a sequence number |
| `CONTROL_STATE` | phone→PC | Current live values (actual ISO and shutter in auto mode, lens position, zoom, active physical lens inside a virtual device). Sent at about 5 Hz and on change |
| `STATUS` | phone→PC | Session running/interrupted (+ reason), thermal state, battery, charging |
| `STATS` | PC→phone | Received FPS, decode time, dropped frames. Feeds bitrate adaptation |
| `PING` / `PONG` | both | Heartbeat every 1 s, used to estimate clock offset and round-trip time. Three missed means the link is dead |
| `LOG` | phone→PC | Companion log lines, shown in the Windows diagnostics view (that's how we debug the phone without Xcode) |
| `BYE` | both | Clean shutdown with a reason |
| `ERROR` | both | Code + message |

### VIDEO payload
```
u64 pts_ns          capture time (phone host clock)
u32 frame_seq
u8  codec           1 = H.264, 2 = HEVC
u8  flags           bit0 keyframe, bit1 config changed, bit2 discontinuity
u16 meta_len
u8[meta_len]        metadata: face/body boxes (id, normalised rect, roll/yaw if present), active lens
u8[]                AVCC length-prefixed NAL units; parameter sets (SPS/PPS/VPS) included on every keyframe
```

### Message type IDs (as implemented, v0.1)

| ID | Type | Body |
|----|------|------|
| 0x0001 | HELLO | JSON. PC: `protocol, role, app, appVersion`. Phone adds `build, model, system, signingExpiry (ISO 8601 or null), bind` |
| 0x0002 | BYE | JSON `{reason}` (or empty) |
| 0x0003 | ERROR | UTF-8 text |
| 0x0004 | PING | binary: `u64 pcTimeNs` |
| 0x0005 | PONG | binary: `u64 pcTimeNs` (echoed), `u64 phoneTimeNs` |
| 0x0006 | LOG | UTF-8 text |
| 0x0007 | STATUS | JSON `{thermal, battery, charging}` |
| 0x0010 | PROBE_REQUEST | JSON `{direction: "down"\|"up", seconds, chunk}` |
| 0x0011 | PROBE_DATA | binary: `u64 seq`, then `byte[i] = i & 0xFF` for i ≥ 8 (lets the receiver detect corruption and gaps) |
| 0x0012 | PROBE_END | JSON `{direction, bytes, seconds, chunks}` from the sender |
| 0x0013 | PROBE_RESULT | JSON `{direction, bytes, seconds, mbps}` from the receiver |
| 0x0021 | CONFIGURE (R3) | JSON `{enabled, codec: "h264", width, height, fps, bitrate, keyframeInterval}`. `enabled:false` stops video |
| 0x0022 | CONFIGURED (R3) | JSON `{ok, enabled, codec, width, height, fps, bitrate}` with what was **actually applied**, or `{ok:false, error}` |
| 0x0030 | VIDEO (R3) | binary: 16-byte header (`u64 ptsNs, u32 frameSeq, u8 codec, u8 flags, u16 metaLength`) + metadata + **AVCC** NAL units (4-byte **big-endian** lengths, as VideoToolbox emits). SPS/PPS in-band on every keyframe. Flags: bit0 keyframe, bit1 config changed, bit2 discontinuity |
| 0x0031 | KEYFRAME_REQUEST (R3) | empty. PC → phone after a gap or decode error. The PC drops frames until the next keyframe |
| 0x0020, 0x0040–0x0042 | CAPS, CONTROL_SET, CONTROL_STATE, STATS | Reserved, defined in later runs |

> **R3 note:** the Windows side and the fake phone implement CONFIGURE/CONFIGURED/VIDEO/KEYFRAME_REQUEST first. The iPhone app gains them in R3's iPhone half. The version stays **0.1** because these are pure additions (unknown types are ignored), so the R1 companion and probe keep working together. The framing is unchanged and still byte-identical on both platforms.

Probe byte counts include the 8-byte message header. PING/PONG are binary rather than JSON so timestamps stay exact.

### Versioning
- **Major version 0 means unstable** (spike phase): both sides require the *same minor version*. The rules below apply from 1.0 on.
- Major version mismatch: the Windows app refuses to connect and shows "Update the iPhone app". It never guesses.
- Minor version: both sides ignore message types and JSON fields they don't know.
- The Windows app carries a **minimum compatible companion version**, so an outdated sideloaded app gets a clear message.

### Flow control and congestion (low latency comes before smoothness)
- The phone keeps **at most 2 encoded frames** waiting to send. If the socket stalls past that, it **drops frames up to the next keyframe**, forces an IDR, and lowers the bitrate by 25%. It raises the bitrate again slowly (+5% every 2 s) up to the configured ceiling when `STATS` look healthy.
- If the Windows decoder hits a gap (`frame_seq` discontinuity) or an error, it sends `KEYFRAME_REQUEST` and holds the last good frame. A **periodic keyframe every 2 s** is the backstop.
- The bitrate ceiling comes from the probe: about 70% of measured throughput, capped by the per-resolution quality table in video-pipeline.md.

### Clocks and latency measurement
Each `PING` carries the PC's QPC time, and the `PONG` adds the phone's `mach_continuous_time`. A min-RTT filter gives the clock offset, so the PC can turn every frame's `pts_ns` into PC time. Latency from capture to decode to publish is then **measured continuously** and shown on the diagnostics page.
