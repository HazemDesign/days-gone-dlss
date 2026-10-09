# Days Gone DLSS — Research Log

Running record of everything found. Newest at the bottom. Sources noted inline.

## 1. Target facts (verified)

- Game: `DaysGone.exe` **1.25.403.0** (from Windows Event Viewer; exe has no
  version resource). 2025-era build (Broken Road DLC period).
- Engine: Unreal Engine 4, **4.10–4.11 era** (Luma wiki says 4.10.1.0,
  PCGamingWiki says 4.11.1 — both = very old UE4). Heavily Bend-modified
  (GamingBolt, DigitalFoundry).
- API: **DX11 only**. `-d3d12` crashes on launch (PCGamingWiki). Forcing DX12
  is not an option.
- TAA is **always enabled**, no in-game off switch. Disabling via console
  injector breaks UI elements + render-scale != 100% (PCGamingWiki note 4).
- Exe dir: `<your-Steam-library>\...\BendGame\Binaries\Win64` (`DaysGone.exe`).
- Output res observed: **1920x1080**.

## 2. Generic Luma-Unreal Engine mod (baseline, FAILED for DLSS)

- Wiki: generic UE mod replaces stock UE4 TAA with DLSS (DLAA)/FSR3, needs
  DX11. Row `Days Gone | DLSS ⛔ | 4.10.1.0`.
- Our `ReShade.log`: TAA candidates found by bytecode pattern @130%
  (`0xB146BBAE / 0x104E65B1 / 0x3B21A41C / 0x4D982586`) but **never promoted**
  to confirmed TAA (promotion needs >=2 color + depth + velocity bindings at
  full res). DLSS never runs. Generic path is a dead end for DLSS on this
  build (HDR side still works).

## 3. M1 crash saga (RESOLVED in M1b)

- Symptom: splash 3s then close. Event Viewer, identical every boot:
  `DaysGone.exe 1.25.403.0 → ucrtbase.dll 0xc0000409 fail-fast`,
  bucket `2144668967998549651`.
- ReShade.log: `No add-on was registered by 'Luma-Days Gone.addon'`.
- Root causes (vs working Heat `main.cpp:2550-2726`):
  1. Missing `Globals::SetGlobals(...)` in DllMain.
  2. Missing `game = new DaysGoneGame()` (core extern, not own static).
  3. Missing `CoreMain(hModule, ...)` (the actual register_addon call).
  4. Missing load-only flags: upgrades None,
     `force_disable_display_composition=true` (core asserts FP16 backbuffer
     on R8 SDR swapchain), `prevent_fullscreen_state=false`.
- Also: never load two `Luma-*.addon` at once, and `dxgi.dll` must come from
  the SAME zip as the `.addon` (mixed 9/24 local dxgi + 9/18 release addon
  also crashes). RenoDX stays `.disabled` during tests.

## 4. DLSS preset labels (CORRECTED)

- Old Heat notes said E/F = Transformer, M invalid. WRONG.
- Per current `nvsdk_ngx_defs.h`: A–D removed (use J/K), **E/F deprecated
  (legacy CNN)**, J/K = Transformer gen1 (K default for DLAA/Quality/
  Balanced), L/M = Transformer gen2 4.5 (M = Perf default, L = UltraPerf
  default). 20/30-series lack FP8 → prefer K there. DLL 310.9.1 supports all.

## 5. M4 capture results (frame.log, 3 frames, ~5700 px draws/frame)

- Output res confirmed 1920x1080. No old TAA hashes (B146…) appear in
  21,600+ frames — they don't run in this build.
- `FFFFFFFF` split: mostly `PS-none` (depth-only/shadow draws, normal), rest
  `PS-unresolved` (not yet dumped).
- **20-hash HDR post family**, all `RT 1920x1080 fmt26 (R11G11B10_FLOAT) +
  D 1920x1080 fmt44 (R24G8_TYPELESS) + t0 1920x1080 fmt17 (R32G32_UINT?) +
  t1 1920x1080 fmt42 (depth-as-SRV?)`, back-to-back mid-frame (~lines
  5412–5443):
  `03B622E1 0DAD1160 183341B8 21FC1C0E 2C7DE5DE 3467CCB3 39534CF5 421C4E5E
  5EB30DBE 71D7B962 77EAE371 87AADF8A BD8F5AED C83AF6AE D709ECFE D79102E3
  D84282AD DB133A17 EB00D485 EE46599E EF75360B`
- No velocity-shaped input in t0–t3 of any member → history/velocity hide at
  t4+. M5 extends capture to t0–t7 **with resource handles** (recurring
  handle across frames = history buffer, its consumer = TAA).
- Downstream of family: bloom-ish chain (B64AFAA4 x8, E056F951 x8),
  tonemap pairs (7D0E7E28/D719C315), UI-ish interleaved
  (8653F014/4B86D932/A9B4CDE0/1846C387).

## 6. HelixMod fix forensics (`<mods-dir>\FixFiles`)

- F5 TAA toggle (z2/w4 cycle) is **dead code in this version**: z2 consumed
  nowhere (20 t120 refs in d3dx.ini, none reads texel 2; all 6 ps_replace
  files ref t120 once = declaration only). Real TAA-off = Engine.ini
  `r.TemporalAASamples=0` (jitter kill only).
- Only TAA-named machinery: `[ShaderRegexTAAObjectsPS1]` — stereo-patches
  OBJECT shaders' velocity math (`div pos/w`, `cb[26]`/`cb[59]`). Confirms
  Days Gone emits motion vectors in object passes with that signature (MV
  source lead for DLSS feed later).
- 3Dmigoto hashes are 16-hex vs Luma 8-hex → no cross-mapping.
- `upscale.ini` = 3D-TV output scaler only. Irrelevant.
- Conclusion: nobody has published this game's TAA resolve hash.

## 7. UE4.11 TAA anatomy (Karis GDC 2013 + Common.usf, verified)

- Order: Lighting → Bloom → DOF → Post → **TAA (firewall)** → Tonemap. TAA
  is a LATE member of our 20-hash family, not the first.
- Inputs: current color + history + **16:16 RG UNORM velocity** (same buffer
  as motion blur, jitter removed) + depth. (`EncodeVelocityToTexture`.)
- `r.TemporalAASamples=0` kills jitter ("global shaking") only, not TAA.
- Game runs fine jitter-killed (HelixMod proves it) = the exact state our
  future DLSS feed wants (we supply our own jitter).

## 8. Long loads (verified: PCGamingWiki live + MiniTool guide)

- Unskippable intro movies are the main vanilla cause:
  `BendGame\Content\Movies\{bend_studio,ps_studios,sie_presents,
  Climax_2024_Ident_wAudio}.mp4` → rename/delete, or Game.ini
  `[movieplayer]` `bWaitForMoviesToComplete=False`,
  `bMoviesAreSkippable=True`, `StartupMovies=`. MiniTool documents the
  stronger variant: rename whole `Movies` folder (fixes stuck-on-loading).
- SSD required-ish (70 GB). Overlays off (Steam/NVIDIA/Discord — ReShade is
  one too; compare boots with `dxgi.dll` renamed out). Windowed launch flag
  if fullscreen flips hang. Verify files / latest patch / GPU driver.

## 9. Status / next

- M5b (`m5b-2026-09-24-freezerall`): freezer works in ALL configs (was
  Test-only → empty menu on Publishing; fixed), list live ~1s after boot.
- M5 (`m5-2026-09-24-fullcap` + M5b): t0–t7 + handles capture.
- NEXT: (a) freeze family tail one-by-one in gameplay (aliasing + HUD intact
  = TAA); (b) one M5 3-frame capture → history-handle + velocity-format read
  = positive ID; (c) wire DLSS DLAA at that draw (UseLumaNGX=true at M7).

## 10. Freezer menu fixes (M5c, M5d)

- M5c: Test flushed histograms to file BEFORE publishing to menu → menu
  permanently empty (41k presents, zero entries). Fixed: publish first,
  flush after.
- M5d: publisher took bottom of top-50 — a 1/frame pass in a 7k-draw game
  ranks below the cut, so `000573B8` never appeared. Fixed: slowest-first
  over the FULL list + manual hex entry (prefilled `000573B8`) + pinned
  section + no pruning.

## 11. Slot found: PS 000573B8 (TAAU resolve)

- M5 capture, 3 frames: exactly 1/frame, sole `1920x1080 → 2560x1440`
  upscale (render-scale <100% ⇒ TAAU). Inputs t1/t2 swap handles per frame
  (ping-pong current/history). Freezing it freezes the whole image ⇒ sole
  writer of the displayed target. Old 4 Glue hashes never run in 1.25.403.0.
- 20-hash HDR post family mapped (all `RT:26/D:44/t0:17/t1:42`), TAAU is a
  late member (UE order: post → TAA → tonemap).

## 12. M7 first light + fixes (Draw ok=1)

- `UseLumaNGX=true`, direct DLSS draw into game RT (UAV-bound), Skip
  cancels native. First run: `render=1920x1080 out=2560x1440`.
- Build saga: Luma has its OWN `ComPtr` (no WRL `GetAddressOf/Reset`) —
  fixed to `put()/reset()/attach()` + raw temps for out-params.
- Depth: slot binds none → t0–t15 scan + R32F + same-frame DSV cache
  (`depth_cached=1` in log).
- Red/blue 30Hz flash ⇒ t1/t2 swap roles per frame, first-pick alternated
  stale/fresh ⇒ writer-map pick (RTV+UAV stamps, current-frame only) + live
  hdr/autoexposure toggles.
- M7d state restore (compute) didn't stop it; M7f added FullGraphics cover.
- Mouse duplication = zero-MV ghosting (expected pre-M8).

## 13. Producer hunt (M7e capture — decisive)

- Color pair cooked by **compute `8AEEB67D`**: `u0=pair ← t1 HDR fmt10
  (R16G16B16A16F) + t0 depth fmt42 + t2 history`. No pixel writer exists —
  hence the stale fallback freeze.
- **Velocity = `DFCE27E0`** (persistent `1920x1080` RG, fmt35 = R16G16_UNORM
  per UE convention): rendered by 6 depth-only fullscreen passes
  (`1E94CABC/5846E9DA/6C6AD505/A1A256FA/A5CB30BF/BE0130E5`), consumed by
  depth/motion computes. Copy observation + compute capture + color-change
  counter added (M7e) to prove freshness live.
