# Reliability and testing

Reliability is priority #1 (R-02), and it's where most of the late effort goes.

## Failure modes and required behaviour

| # | Situation | Detected by | Behaviour |
|---|-----------|-------------|-----------|
| F1 | Cable unplugged | usbmux `Detached` / socket closed | Virtual camera holds the last frame for 1 s → placeholder "iPhone disconnected". Auto-resume on replug, settings restored |
| F2 | AMDS not installed / stopped | connect to :27015 fails | State `NoService`. UI explains and offers to install/start it. Retry every 2 s |
| F3 | Device not trusted | AMDS pairing state / connect error | "Tap Trust on your iPhone, enter your passcode" |
| F4 | Companion not open | `Connect` → refused | "Open Cabled on your iPhone". Retry every 1 s |
| F5 | Companion goes to background / phone locked / incoming call | `STATUS: interrupted(reason)` or socket drop | Placeholder with the specific reason. Resumes on its own |
| F6 | Camera taken by another iPhone app (FaceTime) | interruption reason | Same as F5, specific message |
| F7 | Phone overheats | `STATUS` thermal | Negotiator steps down (4K60 → 4K30 → 1080p60 → 1080p30) and the UI says so. Steps back up after 5 min at nominal |
| F8 | Link congestion / slow USB hub | send-queue drops, `STATS` | Lower the bitrate, then the source size. Suggest a different port in diagnostics |
| F9 | Decoder error / corrupt frame | libavcodec error, seq gap | `KEYFRAME_REQUEST`, hold the last frame |
| F10 | GPU device removed / driver reset (TDR) | `DXGI_ERROR_DEVICE_REMOVED` | Rebuild the D3D11 device + decoder + pipeline. Virtual camera repeats the last CPU frame meanwhile |
| F11 | Windows sleep / resume | power notifications | Tear down on suspend, normal reconnect on resume |
| F12 | Consuming app crashes | — | No effect on the engine. The DLL is stateless by design |
| F13 | Frame Server restarts | pipe disconnect | DLL reconnects to the pipe. The engine re-accepts |
| F14 | Cabled.exe crashes | — | DLL shows the placeholder "Cabled isn't running". A crash dump goes to `%LOCALAPPDATA%\Cabled\crashes` (local only) |
| F15 | Two iPhones connected | multiple `Attached` | Use the last-used UDID, or the first one. A device picker sits in the status bar |
| F16 | Same phone also seen over Wi-Fi sync | `ConnectionType=Network` | **Ignored** |
| F17 | Companion too old / new | `HELLO` major mismatch | "Update the iPhone app" + link |
| F18 | Companion signature expired | app won't launch, so F4 forever | After 15 s in F4, the hint adds "If the app won't open, refresh it in SideStore". The last-known expiry date from a previous `HELLO` makes this precise |
| F19 | Consumer changes resolution mid-session | DLL pipe message | Negotiator reconfigures, debounced |
| F20 | Camera privacy setting blocks desktop apps | MF activation errors / settings check | UI links to Windows camera privacy settings |

**Rule:** every state the user can end up in has a visible message and a next step. No silent black frames.

## Test strategy

### 1. Fake device (most important for development speed)
`cabled-fakephone.exe` runs on Windows and **speaks the companion's protocol** over plain TCP. The engine has a "direct TCP" transport alongside usbmux, for tests only. It streams:
- pre-encoded H.264/HEVC clips with burned-in frame numbers, timecodes and colour bars (made once with FFmpeg),
- synthetic face-metadata tracks (recorded from real sessions or scripted),
- scripted faults: drops, stalls, disconnects, interruptions, thermal escalation, version mismatch.

With it, **I (Claude) can build and test the whole Windows side on this PC without an iPhone**: decode, GPU pipeline, framing, negotiation, virtual camera, reconnect logic. It also runs in Windows CI.

### 2. Unit tests
- C++ (GoogleTest): usbmux plist encode/decode, protocol framing, Negotiator decisions (table-driven: CAPS + consumer choice + link → config), framing maths (replay of tracks → crop rects, checked for smoothness, bounds and the quality guard), state machines.
- Swift (XCTest, simulator in CI): protocol codec, state machine, format list building from fake device descriptions, mobileprovision parsing.
- C# (xUnit): view-model logic, settings migration.

### 3. Virtual camera consumer tests (automated on this PC)
- A small MF capture tool + a DirectShow graph tool that open "Cabled Camera" with each advertised type, grab N frames, then check frame numbers (no stalls or repeats beyond limits), colours (against the bars), timestamps and latency.
- A local HTML page using `getUserMedia` at different constraints, opened in Edge/Chrome through the browser pane.

### 4. Manual device matrix (you, with a real iPhone)
| Axis | Values |
|------|--------|
| iPhone | **iPhone Air** (single rear lens + front, USB 2 path) and **iPhone 17 Pro** (3 rear lenses, USB 3). No Lightning device: the slow-link path is also tested by throttling the fake device |
| Cable/port | USB-C to USB-C with a USB 3 cable (10 Gbit/s), USB-C to USB-A, USB 2 port, hub |
| Windows | 11 (this PC) for 1.0. Windows 10 after 1.0 |
| Consumers | Zoom, Teams (new), Discord, OBS, Chrome, Edge, Firefox, Windows Camera app |
| Scenarios | Cold start, replug, lock phone, incoming call, 2 apps at once, 2 h soak, lens switch during a call, framing walk-around |

### 5. Measurements every release
- **Glass-to-glass latency**: point the phone at a ms-resolution clock on the PC screen, view it in an app, take a screenshot, subtract. Plus our continuous internal measurement.
- **Soak**: 2–4 h streaming at 4K30 → check that frame count, memory, handles and phone temperature stay flat.
- **Colour**: bars from the fake device → screenshot from each consumer → compare.

## Who can test what
- **Me, on this PC:** everything on the Windows side through the fake device, the virtual camera in real consumer apps (once installed here), unit tests, CI.
- **Only you:** anything with a real iPhone: sideloading, trust, real capture, thermals, lens behaviour, auto-framing feel. Each run that changes iPhone behaviour ends with a short test checklist for you.
