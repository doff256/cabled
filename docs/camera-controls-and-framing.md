# Camera controls and auto-framing

## Where each control lives

The rule: **if the camera hardware can do it, the phone does it** (better image, no extra processing). Only what iOS can't do is done on Windows.

| Control (R-xx) | Where | iOS API / method | Notes |
|---------------|-------|-----------------|-------|
| Lens selection (R-40) | Phone | Switch `AVCaptureDeviceInput` between found devices | **Default on first connect: rear main** (D-26), then the last-used lens per phone. Shows only lenses that exist. Includes **"Auto"**, the virtual multi-camera device, for seamless zoom. Switching between physical lenses reconfigures the session, causing a ~0.3–0.8 s glitch that Windows covers by holding the last frame |
| Zoom (R-41) | Phone | `videoZoomFactor`, `ramp(toVideoZoomFactor:withRate:)` for smooth moves | With "Auto", iOS switches lenses optically at `virtualDeviceSwitchOverVideoZoomFactors`. The UI shows **0.5× / 1× / 2× / 3× / 5×** relative to the main wide lens (on the triple camera, raw factor 1.0 = ultra-wide = "0.5×"). Presets come from the switch-over points. Beyond the longest optical lens it's digital, done in the ISP (better than cropping after encode) |
| Focus (P1) | Phone | Continuous auto / locked / manual `setFocusModeLocked(lensPosition:)` | Tap-to-focus on the Windows preview → `focusPointOfInterest`. Some front cameras have fixed focus, so the control is hidden there |
| Exposure (P1) | Phone | Continuous auto / auto + EV compensation (`setExposureTargetBias`) / custom | |
| ISO (R-32) | Phone | `setExposureModeCustom(duration:iso:)` | Limited to `activeFormat.minISO…maxISO`. In auto mode, shown read-only with live values |
| Shutter speed (R-32) | Phone | same | Longest shutter ≤ 1 / FPS. The UI shows that limit |
| White balance (P1) | Phone | Auto / locked / temperature + tint → `deviceWhiteBalanceGains(for:)` | Gains clamped to `maxWhiteBalanceGain` |
| Brightness (R-32) | **Windows** | GPU adjustments pass | Digital. The UI places it next to "Exposure compensation" (phone, better quality) and says which is which |
| Contrast, saturation (R-32) | **Windows** | GPU adjustments pass | Neutral = pass skipped |
| Sharpness (R-32) | **Windows** | Unsharp mask, 0…+ | 0 = add nothing. The iPhone's own sharpening can't be turned off through public APIs |
| Crop (R-32) | **Windows** | Geometry pass | Manual crop rect, used when auto-framing is off |
| Rotation (R-32) | **Phone** | `AVCaptureConnection.videoRotationAngle` | 0/90/180/270. "Auto" follows phone orientation (CoreMotion). Output is always landscape 16:9 by default. A phone in portrait gets cropped to fit |
| Mirroring (R-32) | **Windows** | Geometry pass | Output **not mirrored** by default (Zoom and Teams mirror the self-view themselves). The preview has its own mirror toggle |
| Stabilisation | Phone | `preferredVideoStabilizationMode` | Off by default (latency, crop). Advanced setting |
| Presets (P2) | Windows | JSON | A named snapshot of lens, zoom, framing and all controls. Applied in one go |

Every control has: a supported/unsupported flag per lens/format (from `CAPS`), auto/manual mode, the range, and the live value (`CONTROL_STATE`). The UI greys out what the current lens can't do instead of hiding it, except lenses themselves, which are hidden when they don't exist.

Slider traffic: the Control thread **coalesces** to at most 30 updates/s per control and always sends the final value.

## Auto-framing (R-42, R-43)

### Division of work
- **Detection: phone.** `AVCaptureMetadataOutput` with `.face` (and `.humanBody` so framing still works when a face turns away). The camera hardware produces these boxes with almost no CPU or GPU cost, and they have stable `faceID`s for tracking. They're sent inside each VIDEO message.
- **Framing logic: Windows.** All tuning, settings and smoothing live where the UI is, and they can be tested with recorded tracks without a phone.
- **Fallback detector:** `Windows.Media.FaceAnalysis.FaceTracker` (built into Windows, no model to ship) on a downscaled grey copy, used if a lens or format doesn't produce metadata. ONNX Runtime + DirectML with a modern detector is a later option only if quality needs it.

### Algorithm (per output frame)
1. **Pick the subject.** Single mode: the primary face is the one with the largest area weighted towards the centre. It stays primary (by `faceID`) until it has been missing for 1.5 s, or another face is bigger by more than 2× for 2 s. Multi-person mode: all faces whose size is above a minimum.
2. **Target box.** Grow the face box(es) to head and shoulders: width ≈ 3.2× face width (single) or the union + 30% margin (multi). Put the eye line at the **headroom** setting (default: top of face at 18% of frame height). Lock to the output aspect ratio.
3. **Clamp:**
   - Never outside the source frame.
   - **Quality guard:** the crop is never smaller than `output size ÷ allowed upscale`. The default allowed upscale is **1.0**: never magnify pixels. The **max zoom** setting can relax this up to a hard limit. With a 4K source and 1080p output, that allows 2× framing zoom with no upscaling at all.
4. **Dead zone.** Hold still if the target's centre is within a dead zone around the current crop centre (default 8% of crop width) and its size is within ±10%. **Sensitivity** scales the dead zone and the spring stiffness.
5. **Smoothing.** Centre and size each follow a **critically damped spring** (separate time constants, size slower than position), with a maximum speed. The same motion runs regardless of FPS, so 30 and 60 fps feel the same. Detection arrives at ~frame rate, and the spring fills in between.
6. **Subject lost.** Hold the framing for 2 s, then ease out to the widest framing over ~1.5 s.
7. **Output.** A floating-point crop rect handed to the GPU geometry pass. When the rect is exactly output-size (at the quality-guard limit), the pass is a straight copy.

### Interaction with zoom
- Framing on: the phone's zoom/lens sets the **widest view** framing can use. Framing then crops inside it. The zoom slider effectively sets "how wide the room view is".
- v1 does **not** have framing drive the phone's optical zoom. Combining a phone-side zoom ramp with Windows-side panning is a nice later upgrade (for example switching to telephoto when you sit far back), but it doubles the tuning work.

### Settings (Windows UI)
On/off (default **on**) · headroom · sensitivity · max zoom · multi-person · speed (calm/normal/fast).

### Testing
- Phone metadata tracks are recorded to JSON during manual sessions. The framing engine is a pure function of (tracks, settings, frame size) → crop rects, so it gets **deterministic unit tests** plus a small replay tool that renders the crop path over a recorded clip.