- M8 wires the real velocity (per-frame fmt35 cache, `mv_real=1` in log,
  FRESH/stale UI). Remaining: jitter mapping, MV scale/sign check.

## 14. Skip vs Replaced verdict (M8b/M8c)

- M8b switched to `Replaced` (upstream Unreal parity) ⇒ native image back,
  DLSS invisible. Conclusion: for pixel draws in this core version,
  `Replaced` re-runs native; `Skip` cancels. Slot correctness doubly proven
  (cancel/keep flips the whole image). M8c back to `Skip` + all guards.
- Open freeze/flash causes narrowed to: stale color on some frames (check
  color-changes vs runs) or R10G10B10A2 direct-draw output conversion
  (RGBA16F + copy CS fallback ready as M7d2).

## 15. Output path (M9): own FP16 + pixel copy — freeze cured, hues wrong

- Direct NGX draws into packed `R10G10B10A2` wrote nothing readable
  (frozen frame 1). M9 renders DLSS into own RGBA16F tex + pixel-copies
  (pipeline converts float→UNORM10 free). Image went live, full detail.
- But hues wrong: cyan wash → then red/blue variants. Swizzle switches
  (M9c, both stages) only move the cast. `hdr`/autoexposure toggles live.
- M9e: copies inherited slot blend/viewport/scissor (blended with stale +
  stripes). All 3 copy sites now force blend-off, full viewport, no scissor
  via shared `RunCopyPass`.
- NATIVE control (master OFF, same camp): pixel-perfect. Bug is in OUR path.

## 16. TAAU disassembly (Dump/0x000573B8.ps_5_0.cso, D3DDisassemble)

- 4-tap cubic reconstruction on t1 + fixed 0.25 history blend from t2
  (gated by `cb0[5].x`); t0 = 1x1 params texture (exposure/offset:
  `r0 = r1*(1-t0.w) + t0.rgb`); radial mask, color matrix, hash dither.
- NO velocity, NO depth, NO jitter uniforms — fixed-function temporal blend.
  Zero-MV feed was honest; jitter-0 less wrong than assumed.
- Tail = sRGB piecewise encode (`*12.92/max/log/*0.416667/exp/*1.055-0.055`,
  `min()` quirk, `w=0`). M9f replicates it in the output copy. Still tinted.

## 17. Viewer findings (M9g/M9h) + HDR pivot (M9i)

- t0 = UI-ish/composite; t1 == t2 (identical washes); t3–t7 identical small
  buffers; DEPTH = perfect R-only silhouette (linear depth valid);
  MVS = perfect yellow-neutral UE velocity (valid).
- t1 channels: **R dead black, G = luma scene, B/A flat gray**. The fmt24
  pair is NOT scene RGB in the showcase/menu mode (luma-only data?).
  Native still perfect ⇒ real color lives elsewhere in gameplay.
- M9i: feed the cached **R16G16B16A16F linear HDR** buffer (cf. CS 8AEEB67D
  t1) direct to NGX with `hdr=true`; fmt24 pair demoted to fallback.
  Log reports `hdr_feed=0/1`. Next: hue verdict → jitter mapping (M9+).

## 18. M7 "does nothing" on latest build (M10k fix)

- Root cause: the M10 upscale-only gate (`ow <= rw || oh <= rh → native`)
  blocks render==output, which is exactly DLAA (the M7 goal:
  "render==output, no resize") — i.e. normal 100%-scale gameplay. The M7
  toggle became a no-op; earlier builds at least changed the image.
- Fix (`m10k-2026-10-01-dlaa`): DLAA allowed by default; native path only on
  true downscale (`ow < rw || oh < rh`) or when the new "M10: upscale-only"
  guard is ticked. Menu/map UI modes at render==output stay covered by the
  t0 composite (M10d, on by default).
- Same report, other possible causes (now surfaced in the menu): SR type
  None (M7 needs DLSS SR in the Luma menu), or slot 000573B8 never matching
  because PS hashes are FFFFFFFF on a dump run (Dump fills first — 39k files
  observed in one session — hashes match from the next boot). Menu now shows
  `PS unresolved (FFFFFFFF)` + `attempts` so the two are distinguishable:
  attempts>0/runs=0 = gate/inputs; attempts=0 = hash/SR-type/slot.

## 19. Gameplay TAA is COMPUTE 242D9D62, not pixel 000573B8 (M11)

- M10k frame.log (3 frames, gameplay 1920x1080, m10k build): pixel slot
  000573B8 fires ZERO times, no 2560x1440 anywhere. It only runs in
  showcase/upscale modes (UE TAAU). Real gameplay resolves in
  **CS 242D9D62, exactly 1/frame**: `u0=<stable> fmt24 ← t0 fmt27
  + t1 depth fmt42 + t2 fmt10 linear HDR + t3 fmt24 history`, with u1/t3
  swapping handles every frame (ping-pong proven across 37880/37881/37882).
  UE4 has both paths: TAA (same-res, compute here) + TAAU (upsample, pixel).
- Feeds identified: t2 HDR writer = C9756A11 (1/frame); velocity fmt35
  buffer written by BE0130E5 (one of the 6 known MV producers, 14x/frame);
  t0 fmt27 = multi-writer composite (B8C2DC9F/FC18E61A/96DB2A3D/FC86007D),
  NOT used as MV input (fmt35 cache is the known-good velocity).
- M11 wires NGX DLAA at the compute slot (`m11-2026-10-01-ctaa`): fmt10 t2
  fed direct with `hdr=true` (no unpack!), depth = t1, MVs = converted
  fmt35 cache (zero fallback), jitter 0 (compute CB unmapped — one-shot CB0
  dump tool added, same staging pattern as pixel), NGX → own FP16 → plain
  linear alpha-preserving copy into game u0 (+u1 best-effort) via runtime
  RTV (bails to native with a log if u0 isn't RTV-bindable — then a Copy CS
  is the follow-up). No UI composite (pre-tonemap output, UI downstream
  untouched). Pixel path untouched for upscale modes.
- No-build triage for any session: "Freeze typed hash" 242D9D62 — whole
  image freezing proves sole-writer status; "Log compute TAA" confirms
  u/t dims per mode. Compute freezer Skip already worked (shared path).

## 20. Regression hunt: DLSS worked at 61c1f1a, broke at M10j (M12 fix)

- `git diff 61c1f1a..3e4fb6f` isolates the breakage to M10j: the MV feed
  changed from RAW fmt35 (`mv_res = cached`) to UNORM→float conversion
  (`(v-0.5)*2*res` pixels) while `mvs scale` stayed `0.5*res` — if the core
  centers/scales internally (as the working build implies), the converted
  feed is double-scaled garbage. Same window also defaulted the tonemap UI
  composite ON (look change, not fatal).
- M12 (`m12-2026-10-01-mvjit`): `g_mv_convert` toggle, default OFF = proven
  raw feed on BOTH paths (M7 pixel + M11 compute); tick to A/B conversion
  live. If conversion ever wins, the scale must be re-tuned with it.
- M12 compute jitter: CS CB0[26]/[27] read (32px clamp, shared flip
  toggles), own `g_cjitter_on` default OFF, px values in the first-Draw log
  line. Confirm indices via "Dump compute TAA CB0 floats" before enabling —
  pixel slot's [0]/[1] were dither, not jitter.
- Still open (needs live data): UI symptom specifics (is UI baked into the
  compute pair in showcase/menu, or downstream?), CB0 dump contents, and the
  `cDLSS first Draw` render/out/mv_real/jit line from a running session.

## 21. M10j conversion deleted, compute t0 HUD composite, menu cleanup (M13)

- MV root cause (user: "conversion is not accurate"): the fmt35 velocity
  buffer is UINT, not UNORM — float-sampling it in MVConvert.hlsl produced
  garbage MVs, and keeping `mvs scale 0.5*res` double-scaled on top. Proof
  sketch: 61c1f1a fed the cache RAW with that scale and DLSS ran; under
  "core multiplies without centering" semantics raw-UNORM would carry a
  480px constant bias (never viable), so the core must center internally —
  which makes raw-direct exactly right and any pixel-converted feed only
  approximately right at best. Fix: conversion path DELETED on both
  pathways (raw direct, as proven); the core reads the native UINT format
  correctly itself. Removed: `g_mv_convert` toggle, both conversion blocks,
  MVConvert registration, viewer MVS_CONV source (DLSS_OUT renumbered 11).
- HUD (user: "UI is at t0", SR-mark toggle did not restore it): the compute
  path now composites the slot's t0 over both UAV targets via the shared
  RunUIComposite (size-gated, blend-mode + tonemap/sRGB toggles honored),
  behind `g_ccomposite_ui` default ON. If HUD is still missing with it on,
  HUD is NOT in compute t0 — confirm via "Log compute TAA dims" (t0 should
  be fullscreen; 1x1 = params = composite correctly no-ops).
- Menu cleanup: M3 force-skip deleted outright (decl, dispatch block, menu —
  the 4 old hashes never run); freezer candidate + pinned lists collapsed
  into "Freezer candidates (advanced)"; manual hash now defaults to
  242D9D62 (the live slot).

## 22. Motion shimmer isolation (M14)

- User report: zero-MV mode is stable stationary, shimmers on motion. That
  is the textbook missing-MV signature (static frames need no MVs; pans
  live or die by them) — so zero-MV behaving this way is EXPECTED, and the
  real question is what live MVs do.
- New hypothesis for wrong live MVs: the fmt35 cache may only feed the
  depth/motion COMPUTES, while the TAA itself consumes PROCESSED velocity
  via its t0 (fmt27 multi-writer composite). M14 adds a live A/B:
  `g_cmv_from_t0` (default OFF = fmt35 cache) feeds the slot's own t0 as
  motion vectors. Pan-test both.
- M14 also extends the first-Draw log line with `mvsrc=` (zero/cache/t0)
  and `mvfmt=` (TRUE runtime format of the MV feed — settles UNORM-vs-UINT
  empirically, no more enum-table guessing).
- Isolation matrix (no build needed, all toggles exist): (A) real MVs +
  jitter OFF + depth ON pan-test; (B) + zero-MV (expect shimmer = control);
  (C) real MVs + no-depth (rules depth in/out); plus the menu's live
  "MV cache: FRESH/stale" line (stale = zero fallback = same as B). Jitter
  mismatch is the other classic motion-shimmer source: game renders
  jittered (CB reads ~1px), feed is 0 unless the M12 toggle is on.

