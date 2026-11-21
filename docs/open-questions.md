# Open questions

**Status (2026-11-21): all of Q1–Q16 are answered.** They're kept below as a record of what was decided and why. New questions get added at the bottom as they come up.

## Answered

| # | Question | Answer | Affects |
|---|----------|--------|---------|
| Q1 | Mac available? | ~~Yes~~ → **Not usable for iOS dev (2026-11-21):** a 2020 Intel MacBook on Sonoma can only run Xcode ≤ 16.2, too old for iOS 26 devices, and the owner won't upgrade it. **All iPhone builds come from CI**, installed with SideStore | D-15 |
| Q2 | Repo visibility, GitHub CLI use | **Public** on GitHub. Claude may use `gh` with the owner's account (owner runs `gh auth login` once) to create the repo, push and manage CI | D-15 |
| Q3 | Test iPhones | **iPhone Air** + access to an **iPhone 17 Pro** (see notes below) | testing |
| Q4 | Apple Devices / iTunes as a prerequisite | **Accepted.** There's no driver-free alternative | D-01 |
| Q5 | Install the toolchain on this PC | **Yes, as needed.** Claude installs each tool when a run needs it and reports what it installed | stack |
| Q6 | Windows 10 at launch | **No: Windows 11 first.** Windows 10 DirectShow fallback after 1.0 | D-07 |
| Q7 | Start at login in the tray | **Offered in onboarding, ticked by default** | D-20 |
| Q8 | Licence | **MIT.** No GPL code may be copied (OBS virtualcam is reference-off-limits). FFmpeg stays an LGPL dynamic library | D-24 |
| Q9 | Windows code signing | **Never sign.** Onboarding and the README explain SmartScreen and UAC "Unknown publisher" | D-25 |
| Q10 | Camera always listed | **Yes, always listed**, with a placeholder when idle | D-08 |
| Q11 | Minimum iOS | Owner had no preference, so Claude chose **iOS 17**. Wider reach. Newer or device-specific features are used behind availability checks | D-14 |
| Q12 | Name "Cabled" final? | **Working name only.** The name lives in one config place per platform | D-23 |
| Q13 | SDR only for 1.0 | **Yes, SDR only** | D-12 |
| Q14 | Audio | **No audio** | D-19 |
| Q15 | UI stack | **C# WinUI 3** | D-16 |
| Q16 | Default camera | **Rear main.** Onboarding tells users to mount the phone with its back facing them | camera-controls-and-framing.md |

### Notes on the test phones (Q3)
- **iPhone Air:** a single rear camera (48 MP main) plus the front camera. Tests "only show lenses that exist" and the USB 2 speed path (to be confirmed in R1).
- **iPhone 17 Pro:** main + ultra-wide + telephoto, USB-C with USB 3 when using a USB 3 cable. Tests lens switching, the virtual multi-camera device and high-bandwidth 4K60.
- Both run iOS 26. No Lightning device is available, so the slow-link path is also covered by throttling the fake device.
- To check in R6: the iPhone 17-generation front camera (square sensor, Center Stage). What AVFoundation exposes there, and how it interacts with our auto-framing.
- Still to note at R1: the exact iOS versions on both phones.

## New questions
*(none yet)*
