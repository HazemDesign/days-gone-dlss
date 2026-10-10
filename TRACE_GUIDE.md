# Full-trace safety guide (Days Gone DLSS)

Full-trace writes ONE line per present to `Luma-DaysGone-full.log` plus a
`DaysGone FULL` mirror in `ReShade.log`. Safe bounds are enforced in code
(N clamped 30..300, ~5s max); this note is the procedure around it.

## How to run safely

1. Use a TEST or DEVELOPMENT config if you need the `full.log` file.
   In Publishing, only the `ReShade.log` mirror is written (by design).
2. Turn ALL Viewers OFF first (compute viewer, DLSS feed viewer, pixel
   viewer). The menu refuses to arm while any viewer is ON; F10 logs
   `refused viewer-armed` instead of starting.
3. Stand/ride in OPEN-WORLD GAMEPLAY (not showcase/menu), then press
   `Start full-trace now` (or F10). Default N=300 (~5s at 60fps).
4. Do NOT re-arm while it runs — the menu shows `FULL-TRACE ACTIVE:
   N presents left`, the button hides until it finishes, and F10 logs
   `already running`. Mashing the button cannot extend a run.
5. Keep N in 30..300. The old 1..100000 range is gone: 100k presents is
   ~28 minutes of per-present file open/append/close on the present
   thread (IO hitch) plus ~100MB of logs.
6. Do NOT combine with viewers mid-run. If you open a viewer while a
   trace runs, the trace keeps its countdown (harmless — FULL reads
   live atomics only), but pics/passes SKIP while viewers are armed.

## What to paste back

- The 10 `DaysGone AUDIT` lines (move + pan while they log), or the
  `DaysGone FULL` lines covering a slow 5s pan.
- `first-Draw cDLSS` line, `mvhash=`/`dec=` values, N used.
- Before sharing any bundle: run
  `pwsh -ExecutionPolicy Bypass -File scripts\collect-bundle.ps1`,
  which redacts your user profile (`~`) and absolute game path
  (`...\BendGame\Binaries\Win64`) from all shared text. Never post
  raw `ReShade.log` — it contains local module paths.

## Crash history (fixed)

- `AppendFullLogLine` used a 1024B `sprintf_s` target for a FULL line
  that can reach ~1400B (grows with every BUILD_ID suffix): truncation
  trips the CRT invalid-parameter handler = fail-fast crash. Now 2048B.
- Unbounded N + blind re-arm + no viewer gate: all closed (see above).