## 23. M15 combination matrix (user: shimmer persists, UI wrong, no-depth kills DLSS)

- Test results on M14: fmt35 MVs shimmer, slot-t0 MVs slightly better but
  still shimmer; `no depth` ON stops DLSS entirely (core requires depth —
  Draw fails with null depth, so depth isolation via that toggle is a dead
  end; depth stays mandatory); UI composite lands wrong.
- Log: `mvsrc=cache mvfmt=35` (fmt35 confirmed live; live cSTATS read it
  as fractional 0..0.5006 mean 0.226 — i.e. 0.5-centered NORMALIZED
  behavior, so the DXGI-table UINT reading is wrong in practice and the
  raw-direct feed stands regardless).
- New live options (change ONE at a time while panning, M11 compute path):
  MV scale combo (0.5*res / 1.0 / 0.5), jitter scale combo (x1 / x2 NDC /
  x0.5, applies to both paths), inverted-depth checkbox (both paths), and
  compute UI source combo (t0/t1/t2/t3/OFF — finds the real HUD buffer; if
  OFF looks identical to t0, the HUD was never in the slot). First-Draw log
  now also records `msc=`/`inv=` so reports stay correlatable.
- Deleted dead `g_premult_alpha` (stored, never read — blend combo covers it).
- Suggested order: UI source sweep first (OFF vs t0–t3, watch HUD only),
  then MV scale sweep (watch motion only, HUD ignored), then jitter scale,
  inverted depth last (needs NGX reset between flips — retoggle M11).

## 24. "Not temporal" — history-nuke hunt (M16)

- User: MVs don't look temporal / something isn't working temporally. A
  DLSS that never accumulates history looks exactly like this (perpetual
  shimmer, no stability gain stationary or moving).
- Suspects: (1) BOTH M7 pixel and M11 compute firing in one frame
  (showcase transitions) and fighting over the shared NGX instance +
  UpdateSettings (different dims per call) — destroys history. (2) reset
  stuck true via `force_reset_sr` (set on every toggle/capture-fail and by
  the core). (3) jitter mismatch (game jittered, feed 0/wrong — reprojection
  can never align). (4) `FrameIndex` not advancing (core-side, unverifiable
  here). (5) MVs still wrong (matrix pending).
- M16 instruments it: `g_cdlss_resets` counts executed frames with
  reset=true (menu shows attempts/runs/resets — resets must stay ~1 while
  runs climbs; climbing together = history nuked). M11 gets priority: the
  pixel path parks itself while the compute master is on, plus a both-on
  warning in the menu.

## 25. UI tonemap-mode selector (M17)

- User asked for more tonemap options for the t0 HUD blend. The two
  checkboxes (tone-map-before-composite, sRGB-decode) are replaced by one
  `g_ui_tonemap_mode` combo, shared by the pixel and compute composites:
  0 None (plain keep-alpha copy), 1 Reinhard + sRGB (old default), 2 ACES
  (Narkowicz fit) + sRGB, 3 Hable/Hejl filmic + sRGB, 4 sRGB-decode only
  (old M10i). HLSL `TONEMAP` define replaced by `UI_TM` 0–3 (undefined = 0
  = plain, so all existing variants compile unchanged); two new registered
  variants (ACES, Hable). Suggested sweep: 0 first (does the HUD match with
  zero processing?), then 2/3, then 4 — combined with the blend-mode combo
  (premult/straight/additive/multiplicative) and the M15 UI-source combo.

## 26. Resets = 1, UI near-match, encode-only variant (M18)

- User: `resets` stays 1 while runs climb — NGX history accumulates fine,
  so the history-nuke thread is CLOSED. Remaining shimmer = MV source /
  scale / jitter encoding (M14/M15 sweeps still open — MV scale and jitter
  scale verdicts never reported; t0 beat cache slightly).
- User: ACES + sRGB with straight blend is the closest UI match, but a bit
  more contrasty with slight color shift. Interpretation: every curve mode
  shifts contrast/hue by construction; the matrix lacked a NO-CURVE path
  that still matches dst encoding (0 plain assumes sRGB-in, 4 decodes).
  M18 appends mode 5, "sRGB encode only" (KEEP_ALPHA + SRGB_ENCODE, no
  decode, no tonemap) — the correct op if the HUD buffer is already LINEAR.
  If mode 5 matches with no contrast/hue shift, t0-linear is proven and the
  composite is done; if it looks dark/washed, t0 is sRGB and the residual
  ACES delta is downstream double-grading (game post tonemapping our
  already-graded UI), whose fix would be compositing post-tonemap instead.

## 27. Sweeps change nothing — signal-presence hunt (M19)

- User: MV scale and jitter scale sweeps change NOTHING in motion. Math:
  scale x (zero/constant content) = no visible change. So the fed MVs (and
  possibly jitter) carry ~no signal in the tested scene — the sweeps aren't
  failing, there's nothing to scale. Fits showcase testing (static scene,
  possibly no velocity passes there) while the M11 proof capture was real
  gameplay. Also fits a wrong-CB-index jitter (constant ~1px always).
- M19 adds a one-shot signal-stats logger (staging readback + min/max/mean
  per channel, format-aware: f16/u16/u32/f32/bytes): t2 color, t1 depth,
  t0 HUD/velocity, MV feed in use, plus cache FRESH/STALE. Run it in the
  test scene AND in open-world gameplay while panning: zero-variance MVs =
  degenerate mode (test DLSS in gameplay instead); differing t0 stats =
  mode-dependent buffer.
- M19 knobs: `g_mvs_jittered` (Bend MVs may include jitter, unlike UE
  convention — both DLSS paths), near/far sliders (wrong depth range =
  motion-only disocclusion shimmer that ignores MV/jitter tweaks; both
  paths). Protocol: stats first, then open-world retest, mvs_jittered A/B,
  autoexposure OFF (flicker source), inverted depth reminder.

## 28. Bytecode verdicts: no native MVs, HUD-at-end, sRGB u0 (M20)

- Wrote a DXBC parser (validated: 000573B8 walk reproduces the known
  disassembly exactly — 3 DCL_RESOURCE t0/t1/t2, 4 taps t1, history t2,
  params LD t0, 0.25 blend, sRGB tail) and read the live shaders:
- **242D9D62 (compute TAA, 305 instrs)**: DCL t0 float4 / t1 UINT4 /
  t2 float4 / t3 float4. Reads: t2 color 2x LD, t1 depth 1x LD (edge
  logic), t3 history **10x SAMPLE_L lod0** + 1x LD, **t0 exactly 1x LD at
  instr 293/305 (the very end)**. Tap coords therefore cannot depend on
  t0/t1 — they derive from thread IDs + CB + immediates (FIXED kernel,
  possibly CB-jittered). **The native TAA uses NO motion vectors.**
  Immediates: 0.25 blend, 0.41667/1.055 sRGB tail, dither hashes.
  Stores (op164): u1 at 273 (pre-tail/HUD), u0 at 303 (final). So
  **u1 = linear scene-only history, u0 = sRGB scene + HUD**.
- **BE0130E5 (velocity, 16 instrs)**: pure vertex-driven splats (v0+v1 in,
  MIN clamps, 0.49999-centered immediates, no texture reads) → fmt35 IS
  0.5-centered velocity. Raw-direct feed stands; M10j conversion stays
  deleted. User was right to question the pass — answer: the pass was
  right, the *mode* (showcase without producers) was suspect.
- **B8C2DC9F (t0 writer, 33 instrs)**: 1 texture sample + MIN/NE/MAD,
  immediates only [1.0] (mask/alpha math, NOT velocity math) → t0 is a
  composite/mask buffer. Combined with end-of-shader single LD: **t0 = HUD
  overlay, confirmed structurally** (matches the user report).
- M20 changes: (1) u0 scene copy sRGB-encoded by default
  (`g_c_srgb_output`, keep-alpha, swap-aware via new Encode-BGRA variant;
  u1 history stays LINEAR plain) — the linear copy mismatched native
  encoding (also explains wash + UI-space mismatch); (2) HUD composite
  over u0 ONLY (was: both — UI in u1 history burns in on motion);
  (3) MV cache hash-gated ONLY (format fallback deleted — it cached any
  large RG16 RT as FRESH garbage, worse than honest stale/zero; if
  gameplay goes stale, read producer hashes off the freezer band and
  extend the 6-hash list); (4) first-Draw logs `cso=`.

## 29. Real jitter = CB0[0]/[1], UI defaults locked (M21)

- User: freezing 242D9D62 (DLSS off) freezes the screen — sole-writer
  status CONFIRMED live. UI fixed with premult blend + sRGB-encode-only.
- CB0 dumps (2x, 4096 bytes) decoded: [20]/[21] = 1920/1080 render,
  [24]/[25] = 1920/1080 output, **[22]/[23] == [26]/[27] == constant
  0.00052/0.00093 (~1px: static texel bias, NOT jitter)**,
  **[0]/[1] animate 0.03–0.49px across dumps = the real per-frame jitter
  in pixels**. The M12 mapping ([26]/[27] x res) fed a constant wrong
  offset — which is why the jitter toggle/scale changed the shimmer so
  little. M21 feeds CB0[0]/[1] directly (flips + scale still apply).
- UI defaults locked to the user-proven combo: tonemap mode 5
  (encode-only) + premult blend 0 (was already 0).
- Still open: motion re-test with REAL jitter (toggle on) + MV source A/B
  re-run post-M20 (hash-gated cache, sRGB u0, clean u1 history); cSTATS in
  gameplay (signal presence); MV/jitter scale sweeps re-run (now scaling
  real signals instead of zeros).

## 30. Dark-gradient alpha chain (M22)

- User: dark alpha gradients get darker under the (correct) premult +
  encode-only composite. Diagnosis is the alpha CHANNEL, not the blend
  formula: if our u0.A comes back < 1 (NGX alpha) while native was opaque,
  every downstream alpha use darkens translucent zones — and the composite
  result alpha (sA + dA(1-sA)) inherits it. Forcing scene alpha to 1 also
  forces the composite result to 1 wherever HUD is present.
- M22: `ALPHA_ONE` HLSL branch + 4 forced-opaque scene-copy variants
  (plain/BGRA x encode on/off) + `g_c_alpha_mode` combo (Keep NGX alpha /
  Force opaque, wired into both u0+u1 copies; UI composite untouched).
  If Force opaque fixes the gradients, native u0.A = 1 is proven and it
  becomes the default.
