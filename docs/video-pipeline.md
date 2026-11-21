# Video pipeline, quality and negotiation

## Quality, honestly
- The biggest quality wins are already in the hardware: the iPhone's sensor, lens and ISP. Cabled's job is to **not ruin that**: one encode at a very high bitrate, no needless scaling, correct colour tags.
- **Zoom, Teams, Discord and Meet re-encode your video** at about 0.5–4 Mbit/s and usually cap it at 720p or 1080p30. Whatever we send above that matters for *their encoder's input*: cleaner input gives a visibly better result, but 4K60 won't reach the other side of a Teams call. 4K and 60 fps really pay off in **OBS, browser recording and local capture**.
- Generation loss: phone encode (high bitrate, visually transparent) → our decode → raw frames to the app → the app's own encode. That's one lossy step we add. We can't avoid it on a USB 2 link (see transport-and-protocol.md).

## Stages

### 1. Capture (iPhone)
- Format, FPS and lens are chosen by the Negotiator on Windows and sent in `CONFIGURE`.
- Pixel format `420v`. 10-bit `x420` is only for a possible future HDR path.

### 2. Encode (iPhone, VideoToolbox, hardware)
**Default bitrate ceilings** (before the probe adjusts them; they're about 5–10× typical streaming rates):

| Source | H.264 | HEVC |
|--------|-------|------|
| 720p30 | 15 Mbit/s | 10 |
| 1080p30 | 25 | 18 |
| 1080p60 | 40 | 28 |
| 4K30 | 60 | 45 |
| 4K60 | 100 | 70 |

- Codec choice (D-04): **H.264 by default.** Every PC decodes it in hardware, and it has documented low-latency rate control on iOS. **HEVC** is picked automatically when the probe shows H.264 at the ceiling won't fit (for example 4K60 on a slow link), or manually.
- No B-frames, IDR every 2 s plus on request.

### 3. Decode (Windows)
- **FFmpeg libavcodec with the D3D11VA hardware decoder** (D-05). The decoded frame stays on the GPU as an NV12 texture on *our* D3D11 device, with no copy.
- Why not the Media Foundation decoder? MF's HEVC decoder needs the paid "HEVC Video Extensions" from the Store on many PCs. FFmpeg's D3D11VA path uses the GPU driver's decode support directly, so it works on every PC that has the hardware. One code path for both codecs.
- Low-delay flags, frame threading off (it adds a frame of delay). Software decode is the fallback, with a warning in diagnostics.

### 4. GPU processing (Windows, D3D11)
One D3D11 device is shared by the decoder, processing and preview.

| Pass | When it runs | Notes |
|------|-------------|-------|
| **Geometry**: crop rect (framing/manual crop), rotation, mirror, scale | Always, as one sampling pass | If the crop is exactly the output size and there's no rotation or mirror, this is a straight copy with **no resampling** (D-11). Otherwise a **high-quality filter** (Lanczos-3 or Catmull-Rom bicubic, chosen when we tune the GPU pipeline) with a proper pre-filter when shrinking a lot |
| **Adjustments**: brightness, contrast, saturation, sharpness | **Only when any value isn't neutral** | Neutral means the pass is skipped entirely (R-56). Done in float16. Sharpness is an unsharp mask from 0 upward. 0 means nothing added; there is no API to undo the iPhone's own sharpening |
| **Output conversion** | Always | Converts to the virtual camera's current pixel format (NV12, YUY2 or RGB32) with the correct matrix and range |
| **Readback** | Always | Copy into a ring of 3 staging textures, then map → shared memory. The ring avoids GPU stalls |

Readback cost: 4K60 NV12 is about 750 MB/s across PCIe, fine on any discrete GPU and usually fine on integrated ones. Zero-copy texture sharing with Frame Server is possible later (shared NT handles), not v1 (D-09).

### 5. Output → virtual camera
See virtual-camera.md. Frames carry the **capture timestamp** converted to PC time. If the source falls behind its nominal frame interval, the render thread **repeats the last frame** so consuming apps never stall.

## Colour
- Captured as **BT.709, limited (video) range**, and kept that way end to end. No YUV→RGB→YUV round trip unless an adjustment pass is on.
- MF media types are tagged with `MF_MT_YUV_MATRIX = BT.709`, `MF_MT_VIDEO_NOMINAL_RANGE = 16_235`, `MF_MT_VIDEO_PRIMARIES = BT.709`, `MF_MT_TRANSFER_FUNCTION = 709`.
- **Known industry pitfall:** some apps ignore the tags and assume BT.601 for every webcam, which gives a slight green or magenta shift. A colour-bar test pattern from the fake device shows how each app behaves (see reliability-and-testing.md). If a major app gets it wrong, a per-device "compatibility colour" option can output BT.601. Decide only with evidence.

## HDR (D-12)
**v1 is SDR only.** Windows camera consumers (Zoom, Teams, Discord, browsers, and mostly OBS too) take 8-bit SDR. Sending HLG or Dolby Vision would mean tone-mapping on Windows: extra processing that varies by scene, which goes against R-56, for an output nobody can display. A later "HDR capture → tone-mapped SDR" mode could help scenes with strong backlight. Park it.

## Capability negotiation (R-57)

### Inputs
1. Phone `CAPS`: devices × formats × FPS ranges × pixel formats × zoom/binning.
2. Encoder limits from `CAPS` (codec, level).
3. Link throughput from the `PROBE`, refreshed by `STATS`.
4. Output formats the virtual camera can produce: the list we advertise (see virtual-camera.md).
5. **What the consuming app picked.** The virtual camera DLL reports the media type the app chose (resolution, FPS, subtype). We can't ask an app what it *supports*. We advertise a list and learn what it *chose*.
6. User overrides (manual resolution/FPS/codec/bitrate), framing on/off, chosen lens, phone thermal state.

### Policy, in order
1. **Output size and FPS** = what the consuming app chose. With no consumer yet, use the user's preference, or the default of 1920×1080 at the highest FPS the source supports at that size (60, otherwise 30).
2. **Source FPS** = the output FPS, or the nearest supported rate above it, never below.
3. **Source resolution**:
   - Framing off and zoom handled on the phone: the smallest format **≥ the output size** in both dimensions, at the same aspect ratio. No scaling when it matches exactly.
   - Framing on: the largest format such that (a) it supports the source FPS, (b) the encoder can do it, (c) its bitrate fits the link, (d) the thermal state allows it. Typically **4K source for 1080p output**, which allows a 2× crop with no upscaling.
   - Prefer formats that aren't binned when available at that FPS, and avoid formats that need stabilisation.
4. **Codec** = H.264 unless step 3's choice doesn't fit H.264's bitrate in the link, then HEVC. If HEVC still doesn't fit, step down a source size.
5. **Bitrate** = min(table ceiling, 70% of measured throughput).
6. Recalculate when: a consumer picks a new format, the lens changes, framing is toggled, thermal state changes, or the link degrades. A reconfiguration causes a ~0.3–1 s glitch, which the virtual camera covers by repeating the last frame. The Negotiator **debounces** (no more than one reconfiguration every 3 s, except when the user asks).

### Manual mode
The user can pin any of the following: source format, FPS, codec, bitrate ceiling, output size. Pinned values are never changed automatically except by thermal protection, which is shown in the UI.
