# Cabled

An iPhone → Windows wired webcam. Design docs are in `docs/` (start at `docs/README.md`). Work happens in numbered runs (`docs/roadmap.md`), and each run has a page in `docs/runs/` with what was built, a device-test checklist for the owner, and results.

- Requirements are `R-xx` (`docs/requirements.md`), decisions `D-xx` (`docs/decisions.md`). Owner answers are in `docs/open-questions.md`. Key ones: MIT licence (copy no GPL code), Windows binaries never signed, "Cabled" is a working name (keep it in one config place per platform; never change the virtual camera CLSIDs once created), iOS 17 minimum, SDR only, no audio, Windows 11 first.
- The wire protocol must stay byte-identical between `windows/src/common/protocol.*` and `ios/Cabled/Transport/WireProtocol.swift`. Both test suites check the same byte vectors. Update `docs/transport-and-protocol.md` with any change.
- Windows build (dev PC has VS 2026 Build Tools; use its bundled cmake/ctest under `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`): `cmake -S windows -B build`, `cmake --build build --config Release`, `ctest --test-dir build -C Release`.
- The iPhone app can't be built on this PC. CI (`.github/workflows/ios.yml`) builds it on macOS. The owner has a Mac for Xcode work. Test the Windows side without a phone with `cabled-fakephone` (D-21).
- Virtual camera (`windows/vcam/`, `cabled_vcam_mf.dll`): Frame Server loads it from `C:\Program Files\Cabled\Dev` and caches it. After changing the DLL, run `windows/scripts/vcam-dev-install.ps1` (one UAC prompt: stops Frame Server, copies, registers). Test in-process first with `cabled-vcam selftest` (no admin), then `cabled-vcam feed` + `cabled-vcam check`. Its log is `C:\ProgramData\Cabled\logs\vcam.log`. Never change the CLSID in `vcam_shared.h`.
- Only the owner can test with a real iPhone (iPhone Air, iPhone 17 Pro). End iPhone-touching runs with a checklist in `docs/runs/Rn.md`.
- Claude may install dev tools on this PC as runs need them and may use `gh` with the owner's account. Report what was installed.