- M22 blend modes (shared composite): 4 alpha-additive (SRC_ALPHA/ONE —
  glow-style UI adds instead of darkening), 5 premult RGB-only (write mask
  drops alpha — dst alpha preserved exactly), 6 replace/no-blend
  (diagnostic: shows the raw UI texel, isolating texel-vs-blend faults).
  Suggested order: scene-alpha force-opaque first (single toggle, fixes
  the whole chain if alpha is it), then blend 4/5/6, then tonemap modes.
## 31. Still not temporal — jitter default ON + frame-index check (M23)

- User: stationary OK, motion flickers hard, nothing temporal by any
  setting. Ruled out by logs so far: resets=1 (history kept), slot proven
  (freeze test), all inputs live with signal (color/depth/HUD/MVs),
  sRGB u0 + clean u1 + HUD-over-u0-only (bytecode replication), UI fixed.
- M23: compute jitter defaults ON (CB-proven [0]/[1] pixels; feeding 0
  while the game jitters was a guaranteed constant error). If stationary
  breaks with it on, that itself proves [0]/[1] aren't jitter — revert.
- M23: cTAA logger line gains `fidx=` (core FrameIndex fed to NGX). Press
  the log button twice seconds apart: identical fidx = stuck counter, and
  that alone explains zero temporality regardless of every other setting.
- fmt35 correction: live stats read fractional 0..0.5006 (mean 0.226),
  i.e. 0.5-centered NORMALIZED behavior — the UINT table reading is wrong
  in practice; raw-direct stands either way.
- Short protocol (M23, open-world gameplay, slow pan): (1) retest motion
  with jitter now on (default) — stationary + moving verdict; (2) log
  button twice, paste both fidx values; (3) inverted depth ON + M11
  retoggle. Optional 4th: autoexposure OFF (flicker source).

## 38. Viewer fixed (?) + stable-means-native + per-pass freshness (M28)

- User: compute viewer shows nothing. Root cause candidate: typeless
  resources (HUD t0 is R8G8B8A8_TYPELESS; depth is R24G8_TYPELESS) fail
  nullptr-desc SRV creation — the t0/depth views could never build. Fix:
  `CreateViewSRV` helper (typeless -> concrete UNORM view) used by all
  compute stash sites + pixel MVS case, plus a one-shot `cVIEW stash
  FAIL src=N` log and a live "Viewer blit pending" menu line so a silent
  viewer is diagnosable instead of mysterious.
- User: 6C6AD505/A1A256FA "stable" while others shimmer. Interpretation:
  stale-selected passes fall back to NATIVE (runs stall) — stable image
  means DLSS ISN'T RUNNING there, not that those MVs are better. The menu
  now shows per-selection FRESH (feeds DLSS) vs STALE (native fallback)
  so this is directly readable; verify via runs counter (stalls = native).
- M28 also moves the producer hash/name tables to file scope (shared by
  feed + menu + stats) and makes the stats mv-feed follow the MV source
  selection, so all three agree on which buffer is under test.

## 39. Status: m27 live, M28 pending test

- Installed build is still m27 (`m27-2026-10-02-velpass` in marker; M28
  pushed, CI building). Latest M27 session first-Draw: `ok=1 reset=1
  render=1920x1080 out=1920x1080 sr_type=0 mv_real=1 mvsrc=cache mvfmt=34
  mvhash=BE0130E5 msc=0 dec=1 inv=0 cso=0 jit=0,0` — auto cache from
  BE0130E5, x2 decode live, jitter OFF, plain linear output.
- No cVIEW/cSTATS lines this session (viewer/log buttons not pressed).
- Awaiting M28 test: (1) compute viewer on t2/t3/u0 (concrete formats,
  must show) then t0 (typeless — the fixed path); pending line + stash
  FAIL log diagnose silently-dead views; (2) per-pass FRESH/STALE + runs
  behavior for each of the 6 producers (stable = native fallback unless
  runs climb); (3) with a WORKING producer identified, MV scale sweep +
  jitter ON retest (both scale real signals now, not zeros).

## 37. Named velocity-pass selector + gameplay viewer (M27)

- User asked: feed DLSS from the velocity passes (not the blended cache)
  + a viewer with a dropdown of all known named passes.
- MV side: per-producer cache map (`g_mv_by_hash`, own ref each, frame
  stamped) filled in the RTV block; "M27: MV source (velocity pass)" combo:
  Auto + the 6 hashes (BE0130E5 first — analyzed 16-instr vertex splats).
  Selected buffer must be this-frame fresh else honest zero fallback
  (runs stall visibly instead of feeding stale). First-Draw log gains
  `dec=` (decode mode) next to `mvsrc=`/`mvfmt=`/`mvhash=`. Slot-t0 stays
  on its own M14 checkbox.
- Viewer side: pixel "next source" button -> "Pixel viewer source" combo
  (same 13 entries); new "Compute viewer source (gameplay)" combo with
  named TAA u/t + caches + DLSS output. Compute side stashes an SRV at the
  slot (masterless, depth gets its R24 view) and blits it over the next
  large pixel draw + Skip; staleness cleared in OnPresent and when
  switched OFF. Channel combo shared.
- Protocol: in gameplay, view Velocity cache vs TAA t2/t3 (is there
  motion?), then set MV source to BE0130E5 and pan; compare against Auto.

## 32. MV encoding DECODED — the shimmer root cause (M24)

- cSTATS on the fmt35 buffer: **range [0, 0.5], mean 0.226 while
  stationary**. A 0.5-centered buffer (v*0.5+0.5) would read 0.5 stationary
  with range [0,1]. It doesn't. The BE0130E5 shader constants **0.2495 /
  0.49999** are exactly the (v+1)*0.25 encoding factors.
- So fmt35 is **(v+1)*0.25** encoded: no motion = 0.25, max = 0.5. We fed
  it RAW, so DLSS read 0.25 as "half-max reverse motion" every frame — a
  constant wrong reprojection offset. Stationary = consistent offset = looks
  OK. Moving = offset + real motion = wrong reprojection = shimmer. This is
  the exact reported symptom and why no scale/flip/jitter toggle helped
  (they were scaling/offsetting a wrongly-centered value).
- M24: MVConvert shader gains a DECODE param (0 raw, 1 x2 = correct for
  (v+1)*0.25, 2 (v-0.5)*2, 3 v+0.5). Default **1 (x2)**. The x2 decode maps
  [0,0.5] -> [0,1] with no-motion at 0.5, then the existing 0.5*res scale
  converts to pixels. Compute path wired; pixel path inherits via the same
  selector. Test: motion with decode x2 (default) — shimmer should collapse.

## 33. User verdicts: sRGB wrong, t0-MV stationary-perfect, jitter softens (M25)

- User verdicts, all decisive: (1) M20 sRGB scene output "bullshit" (no
  help) → native u0 is LINEAR; the sRGB-tail immediates must serve another
  branch (dither/UI), not u0 — bytecode inference retracted, user data wins.
  Default back to plain. (2) t0-as-MVs = PERFECT stationary (broken on
  motion) → NGX history accumulation WORKS (resets fine, output path fine);
  t0 is static content (HUD) so it tracks nothing — confirms the ONLY broken
  pieces are live motion inputs. (3) CB0[0]/[1] jitter ON = soft/blurry
  image → those values are NOT the reprojection jitter (or need sign/scale
  work); default back OFF.
- M25 = M24 x2-decode (untested at the time of writing — the first M24 push
  failed CI, only the f4d63ee fix builds) + reverted sRGB/jitter defaults +
  everything else unchanged. Baseline: M11 + composite + x2 decode, all else
  default/off, tested in OPEN-WORLD GAMEPLAY (not showcase).

## 34. Temporal data lives here + feed instability proof (M26)

- WHERE IS THE TEMPORAL DATA (user question), answered: (1) NGX-internal
  history (invisible; needs reset=false + advancing frame_index + USABLE
  MVs — first two proven, third suspect); (2) game history u1/t3 pair
  (visible in capture; we overwrite it with our output every frame).
  "Not temporal" = NGX discards history every frame, i.e. MVs unusable.
- Smoking gun from the M24 log: `mvfmt=35` one session, `mvfmt=34` the
  next. The cached MV resource CHANGES format across sessions/modes —
  multiple producers write different buffers and latest-wins. Any fixed
  decode/scale is right for one and wrong for the other, which is why no
  single setting ever stabilized motion.
- M26 logs `mvhash=` (which producer filled the cache) in the first-Draw
  line. One gameplay pan + that line identifies the combo; the decode is
  then hardened per (hash, format) instead of guessed globally.

## 35. Status consolidation (M24-fix session + M25/M26 state)

- Latest live session (M24-fix, still current installed build): first Draw
  `ok=1 reset=1 render=1920x1080 out=1920x1080 sr_type=0 mv_real=1
  mvsrc=cache-decoded mvfmt=34 msc=0 inv=0 cso=1 jit=-0.90,0.95`.
  So the x2 decode IS executing, jitter IS flowing — and motion still
  shimmers. Decode-centering alone was not sufficient.
- Standing data points: fmt flips 34/35 across sessions (feed
  non-determinism); t0 = HUD (stats + bytecode + user); u1 = linear
  history, u0 = final (bytecode store order); native TAA uses no MVs
  (fixed kernel); resets=1; fidx advances; freeze test proves the slot;
  UI fixed (premult + encode-only); sRGB-u0/defaults reverted per user
  (M25: plain u0, jitter OFF, x2 decode baseline).
- Still open, in order: (1) mvhash per mode (M26 untested — identifies the
  exact (producer, format) combo); (2) MV magnitude/scale for the decoded
  feed (centering fixed, scale unverified — 0.5*res may be 4x off);
  (3) jitter sign (flips untested with real values); (4) inverted depth +
  mvs_jittered (never confirmed tested); (5) open-world gameplay verdict
  with M25 defaults (showcase may be degenerate throughout).

## 40. M28 session verdicts + viewer root causes + toggle cleanup (M29)

- Session `m28-2026-10-02-viewfix`, 22:52–23:16: clean boot (5x
  attach/enumerate + `first-present`, no crash), `cDLSS first Draw ok=1
  reset=1 render=1920x1080 out=1920x1080 mvsrc=cache mvfmt=34
  mvhash=BE0130E5 dec=1 jit=0,0`. DLSS executes at compute slot 242D9D62.
