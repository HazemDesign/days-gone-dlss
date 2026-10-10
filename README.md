# Days Gone DLSS (Luma-Framework mod)

DLSS Super Resolution for Days Gone (PC / DX11 / UE 4.10 Bend), built as a
game module on top of [Luma-Framework](https://github.com/Filoppi/Luma-Framework)
(ReShade addon). Days Gone uses a custom Bend TAA pass instead of stock UE4
TAA, so the generic Luma-Unreal mod never engages here — this project targets
the actual resolve slots (pixel TAAU `000573B8` for showcase/upscale modes,
compute TAA `242D9D62` for gameplay).

Current build id: `m55-...-mvscale2x-hybrid`
(see `DG_BUILD_ID` in `game/Days Gone/main.cpp`). Plan and milestones:
`Todolist.txt`. Research notes: `RESEARCH.md`.

**No motion-blur slot in this project.** DLSS replaces the TAA draw directly,
with a HUD-preserving path (UI composite options in the menu).

## Install a CI-built mod

1. Download `Luma-Days_Gone*.zip` from the latest `days-gone-latest-N`
   release on GitHub.
2. Extract next to `DaysGone.exe` (overwrite on updates).
3. Overwrite `dxgi.dll` with the one from the zip (matched ReShade pair).
4. Disable other addons for the test (`Luma-Unreal Engine.addon`,
   `renodx-dlss-*.addon64` — rename or list under `[ADDON] DisabledAddons=`).
5. Boot, press Home — the overlay header shows draw counts + build id.
6. Marker `Luma-DaysGone-load-marker.txt` first line must be the build id.

## In-game menu guide

Open the ReShade/Luma menu (Home). The Days Gone section has:

- **DLSS Masters** — `M7 DLSS at TAAU 000573B8 (showcase/upscale)` and
  `M11 DLSS at COMPUTE TAA 242D9D62 (gameplay DLAA)`. Both on = M11 while the
  compute slot fires, M7 takes over when it goes silent. Needs Super
  Resolution set to DLSS in the Luma menu. Hotkeys: F9 = M11 toggle,
  F10 = start full-trace.
- **Motion / MV / jitter tuning** — MV source and decode mode, own-camera MVs,
  `Hybrid MVs (game truth + own fill)` (default OFF, M11/compute only),
  MV/jitter scale modes (incl. `2.0 (half-gain compensation test)`),
  inverted depth, near/far planes, FOV slider (set ~0.64–0.69 — the 1.047
  default is proven wrong, true view is ~0.688), UI composite
  (source, blend, tonemap), sRGB/plain output switches. View/depth caches
  only accept gameplay-sized targets now (shadow-only draws can't poison
  the pool or depth — watch `preads/pskip/pickck`, `dskip=` in FULL).
  Defaults are the tested-good combo; change one thing at a time.
- **Viewer** — `Compute viewer source` (TAA t0–t3/u0–u1, velocity/depth/HDR
  caches, DLSS output, MV feed), `DLSS feed (exact NGX input)` (M11/M7
  color/depth/MV/output — what DLSS actually got), `Viewer channel`
  (R/G/B/A/ALL/DepthN). Shows each feed fullscreen for debugging.
- **Tools (one-shot diagnostics)** —
  - `Audit DLSS inputs` (10 feeds, both paths; `DaysGone AUDIT` lines),
  - `Stats: compute inputs` / `Stats: M7 pixel inputs` (signal min/max/mean),
  - `Log compute TAA u/t dims`, `Dump compute TAA CB0 floats`,
  - `Scan TAA CBs for projection matrices`,
    `Scan velocity-pass CBs (VS+PS)`,
  - `Sniff VS CBs for shared matrices (brief hitch)`,
  - `Capture 3 frames (TAA hunt)` → `Luma-DaysGone-frame.log`,
  - `Save 3 pictures now` → backbuffer BMPs,
  - `Save all passes now` → one BMP per debug pass (~19 presents),
  - `Save full bundle now` → arms all of the above at once,
  - `LOG EVERYTHING (full-trace, next N frames)` / `Start full-trace now`
    → `Luma-DaysGone-full.log` + `DaysGone FULL` mirror
    (N=600 presents ≈ 10s by default).

## Reading the outputs

All files land next to the game exe / addon; every file write also mirrors a
line to `ReShade.log` (the mirror works in all configs, files need a
Test/Development build):

- `Luma-DaysGone-frame.log` — per-draw RT/depth/texture bindings (TAA hunt).
- `Luma-DaysGone-full.log` — one summary line per present (`full ...` with
  draws, slot attempts/runs/resets, MV state, jitter, viewer status) plus a
  `DaysGone FULLAGG` report card every 600 presents.
- `Luma-DaysGone-pic-<present>.bmp` — backbuffer pictures.
- `Luma-DaysGone-pass-<src>-<present>.bmp` — one per debug pass
  (t0–t3/u0–u1, caches, M11/M7 feeds; depth saves raw and looks dark).
- `DaysGone PIC ...` / `DaysGone BUNDLE done ...` lines in `ReShade.log`.
- `python tools/parse-logs.py <bundle-dir-or-ReShade.log>` parses a bundle
  (`--frame` / `--full` override the log paths).
- `scripts/collect-bundle.ps1 -GameDir <game folder>` gathers marker, draws,
  frame/full tails, filtered ReShade view, and `summary.txt` into
  `artifacts/debug-bundles/<stamp>/` (read-only on the game dir).

## Local development

- Visual Studio 2022 (C++ workload) + Windows 10/11 SDK, Git, NVIDIA driver.
- Clone Luma-Framework **next to** this repo (`--recurse-submodules`), then:
  ```powershell
  pwsh -ExecutionPolicy Bypass -File scripts\inject-game.ps1 -LumaRoot ..\Luma-Framework
  ```
- Open `..\Luma-Framework\Luma.sln`, build `Days Gone`
  (`Development-Release|x64` for shader dumping).
- Set env var `LUMA_DAYS_GONE_BIN_PATH` to
  `<your-Steam-library>\...\BendGame\Binaries\Win64` — post-build copies the
  `.addon`. Or build + copy in one step:
  ```powershell
  pwsh -ExecutionPolicy Bypass -File scripts\build-local.ps1
  ```
- Install ReShade 6.6.0+ into the game folder, boot, press Home.

## CI builds

Workflow: `.github/workflows/build.yml`. Push to `main` builds
**Development-Release + Test-Release + Publishing-Release / x64** on top of a
fresh Luma-Framework checkout, packages with Luma's `Scripts/package.ps1`,
uploads artifacts, cuts a `days-gone-latest-N` release.

## Layout

Luma-Framework is **not vendored** here. CI merges this overlay into a fresh
upstream checkout on every build:

| This repo | Lands in the Luma checkout as |
|---|---|
| `game/Days Gone/` | `Source/Games/Days Gone/` (vcxproj + `main.cpp`) |
| `shaders/Days Gone/` | `Shaders/Days Gone/` (GameCBuffers / Settings / Common) |
| `scripts/inject-game.ps1` | copies the overlay + registers the project in `Luma.sln` |

Project GUID `{0D346BDD-7E1D-4360-9C41-48052DB7EEA5}`, x64-only.
Target binary: `Luma-Days Gone.addon`.

## License

This project is licensed under the MIT License — see [LICENSE](LICENSE).