- cSTATS x2 (gameplay-ish): t2 fmt10 HDR live, t1 fmt42 depth live, t0
  fmt27 HUD live, `mvcache FRESH`, `mv-feed fmt=35 [0,0.5001] mean
  ~0.23-0.26` — re-proves (v+1)*0.25 encoding; x2 decode direction stands.
  `mvfmt=34` in the first-Draw line is the converted R16G16F target, not
  the source — not a flip.
- No cVIEW lines at all this session: viewer buttons were never pressed,
  so "viewer not working" is interactive-only, not log-proven.
- Viewer root causes found in code: (1) compute stash gated to
  `ccs == 242D9D62`, so cache sources 6-9 die in menu/showcase where the
  slot never fires (and pixel viewer symmetrically dies in gameplay);
  (2) `OnPresent` wiped `g_cview_pending` every frame, but the slot fires
  late (post→TAA→tonemap→UI) — whenever no large pixel draw remained
  after the slot that frame, the stash was discarded before display.
- M29 (`m29-2026-10-03-viewfix2-cleanup`): cache sources 6-9 stash outside
  the slot gate (work in any mode); pending persists until consumed or
  source OFF (1-frame-stale worst case); `cVIEW stash ok/FAIL` both logged.
- Toggle cleanup (menu only, logic untouched): deleted `no depth`
  (core requires depth — guaranteed Draw fail), `jitter off` (redundant),
  `MVs from slot t0` (t0 proven HUD); trimmed MV decode to Raw/x2 (2/3
  were aliases — only DECODE-1 shader exists); collapsed pixel-showcase
  feeds, pixel jitter/viewer, and the M15/M19/M20 motion matrix into
  `CollapsingHeader`s. Shared controls (autoexp, swap-output, mark-SR,
  blend, tonemap, zero-MV) stay top-level.
- Not compile-checked locally (no Luma checkout / msbuild here) — brace
  balance verified (568/568); needs CI `Test-Release + Publishing-Release`
  build.

## 41. Motion-temporal root cause: MV double-scale + uncentered (M30)

- Compared against upstream `Unreal Engine/main.cpp` (working stock-UE4
  DLSS) + NVIDIA DLSS programming guide + upstream
  `Luma_MotionVec_UE4_Decode.hlsl`. Luma convention is unambiguous:
  - MV texture holds PIXELS (upstream decode ends
    `-screenSpaceDelta * (0.5,-0.5)` = NDC delta * res * 0.5).
  - `mvs_x/y_scale = 1.0` pass-through (NVIDIA: "1.0 if motion vectors
    do not need to be scaled, never 0.0").
  - `inverted_depth = true`, `mvs_jittered = false`, jitter in PIXELS
    at render size (`jitter.xy * res * (0.5,-0.5)`, Y negated).
  - Upstream near/far: `near/100` (cm→m), far `FLT_MAX`.
- Our M24/M15 defaults violated all of it: the convert shader emitted
  `(c*2)*res` (no-motion 0.25 → 960px constant bias) AND settings
  multiplied by `0.5*res` again (~960x motion gain, `inv=0`). No scale
  or jitter toggle could ever fix that — and showcase tests (zero MVs)
  hid it (`scale x 0 = 0`, RESEARCH #27).
- M30 (`m30-2026-10-03-mvfix`): DECODE 1 now emits `(c*2-0.5)*0.5*res`
  (zero-centered pixels, no-motion = 0); scale default = 1.0
  (legacy 0.5*res kept as mode 1 for A/B); `inverted_depth` default TRUE.
  Jitter stays OFF (native TAA is a fixed kernel — game likely unjittered;
  unproven CB0 values stay opt-in), near/far untouched (10/200000).
- Test protocol (gameplay, slow pan): M30 defaults first — motion should
  stabilize vs M28. If ghosting flips direction, MV sign (no flip toggles
  yet — report it) or `mvs_jittered` is next; if disocclusion halos only,
  depth range is next. First-Draw `msc=/dec=/inv=` line stays correlatable.

## 42. Core semantics proven + bytecode round-trip: decode corrected again

- Read upstream `Source/Core/dlss/DLSS.cpp::Draw`: `InMVScale`,
  `InJitterOffset` are passed STRAIGHT to NGX — no centering in core.
  NVIDIA guide: scale 1.0 = pass-through. So the MV texture MUST hold
  zero-centered pixels and scale MUST be 1.0. The M24-era
  `(pixel texture) x (0.5*res scale)` = ~960x gain + ~960px bias:
  broken under every semantics. That part of M30 stands.
- Disassembled `Dump/0xBE0130E5.ps_5_0.cso` constants
  (byte scan: `1.0 / 0.2495 / 0.499992 / 4.008016 / -2.003978`):
  `4.008016 == 1/0.2495` exactly and
  `0.499992*4.008016-2.003978 ~= 0` — the shader carries the exact
  encode/decode round-trip pair for `e = v*0.2495 + 0.499992`
  (0.5-centered, `1.0` = min-clamp bound), byte-identical in structure
  to upstream `Luma_MotionVec_UE4_Decode`
  (`(sample-32767/65535)/(0.499*0.5)`, same constants). The live
  velocity feed is STOCK UE 0.5-centered — not 0.25-centered.
- Consequence: the m28 cSTATS `0.25 / identical X+Y channels` sample was
  a non-velocity buffer (latest-wins cache caught the wrong producer in
  a degenerate mode — exactly the M26 non-determinism warning). M30's
  first DECODE-1 formula targeted that phantom buffer and would have fed
  a 480px bias on the real feed. Corrected before any build:
  DECODE 1 = upstream `(c-0.499992)*4.008016*0.5*res`, DECODE 2 keeps the
  0.25-centered fallback as a registered variant (menu actually switches
  shaders now — previously modes 2/3 silently ran the DECODE-1 shader).
- So "am I sure": the direction is now proven at both ends (bytecode
  encoding + core pass-through), converging on stock-UE handling. What
  is NOT proven until a gameplay pan: MV sign convention (core comment:
  "positive towards top-left"; our convert does not negate/flip while
  upstream emits `-delta*(0.5,-0.5)`), per-producer centers in gameplay
  (Stats per M27 selection, X/Y must differ), and depth range.

## 43. M30 live: DLSS runs, viewer mechanics diagnosed (M31 fix)

- Session `m30-2026-10-03-mvfix`: `cDLSS first Draw ok=1 reset=1
  render=1920x1080 out=1920x1080 mvsrc=cache mvfmt=34 mvhash=BE0130E5
  msc=0 dec=1 inv=1` — M30 defaults live (scale 1.0, inverted depth).
- User viewer report: velocity cache = yellow bike wheels only;
  depth/HDR/DLSS_OUT = BLACK; TAA t-slots = no visible change. Logs show
  `cVIEW stash ok` for 0,2,3,4,5,6,7,8,9 and FAIL only for src=1 (t1
  depth SRV) — so stashes succeed but the single one-shot blit lands on
  an arbitrary mid-frame pass: invisible when buried by later passes,
  fullscreen-black when it hits the final draw. Both symptoms, one cause.
- cSTATS x3: color/depth/HUD live, cache FRESH,FRESH,STALE; mv-feed
  fmt35 `[0,~0.5]` mean 0.12-0.15, X≈Y. Below either center in a static
  moment — coherent one-direction motion (riding/panning) shifts the
  mean; X≈Y identical stays suspicious (see #42: verify per-producer).
- M31 (`m31-2026-10-03-viewpersist`): viewer blit is PERSISTENT (every
  large draw replaced until source OFF — last draw before present wins,
  always visible), DLSS slots excluded from replacement, t1 depth tries
  R24/R32-generic SRV formats. Retest: every source must show a stable
  fullscreen image; bg gray (= 0.5-centered, trusts DECODE 1) vs black
  (= 0-centered or empty texture) settles the encoding empirically.

## 44. Verdict: black bg + direction-flaky producers → own camera MVs (M32)

- User confirms velocity-view bg is BLACK (cleared, not 0.5 gray) and
  that producers work looking one way, fail looking another. Black bg =
  phantom motion on every static pixel under any centered reading;
  direction-dependence = latest-wins lottery across producers. Game MVs
  are not fixable by decode math — decision: synthesize camera MVs.
- Capture proof there is nothing better to switch to: one shared fmt35
  buffer, all 6 producers, BE0130E5 last before TAA, TAA 1/frame.
- M32 (`m32-2026-10-03-ownmv`, this build): (1) DECODE 3 = upstream +
  zero-snap (exact-0 texels → 0px — the cleared-bg triage), DECODE 4 =
  negated upstream (sign triage; core wants top-left-positive); both as
  real registered variants, menu switches shaders for real now.
  (2) One-shot CB projection scanner at the compute slot (CB0-7 sizes +
  64B-aligned projection-shaped 4x4s with m20/m21/m22/m23/m32/m33) —
  finds ViewToClip/ClipToPrevClip for the generator.
- Protocol: pan-test decodes 1/3/4 (one at a time) + press the scan
  button in gameplay and paste the `cSCAN` lines. Next (M33): generator
  CS (depth → NDC → clip_to_prev → pixel MVs, camera motion exact;
  object motion approximate), fed with scale 1.0.

## 45. Zero-snap fixes stationary only + scan finds no matrices (M33)

- User verdict on m32: decode 3 stable stationary, motion still
  shimmers; modes 1/2/4 shimmery everywhere. Reads as: bg snap fixed
  the phantom background, but MOTION texels decode wrong — motion
  content may be `(v+1)*0.25`-encoded while clear is 0 (mode 2 without
  snap carries a -480px bg bias, hence shimmery-stationary there too).
- cSCAN (pressed 5x): CB0–CB3 all 4096 bytes, slots 4–7 empty, ZERO
  `PROJ?` hits. The M32 test only matched row-major layout — HLSL CBs
  are column-major, so transposed matrices were invisible to it.
- M33 (`m33-2026-10-03-decode5`): DECODE 5 = zero-snap + 0.25-centered
  (registered variant, menu-switchable — now 6 modes); scanner tests
  ROW and COL layouts and tags hits. Protocol: pan-test mode 5; press
  scan again and paste any `PROJ?` lines (CB#/offset/ROW-or-COL tells
  M34 exactly where the matrices live).

## 46. Mode 3 nails stationary, mode 5 fails, scans empty — sign + VS/PS (M34)

- User: zero-snap+upstream (3) excellent stationary, shimmer on motion;
  zero-snap+0.25 (5) "way off". So the bg theory is proven (snap fixes
  stationary) and motion content is 0.5-family — but direction/magnitude
  still wrong on motion. Prime suspect: SIGN. Mode 4's old "shimmery"
  verdict is VOID (no snap → bg bias dominated); sign was never tested.
  Backwards reprojection is exactly stable-stationary + chaos-on-motion.
- cSCAN x4 (m33, ROW+COL): CB0–3 present (4096B; CB3 once 2048 =
  mode-dependent bindings), ZERO hits. Conclusion: the fixed-kernel
  TAA's CBs carry no projections (it needs none — scalars only). The
  matrices live in the VERTEX stage of the velocity passes.
- M34 (`m34-2026-10-03-signscan`): (1) DECODE 6 = negated upstream +
  zero-snap — the first VALID sign test (mode 4 marked invalid in menu).
  (2) Scanner refactored to a shared helper + new one-shot VS/PS scan
  at the six MV producers (finds object→clip / prev matrices where the
  velocity geometry runs).
- No-build triage (do these NOW, they cost nothing): while panning,
  does the menu's MV-cache line stay FRESH and do runs climb? (STALE +
  stalled runs = zero fallback on motion = same signature.) With mode 3
  active, try MV scale 0.5 (magnitude-halving test for a 2x-sized feed).
- Protocol: mode 6 pan-test; press BOTH scan buttons in gameplay; paste
  `PROJ?` lines. If mode 6 locks motion, sign was the whole bug.

## 47. All-MV-stale in gameplay → format+depth gated cache (M35)

- User: ALL MV sources STALE in open-world gameplay, M11 ON, runs climb
  (= DLSS executes on the zero fallback every frame — motion shimmers,
  stationary OK).
- Root cause: the 6-hash gate is mode-fragile. Capture 51950-52 proves
  `BE0130E5` runs ZERO times there (5 other producers write the shared
  `1550F2020:1920x1080:35` RT instead); object-velocity shaders vary per
  scene, so unknown hashes never fill the cache and every named source
  (plus Auto) reads STALE.
- M35 (`main.cpp` RTV block): cache fills on `fmt35 + fullscreen +
  scene-depth-bound` instead of hash membership (every capture producer
  binds `D:1920x1080:44`; bloom/LUT scratch never binds scene depth, so
  the M20 mis-cache objection doesn't apply). Per-hash map still keyed
  by the ACTUAL writer — first-Draw `mvhash=` + manual selector stay
  truthful; unknown hashes feed Auto latest-wins. Write side now takes
  `g_mvmap_mutex` (readers already did — fixes the race too).
- Test: gameplay pan, menu MV-cache line must read FRESH and first-Draw
  `mvhash=` names the real producer; then re-run the decode 1/3/6 sign
  verdict (it was previously tested on zero-MV fallback = void).

## 48. M36 live diagnostics (user: more numbers in the addon)

- All atomics, zero GPU work. Menu, compute section:
  `MV feeds real/zero` (blind-DLSS ratio — zero climbing on motion =
  fix MVs first), `Fallbacks: capture-fail / u0-copy-fail` (splits the
  two native-fallback causes; one-shot warnings stay in the log),
  `Slot fire rate` (attempts/presents: 1.0 = gameplay, ~0 =
  menu/showcase/loading — no more guessing which mode the slot runs in),
  `Last cDLSS` snapshot (ok/reset/dims/mv-real-or-ZERO + source
  code/fmt/hash/jitter-fed — first-Draw log goes stale on scene change,
  this never does), `MV age` per producer (frames since each of the 6
  ran, `--` = never this session — answers "which velocity runs HERE"
  without a capture), `Auto MV cache age` (0 = this frame).
- Source codes: 0 zero, 1 auto-cache, 2 slot-t0, 3 named pick.
- Counters bump only on frames that reach NGX (good inputs); fail
  counters bump every failing frame (rates visible, not just once).

## 49. Session 18:03–18:13 — M35/M36 live, real MVs flow, pSCAN data (m37 prep)

- Screenshot (camp, standing): `attempts=runs=16196 resets=1`,
  `MV feeds real 14779 / zero 1417` (91% real — M35 works),
  capture/copy fails 0, fire rate 0.69 (menu open), `Last: ok=1
  1920x1080->1920x1080 mv=real(auto-cache) fmt=34 hash=6C6AD505
  jit=0,0`, `MV age 6C:1f A1:1f` (both fresh), decode 6, jitter OFF,
  preset K. First-Draw logged `mvsrc=zero` at boot (empty cache —
  expected transient; recovered).
- Capture 22673-75: slot 1/frame ping-pong intact; producers in THIS
  scene: 1E94CABC ~24/frame, 6C6AD505 + A1A256FA 1/frame each,
  5846E9DA/A5CB30BF/BE0130E5 zero — latest-wins Auto is the only
  strategy that survives per-scene rotation.
- cSTATS x3: t2 HDR + t1 depth + t0 HUD all live; mvcache FRESH;
  mv-feed fmt35 `[0, ~0.5] mean ~0.29`, X≈Y. Read via the UNORM path
  (true normalized values).
- pSCAN x4 (6C6AD505/A1A256FA): VSCB0 256B, PSCB0/VSCB2/PSCB2 2048B;
  `+384 ROW/COL PROJ?` hits carry m20/m21≈0 + m23=1.0/m32=10.0 and
  WOBBLE across presses (-0.00031/+0.00051/-0.00072) = animated
  per-object constants, NOT stable projections → false positives.
  CSCB (TAA slot): sizes only, no hits (fixed kernel confirmed again).
  Own-camera-MV generator still blocked on real matrices.
- cTAA CB0 dumps x2 (65s apart): [0]/[1] = 0.97/0.61 → 0.53/0.39
  (slow drift, ~0.4–1.0 — not plausible pixel jitter; Halton would
  oscillate frame-to-frame). [22]/[23]==[26]/[27]==1px const (bias,
  not jitter). Jitter stays OFF. fidx 22429→22920 advances.
- Decisive verdicts: mode 6 motion = same shimmer; mode 5 stationary
  = BAD. Mode 5 on 0.5-content = +480px bias → bad-stationary is
  EXPECTED, so 0.5-centered bulk is PROVEN (mode 3 excellent + mode 5
  bad = closed). Sign matrix gap: 3 (none) and 6 (both) both shimmer;
  single-axis flips never tested.

## 50. M37 single-axis sign flips (this build)

- DECODE 7 = Y-neg + snap, DECODE 8 = X-neg + snap (registered
  variants, menu-switchable — combo extended to 9 entries). 8 mirrors
  upstream `-delta*(0.5,-0.5)` (X-only flip vs naive) = theory
  favorite. Protocol: pan-test 7, then 8 (one at a time); if motion
  locks on either, sign was the whole bug. If both shimmer identically
  to 3/6, sign is OUT and depth range (near/far) is next.
- Build ID bumped m34→m37 (M35/M36 shipped mislabeled).

## 51. Sign OUT, depth/exposure/magnitude triage (M38)

- Verdicts: decode 7 shimmers, 8 shimmers a bit less but still bad.
  Full sign matrix (none/both/X/Y) now tested — 8 marginally best.
  Sign is OUT as the root cause; 8 becomes the default.
- User localizes shimmer to EVERYTHING on motion (not foliage-only,
  not edge halos) → global error, not normal DLSS foliage behavior or
  pure disocclusion edges. Remaining globals, in order:
  (1) MV magnitude — the M15 scale sweep ran on zero fallback (void)
  and was never re-run on the now-91%-real feed; menu-only A/B with
  scale 0.5 (any change in either direction = magnitude matters);
  (2) depth inversion/range — NEVER validated (inv=TRUE + near 10.0 /
  far 200000 defaults; upstream wants near/100 cm→m + FLT_MAX; a 10cm
  game-near = 0.1m to NGX, 100x off what we feed);
  (3) autoexposure flicker (menu checkbox, free test).
- M38 code: decode default 1→8; near slider min 1.0→0.01 (%.2f) so
  0.1 is reachable live (old min blocked the upstream-parity value).
- Protocol (menu-only, pan between each): autoexp OFF → scale 0.5 →
  inverted flip + M11 retoggle → near 0.1. Report each separately.

## 52. Own camera MVs — YES, same approach as DX9 DLSS addons (M39 stage 1)

- User: build our own MVs instead of fighting the engine buffer. Agreed:
  it dissolves encoding/sign/magnitude/rotation/staleness in one move
  (we define zero-centered pixels). Camera motion becomes exact; object
  motion stays approximate (DLSS tolerates this well at DLAA, and the
  DX9 retrofit addons prove the pattern: depth + matrices → camera MVs).
- Honest limits: (1) needs the view matrices — the actual hunt;
  (2) does NOT escape depth linearization (near/far/inverted still feed
  the unproject) — but with known-good MVs, depth errors show as clean
  edge halos, separable from full-frame shimmer for the first time.
- Why pSCAN failed: shape-matching single 4x4s finds animated
  per-object constants. View matrices are constant ACROSS producers
  within a frame — so M39 captures VS CB0/CB1 at up to 4+ distinct
  producers and diffs them (FNV over exact bytes): bit-identical in the
  same frame = view/viewproj candidate, logged with full values as
  `pSCAN2 CANDIDATE`. Legacy shape scan kept for sizes at first hit.
- Protocol: gameplay, ONE press of "Scan velocity-pass CBs", paste the
  `pSCAN2` lines. Then M40 builds the generator CS (depth → NDC →
  clip_to_prev → pixel MVs) behind an own-vs-game toggle for live A/B.

## 53. Viewer defects owned + fixed (M40) — user was right

- User: viewer breaks the game, OFF doesn't work, any view halts.
  All three confirmed in code (compute-viewer blit, `main.cpp`):
  (1) OFF cleanup lived ONLY inside the master-gated compute block —
  with masters OFF the stale `pending` flag survived forever, so every
  large draw kept being overwritten+skipped after OFF (stuck viewer
  until reboot). Fix: OFF clears pending+SRV in the masterless blit
  path itself. (2) Halt: persistent last-wins blit replaced+skipped
  EVERY large draw (150+/frame incl. 80x velocity) = hundreds of
  fullscreen copies/frame — fill-rate death on any GPU. Fix: one blit
  per distinct target per frame (~15 copies, last-wins kept).
  (3) The blit overwrote velocity (fmt35) writers, starving the MV
  feed while viewing. Fix: fmt35 targets never blitted.
- The pixel viewer was innocent (one-shot per slot fire). No gameplay
  session needed to validate: OFF returns to native immediately, views
  cost a few fps instead of halting.

## 54. M41 broad VS-CB census (Path 1 first build)

- Upstream recon (fetched `Unreal Engine/main.cpp`): generic promotes
  TAA only on >=2 color + depth + velocity bindings (Days Gone's
  compute TAA binds none → structurally invisible → wiki ⛔ explained);
  successful mods READ jitter/near/far/FOV from clip_to_prev_clip +
  view_to_clip tracked CPU-side via Map/Unmap hooks (CBs are pooled,
  matched by size), MVs decoded at scale 1.0 / inverted / unjittered /
  HDR-linear / DLAA-only. Our feed already mirrors that pattern — the
  gap is exactly jitter (we feed 0) + near/far/FOV (we feed sliders).
- M41 instead of Map hooks (signatures unverifiable without a local
  Luma checkout — a red CI build helps nobody): one-shot census with
  proven primitives (VSGetConstantBuffers + staging, same as pSCAN2).
  While armed, sniffs small VS CBs (256/512/1024/2048) at every pixel
  draw, shape-tags each 64B-aligned 4x4 (PROJ = upstream
  MatrixLikeProjection verbatim; VIEW = last-row-(0,0,0,1) + sane
  rotation rows — upstream's test REQUIRES m33~0 so views were
  invisible to it), dedups by exact hash, auto-disarms at 48 sniffs
  (~1-2 frames), logs n>=2 entries with sharing rank; shared across
  2+ shaders = CANDIDATE with full values.
- Protocol: stand still in gameplay, ONE press of the sniff button,
  paste the `MSCAN` lines. Candidates → M42 wire-up (jitter px +
  computed near/far/FOV); nothing shared → Path 1 branch ends, its
  data feeds Path 2's matrix ID for free.

## 55. sr_type=0 is DLSS, not None — log-reading correction (M45)

- User was right: DLSS WAS on (`ReShade.ini SRUserType=2` = DLSS).
  Upstream `Source/Core/includes/super_resolution.h` (fetched):
  `enum class Type { DLSS, FSR, None = -1 }` — so `sr_type=0` in every
  first-Draw line meant DLSS all along, and `SRUserType { None, Auto,
  DLSS, FSR_3 }` confirms ini value 2 = DLSS. No code-logic bug:
  all gates use the symbolic `SR::Type::None`, only the human
  reading of the raw int was wrong.
- M45: `SrTypeName()` helper, both first-Draw lines + ADIAG start +
  overlay now log/show `sr=DLSS/FSR/None`. With SR=DLSS confirmed
  live, the motion-shimmer hunt (own camera MVs) stands as before.
- Standing data (m43 ADIAG): fidx advances, resets frozen, real MVs
  (1E94CABC this scene), CB0[0]/[1] swing per-frame (not jitter —
  jitter stays OFF), slot descs stable. MSCAN finished in 16ms
  (pre-pan) so the view pair stayed static — M44 spans it over
  ~240 presents; next run pans 6-7s.

## 56. View + proj proven by math, own-MV generator ships (M47)

- m46 throttled MSCAN (4 sniffs/frame over 240 presents) caught the
  pan: `vs1+128 distinct=44 dmax=0.068` (+192 transpose pair).
  Offline proof (python): R rows unit-length, mutually orthogonal
  (dots ~5e-5) = perfect rotation, row-major; VP at vs1+0 satisfies
  VP=V*P with reversed-Z infinite proj (w-col == R col2 exactly,
  p00=1.572/p11=2.795 from two independent ratios, aspect 1.778 =
  16:9, near=10 = UE cm, vert FOV ~39.4 deg). Holds first AND last.
- Camera translation is NOT in the sniffed 256B (R has no
  translation; vs1+64 row3 is per-frame but layout-unproven) → M47
  is rotation-only: exact for look-around, translation still rides
  game MVs via the Off toggle. Translation hunt (deeper CB offsets)
  is v2 and needs no user run (extend sniff locally).
- M47: per-frame stash (VS CB1 2048B, shape-validated, 1 readback/
  frame) → dynamic 144B CB (Rcur+Rprev+ProjP+ResWH) → fullscreen
  OwnMV PS into the convert target → NGX (scale 1.0, top-left-
  positive: mv=(-dx*w/2, +dy*h/2)). Two registered variants
  (normal/negated) + menu combo Off/Rotation/Rotation-neg +
  stash-age line; `own-rot` logged in mvsrc (menu code 4).
- Protocol (menu-only, gameplay pan): Off vs Rotation vs
  Rotation-neg. Rotation locking motion = generator proven, then
  translation work starts. Rotation-neg winning = sign flip only.

## 57. Viewer forensics + stale-input verdict plan (M48)

- t1 red-edges = REAL depth. R24-as-SRV carries data in R only
  (G/B read 0 → red display); field black = reversed-Z far-skew
  (far ~0). cSTATS byte variance + silhouette tracing confirm
  signal. Implication: depth is nearly FLAT → NGX gets almost no
  disocclusion separation (supports the near/far direction, but a
  flat buffer caps what sliders can do).
- Velocity olive = CORRECT neutral (R=G=0.5, B=0 → olive while
  still). Game MVs exonerated again; shapes = slight motion/stale.
- t3 black: cannot hurt OUR DLSS (NGX keeps its own internal
  history; native t3/u1 are bypassed via Skip) — but unresolved
  whether stash artifact or engine-side. Settles via u0/u1 views.
- Better theory for GLOBAL shimmer (depth-range is edge-local):
  STALE input pick (stationary looks fine on stale data, motion
  fuses mismatched frames = full-frame shimmer). M48 logs per-feed
  resource identity + writer stamps over 4 frames (`ADIAG feed`):
  stable stamps == current frame exonerates the pick.
- M48 views: `MV feed (NGX input)` src10 (=srv_mvs_conv: convert
  OR own-rot output — generator validation) + `DepthN` channel
  (1-R near-white for reversed-Z). DrawData audit: frame_index IS
  fed+advancing; exposure null+auto (overlay says OFF — open how
  auto maps); vert_fov hardcoded 60deg vs proven 39.4 (suspect #2,
  untouched: game FOV may vary).

## 58. Full-diagnosis top-up (M49, no behavior change)

- ADIAG start gains `aexp=` (exposure correlation) + `own=`/`p=`
  (generator derivation tracking: p drift = in-game FOV change).
- cSTATS gains `t3-hist` + `u1-hist` CONTENT (settles black-t3
  numerically in the next log — no extra run).
- Compute CB0 dump 32→64 floats (params may live deep).
- MSCAN sniffs the FULL CB (was first 256B): tracker is now
  [4]x[32] offsets, so per-frame-varying data deeper in the 2048B
  CBs (camera translation candidate for 6DOF v2) gets first/last/
  distinct LOC lines for free. Entry table stays 256 (LOC is the
  output that matters). Zero behavior change: same throttle/span.

## 59. fmt35 is a mask, not velocity + stash poisoning fixed (M50)

- User verdict (viewer, live): the "velocity" buffer stays YELLOW
  while moving -- object shapes (character/bike/foliage), no
  motion response. It is a static object MASK, not motion vectors.
  Fits every old anomaly at once: X~=Y forever, means below any
  center, all decode sweeps void (scaling a mask changes nothing),
  Off-vs-Rotation mystery (BOTH feeds broken differently: mask =
  constant wrong offset, rotation-only = zero translation).
- Consequence: game-MV decodes are DEAD as a fix path (keep as
  fallback only). Translation now has NO source except full own-MVs
  (m49 deep-sniff data locates it). Rotation-only stays the
  pan-test diagnostic.
- Second bug from the same report: the stash takes the FIRST
  qualifying draw, which can be a SHADOW/other view (also
  rotation-shaped) -- own-rot fed garbage. M50 continuity gate:
  reject jumps >0.8 vs stashed R (camera motion is continuous),
  re-anchor after 5 straight rejects (cuts/teleports); `rej=`
  counter in the menu. If stash ages grow instead, first-draws are
  shadows and the fix becomes last-draw-wins.

## 60. Forward-move data: translation rows live, v2 ships (M51)

- m50 ADIAG-with-riding: T rows animate. `+640` row3 w=1 drifts
  cm-scale (17u idle) with an EXACT negated twin at `+896` row0
  (same dmax) = engine's own current-frame temporal pair;
  `+832` rows repeat the same T. `+64/+768/+1024` rows drift
  46-12587u with near baked in = shadow/other-camera junk
  (also explains M50 poisoning). Stash already used the right
  block; v2 extends it to 768B (+640 row3 at floats 172-175).
- P at `+384` + Pinv at `+448` re-confirmed static full-span;
  `+1088` row3 has 241 distinct SUB-precision variations (possible
  micro-jitter, watching, not actionable); CB0 deep half =
  flags/identity padding; `+1792..` bind NaN garbage (avoid).
- t3/u1 CONTENT healthy (means ~128, not black) — viewer artifact
  closed. Feed: own-rot live x4, depth stamp fresh, color via-copy
  (stamp blind spot), p rock-solid, resets=2.
- M51: 768B stash, 192B CB (+Tcur/Tprev/Proj/Res/Near=10cm TRUE),
  OwnMV gains FULLMODE 1 (V-row T: world=Rt*v+C, C=-(Rt*t)) and
  2 (campos T), R24 depth sampled (reversed-infinite z=Near/d,
  sky/invalid falls back to rotation per-pixel), X3206 muls
  replaced by explicit row-vector helpers. Menu 5 modes;
  Full-in-showcase falls back to game MVs (compute only).
- Protocol (ride, not pan): Off vs Rot(/Neg) vs FullA vs FullB.
  Sign transfers across modes (shared NDC math); A-vs-B settles
  T semantics. Depth-slider + autoexp verdicts still open.

## 61. Full-B wins: ghosting gone, two issues left (M52 tuning)

- User: Full-B very close to working. Ghosting GONE = MVs
  validated end to end (rotation + translation-campos form +
  sign + scale). Remaining: (1) image slightly darker with DLSS
  on (since day one); (2) motion blur like native TAA, no ghost.
- Darkness suspects, in order: (a) autoexposure (null exposure +
  auto ON since day one; overlay says OFF -- mapping open);
  (b) scene alpha (NGX alpha vs native opaque -- M22 force-opaque
  test); (c) output encoding (linear into sRGB-expecting u0 --
  old M25 verdict may have been confounded, revisit last).
- Blur suspects: FIRST in-game Motion Blur setting (UE4 post
  effect, blurs regardless of DLSS -- free check); then DLSS
  accumulation softness (jitter/mip/preset, one at a time).
  Note: native TAAU is itself a 4-tap+history softener, so
  "like TAA" blur may be partly the reference looking soft too.

## 62. Depth comes from slot t1 (fresh) + leak audit + M52 batch

- c_depth = slot t1 SRV resource DIRECTLY (not the cache) -- fresh
  by construction. So NGX depth cannot be stale; the ghosting seen
  in the depth-cache VIEW is the cache/stash, not the feed. The
  color@0 feed stamps were a tracking blind spot (HDR written via
  copy): M52 stamps copy destinations too.
- Darkness: autoexp OFF + force-opaque do nothing, sRGB OVERcorrects
  (bright/washed). Bracketed: plain dark, sRGB bright -- neither end
  right. M52 logs `dlss-out` means next to t2: ratio < 1 = NGX
  darkens (exposure), ~= 1 = output/downstream does it.
- Ghosting-reduced-not-gone + depth-view trails: with depth fresh
  and MVs right, remainder = depth RANGE (linearization) for edge
  rejection. User's depth-slider verdict still open.
- RGB toggle: channel combo MOVED to the Viewer header (was buried
  in pixel-legacy; honors all views, both viewers).
- LEAK AUDIT (all 30 GetResource sites script-checked): every one
  releases or attaches into ComPtr (put/attach release properly per
  upstream com_ptr.h). All Create* go to ComPtr or reset-guarded
  members; staging is reused; textures resize-gated; histograms
  flush; writer maps bounded. ONE real leak found+fixed:
  g_mv_by_hash grew unbounded (stale entries pin fullscreen
  textures across scenes) --> pruned to last 600 presents in
  OnPresent (readers treat missing as stale already).

## 63. 50% render scale kills the compute slot (M53 auto-unpark)

- draws.log at 50% scale: NO 242D9D62 for thousands of frames +
  PS-unresolved 3x normal. The game abandons the compute TAA and
  switches to its TAAU upscale resolve path. Our M11 slot is dead
  there -- and the M16 park rule ALSO held M7 down whenever the
  M11 master was ticked. Net: DLSS fully idle at reduced scale.
- M53: park becomes silence-gated. Compute fire stamps a heartbeat
  (g_cdlss_last_fire); M7 engages after 30 silent presents and
  re-parks on the next compute fire -- the two can never fight over
  the shared NGX instance (the thing M16 feared). Side effect and
  improvement: showcase DLSS now works with both masters on too.
- Open: whether the 50% TAAU resolve uses hash 000573B8 (found at
  130%). If it fires, M7 runs (watch DLSS attempts pixel counter
  + the M53 unpark log line). If a different hash, the freezer +
  "Log slot dims" identify it next.

## 65. Attempts-1680/runs-0 cause: 1000px gates (M55)

- m54 menu proved the state live: UNPARKED but pixel attempts 1680
  / runs 0. Root cause: EIGHT `>= 1000` fullscreen gates (color
  pick, MV/depth/HDR caches, compute inputs, viewer blits). At 50%
  (960 wide) every one rejects -- caches starve, color pick fails,
  viewer skips. M55 lowers all to 400x200.
- M-switch crash: Event Viewer shows the SAME fail-fast bucket in
  EVERY session since Sept (exit-time teardown, pre-mod era too --
  dozens of reports). Not preset-specific evidence. If M/L crash
  repeatably mid-game while K/J never do, it's NGX gen2 re-init
  (stay on K: also the correct DLAA preset). If random, it's the
  chronic game instability -- ignore.
- Note: game updated itself 1.25.403 --> 1.25.679 (updates/).

## 64. Attempts-0 cause: m52 installed, park still active (M54 livediag)

- User's marker read m52 while m53 (auto-unpark) sat uninstalled:
  with the M11 master ticked, the old park held M7 down and the
  compute slot is dead at 50% scale, so both paths idled. No code
  defect beyond "install the latest". Lesson: the menu must SHOW
  path state, not just counters.
- M54 extends Live Diagnostics with everything: M7 PARKED/UNPARKED
  + compute-silence age, full feed-settings mirror (decode/src/
  own/scale/inv/jitmode/mvsjit/zero/aexp), depth range, UI combo
  states, depth + HDR cache ages, own-MV stash mirror with
  rejects. All data = zero GPU work, all atomics.

## 66. M56: HDR+plain proven, M7 temporal root cause + merge (this build)

- User: `HDR color feed + PLAIN output` = good image, still temporally
  unstable on movement. Both defaults flipped ON (`g_prefer_hdr`,
  `g_plain_output`).
- Root cause of the shimmer: M7 fed the fmt35 cache RAW while M11
  decodes it via MVConvert to zero-centered pixels (same scale 1.0).
  Raw 0.5-center = constant full-screen motion = stable stationary,
  chaos on motion. M56 ports the whole M11 MV stack into M7:
  ownMV 1-4 (rot + fullA/B with depth), named/auto selector
  (`g_mv_src_mode`), decode variants (`g_mv_decode`). Scale/inv/
  jitter/near/far/preset/autoexp were already shared globals.
- Views for the hunt: pixel viewer gains `HDR` (cached HDR = NGX
  color when preferred) + `MVFEED` (`srv_mvs_conv` = exact NGX MV
  input, own or decoded); compute viewer already had both. New
  one-shot `Log M7 feed identity` mirrors ADIAG feed (color/depth/
  MV handles + writer stamps + hdr flag).
- Protocol (50% scale, ride/pan, HDR+PLAIN on): views OFF vs HDR
  vs MVFEED vs DLSS_OUT (MVFEED must sit mid-gray, motion edges
  only); `Log M7 feed identity` while moving (stamps must equal
  current frame); then decode 8 vs 1/3/6, own FullB vs game MVs.

## 67. M57: blur at 100% too — full-input audit (this build)

- Blur/shimmer persists at 100% (M11 path), so it was never a
  50%-only bug. Fix-by-toggle is exhausted; M57 instruments EVERY
  DLSS input instead: new `Audit DLSS inputs (10 feeds)` button
  logs 10 consecutive feeds on BOTH paths via one shared sequence
  (`LogAuditFeed`): path, seq, frame, fidx, reset, render->out,
  hdr/inv/mvsjit/aexp, preset, dec/src/own/msc, jitter fed,
  near/far/fov, color/depth/MV descs + writer stamps + handle
  change flags (`*`=changed, `!`=fidx skip), ok.
- Closed gaps found while mapping: M7 had no reset counter (now
  `g_dlss_resets`, shown in Live Diagnostics), M7 output wrote
  alpha 0 while M11 preserves (now `keep_alpha=true` both), FOV
  was hardcoded 60deg=1.047 while the view proof says ~39.4deg=
  0.688 (now shared `g_vert_fov` slider, both paths), M7 had no
  signal-stats equivalent (new `Stats: M7 pixel inputs`).
- Verdict rules for the 10-line audit (move + pan while it runs):
  fidx must advance by 1 each line (`!` = stuck counter = no
  temporality, stop here); resets must stay ~1 total (climbing
  with runs = history nuked); jitter fed must vary while moving
  (constant 0,0 with animated CB = reprojection can never align);
  color/depth/MV stamps must equal current frame (`*` flapping
  every line = ping-pong/wrong pick); mvfmt must not flip
  mid-run; `*` on all three handles at once = path switch
  (M7<->M11 fight over the shared NGX instance).
- Follow-up runs, in order: (1) audit at 100% while riding +
  `Stats: compute inputs` still vs moving (MV mean ~0 still,
  variance moving; zero-variance = mask, not MVs); (2) same at
  50% + `Stats: M7 pixel inputs`; (3) FOV 1.047 vs 0.688 A/B;
  (4) depth `DepthN` channel view + near/far sweep (flat depth =
  no disocclusion separation, edge blur stays).

## 68. M58: t1 is accumulation, not depth — depth-source selector

- User (viewer): slot t1 shows TAA accumulation, not depth.
  Correct call: DLSS `depth_buffer` must be depth. Feeding color/
  history as depth breaks NGX linearization + edge rejection and
  smears the whole frame — fits the 100% blur exactly.
- Why it hid: M11 took `csrvs[1]` blindly as depth with no format
  check (`cinputs_ok` only validates color fmt10). The old
  "t1 red-edges = depth" verdict (M48) was mode-specific; bindings
  evidently differ by mode/resolution.
- Fix: `g_cdepth_src` (Motion tuning, shared): 0 Auto = cached
  DSV when fresh this frame else slot t1 (default), 1 slot t1,
  2 cache-DSV only. The cache is filled from the OM DSV at a
  fullscreen draw, so it is depth by construction. Own-FullB
  unprojection already used the cache; only the NGX feed was
  suspect. First-Draw + `AUDIT` lines now carry `dsrc=`.
- Protocol: `Viewer: TAA t1` vs `Depth cache` (channel ALL, then
  DepthN). Silhouette = depth; scene content = accumulation.
  Then ride each `dsrc` mode once (Auto first). Paste the 10
  `AUDIT` lines — `depth=(...)` + `dsrc=` settles it.

## 69. M59: viewer shows the exact NGX feeds, named (this build)

- Old viewers showed slot bindings / caches, never the resolved
  NGX inputs (M11 color = slot t2 vs M7 HDR-or-unpack? depth =
  slot t1 vs cache-DSV? MV = zero/cache/decoded/own?). The
  shimmer hunt kept comparing the wrong buffers.
- Now both Draw sites `StoreFeed()` the four resources DLSS got
  (color/depth/MV/out per path, frame-stamped). `Viewer → DLSS
  feed (exact NGX input)`: `M11 color/depth/MV/out`, `M7 color/
  depth/MV/out`, with per-path age + `WxH fmt` descs. Backend
  reuses the persistent blit (slot viewers OFF while using it;
  >30-frame-old feed shows nothing instead of a stale lie).
- Protocol: ride, step through all 8 while moving. MV = mid-gray
  + motion edges (black/flat = dead feed); depth = silhouette
  (use DepthN channel; scene content = accumulation = switch
  `M11 depth source` to cache); color = scene RGB (cyan = pair
  instead of HDR); output = DLSS result (compare vs OFF).