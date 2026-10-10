// Days Gone Luma mod ??" M2 freezer (see Todolist.txt, milestone M2).
//
// Goal: Luma loads into DaysGone.exe (DX11 / UE 4.10 Bend), overlay + logging work,
// ZERO rendering modifications yet. Custom TAA work builds on top of this file.
//
// Notes:
// - Days Gone TAA is NOT stock UE4 TAA (custom Bend pass, renders scene+UI path).
//   Generic Luma-Unreal finds candidates (0xB146BBAE etc) but never promotes to
//   confirmed TAA, so DLSS never runs. This project forks the UE logic for 4.10.
// - NO motion-blur slot (per user request). DLSS will replace the TAA draw
//   directly, with a HUD-preserving path (see g_taa_disable_keep_hud).
// - M1: UseLumaNGX=false (avoid 1114 loader error). Re-enable at M7.

#define GAME_DAYS_GONE 1

#define GEOMETRY_SHADER_SUPPORT 0

// Upstream pattern (Unreal Engine/main.cpp:5): skip non-D3D11 command lists
// before core.hpp so OnInitCommandList never QIs a foreign/proxy list.
#define CHECK_GRAPHICS_API_COMPATIBILITY 1

#include "..\..\Core\core.hpp"

#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Hotkeys (#define-configurable here): F9 toggles the M11 DLSS master,
// F10 starts a full-trace (same as the Tools-menu button). Edge-triggered
// per present in OnPresent; every action mirrors to ReShade.log.
#define DG_HOTKEY_DLSS VK_F9
#define DG_HOTKEY_TRACE VK_F10

static const char* DG_BUILD_ID = "m55-2026-10-05-smallgates-viewfix-fulltrace-apifix2-zoomfix-viewscale-viewpick-perfcap-templog-crashfix-pics-allpasses-fullbundle-mvsrc-blitguard-bundlehang-shakediag-projhold-fullscale-hotkeys-viewfilter-depthfilter-mvscale2x-hybrid-menuclean-mvraw-tracefix-cleanup";

static std::atomic<uint64_t> g_draws_this_frame{ 0 };
static std::atomic<uint64_t> g_draws_last_frame{ 0 };
static std::atomic<uint64_t> g_hist_frame{ 0 };

static HMODULE g_marker_module = nullptr;
static void WriteLoadMarker(HMODULE hModule, const char* phase);
static void WriteFullLine(HMODULE hModule, uint64_t present);

// Histograms + freezer work in ALL configs (M5b: Publishing showed an empty
// menu because publishing never populated them). Only file logging stays
// TEST/DEVELOPMENT-gated.
static std::mutex g_hist_mutex;
static std::unordered_map<uint32_t, uint64_t> g_ps_hist;
static std::unordered_map<uint32_t, uint64_t> g_cs_hist;
static std::atomic<uint64_t> g_ps_empty{ 0 };      // draws with no pixel shader (depth-only etc)
static std::atomic<uint64_t> g_ps_unresolved{ 0 }; // draws hashed FFFFFFFF (not yet dumped)
#if TEST || DEVELOPMENT
static void WriteDrawHistogram(HMODULE hModule);
// M4: 3-frame I/O capture. While armed, every pixel draw logs its output +
// input sizes/formats: cap <frame> PS <hash> RT:<WxH>:<fmt> D:<.../null>
// t0:<.../null> t1:.. t2:.. t3:.. ??" TAA is the fullscreen pass reading 2x
// HDR color + depth + velocity at output res. Raw COM, everything Released.
static std::atomic<int> g_cap_frames_left{ 0 };
static void WriteCapLine(HMODULE hModule, uint64_t frame, uint32_t ps, ID3D11DeviceContext* ctx);
static void WriteCapCSLine(HMODULE hModule, uint64_t frame, uint32_t cs, ID3D11DeviceContext* ctx);
static void CopyTexDesc(uint64_t handle, char* out, size_t out_size);
static void WriteCopyLine(HMODULE hModule, uint64_t dst, uint64_t src, const char* dd, const char* sd);
#endif

// Pictures: one-shot backbuffer BMP saver (display/file-logging only).
// Armed by the Tools menu button; consumed in OnPresent. Counter lives in
// ALL configs so the ReShade.log mirror always works; the .bmp FILE write
// itself stays TEST||DEVELOPMENT-gated like every other file log.
static std::atomic<int> g_pic_frames_left{ 0 };
static void WritePicFile(HMODULE hModule, ID3D11Device* dev, uint64_t present);

// All-passes: round-robin BMP saver, one debug pass per present (~19
// presents per button press). Display/file-logging only: never touches NGX
// feeds, MV/depth/jitter state, or election logic. Slot bindings (t0-t3/u0-
// u1) are ref-stashed masterlessly at the compute slot while armed (same
// CSGet pattern as the viewer stash, no GPU work); caches/feeds resolve at
// Present from their existing stored refs. A missing source logs SKIP and
// advances -- never stalls, never force-stashes inside NGX paths.
struct PassSrc { int id; const char* tag; const char* name; };
static const PassSrc kPassSrcs[] = {
   { 0, "t0", "TAA t0 (HUD?)" }, { 1, "t1", "TAA t1 (depth)" },
   { 2, "t2", "TAA t2 (HDR color)" }, { 3, "t3", "TAA t3 (history)" },
   { 4, "u0", "TAA u0 (current)" }, { 5, "u1", "TAA u1 (hist scratch)" },
   { 6, "vel-cache", "Velocity cache" }, { 7, "depth-cache", "Depth cache" },
   { 8, "hdr-cache", "HDR cache" }, { 9, "dlss-out", "DLSS output" },
   { 10, "mv-feed", "MV feed (NGX input)" },
   { 100, "m11-color", "M11 color (NGX src)" }, { 101, "m11-depth", "M11 depth (NGX depth)" },
   { 102, "m11-mv", "M11 MV (NGX MV)" }, { 103, "m11-out", "M11 output (NGX out)" },
   { 104, "m7-color", "M7 color (NGX src)" }, { 105, "m7-depth", "M7 depth (NGX depth)" },
   { 106, "m7-mv", "M7 MV (NGX MV)" }, { 107, "m7-out", "M7 output (NGX out)" },
};
static const int kPassCount = 19;
static std::atomic<bool> g_pass_active{ false };
static std::atomic<int> g_pass_pos{ 0 };
static std::mutex g_pass_slot_mutex;
static ComPtr<ID3D11Resource> g_pass_slot[6];
static void WritePassOne(HMODULE hModule, ID3D11Device* dev, DeviceData& dd, uint64_t present);

// Full bundle: ONE button arming all four existing capture paths at once
// (wiring only -- each sub-path keeps its own counters, filenames, log
// lines, and auto-disarm rules). Done when every sub-path is done; then a
// single 'DaysGone BUNDLE done ...' summary mirrors to ReShade.log.
static std::atomic<bool> g_bundle_active{ false };
static std::atomic<int> g_bundle_pics{ 0 };
static std::atomic<int> g_bundle_passes{ 0 };
static std::atomic<int> g_bundle_cap{ 0 };
static std::atomic<int> g_bundle_trace{ 0 };

// M2 live pass freezer (Heat M4 pattern, no motion-blur slot here).
// The old hardcoded TAA hashes (B146BBAE etc from generic Luma-Unreal) never
// run in this build ??" draws.log proves it ??" so instead of guessing, the
// overlay publishes the top per-window hashes as skip checkboxes. Checking
// one cancels that draw live (Skip). All off = zero rendering changes.
// Kill the TAA resolve and you get instant aliasing/shimmer; kill lighting
// and light dies; UI draws use different hashes so HUD survives a scene-only
// skip ??" that's the HUD-preserving disable test.
struct FreezeCandidate { uint32_t hash; uint64_t count; bool is_compute; };
static std::mutex g_freeze_mutex;
static std::vector<FreezeCandidate> g_freeze_candidates;
static std::unordered_set<uint32_t> g_freeze_skips;
static std::atomic<bool> g_freeze_master{ false };
static std::atomic<uint64_t> g_frozen_skipped_this_frame{ 0 };
static std::atomic<uint64_t> g_frozen_skipped_last_frame{ 0 };
struct DaysGoneDeviceData : public GameDeviceData
{
#if ENABLE_SR
   // M7 DLSS first-light resources.
   ComPtr<ID3D11Texture2D> tex_dlss_zero_mvs; // R16G16F zero motion (M7: real MVs mapped at M8+)
   ComPtr<ID3D11ShaderResourceView> srv_dlss_zero_mvs;
   uint32_t zero_mvs_w = 0, zero_mvs_h = 0;
   // M7b: per-frame depth cache -- the TAAU slot binds no depth itself, but
   // the HDR post family does (full-res R24G8 DSV). Latest seen wins.
   ComPtr<ID3D11Resource> cached_depth;
   uint64_t cached_depth_frame = 0;
   // M8: per-frame velocity cache. Producers: 1E94CABC/5846E9DA/6C6AD505/
   // A1A256FA/A5CB30BF/BE0130E5 render the persistent 1920x1080 RG buffer
   // (fmt35 = R16G16_UNORM per UE convention); depth/motion CS consume it.
   ComPtr<ID3D11Resource> cached_mvs;
   uint64_t cached_mvs_frame = 0;
   uint32_t cached_mvs_hash = 0; // M26: which producer filled it
   // M9i: per-frame HDR color cache. The fmt24 pair is mode-dependent
   // (showcase t1 = luma-only); the R16G16B16A16F linear HDR buffer (fmt10,
   // cf. CS 8AEEB67D t1) is unambiguous scene color in every mode.
   ComPtr<ID3D11Resource> cached_hdr;
   uint32_t cached_hdr_w = 0, cached_hdr_h = 0;
   uint64_t cached_hdr_frame = 0;
   // M9: own DLSS output (RGBA16F + UAV) + SRV for the copy pass. Direct
   // NGX draws into the packed game RT wrote nothing readable (frozen).
   ComPtr<ID3D11Texture2D> tex_dlss_out;
   ComPtr<ID3D11ShaderResourceView> srv_dlss_out;
   uint32_t dlss_out_w = 0, dlss_out_h = 0;
   // M9b: own FP16 color input. NGX misreads packed fmt24 (red channel
   // lost -- cyan image); hardware converts UNORM10 -> float on load.
   ComPtr<ID3D11Texture2D> tex_color_in;
   ComPtr<ID3D11ShaderResourceView> srv_color_in;
   ComPtr<ID3D11RenderTargetView> rtv_color_in;
   uint32_t color_in_w = 0, color_in_h = 0;
   // M24: decoded MV target (fmt35 (v+1)*0.25 -> 0.5-centered via x2 pass).
   ComPtr<ID3D11Texture2D> tex_mvs_conv;
   ComPtr<ID3D11ShaderResourceView> srv_mvs_conv;
   ComPtr<ID3D11RenderTargetView> rtv_mvs_conv;
   uint32_t mvs_conv_w = 0, mvs_conv_h = 0;
   // Hybrid MVs (M11/compute only): game truth + own fill target. Create/
   // resize discipline mirrors tex_mvs_conv (R16G16F, render size).
   ComPtr<ID3D11Texture2D> tex_mvs_hybrid;
   ComPtr<ID3D11ShaderResourceView> srv_mvs_hybrid;
   ComPtr<ID3D11RenderTargetView> rtv_mvs_hybrid;
   uint32_t mvs_hybrid_w = 0, mvs_hybrid_h = 0;
   // M9e: no-scissor rasterizer for copy passes (game leaves blending,
   // 1920-viewport and scissor set -- inherited state blended our copies
   // with stale content = pink hues + unwritten stripes).
   ComPtr<ID3D11RasterizerState> rs_copy;
   // M10: staging copy of the slot's cb0 (jitter lives in cb0[0]).
   ComPtr<ID3D11Buffer> staging_cb;
   uint32_t staging_size = 0;
   bool first_dlss_frame = true;
   uint32_t dlss_runs = 0;
   bool logged_no_uav = false;
   bool logged_draw_result = false;
   bool logged_capture_fail = false;
    // M11 compute-TAA (CS 242D9D62) DLSS state.
    bool first_cdlss_frame = true;
    uint32_t cdlss_runs = 0;
    bool logged_cdraw_result = false;
    bool logged_ccapture_fail = false;
    bool logged_crtv_fail = false;
    // M47: own-MV view stash (VS CB1 2048B: VP at +0, R at +128).
    // M51: 768B (adds +640 translation row for full 6DOF).
    float view_cb_cur[192] = {};
    float view_cb_prev[192] = {};
    uint64_t view_frame_cur = 0;
    uint64_t view_frame_prev = 0;
    // viewpick: per-frame N-candidate pool + election state. First-wins
    // locked onto non-gameplay views at some angles (projection jumps while
    // rotation stays continuous); capture pools, first use-point elects.
    struct ViewCand { float cb[192]; float p00; float p11; int hits; };
    ViewCand view_pool[4] = {};
    int view_npool = 0;
    int view_pool_reads = 0; // perfcap: staging reads this frame (cap 8)
    uint64_t view_pool_frame = 0;
    uint64_t view_elect_frame = 0; // election once-per-frame guard
    float view_c_p00 = 0.0f, view_c_p11 = 0.0f; // committed projection
    int view_pick = -1;   // last elected pool index
    int view_pick_n = 0;  // pool size at last election
    int view_pick_run = 0; // consecutive challenger wins (hysteresis)
    int view_reanchor_run = 0; // straight frames with zero jd passers (cut)
    float view_chal_p00 = 0.0f, view_chal_p11 = 0.0f; // challenger signature
    float view_chal_r[16] = {}; // challenger R signature
    // projhold: cross-frame projection-hold escape state. While a held
    // challenger projection stays stable, count straight frames; re-anchor
    // after 5 (same 5-count style as the rotation re-anchor). Oscillation
    // never stabilizes, so it never re-anchors; real zoom/cuts do.
    float view_hold_p00 = 0.0f, view_hold_p11 = 0.0f;
    int view_hold_run = 0;
    ComPtr<ID3D11Buffer> cb_ownmv; // M47 144B / M51 192B / DeltaC 208B (R+T+Proj+Res+Near+dC)
    ComPtr<ID3D11Buffer> cb_viewscale; // viewer TargetWH (16B, display-only stretch-to-fill)
#endif
};

// M7 first light: DLSS Super Resolution AT the TAAU resolve (PS 000573B8).
// Proven slot: sole 1920x1080 -> 2560x1440 temporal upscaler, ping-pong
// history inputs, 1/frame (see RESEARCH.md section 5).
// M7 assumptions (expect artifacts ??" fixed at M8+): zero motion vectors,
// jitter 0 (game IS jittered, mapped at M8), fmt24 treated as LDR (hdr=false),
// near/far guesses. Direct-draw into the game's own RT (no copy CS yet) ??"
// requires its UAV bind; otherwise bails to native with a log line.
static const uint32_t kDlssSlotPS = 0x000573B8;
static std::atomic<bool> g_dlss_master{ false }; // default OFF
static std::atomic<uint64_t> g_dlss_attempts{ 0 };
static std::atomic<uint64_t> g_dlss_runs{ 0 };
// M7c: last-writer map (RTV handle -> present index). The slot's t1/t2
// ping-pong swaps roles every frame; feeding them alternately = 30Hz
// red/blue flash. Color = the candidate WRITTEN this frame (current).
// Master-gated (costs one OM call per draw while testing).
static std::mutex g_writer_mutex;
static std::unordered_map<uint64_t, uint64_t> g_rt_last_writer;
static std::atomic<bool> g_dlss_hdr{ false }; // live-toggled (fmt24 = LDR?)
static std::atomic<bool> g_dlss_autoexp{ true };
// M9g: input viewer. -1=off, 0-7=slot t-slot, 8=cached depth,
// 9=cached MVs, 10=unpacked FP16 color, 11=DLSS output. Shows each feed
// fullscreen so a pink source is caught red-handed instead of guessed at.
// M9h: channel filter 0=R,1=G,2=B,3=A,4=all(sRGB).
static std::atomic<int> g_view_src{ -1 };
static std::atomic<int> g_view_chan{ 4 };

// M9c: R/B swizzle switches (game stores BGR-ordered data somewhere).
static std::atomic<bool> g_swap_unpack{ false };
static std::atomic<bool> g_swap_output{ false };
// M10c: UI triage. Pair (writer-map, may carry UI) primary again; HDR cache
// only when preferred. has_drawn_sr mark suppressible (core may alter
// downstream passes when it sees SR-engaged).
// M56: default ON (user-proven at 50% scale: fmt24 pair is luma-only there,
// HDR cache is the real scene RGB; BYPASS stays OFF).
static std::atomic<bool> g_prefer_hdr{ true };
static std::atomic<bool> g_mark_sr{ true };
// M10k: upscale-only guard (M10 regression fix). M10 hard-blocked
// render==output, which is exactly DLAA (the M7 goal: "render==output, no
// resize") -- i.e. normal 100%-scale gameplay -- so the M7 toggle became a
// no-op on the latest build. Default OFF (DLAA allowed); menu/map UI modes
// at render==output are handled by the t0 composite. Tick to restore the
// strict upscale-only behavior.
static std::atomic<bool> g_require_upscale{ false };
// M10d: UI is t0 (user report). Composite it back over the DLSS result --
// size-gated (fullscreen only, so a 1x1 params t0 can never whitewash).
// Viewer decoupled from SR type (works with sr_type None) + one-shot slot
// logger (t0/t1 WxH:fmt) so the next report has dims, not just "no UI".
static std::atomic<bool> g_composite_ui{ true };
static std::atomic<bool> g_log_slot{ false };
// M56: one-shot M7 feed-identity log (which color/depth/MV resources reach
// NGX + writer stamps; mirrors ADIAG feed for the compute path).
static std::atomic<bool> g_m7feed{ false };
// M10f: full CB dumper -- logs cb0[0..63] raw floats so we can find the real
// jitter (cb0[0]/[1] = dither offset, not jitter). One-shot, like g_log_slot.
static std::atomic<bool> g_dump_cb{ false };
// M10h: blend mode selector for the UI composite. Lets the user pick the
// best-looking blend live. 0=premult(ONE/INV_SRC_ALPHA), 1=straight
// (SRC_ALPHA/INV_SRC_ALPHA), 2=additive(ONE/ONE), 3=multiplicative
// (ZERO/SRC_COLOR).
static std::atomic<int> g_blend_mode{ 0 };
// M10i flicker isolates: zero MV / no depth / jitter off.
static std::atomic<bool> g_zero_mv{ false };
static std::atomic<bool> g_no_depth{ false };
static std::atomic<bool> g_jitter_off{ false };
// M17: UI tonemap mode for the t0 HUD blend (shared pixel + compute paths).
// 0 = none (plain copy), 1 = Reinhard + sRGB (old default), 2 = ACES + sRGB,
// 3 = Hable filmic + sRGB, 4 = sRGB-decode only (old M10i, no tonemap).
// M18: 5 = sRGB-encode only (no tonemap, no decode) -- covers the case where
// the HUD buffer is already LINEAR (encode-only then matches dst encoding
// without any curve shifting contrast/hue).
// M21: default 5 + premult blend 0 (user-proven winning combo).
static std::atomic<int> g_ui_tonemap_mode{ 5 };
// M9j: plain (non-sRGB) output toggle. If plain matches native, the input
// was already sRGB-encoded (double encode = wash).
// M56: default ON (user-proven with HDR feed at 50%: sRGB encode overbrightens).
static std::atomic<bool> g_plain_output{ true };
static std::atomic<bool> g_dlss_bypass{ false };
// M10: game jitter feed (staging readback of slot cb0). Off by default
// until the mapping proves itself live.
static std::atomic<bool> g_jitter_on{ false };
static std::atomic<bool> g_jit_flip_x{ false };
static std::atomic<bool> g_jit_flip_y{ true };
static std::atomic<float> g_jit_rawx{ 0.0f }, g_jit_rawy{ 0.0f };
static std::atomic<float> g_jit_pxx{ 0.0f }, g_jit_pxy{ 0.0f };
// M11 jitter probe (log-only): snapshot of the compute-path CB0 read every
// dispatch. Separate atoms -- do NOT reuse g_jit_rawx/y or g_jit_pxx/y
// (aliased with the M7 pixel path).
static std::atomic<float> g_jitdbg_00{ 0.0f }, g_jitdbg_01{ 0.0f };
static std::atomic<float> g_jitdbg_26{ 0.0f }, g_jitdbg_27{ 0.0f };
static std::atomic<float> g_jitdbg_cjit{ 0.0f }, g_jitdbg_cjit_y{ 0.0f };
// M7e: color freshness proof -- handle fed last run + change count. Changes==0
// across runs while the scene moves = stale fallback input (frozen screen).
static std::atomic<uint64_t> g_last_color_handle{ 0 };
static std::atomic<uint64_t> g_color_changes{ 0 };

// M11 compute gameplay TAA (CS 242D9D62). The pixel TAAU (000573B8) only runs
// in showcase/upscale modes; real gameplay at 1920x1080 resolves here:
// 1/frame, u0 = current (stable handle), u1/t3 = ping-pong history,
// t0 = fmt27 composite (multi-writer, NOT used), t1 = depth fmt42,
// t2 = fmt10 linear HDR scene color. Proved by M10k frame.log capture.
static const uint32_t kDlssComputeSlotCS = 0x242D9D62;
static std::atomic<bool> g_cdlss_master{ false }; // default OFF
static std::atomic<uint64_t> g_cdlss_attempts{ 0 };
static std::atomic<uint64_t> g_cdlss_runs{ 0 };
// M53: last present with a compute-slot fire (auto-unpark: the M7 pixel
// path engages when the compute slot goes silent, e.g. reduced render
// scale switches the game to its TAAU upscale resolve).
static std::atomic<uint64_t> g_cdlss_last_fire{ 0 };
// M16: how many executed DLSS frames carried reset=true. Resets climbing WITH
// runs = NGX history is nuked continuously = nothing can ever look temporal.
static std::atomic<uint64_t> g_cdlss_resets{ 0 };
// M36: live feed/fail counters + last-frame snapshot (menu diagnostics --
// zero GPU work, all atomics). real-vs-zero quantifies "running blind";
// fail counters split the two native-fallback causes; the snapshot keeps
// the first-Draw info live across scene/mode changes (first-Draw logs once
// and goes stale when showcase<->gameplay flips mid-session).
static std::atomic<uint64_t> g_cmv_frames_real{ 0 };
static std::atomic<uint64_t> g_cmv_frames_zero{ 0 };
static std::atomic<uint64_t> g_cfail_capture{ 0 };
static std::atomic<uint64_t> g_cfail_rtv{ 0 };
static std::atomic<uint64_t> g_cfail_draw{ 0 }; // M63: NGX Draw ok=0 count
static std::atomic<int> g_clast_rw{ 0 }, g_clast_rh{ 0 }, g_clast_ow{ 0 }, g_clast_oh{ 0 };
static std::atomic<int> g_clast_mvfmt{ -1 }, g_clast_code{ 0 };
static std::atomic<uint32_t> g_clast_mvhash{ 0 };
static std::atomic<int> g_clast_mvreal{ 0 }, g_clast_reset{ 0 }, g_clast_ok{ 0 };
// FED MV identity (log-only): which MV buffer NGX actually ate this present.
// g_clast_code already holds c_mv_code; mvcode mirrors it for the FULL line,
// mvsrc holds the c_mv_src label, mvselframe the lookup frame (-1 = no pick).
static std::atomic<int> g_clast_mvcode{ 0 };
static std::atomic<long long> g_clast_mvselframe{ -1 };
static char g_clast_mvsrc[32] = {};
// Own-MV shake diagnostics (log-only): guard reason at the own use-points
// (0=own-fed-ok, 1=election-hold, 2=prev/cur pairing fail, 3=projection shape
// reject, 4=own-mode-off, 5=fell back to game MVs, 6=fell back to zero) plus
// the M11 pairing triple (view_frame_cur/prev + oframe).
static std::atomic<int> g_clast_ownguard{ 4 };
static std::atomic<unsigned long long> g_clast_vfcur{ 0 }, g_clast_vfprev{ 0 }, g_clast_oframe{ 0 };
static std::atomic<float> g_clast_jitx{ 0.0f }, g_clast_jity{ 0.0f };
// M11 one-shot compute-slot tools (masterless): I/O logger + CB0 dumper.
static std::atomic<bool> g_clog_slot{ false };
static std::atomic<bool> g_cdump_cb{ false };
// M12/M23: compute-TAA jitter feed from CB0[0]/[1] (pixels, CB-dump proven).
// M25: default OFF again -- user proved ON softens/blurs the image ([0]/[1]
// are animated but apparently not the reprojection jitter, or need
// sign/scale work). Toggle stays for experiments.
static std::atomic<bool> g_cjitter_on{ true };
// M13: composite the compute slot's t0 HUD over the DLSS result (user report:
// HUD lives in t0; unticking the SR mark did not bring it back). Size-gated
// inside RunUIComposite, same as the pixel path. Default ON.
static std::atomic<bool> g_ccomposite_ui{ true };
// M14: MV source for the compute path. Default = fmt35 velocity cache (raw).
// Alternative = the slot's own t0 (fmt27 multi-writer composite -- possibly
// the PROCESSED velocity the native TAA consumes; the fmt35 may only feed
// the depth/motion computes). Experimental: A/B live while panning.
static std::atomic<bool> g_cmv_from_t0{ false };
// M15: combination matrix for motion shimmer + wrong UI composite. MV scale:
// M30: 0 = 1.0 pass-through (UPSTREAM PARITY -- MV texture already holds
// pixels, NVIDIA guide: "1.0 if motion vectors do not need to be scaled"),
// 1 = 0.5*res (legacy M14 proven-wrong double-scale, kept for A/B),
// 2 = 0.5. Jitter scale: 0 = x1, 1 = x2 (NDC convention),
// 2 = x0.5. Inverted depth for the linearizer. Compute UI source: t-slot (or
// OFF) composited over the DLSS result -- finds the real HUD buffer live.
static std::atomic<int> g_mv_scale_mode{ 0 };
static std::atomic<int> g_jit_scale_mode{ 0 };
// M30: default TRUE (upstream Unreal parity -- was FALSE since M15, never
// confirmed tested; UE depth convention needs it).
static std::atomic<bool> g_inverted_depth{ true };
static std::atomic<int> g_cui_src{ 0 };
// M58: M11 depth source. 0 auto (cached DSV when fresh this frame, else slot
// t1), 1 slot t1 only, 2 cached DSV only. Slot t1 was assumed depth fmt42,
// but if the viewer shows scene content there it is accumulation, and feeding
// accumulation as depth destroys NGX disocclusion (full-frame smear).
static std::atomic<int> g_cdepth_src{ 0 };
// M20: sRGB-encode the compute scene copy to u0 (native u0 carries the TAA's
// sRGB tail -- bytecode proof: 0.41667/1.055 immediates + TAAU precedent; u1
// history stays LINEAR, written pre-tail). Default ON.
// M22: scene-copy alpha: 0 = keep NGX alpha, 1 = force opaque. If our u0.A
// comes back < 1 (NGX alpha) while native was opaque, every downstream alpha
// use darkens translucent gradient zones. Forcing opaque also makes the HUD
// composite result opaque (A = sA + 1-sA = 1).
// M25: default OFF (plain linear) -- user proved sRGB output wrong (the
// sRGB-tail immediates must serve another branch, not u0).
static std::atomic<bool> g_c_srgb_output{ false };
static std::atomic<int> g_c_alpha_mode{ 0 };
// M19: game MVs already contain jitter (Bend custom vs UE convention)?
static std::atomic<bool> g_mvs_jittered{ false };
// M24/M30: MV decode mode. 0 raw (diagnostic), 1 upstream UE 0.5-centered
// (bytecode-proven), 2 = 0.25-centered fallback (A/B only),
// 3 = upstream + zero-snap for cleared bg (M32), 4 = negated upstream
// (sign triage, M32 -- verdict INVALID without snap), 5 = zero-snap
// + 0.25-centered (M33 -- stationary-BAD verdict = bulk is 0.5-centered),
// 6 = negated + zero-snap (M34), 7 = Y-neg + snap, 8 = X-neg + snap
// (M37: full sign matrix done -- 8 shimmers least, now the DEFAULT).
static std::atomic<int> g_mv_decode{ 8 };
// M47: own rotation-exact camera MVs (0 off, 1 rotation, 2 rot-neg).
// M51: + full 6DOF (3 Full-A V-row T, 4 Full-B campos T). Stash-fed.
static std::atomic<int> g_ownmv{ 0 };
// M49: last own-MV derivation (for log correlation: p drift = FOV change).
static std::atomic<float> g_own_p00{ 0.0f }, g_own_p11{ 0.0f };
static std::atomic<int> g_own_used{ 0 };
// M50: view-stash continuity (first qualifying draw may be a shadow/other
// view -- also rotation-shaped). Rejects + consecutive-reject run (cut
// recovery: re-anchor after 5 straight rejects, e.g. teleports/cuts).
static std::atomic<uint64_t> g_view_rejects{ 0 };
static std::atomic<int> g_view_rejrun{ 0 };
// viewpick: projection-stability rejections (election drop path only).
static std::atomic<uint64_t> g_view_projrej{ 0 };
// viewfilter: small-RT draws refused pooling (log-only counter) + per-present
// snapshots (pool reads, skips) + FNV-1a of the elected R block.
static std::atomic<uint64_t> g_view_pool_skips{ 0 };
// depthfilter: large DSVs refused caching when no gameplay RT is alongside.
static std::atomic<uint64_t> g_depth_cache_skips{ 0 };
static std::atomic<int> g_clast_preads{ 0 };
static std::atomic<unsigned long long> g_clast_pskip{ 0 };
static std::atomic<uint32_t> g_view_pick_cksum{ 0 };
// templog: FULL/FULLAGG snapshot + accumulator atoms (log-only, no behavior).
static std::atomic<int> g_view_pick_snap{ -1 };
static std::atomic<int> g_view_npool_snap{ 0 };
static std::atomic<float> g_view_rotmag{ 0.0f };
static std::atomic<uint64_t> g_clast_dage{ 0 };
static std::atomic<uint64_t> g_agg_jitn{ 0 };
static std::atomic<int64_t> g_agg_jitsumx{ 0 }, g_agg_jitsumy{ 0 }; // milli-pixels
static std::atomic<uint64_t> g_agg_q00{ 0 }, g_agg_q01{ 0 }, g_agg_q10{ 0 }, g_agg_q11{ 0 };
static std::atomic<uint64_t> g_agg_rotn{ 0 };
static std::atomic<int64_t> g_agg_rotsum{ 0 }, g_agg_rotmax{ 0 }; // milli units
static std::atomic<uint64_t> g_agg_resets{ 0 };
static std::atomic<uint64_t> g_agg_projrej{ 0 };
// viewfilter: FNV-1a over the 64B R block (cb+32) identifying a pooled view.
static uint32_t ViewPickCksum(const float* cb)
{
   const uint8_t* b = (const uint8_t*)(cb + 32);
   uint32_t h = 2166136261u;
   for (int i = 0; i < 64; i++) { h ^= b[i]; h *= 16777619u; }
   return h;
}
// viewpick commit: legacy stash commit (ex-1933-1939) + election bookkeeping.
static void CommitViewPick(DaysGoneDeviceData& god, const float* src_cb, float p00, float p11, uint64_t frame, int pickidx)
{
   g_view_rejrun.store(0, std::memory_order_relaxed);
   if (god.view_frame_cur != 0)
   {
      memcpy(god.view_cb_prev, god.view_cb_cur, sizeof(god.view_cb_cur));
      god.view_frame_prev = god.view_frame_cur;
   }
   memcpy(god.view_cb_cur, src_cb, 768);
   god.view_frame_cur = frame;
   god.view_c_p00 = p00;
   god.view_c_p11 = p11;
   god.view_pick = pickidx;
   g_view_pick_snap.store(pickidx, std::memory_order_relaxed);
   g_view_npool_snap.store(god.view_pick_n, std::memory_order_relaxed);
   g_view_pick_cksum.store(ViewPickCksum(src_cb), std::memory_order_relaxed);
   g_own_p00.store(p00, std::memory_order_relaxed);
   g_own_p11.store(p11, std::memory_order_relaxed);
}
// viewpick: per-frame candidate election with projection-stability gate.
// Capture only pools; the first use-point per frame elects+commits.
static void ElectViewPick(DaysGoneDeviceData& god, uint64_t frame)
{
   if (god.view_elect_frame == frame) return;
   god.view_elect_frame = frame;
   if (god.view_frame_cur == frame) return; // already committed this frame
   if (god.view_pool_frame != frame || god.view_npool <= 0) return; // nothing pooled
   god.view_pick_n = god.view_npool;
   if (god.view_npool == 1 || god.view_frame_cur == 0)
   {
      // Fast path: single candidate (or no incumbent): max-hits legacy commit.
      int best = 0;
      for (int i = 1; i < god.view_npool; i++)
         if (god.view_pool[i].hits > god.view_pool[best].hits) best = i;
      const auto& w = god.view_pool[best];
      if (god.view_frame_cur != 0)
      {
         // projhold: cross-frame check vs COMMITTED with the same tolerance
         // the within-frame gate uses. Rotation continuous + projection
         // jumped discretely = mismatched-view alternation: HOLD the commit
         // entirely (no cur->prev shift, no restamp, pair stays live) and
         // count a projection rejection so FULL shows projrejD+ climbing.
         // The own path then reuses the last good pair via its guard
         // (pairing-fail owng=2 falls back to game MVs -- honest, never
         // mismatched matrices with owng=0).
         float c0 = god.view_c_p00, c1 = god.view_c_p11;
         float ac0 = c0 < 0 ? -c0 : c0, ac1 = c1 < 0 ? -c1 : c1;
         float tol0 = 0.015f * ac0 > 0.015f ? 0.015f * ac0 : 0.015f;
         float tol1 = 0.015f * ac1 > 0.015f ? 0.015f * ac1 : 0.015f;
         float jd = 0.0f;
         for (int q = 0; q < 16; q++)
         {
            float dd = w.cb[32 + q] - god.view_cb_cur[32 + q];
            if (dd < 0) dd = -dd;
            if (dd > jd) jd = dd;
         }
         float d0 = w.p00 - c0; if (d0 < 0) d0 = -d0;
         float d1 = w.p11 - c1; if (d1 < 0) d1 = -d1;
         if (jd < 0.8f && (d0 > tol0 || d1 > tol1))
         {
            g_view_projrej.fetch_add(1, std::memory_order_relaxed);
            g_agg_projrej.fetch_add(1, std::memory_order_relaxed);
            // Escape hatch: a challenger projection STABLE across 5 straight
            // frames (real zoom/cut) re-anchors to it.
            float hd0 = w.p00 - god.view_hold_p00; if (hd0 < 0) hd0 = -hd0;
            float hd1 = w.p11 - god.view_hold_p11; if (hd1 < 0) hd1 = -hd1;
            if (god.view_hold_run > 0 && hd0 <= tol0 && hd1 <= tol1)
               god.view_hold_run++;
            else
            {
               god.view_hold_run = 1;
               god.view_hold_p00 = w.p00;
               god.view_hold_p11 = w.p11;
            }
            if (god.view_hold_run >= 5)
            {
               CommitViewPick(god, w.cb, w.p00, w.p11, frame, best);
               god.view_hold_run = 0;
            }
            god.view_pick_run = 0;
            god.view_reanchor_run = 0;
            return;
         }
         god.view_hold_run = 0;
      }
      CommitViewPick(god, w.cb, w.p00, w.p11, frame, best);
      god.view_pick_run = 0;
      god.view_reanchor_run = 0;
      return;
   }
   float c0 = god.view_c_p00, c1 = god.view_c_p11;
   float ac0 = c0 < 0 ? -c0 : c0, ac1 = c1 < 0 ? -c1 : c1;
   float tol0 = 0.015f * ac0 > 0.015f ? 0.015f * ac0 : 0.015f;
   float tol1 = 0.015f * ac1 > 0.015f ? 0.015f * ac1 : 0.015f;
   int jdpass = 0, best = -1;
   float bestdp = 0.0f;
   for (int i = 0; i < god.view_npool; i++)
   {
      float jd = 0.0f;
      for (int q = 0; q < 16; q++)
      {
         float dd = god.view_pool[i].cb[32 + q] - god.view_cb_cur[32 + q];
         if (dd < 0) dd = -dd;
         if (dd > jd) jd = dd;
      }
      if (jd >= 0.8f) continue; // rotation gate vs committed
      jdpass++;
      float d0 = god.view_pool[i].p00 - c0; if (d0 < 0) d0 = -d0;
      float d1 = god.view_pool[i].p11 - c1; if (d1 < 0) d1 = -d1;
      if (d0 > tol0 || d1 > tol1) { g_view_projrej.fetch_add(1, std::memory_order_relaxed); g_agg_projrej.fetch_add(1, std::memory_order_relaxed); continue; }
      float dp = d0 + d1;
      if (best < 0 || god.view_pool[i].hits > god.view_pool[best].hits ||
          (god.view_pool[i].hits == god.view_pool[best].hits && dp < bestdp))
      { best = i; bestdp = dp; }
   }
   if (jdpass == 0)
   {
      // Possible cut/teleport: re-anchor to max-hits after 5 straight.
      god.view_pick_run = 0;
      if (++god.view_reanchor_run >= 5)
      {
         int bh = 0;
         for (int i = 1; i < god.view_npool; i++)
            if (god.view_pool[i].hits > god.view_pool[bh].hits) bh = i;
         const auto& w = god.view_pool[bh];
         CommitViewPick(god, w.cb, w.p00, w.p11, frame, bh);
         god.view_reanchor_run = 0;
      }
      return;
   }
   god.view_reanchor_run = 0;
   if (best < 0) { god.view_pick_run = 0; return; } // all jd-passers proj-dropped: hold
    const auto& w = god.view_pool[best];
    float mjd = 0.0f;
    for (int q = 0; q < 16; q++)
    {
       float dd = w.cb[32 + q] - god.view_cb_cur[32 + q];
       if (dd < 0) dd = -dd;
       if (dd > mjd) mjd = dd;
    }
    g_view_rotmag.store(mjd, std::memory_order_relaxed); // templog: winner rotation delta
    g_agg_rotn.fetch_add(1, std::memory_order_relaxed);
    g_agg_rotsum.fetch_add((int64_t)(mjd * 1000.0f), std::memory_order_relaxed);
    {
       int64_t mv = (int64_t)(mjd * 1000.0f);
       int64_t cur = g_agg_rotmax.load(std::memory_order_relaxed);
       while (mv > cur && !g_agg_rotmax.compare_exchange_weak(cur, mv)) {}
    }
   if (mjd < 0.05f)
   {
      CommitViewPick(god, w.cb, w.p00, w.p11, frame, best); // continuous
      god.view_pick_run = 0;
      return;
   }
   // Challenger: switch incumbent only after 3 consecutive wins.
   float cd0 = w.p00 - god.view_chal_p00; if (cd0 < 0) cd0 = -cd0;
   float cd1 = w.p11 - god.view_chal_p11; if (cd1 < 0) cd1 = -cd1;
   float cjd = 0.0f;
   for (int q = 0; q < 16; q++)
   {
      float dd = w.cb[32 + q] - god.view_chal_r[q];
      if (dd < 0) dd = -dd;
      if (dd > cjd) cjd = dd;
   }
   if (god.view_pick_run > 0 && cd0 <= tol0 && cd1 <= tol1 && cjd < 0.05f)
      god.view_pick_run++;
   else
   {
      god.view_pick_run = 1;
      god.view_chal_p00 = w.p00;
      god.view_chal_p11 = w.p11;
      memcpy(god.view_chal_r, w.cb + 32, 64);
   }
   if (god.view_pick_run >= 3)
   {
      CommitViewPick(god, w.cb, w.p00, w.p11, frame, best);
      god.view_pick_run = 0;
   }
   // else: hold committed (use-point guard falls back to game MVs)
}
// M32: one-shot projection-matrix scan (own-camera-MV groundwork). At the
// compute slot, reads CS CB slots 0..7 and logs 64B-aligned 4x4 candidates
// shaped like projections (upstream MatrixLikeProjection test) -- finds
// ViewToClip / ClipToPrevClip for the MV generator.
static std::atomic<bool> g_cscan_cb{ false };
// M34: same scan at the velocity passes (VS+PS CBs -- object->clip matrices
// live in the vertex stage; the fixed-kernel TAA's CBs carry none).
static std::atomic<bool> g_pscan_cb{ false };
// M19: depth-range triage for motion disocclusion shimmer.
static std::atomic<float> g_near_plane{ 10.0f };
static std::atomic<float> g_far_plane{ 200000.0f };
// M19: one-shot compute input signal stats (min/max/mean per channel).
static std::atomic<bool> g_cstats{ false };
// M27: per-producer MV buffers + velocity-pass selector for the DLSS
// feed (0 auto/latest cache, 1-6 the six known producers; slot t0 stays
// on its own M14 checkbox). Latest-wins single cache flips across modes.
static std::mutex g_mvmap_mutex;
struct MvBuf { ComPtr<ID3D11Resource> res; uint64_t frame = 0; };
static std::unordered_map<uint32_t, MvBuf> g_mv_by_hash;
static std::atomic<int> g_mv_src_mode{ 0 };
// Hybrid MVs (M11/compute only): game truth where nonzero, own fill where
// cleared. Default OFF (pure game/own/zero feeds as today).
static std::atomic<bool> g_hybrid_mv{ false };
// M28: canonical producer table (file scope: DLSS feed + menu + stats).
static const uint32_t kMvHashes[6] = { 0xBE0130E5, 0x1E94CABC, 0x5846E9DA, 0x6C6AD505, 0xA1A256FA, 0xA5CB30BF };
static const char* kMvNames[6] = { "BE0130E5", "1E94CABC", "5846E9DA", "6C6AD505", "A1A256FA", "A5CB30BF" };
// M27: compute-slot viewer (gameplay): stash the selected input/output
// at the slot, blit it over the next large pixel draw + Skip. The pixel
// viewer only fires where the pixel TAAU runs (showcase). Named sources,
// masterless.
static std::mutex g_cview_mutex;
static ComPtr<ID3D11ShaderResourceView> g_cview_srv;
static std::atomic<bool> g_cview_pending{ false };
static std::atomic<int> g_cview_src{ -1 };
// M40: throttle -- blitting EVERY large draw (150+/frame incl. 80x velocity)
// halts fill-rate. One blit per distinct target per frame keeps last-wins
// visibility at ~15 copies instead of ~150.
static std::atomic<uint64_t> g_cview_last_target{ 0 };
static std::atomic<uint64_t> g_cview_target_frame{ 0 };
// Viewfix: honest viewer status (menu + log). stash_ok: -1=none yet, 0=FAIL, 1=OK.
static std::atomic<int> g_cview_stash_ok{ -1 };
static std::atomic<int> g_cview_stash_src{ -99 };
static std::atomic<uint64_t> g_cview_stash_frame{ 0 };
static char g_cview_stash_reason[96] = {};
static std::atomic<uint64_t> g_cview_blits{ 0 };
// Full-trace master switch (bounded cost: ONE line per present, never per-draw).
// Visible in ALL configs; the full.log FILE is TEST||DEVELOPMENT-only (same
// gating as frame/draws logs). ReShade.log mirror runs in every config.
static std::atomic<bool> g_fulltrace{ false };
static std::atomic<int> g_fulltrace_frames_left{ 0 };
// Tracefix: default 300 (~5s), hard clamp 30..300 at every arm site.
// N=100000 meant ~28min of per-present file open/append/close + 1.4KB
// ReShade mirror on the present thread (IO hitch + 100MB+ logs).
static std::atomic<int> g_fulltrace_n{ 300 };
// M59: exact NGX feeds, stored at Draw time (what DLSS actually got, named).
// Slot viewers show bindings; these show the resolved inputs per path.
static std::mutex g_feed_mutex;
static ComPtr<ID3D11Resource> g_feed_m11_color, g_feed_m11_depth, g_feed_m11_mv, g_feed_m11_out;
static ComPtr<ID3D11Resource> g_feed_m7_color, g_feed_m7_depth, g_feed_m7_mv, g_feed_m7_out;
static std::atomic<uint64_t> g_feed_m11_frame{ 0 }, g_feed_m7_frame{ 0 };
// M60: MV keeps last-good (zero fallback skipped) -- these stamp when the MV
// itself was stored, so the menu MV age exposes staleness (static camera,
// cache miss at an angle) instead of showing a black zero texture.
static std::atomic<uint64_t> g_feed_m11_mvframe{ 0 }, g_feed_m7_mvframe{ 0 };
// M60: which depth actually fed NGX ("cache-DSV" vs slot fallback).
static char g_feed_m11_dsrc[24] = {}, g_feed_m7_dsrc[24] = {};
static std::atomic<int> g_feedview{ -1 }; // -1 OFF, 0-3 M11 color/depth/MV/out, 4-7 M7 color/depth/MV/out
// M62: last seen output dims per path. A change = window resize = NGX
// resolution change, which REQUIRES a reset (else DLSS keeps evaluating
// against stale-sized internal targets and never recovers).
static std::atomic<uint32_t> g_c_last_cw{ 0 }, g_c_last_ch{ 0 };
static std::atomic<uint32_t> g_m7_last_ow{ 0 }, g_m7_last_oh{ 0 };
// M64: render dims too -- a render-scale change at a fixed window size is
// the same NGX-resolution-change event as a window resize.
static std::atomic<uint32_t> g_c_last_crw{ 0 }, g_c_last_crh{ 0 };
static std::atomic<uint32_t> g_m7_last_rw{ 0 }, g_m7_last_rh{ 0 };
// M60: private snapshot targets (one per feed slot). StoreFeed copies the
// live game texture into these so the viewer shows what DLSS consumed, not
// whatever the game left in the reused texture afterwards.
static ComPtr<ID3D11Texture2D> g_snap_m11_color, g_snap_m11_depth, g_snap_m11_mv, g_snap_m11_out;
static ComPtr<ID3D11Texture2D> g_snap_m7_color, g_snap_m7_depth, g_snap_m7_mv, g_snap_m7_out;
// A concrete depth texture cannot take SRV bind flags, so snapshot it
// through its typeless counterpart (the standard depth-readback recipe;
// same-family copies are legal).
static DXGI_FORMAT SnapFormat(DXGI_FORMAT f)
{
   switch (f)
   {
   case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
   case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
   case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
   default: break;
   }
   return f;
}
// Copy a live texture into its snapshot slot (creating/resizing the slot as
// needed). Falls back to the live ref when a copy is impossible (MSAA,
// arrays, creation failure) -- old behavior, still viewable.
static void SnapshotRes(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Resource* src,
   ComPtr<ID3D11Texture2D>& slot, ComPtr<ID3D11Resource>& out)
{
   out.reset();
   if (!src) return;
   ComPtr<ID3D11Texture2D> t;
   if (!dev || !ctx || FAILED(src->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(t.put()))) || !t)
   { out = src; return; }
   D3D11_TEXTURE2D_DESC d = {};
   t->GetDesc(&d);
   if (d.SampleDesc.Count != 1 || d.ArraySize != 1 || d.MipLevels != 1)
   { out = src; return; }
   DXGI_FORMAT df = SnapFormat(d.Format);
   bool match = false;
   if (slot)
   {
      D3D11_TEXTURE2D_DESC sd = {};
      slot->GetDesc(&sd);
      match = (sd.Width == d.Width && sd.Height == d.Height && sd.Format == df);
   }
   if (!match)
   {
      slot.reset();
      D3D11_TEXTURE2D_DESC nd = {};
      nd.Width = d.Width; nd.Height = d.Height;
      nd.MipLevels = 1; nd.ArraySize = 1;
      nd.Format = df; nd.SampleDesc.Count = 1;
      nd.Usage = D3D11_USAGE_DEFAULT;
      nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      ComPtr<ID3D11Texture2D> nt;
      if (FAILED(dev->CreateTexture2D(&nd, nullptr, nt.put())) || !nt)
      { out = src; return; }
      slot = nt; // same-type assign (this ComPtr has no cross-type operator=)
   }
   ctx->CopySubresourceRegion(slot.get(), 0, 0, 0, 0, src, 0, nullptr);
   ID3D11Resource* rr = nullptr;
   if (SUCCEEDED(slot->QueryInterface(__uuidof(ID3D11Resource), reinterpret_cast<void**>(&rr))) && rr)
      out.attach(rr); // QI ref -> attach (both patterns used elsewhere)
   else
      out = src;
}
static void StoreFeed(int path, ID3D11Resource* color, ID3D11Resource* depth,
   ID3D11Resource* mv, ID3D11Resource* out, ID3D11Device* dev, ID3D11DeviceContext* ctx,
   bool mv_real, const char* dsrc)
{
   std::lock_guard<std::mutex> lk(g_feed_mutex);
   // Snapshot only the path the feed viewer is actually watching -- GPU
   // copies are cheap but pointless when nobody is looking. DLSS feeds are
   // untouched either way; this only changes what the viewer holds.
   int fsel = g_feedview.load(std::memory_order_relaxed);
   bool want = (path == 0) ? (fsel >= 0 && fsel <= 3) : (fsel >= 4 && fsel <= 7);
   bool snap = want && dev && ctx;
   uint64_t fframe = g_hist_frame.load(std::memory_order_relaxed);
   if (path == 0)
   {
      if (snap)
      {
         SnapshotRes(dev, ctx, color, g_snap_m11_color, g_feed_m11_color);
         SnapshotRes(dev, ctx, depth, g_snap_m11_depth, g_feed_m11_depth);
         if (mv_real) SnapshotRes(dev, ctx, mv, g_snap_m11_mv, g_feed_m11_mv);
         // !mv_real: keep last good MV. The zero fallback blanked the view
         // whenever the camera was static or the cache missed at an angle.
         SnapshotRes(dev, ctx, out, g_snap_m11_out, g_feed_m11_out);
      }
      else
      {
         if (color) { g_feed_m11_color = color; } else { g_feed_m11_color.reset(); }
         if (depth) { g_feed_m11_depth = depth; } else { g_feed_m11_depth.reset(); }
         if (mv_real) { if (mv) { g_feed_m11_mv = mv; } else { g_feed_m11_mv.reset(); } }
         if (out) { g_feed_m11_out = out; } else { g_feed_m11_out.reset(); }
      }
      if (mv_real) g_feed_m11_mvframe.store(fframe, std::memory_order_relaxed);
      if (dsrc && dsrc[0]) snprintf(g_feed_m11_dsrc, sizeof(g_feed_m11_dsrc), "%s", dsrc);
      g_feed_m11_frame.store(fframe, std::memory_order_relaxed);
   }
   else
   {
      if (snap)
      {
         SnapshotRes(dev, ctx, color, g_snap_m7_color, g_feed_m7_color);
         SnapshotRes(dev, ctx, depth, g_snap_m7_depth, g_feed_m7_depth);
         if (mv_real) SnapshotRes(dev, ctx, mv, g_snap_m7_mv, g_feed_m7_mv);
         SnapshotRes(dev, ctx, out, g_snap_m7_out, g_feed_m7_out);
      }
      else
      {
         if (color) { g_feed_m7_color = color; } else { g_feed_m7_color.reset(); }
         if (depth) { g_feed_m7_depth = depth; } else { g_feed_m7_depth.reset(); }
         if (mv_real) { if (mv) { g_feed_m7_mv = mv; } else { g_feed_m7_mv.reset(); } }
         if (out) { g_feed_m7_out = out; } else { g_feed_m7_out.reset(); }
      }
      if (mv_real) g_feed_m7_mvframe.store(fframe, std::memory_order_relaxed);
      if (dsrc && dsrc[0]) snprintf(g_feed_m7_dsrc, sizeof(g_feed_m7_dsrc), "%s", dsrc);
      g_feed_m7_frame.store(fframe, std::memory_order_relaxed);
   }
}
// Depth needs an explicit view (typed D24/S8 or typeless R24G8 both fail
// nullptr-desc SRV). Try concrete depth views, then the generic helper
// (forward-declared here, defined below with the other view helpers).
static bool CreateViewSRV(ID3D11Device* dev, ID3D11Resource* res, ComPtr<ID3D11ShaderResourceView>& out);
static bool CreateDepthViewSRV(ID3D11Device* dev, ID3D11Resource* res, ComPtr<ID3D11ShaderResourceView>& out)
{
   if (!dev || !res) return false;
   const DXGI_FORMAT tryfmts[2] = { DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS };
   for (int i = 0; i < 2; i++)
   {
      D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
      vd.Format = tryfmts[i];
      vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      vd.Texture2D.MipLevels = 1;
      if (SUCCEEDED(dev->CreateShaderResourceView(res, &vd, out.put())))
         return true;
      out.reset();
   }
   return CreateViewSRV(dev, res, out);
}
// M42: auto-diagnostic mode. Arms all one-shot diagnostics (cSTATS, CB dump,
// MSCAN, pSCAN) and auto-fires them on the next compute slot fire. No user
// interaction needed beyond pressing the button and playing the game.
static std::atomic<bool> g_auto_diag{ false };
// M43: multi-frame ADIAG countdown (4 consecutive compute-slot fires).
static std::atomic<int> g_adiag_left{ 0 };
// M48: feed-identity countdown (4 consecutive NGX feeds: which color/depth/
// MV resources actually reach DLSS + their writer stamps).
static std::atomic<int> g_adiag_feed{ 0 };
// M57: DLSS input audit (N consecutive NGX feeds, BOTH paths). Logs every
// DrawData + SettingsData value per feed + per-feed deltas, then a verdict
// summary. The blur/shimmer root cause must be visible here: stuck fidx,
// reset every frame, zero jitter while CB animates, handles flip-flopping,
// MV fmt changes, or settings flapping between paths.
static std::atomic<int> g_audit_left{ 0 };
static std::atomic<uint64_t> g_audit_seq{ 0 };
static std::atomic<uint64_t> g_dlss_resets{ 0 }; // M57: M7 reset counter (M11 has g_cdlss_resets)
static std::atomic<float> g_vert_fov{ 1.047f }; // M57: was hardcoded 60deg; proven view ~39.4deg=0.688
static std::atomic<bool> g_m7stats{ false }; // M57: one-shot M7 pixel-input signal stats

// M15/M30: MV scale is 1.0 pass-through, always (upstream parity -- the MV
// texture already holds zero-centered pixels; NVIDIA guide: "1.0 if motion
// vectors do not need to be scaled"). Legacy modes 1 (0.5*res double-scale),
// 2 (0.5), 3 (2.0 test) retired by cleanup: the menu combo is parked in the
// Legacy header and g_mv_scale_mode stays defined for compat, but the feed
// no longer reads it. Jitter scale likewise x1 always.
static void GetMVScales(float rw, float rh, float& sx, float& sy)
{
   (void)rw; (void)rh;
   sx = 1.0f; sy = 1.0f;
}
static float GetJitScale()
{
   return 1.0f;
}
// M45: sr_type log trap -- upstream SR::Type is DLSS=0, FSR=1, None=-1
// (super_resolution.h), so raw %d misreads DLSS as None. Always log the name.
static const char* SrTypeName(SR::Type t)
{
   switch (t)
   {
   case SR::Type::DLSS: return "DLSS";
   case SR::Type::FSR: return "FSR";
   case SR::Type::None: return "None";
   default: return "?";
   }
}

// M19: IEEE-754 binary16 -> float (for input signal stats).
static float HalfToFloat(uint16_t h)
{
   uint32_t s = (h >> 15) & 0x1u;
   uint32_t e = (h >> 10) & 0x1Fu;
   uint32_t m = h & 0x3FFu;
   uint32_t f = 0;
   if (e == 0)
   {
      if (m == 0) f = s << 31;
      else
      {
         // Subnormal: renormalize.
         e = 1;
         while ((m & 0x400u) == 0) { m <<= 1; e--; }
         m &= 0x3FFu;
         f = (s << 31) | ((e + 112) << 23) | (m << 13);
      }
   }
   else if (e == 31) f = (s << 31) | (0xFFu << 23) | (m << 13); // inf/nan
   else f = (s << 31) | ((e + 112) << 23) | (m << 13);
   union { uint32_t u; float f; } cvt;
   cvt.u = f;
   return cvt.f;
}

// M19: one-shot signal stats for a resource (min/max/mean of the first 4
// lanes). Answers "is there any signal?" per buffer per mode: all-zero MVs
// or constant jitter make every scale sweep a no-op (scale x 0 = 0).
static void LogResStats(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Resource* res, const char* name)
{
   char b[512];
   if (!res) { snprintf(b, sizeof(b), "DaysGone cSTATS %s=null", name); reshade::log::message(reshade::log::level::info, b); return; }
   ComPtr<ID3D11Texture2D> t;
   if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(t.put()))) || !t)
   { snprintf(b, sizeof(b), "DaysGone cSTATS %s=n2d", name); reshade::log::message(reshade::log::level::info, b); return; }
   D3D11_TEXTURE2D_DESC d = {};
   t->GetDesc(&d);
   D3D11_TEXTURE2D_DESC sd = d;
   sd.Usage = D3D11_USAGE_STAGING;
   sd.BindFlags = 0;
   sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   sd.MiscFlags = 0;
   ComPtr<ID3D11Texture2D> st;
   if (FAILED(dev->CreateTexture2D(&sd, nullptr, st.put())))
   { snprintf(b, sizeof(b), "DaysGone cSTATS %s=stag-fail fmt=%d", name, (int)d.Format); reshade::log::message(reshade::log::level::info, b); return; }
   ctx->CopySubresourceRegion(st.get(), 0, 0, 0, 0, t.get(), 0, nullptr);
   D3D11_MAPPED_SUBRESOURCE m = {};
   if (FAILED(ctx->Map(st.get(), 0, D3D11_MAP_READ, 0, &m)) || !m.pData)
   { snprintf(b, sizeof(b), "DaysGone cSTATS %s=map-fail", name); reshade::log::message(reshade::log::level::info, b); return; }
   // Lane reader: 0 = f16 lanes, 1 = u16 lanes, 2 = u32/f32 lanes, 3 = bytes.
   int kind = 3, lanes = 4;
   double norm = 1.0;
   switch (d.Format)
   {
   case DXGI_FORMAT_R16G16B16A16_FLOAT:
   case DXGI_FORMAT_R16G16_FLOAT: kind = 0; lanes = (d.Format == DXGI_FORMAT_R16G16_FLOAT) ? 2 : 4; break;
   case DXGI_FORMAT_R16G16_UINT: kind = 1; lanes = 2; norm = 1.0; break;
   case DXGI_FORMAT_R16G16_UNORM: kind = 1; lanes = 2; norm = 65535.0; break;
   case DXGI_FORMAT_R16G16_SNORM: kind = 1; lanes = 2; norm = 32767.0; break;
   case DXGI_FORMAT_R32G32_UINT:
   case DXGI_FORMAT_R32G32_FLOAT:
   case DXGI_FORMAT_R32_FLOAT: kind = 2; lanes = (d.Format == DXGI_FORMAT_R32_FLOAT) ? 1 : 2; break;
   default: kind = (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM) ? 1 : 3; lanes = 4; norm = (kind == 1) ? 255.0 : 1.0; break;
   }
   double mn[4] = { 1e30, 1e30, 1e30, 1e30 }, mx[4] = { -1e30, -1e30, -1e30, -1e30 }, sum[4] = {};
   uint64_t n = 0;
   for (uint32_t y = 0; y < d.Height; y++)
   {
      const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
      for (uint32_t x = 0; x < d.Width; x++)
      {
         for (int c = 0; c < lanes; c++)
         {
            double v = 0.0;
            if (kind == 0) v = (double)HalfToFloat(((const uint16_t*)(row + (size_t)x * lanes * 2))[c]);
            else if (kind == 1) v = (double)((const uint16_t*)(row + (size_t)x * lanes * 2))[c] / norm;
            else if (kind == 2) v = (double)((const uint32_t*)(row + (size_t)x * lanes * 4))[c];
            else v = (double)row[(size_t)x * lanes + c];
            if (v < mn[c]) mn[c] = v;
            if (v > mx[c]) mx[c] = v;
            sum[c] += v;
         }
         n++;
      }
   }
   ctx->Unmap(st.get(), 0);
   int pos = snprintf(b, sizeof(b), "DaysGone cSTATS %s fmt=%d %ux%u", name, (int)d.Format, d.Width, d.Height);
   for (int c = 0; c < lanes && pos < (int)sizeof(b) - 64; c++)
      pos += snprintf(b + pos, sizeof(b) - pos, " ch%d[%.4g,%.4g,%.4g]", c, mn[c], mx[c], sum[c] / (double)(n ? n : 1));
   reshade::log::message(reshade::log::level::info, b);
}

// M11: describe any resource as WxH:fmt (for the compute-slot logger).
static void DescribeResHandle(ID3D11Resource* res, char* out, size_t n)
{
   if (!res) { strcpy_s(out, n, "null"); return; }
   ID3D11Texture2D* t = nullptr;
   if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t)
   {
      D3D11_TEXTURE2D_DESC d = {};
      t->GetDesc(&d);
      sprintf_s(out, n, "%ux%u f%d", d.Width, d.Height, (int)d.Format);
      t->Release();
   }
    else strcpy_s(out, n, "n2d");
}

// M48: last-writer frame of a resource (for the stale-input verdict: a
// healthy per-frame pick stamps the CURRENT frame every time; a stale
// fallback shows older frames or flip-flops). 0 = never stamped/unknown.
static uint64_t WriterFrame(ID3D11Resource* res)
{
   if (!res)
      return 0;
   std::lock_guard<std::mutex> lk(g_writer_mutex);
   auto it = g_rt_last_writer.find((uint64_t)res);
   return (it != g_rt_last_writer.end()) ? it->second : 0;
}

// M57: one audit line per NGX feed. Everything DLSS sees, in one place:
// path, seq, frame, fidx, reset, render->out, hdr/inv/mvsjit/aexp, preset,
// scale/dec/src/own, jitter fed, near/far/fov, resource descs + writer
// stamps + handle-change flags, ok. Deltas across 10 lines expose the bug:
// fidx stuck, reset>1, jitter constant while moving, handles flapping.
static void LogAuditFeed(const char* path, ID3D11Resource* color, ID3D11Resource* depth,
   ID3D11Resource* mv, const char* mvsrc, int mvfmt, uint32_t rw, uint32_t rh,
   uint32_t ow, uint32_t oh, bool hdr, bool reset, float jx, float jy, bool ok,
   const char* dsrc = "")
{
   char fcb[64] = {}, fdb[64] = {}, fmb[64] = {};
   DescribeResHandle(color, fcb, sizeof(fcb));
   DescribeResHandle(depth, fdb, sizeof(fdb));
   DescribeResHandle(mv, fmb, sizeof(fmb));
   uint64_t fframe = g_hist_frame.load(std::memory_order_relaxed);
   uint64_t seq = g_audit_seq.fetch_add(1, std::memory_order_relaxed);
   static uint64_t last_fidx = 0, last_ch = 0, last_dh = 0, last_mh = 0;
   uint64_t fi = (uint64_t)cb_luma_global_settings.FrameIndex;
   uint64_t ch = (uint64_t)color, dh = (uint64_t)depth, mh = (uint64_t)mv;
   char dch = (ch != last_ch) ? '*' : '=';
   char ddh = (dh != last_dh) ? '*' : '=';
   char dmh = (mh != last_mh) ? '*' : '=';
   char dfi = (fi != last_fidx + 1 && seq > 0) ? '!' : '=';
   last_fidx = fi; last_ch = ch; last_dh = dh; last_mh = mh;
   char fl[768];
   snprintf(fl, sizeof(fl),
      "DaysGone AUDIT %s #%llu frame=%llu fidx=%llu%c reset=%d %ux%u->%ux%u hdr=%d inv=%d mvsjit=%d aexp=%d sr=%s dec=%d src=%d own=%d p=%.3f,%.3f msc=%d jit=%.3f,%.3f near=%.2f far=%.0f fov=%.3f mvfmt=%d mv=%s color=%s@%llu%c depth=%s(%s)@%llu%c mvres=%s@%llu%c ok=%d",
      path, (unsigned long long)seq, (unsigned long long)fframe, (unsigned long long)fi, dfi, (int)reset,
      rw, rh, ow, oh, (int)hdr, (int)g_inverted_depth.load(std::memory_order_relaxed),
      (int)g_mvs_jittered.load(std::memory_order_relaxed), (int)g_dlss_autoexp.load(std::memory_order_relaxed),
      SrTypeName(SR::Type::DLSS),
      g_mv_decode.load(std::memory_order_relaxed), g_mv_src_mode.load(std::memory_order_relaxed),
      g_own_used.load(std::memory_order_relaxed),
      g_own_p00.load(std::memory_order_relaxed), g_own_p11.load(std::memory_order_relaxed),
      g_mv_scale_mode.load(std::memory_order_relaxed),
      jx, jy,
      g_near_plane.load(std::memory_order_relaxed), g_far_plane.load(std::memory_order_relaxed),
      g_vert_fov.load(std::memory_order_relaxed), mvfmt, mvsrc,
      fcb, (unsigned long long)WriterFrame(color), dch,
      fdb, dsrc, (unsigned long long)WriterFrame(depth), ddh,
      fmb, (unsigned long long)WriterFrame(mv), dmh, (int)ok);
   reshade::log::message(reshade::log::level::info, fl);
}

// M28: SRV for viewing ANY game resource. Typeless resources (HUD t0,
// depth-as-SRV, typeless MV buffers) need an explicit concrete view
// format; a nullptr-desc CreateShaderResourceView FAILS on those, which
// was the "viewer shows nothing" bug for t0-like sources.
static bool CreateViewSRV(ID3D11Device* dev, ID3D11Resource* res, ComPtr<ID3D11ShaderResourceView>& out)
{
   if (!dev || !res)
      return false;
   ComPtr<ID3D11Texture2D> t;
   if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(t.put()))) || !t)
      return false;
   D3D11_TEXTURE2D_DESC d = {};
   t->GetDesc(&d);
   if (d.SampleDesc.Count != 1 || d.ArraySize != 1)
      return false;
   DXGI_FORMAT vf = d.Format;
   switch (vf)
   {
   case DXGI_FORMAT_R8G8B8A8_TYPELESS: vf = DXGI_FORMAT_R8G8B8A8_UNORM; break;
   case DXGI_FORMAT_R16G16_TYPELESS: vf = DXGI_FORMAT_R16G16_UNORM; break;
   case DXGI_FORMAT_R24G8_TYPELESS: vf = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
   case DXGI_FORMAT_R32G32_TYPELESS: vf = DXGI_FORMAT_R32G32_FLOAT; break;
   case DXGI_FORMAT_R32_TYPELESS: vf = DXGI_FORMAT_R32_FLOAT; break;
   default: break;
   }
   if (vf == d.Format)
      return SUCCEEDED(dev->CreateShaderResourceView(res, nullptr, out.put()));
   D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
   vd.Format = vf;
   vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
   vd.Texture2D.MipLevels = 1;
   return SUCCEEDED(dev->CreateShaderResourceView(res, &vd, out.put()));
}

// Viewfix: typeless-tolerant stash SRV. nullptr-desc fails on typeless
// (R8G8B8A8_TYPELESS HUD t0, R24G8 depth); concrete UNORM views succeed.
static bool CreateStashSRV(ID3D11Device* dev, ID3D11Resource* res, ComPtr<ID3D11ShaderResourceView>& out)
{
   if (CreateViewSRV(dev, res, out))
      return true;
   out.reset();
   return CreateDepthViewSRV(dev, res, out);
}

// Viewfix: menu channel index (R,G,B,A,ALL,DepthN) -> RunCopyPass view_chan
// (R,G,B,A,DepthN=4, ALL=-1). The old ">=5?-1" mapping showed DepthN when
// ALL was selected and ALL when DepthN was selected.
static int ViewChanForBlit(int menu_chan)
{
   if (menu_chan == 4) return -1; // ALL
   if (menu_chan == 5) return 4;  // DepthN
   if (menu_chan >= 0 && menu_chan <= 3) return menu_chan;
   return -1;
}

// Viewfix: one log line per stash result + honest menu status. Called on
// every stash attempt outcome change (per-source dedup inside).
static void CViewStashReport(bool ok, int src, const char* detail)
{
   uint64_t curf = g_hist_frame.load(std::memory_order_relaxed);
   {
      std::lock_guard<std::mutex> lk(g_cview_mutex);
      g_cview_stash_ok.store(ok ? 1 : 0, std::memory_order_relaxed);
      g_cview_stash_src.store(src, std::memory_order_relaxed);
      g_cview_stash_frame.store(curf, std::memory_order_relaxed);
      snprintf(g_cview_stash_reason, sizeof(g_cview_stash_reason), "%s", detail ? detail : "");
   }
   static int last_ok = -2;
   static int last_src = -999;
   static char last_detail[96] = {};
   if (last_ok != (ok ? 1 : 0) || last_src != src ||
       strcmp(last_detail, detail ? detail : "") != 0)
   {
      last_ok = ok ? 1 : 0;
      last_src = src;
      snprintf(last_detail, sizeof(last_detail), "%s", detail ? detail : "");
      char b[192];
      if (ok)
         snprintf(b, sizeof(b), "DaysGone cVIEW stash ok src=%d (%s)", src, detail ? detail : "");
      else
         snprintf(b, sizeof(b), "DaysGone cVIEW stash FAIL src=%d (%s)", src, detail ? detail : "");
      reshade::log::message(ok ? reshade::log::level::info : reshade::log::level::warning, b);
   }
}

// Blit-guard: the persistent viewer blit issues its own Draw, which re-enters
// this same draw hook and satisfies the same blit gate (large target, vsrc
// set) -> unbounded recursion -> silent death (no Windows event). The
// g_cview_last_target/g_cview_target_frame throttle was never wired, so this
// thread_local re-entrancy flag is the guard: inner Draws skip the blit and
// fall through to the normal draw path. Display path only.
static thread_local bool g_in_copy_blit = false;
struct CopyBlitScope
{
   ~CopyBlitScope() { g_in_copy_blit = false; }
};

// M9e: fullscreen copy with FORCED clean state. The slot leaves blending,
// a 1920-viewport and scissor enabled -- inheriting them blended our copies
// with stale RT content (pink hues) and left stripes unwritten.
// M11: keep_alpha selects the alpha-preserving variants (compute-TAA output:
// native alpha behavior unknown, zeroing it could punch through downstream
// blends -- preserve by default there).
static bool RunCopyPass(ID3D11Device* dev, ID3D11DeviceContext* ctx, DeviceData& device_data,
   DaysGoneDeviceData& gd, ID3D11ShaderResourceView* src_srv, ID3D11RenderTargetView* dst_rtv,
   uint32_t w, uint32_t h, bool swap_rb, bool srgb_encode, int view_chan = -1, bool keep_alpha = false,
   bool alpha_one = false, bool is_viewer = false)
{
   // Cleanup: table-driven variant select (was 16x if-else; same keys, same
   // priority: keep_alpha > alpha_one > view_chan > srgb > swap > default).
   // Note view_chan only applies when !keep_alpha (as before). Keys are
   // prehashed from literals (never a runtime string) so this compiles
   // whether CompileTimeStringHash is constexpr-loop or array-template style.
   static const uint32_t kKeepAlpha[2][2][2] = {
      {
         { CompileTimeStringHash("DaysGone UI Copy PS"), CompileTimeStringHash("DaysGone UI Encode PS") },
         { CompileTimeStringHash("DaysGone UI Copy BGRA PS"), CompileTimeStringHash("DaysGone UI Encode BGRA PS") }
      },
      {
         { CompileTimeStringHash("DaysGone UI Copy AlphaOne PS"), CompileTimeStringHash("DaysGone UI Encode AlphaOne PS") },
         { CompileTimeStringHash("DaysGone UI Copy BGRA AlphaOne PS"), CompileTimeStringHash("DaysGone UI Encode BGRA AlphaOne PS") }
      }
   };
   static const uint32_t kViewR = CompileTimeStringHash("DaysGone View R PS");
   static const uint32_t kViewG = CompileTimeStringHash("DaysGone View G PS");
   static const uint32_t kViewB = CompileTimeStringHash("DaysGone View B PS");
   static const uint32_t kViewA = CompileTimeStringHash("DaysGone View A PS");
   static const uint32_t kViewD = CompileTimeStringHash("DaysGone View DepthN PS");
   static const uint32_t kSrgb = CompileTimeStringHash("DaysGone DLSS sRGB PS");
   static const uint32_t kBgra = CompileTimeStringHash("DaysGone DLSS Copy BGRA PS");
   static const uint32_t kCopy = CompileTimeStringHash("DaysGone DLSS Copy PS");
   uint32_t copy_key = kCopy;
   if (keep_alpha)
      copy_key = kKeepAlpha[alpha_one ? 1 : 0][swap_rb ? 1 : 0][srgb_encode ? 1 : 0];
   else if (view_chan == 0)
      copy_key = kViewR;
   else if (view_chan == 1)
      copy_key = kViewG;
   else if (view_chan == 2)
      copy_key = kViewB;
   else if (view_chan == 3)
      copy_key = kViewA;
   else if (view_chan == 4)
      copy_key = kViewD;
   else if (srgb_encode)
      copy_key = kSrgb;
   else if (swap_rb)
      copy_key = kBgra;
   ID3D11PixelShader* ps = device_data.native_pixel_shaders[copy_key].get();
   ID3D11VertexShader* vs = device_data.native_vertex_shaders[CompileTimeStringHash("Copy VS")].get();
   if (!ps || !vs || !src_srv || !dst_rtv || w == 0 || h == 0)
      return false;
   if (!gd.rs_copy)
   {
      D3D11_RASTERIZER_DESC rd = {};
      rd.FillMode = D3D11_FILL_SOLID;
      rd.CullMode = D3D11_CULL_NONE;
      rd.FrontCounterClockwise = FALSE;
      rd.DepthClipEnable = TRUE;
      rd.ScissorEnable = FALSE;
      rd.MultisampleEnable = FALSE;
      rd.AntialiasedLineEnable = FALSE;
      if (FAILED(dev->CreateRasterizerState(&rd, gd.rs_copy.put())))
         return false;
   }
   DrawStateStack<DrawStateStackType::FullGraphics> cs;
   cs.Cache(ctx, device_data.uav_max_count);
   ID3D11UnorderedAccessView* null_uavs[D3D11_1_UAV_SLOT_COUNT] = {};
   ctx->CSSetUnorderedAccessViews(0, device_data.uav_max_count, null_uavs, nullptr);
   D3D11_VIEWPORT vp = {};
   vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
   vp.Width = (float)w; vp.Height = (float)h;
   vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
   ctx->RSSetViewports(1, &vp);
   ctx->RSSetState(gd.rs_copy.get());
   ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
   ctx->OMSetDepthStencilState(nullptr, 0);
   ctx->OMSetRenderTargets(1, &dst_rtv, nullptr);
   ctx->VSSetShader(vs, nullptr, 0);
   ctx->PSSetShader(ps, nullptr, 0);
   // Viewer stretch-to-fill (display only): target dims for the View/Copy
   // shader so a half-res source fills the whole target. Bound on every
   // viewer blit regardless of channel (ALL=-1 uses the Copy PS, which now
   // scales too). Non-viewer copy paths pass is_viewer=false and bind no
   // cbuffer -- byte-for-byte old behavior there.
   bool use_viewcb = (view_chan >= 0 || is_viewer);
   if (use_viewcb)
   {
      if (!gd.cb_viewscale)
      {
         D3D11_BUFFER_DESC vbd = {};
         vbd.ByteWidth = 16;
         vbd.Usage = D3D11_USAGE_DYNAMIC;
         vbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
         vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
         dev->CreateBuffer(&vbd, nullptr, gd.cb_viewscale.put());
      }
      if (gd.cb_viewscale)
      {
         D3D11_MAPPED_SUBRESOURCE vm = {};
         if (SUCCEEDED(ctx->Map(gd.cb_viewscale.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &vm)) && vm.pData)
         {
            float* vf = (float*)vm.pData;
            vf[0] = (float)w; vf[1] = (float)h; vf[2] = 0.0f; vf[3] = 0.0f;
            ctx->Unmap(gd.cb_viewscale.get(), 0);
            ID3D11Buffer* vcb = gd.cb_viewscale.get();
            ctx->PSSetConstantBuffers(0, 1, &vcb);
         }
         else
            use_viewcb = false;
      }
      else
         use_viewcb = false;
   }
   ctx->PSSetShaderResources(0, 1, &src_srv);
   ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
   ctx->Draw(4, 0);
   if (use_viewcb)
   {
      ID3D11Buffer* nullvcb = nullptr;
      ctx->PSSetConstantBuffers(0, 1, &nullvcb);
   }
   cs.Restore(ctx);
   return true;
}

// M10d: UI composite -- t0 over the DLSS result with alpha blend ON.
// RunCopyPass forces blend OFF (correct for opaque scene copies); UI needs
// SRC_ALPHA/INV_SRC_ALPHA. Plain load, no sRGB encode (UI is LDR already),
// no swizzle (UI verified via viewer, not guessed). Returns false if t0 is
// small (1x1 params texture in some modes) so it can never whitewash.
static bool RunUIComposite(ID3D11Device* dev, ID3D11DeviceContext* ctx, DeviceData& device_data,
   DaysGoneDeviceData& gd, ID3D11ShaderResourceView* ui_srv, ID3D11RenderTargetView* dst_rtv,
   uint32_t w, uint32_t h)
{
     // M17: tonemap-mode selector (all alpha-preserving). 0 plain, 1 Reinhard,
     // 2 ACES, 3 Hable, 4 sRGB-decode only, 5 sRGB-encode only.
      ID3D11PixelShader* ps = nullptr;
      switch (g_ui_tonemap_mode.load(std::memory_order_relaxed))
      {
      case 1:
         ps = device_data.native_pixel_shaders[CompileTimeStringHash("DaysGone Tonemap PS")].get();
         break;
      case 2:
         ps = device_data.native_pixel_shaders[CompileTimeStringHash("DaysGone UI ACES PS")].get();
         break;
      case 3:
         ps = device_data.native_pixel_shaders[CompileTimeStringHash("DaysGone UI Hable PS")].get();
         break;
      case 4:
         ps = device_data.native_pixel_shaders[CompileTimeStringHash("DaysGone UI sRGB Decode PS")].get();
         break;
      case 5:
         ps = device_data.native_pixel_shaders[CompileTimeStringHash("DaysGone UI Encode PS")].get();
         break;
      case 0:
      default:
         ps = device_data.native_pixel_shaders[CompileTimeStringHash("DaysGone UI Copy PS")].get();
         break;
      }
   ID3D11VertexShader* vs = device_data.native_vertex_shaders[CompileTimeStringHash("Copy VS")].get();
   if (!ps || !vs || !ui_srv || !dst_rtv || w == 0 || h == 0)
      return false;
   // Size gate: UI is fullscreen; t0 is 1x1 params in scene modes.
   {
      ID3D11Resource* tmp = nullptr;
      ui_srv->GetResource(&tmp);
      ComPtr<ID3D11Resource> r;
      r.attach(tmp);
      ComPtr<ID3D11Texture2D> t;
      if (!r || FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(t.put()))))
         return false;
      D3D11_TEXTURE2D_DESC d = {};
      t->GetDesc(&d);
      if (d.Width < 1000 || d.Height < 500)
         return false; // params texture, not UI -- leave the frame alone
   }
   if (!gd.rs_copy)
   {
      D3D11_RASTERIZER_DESC rd = {};
      rd.FillMode = D3D11_FILL_SOLID;
      rd.CullMode = D3D11_CULL_NONE;
      rd.FrontCounterClockwise = FALSE;
      rd.DepthClipEnable = TRUE;
      rd.ScissorEnable = FALSE;
      rd.MultisampleEnable = FALSE;
      rd.AntialiasedLineEnable = FALSE;
      if (FAILED(dev->CreateRasterizerState(&rd, gd.rs_copy.put())))
         return false;
   }
    // Alpha-blend state (created fresh each call -- 1/frame at most, cheap).
    // M10h: blend mode selector -- user picks the best live.
    ComPtr<ID3D11BlendState> blend;
    {
       D3D11_BLEND_DESC bd = {};
       bd.RenderTarget[0].BlendEnable = TRUE;
       UINT write_mask = D3D11_COLOR_WRITE_ENABLE_ALL;
       switch (g_blend_mode.load(std::memory_order_relaxed))
       {
       case 1: // straight alpha
          bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
          bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
          break;
       case 2: // additive
          bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
          bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
          break;
       case 3: // multiplicative
          bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
          bd.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
          break;
       case 4: // M22 alpha-additive (glow adds; dark gradients cannot darken)
          bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
          bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
          break;
       case 5: // M22 premult, RGB-only write (preserve dst alpha)
          bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
          bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
          write_mask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
          break;
       case 6: // M22 replace, no blend (diagnostic: raw UI texel)
          bd.RenderTarget[0].BlendEnable = FALSE;
          break;
       case 0: // premultiplied alpha (default)
       default:
          bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
          bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
          break;
       }
       bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
       bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
       bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
       bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
       bd.RenderTarget[0].RenderTargetWriteMask = write_mask;
      if (FAILED(dev->CreateBlendState(&bd, blend.put())))
         return false;
   }
   DrawStateStack<DrawStateStackType::FullGraphics> cs;
   cs.Cache(ctx, device_data.uav_max_count);
   ID3D11UnorderedAccessView* null_uavs[D3D11_1_UAV_SLOT_COUNT] = {};
   ctx->CSSetUnorderedAccessViews(0, device_data.uav_max_count, null_uavs, nullptr);
   D3D11_VIEWPORT vp = {};
   vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
   vp.Width = (float)w; vp.Height = (float)h;
   vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
   ctx->RSSetViewports(1, &vp);
   ctx->RSSetState(gd.rs_copy.get());
   float bf[4] = { 0, 0, 0, 0 };
   ctx->OMSetBlendState(blend.get(), bf, 0xFFFFFFFF);
   ctx->OMSetDepthStencilState(nullptr, 0);
   ctx->OMSetRenderTargets(1, &dst_rtv, nullptr);
   ctx->VSSetShader(vs, nullptr, 0);
   ctx->PSSetShader(ps, nullptr, 0);
   ctx->PSSetShaderResources(0, 1, &ui_srv);
   ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
   ctx->Draw(4, 0);
   cs.Restore(ctx);
   return true;
}

// M39: differential matrix hunt (own-camera-MV groundwork). pSCAN shape
// matching only found animated per-object constants. But a VIEW matrix is
// constant across ALL producers within one frame -- so instead of judging
// single 4x4s by shape, capture VS CB0/CB1 (the 256B single-matrix CBs) at
// several producers and diff them: bit-identical across producers in the
// same frame = per-frame constant = view/viewproj candidate. FNV hash over
// exact bytes (same upload = identical bits; no epsilon needed).
static uint32_t Fnv1aBytes(const void* p, size_t n)
{
   const uint8_t* b = (const uint8_t*)p;
   uint32_t h = 2166136261u;
   for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
   return h;
}
struct PScanCap { uint32_t ps = 0; uint64_t frame = 0; float vs0[16] = {}; float vs1[16] = {}; bool has_vs0 = false; bool has_vs1 = false; };
static std::mutex g_pscan_mutex;
static PScanCap g_pscan_caps[8];
static int g_pscan_ncaps = 0;
static bool ReadCB16(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Buffer* cb, float out16[16])
{
   if (!dev || !ctx || !cb || !out16)
      return false;
   D3D11_BUFFER_DESC bd = {};
   cb->GetDesc(&bd);
   if (bd.ByteWidth < 64)
      return false;
   D3D11_BUFFER_DESC sd = {};
   sd.ByteWidth = bd.ByteWidth;
   sd.Usage = D3D11_USAGE_STAGING;
   sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   ComPtr<ID3D11Buffer> st;
   if (FAILED(dev->CreateBuffer(&sd, nullptr, st.put())))
      return false;
   ctx->CopySubresourceRegion(st.get(), 0, 0, 0, 0, cb, 0, nullptr);
   D3D11_MAPPED_SUBRESOURCE m = {};
   if (FAILED(ctx->Map(st.get(), 0, D3D11_MAP_READ, 0, &m)) || !m.pData)
      return false;
   memcpy(out16, m.pData, 64);
   ctx->Unmap(st.get(), 0);
   return true;
}
// M41: broad VS-CB matrix census (Path 1, upstream parity). pSCAN2 hunts
// at known velocity producers; this sniffs small VS CBs at EVERY pixel
// draw while armed (one-shot, ~1-2 frames of hitch) and ranks 4x4s by
// cross-shader sharing: a constant shared across shaders in one frame is
// a view/viewproj candidate. Shape tags: PROJ = upstream
// MatrixLikeProjection exactly; VIEW = last row (0,0,0,1) + sane rotation
// rows (upstream only tests projections -- views fail its m33~0 rule, so
// views need their own test or they stay invisible).
static std::atomic<bool> g_mscan_cb{ false };
struct MScanEntry { uint32_t hash = 0; int count = 0; uint32_t eg_ps = 0; int eg_slot = 0; int eg_off = 0; int shape = 0; float m[16] = {}; float m1[16] = {}; uint64_t f0 = 0, f1 = 0; uint32_t ps_b = 0, ps_c = 0; int nps = 0; }; // M43: m1 = last-seen values (pan-delta = camera score)
static MScanEntry g_mscan[256]; // M46: raised (panning fragments hashes)
static int g_mscan_n = 0;
static int g_mscan_sniffs = 0;
static uint64_t g_mscan_frame0 = 0; // M46: span start (sniff must cover panning)
static uint64_t g_mscan_lastframe = 0; // M46: throttle window
static int g_mscan_perframe = 0; // M46: sniffs used in g_mscan_lastframe
// M43: per-LOCATION tracker (slot 0-3 x 64B-offset 0-3). Content-hash entries
// fragment when the camera pans (new hash per value), so animation is tracked
// here instead: first vs last values + distinct-value count per VS-CB slot.
// PAN while sniffing: distinct>>1 + dmax>>0 = camera-animated VIEW matrix;
// distinct==1 + dmax~0 = static PROJ matrix. This is the own-MV feed pick.
struct MLocTrack { bool seen = false; float first[16] = {}; float last[16] = {}; int distinct = 0; uint32_t lasth = 0; uint32_t wlast = 0; };
static MLocTrack g_mloc[4][32]; // M49: full-CB offsets (translation hunt)
static void MScanResetLoc() { for (int s = 0; s < 4; s++) for (int o = 0; o < 32; o++) g_mloc[s][o] = MLocTrack(); }
static bool M44LikeProj(const float* m)
{
   auto nz = [](float v) { return v > -1e-3f && v < 1e-3f; };
   if (!nz(m[1]) || !nz(m[2]) || !nz(m[3])) return false;
   if (!nz(m[4]) || !nz(m[6]) || !nz(m[7])) return false;
   if (!(m[11] > 0.95f || m[11] < -0.95f)) return false;
   if (!nz(m[12]) || !nz(m[13]) || !nz(m[15])) return false;
   if ((m[10] > -1e-6f && m[10] < 1e-6f) && (m[14] > -1e-6f && m[14] < 1e-6f)) return false;
   return true;
}
static bool M44LikeView(const float* m)
{
   auto nz = [](float v) { return v > -1e-3f && v < 1e-3f; };
   if (!nz(m[12]) || !nz(m[13]) || !nz(m[14])) return false;
   if (m[15] < 0.999f || m[15] > 1.001f) return false;
   for (int r = 0; r < 3; r++)
   {
      float len2 = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2] + m[r * 4 + 3] * m[r * 4 + 3];
      if (len2 < 0.25f || len2 > 4.0f) return false;
   }
   return true;
}
static int M44ShapeTag(const float* m)
{
   float t[16];
   if (M44LikeProj(m) || M44LikeView(m)) return M44LikeProj(m) ? 1 : 2;
   for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) t[r * 4 + c] = m[c * 4 + r];
   if (M44LikeProj(t)) return 1;
   if (M44LikeView(t)) return 2;
   return 0;
}
// M34: scan one constant buffer for projection-shaped 4x4s (ROW + COL
// layouts). Shared by the compute-TAA scan and the velocity-pass scan.
static void ScanOneCBForProj(ID3D11Device* dev, ID3D11DeviceContext* ctx,
   const char* stage, int slot, ID3D11Buffer* cb)
{
   if (!dev || !ctx || !cb)
      return;
   D3D11_BUFFER_DESC cbd = {};
   cb->GetDesc(&cbd);
   char hb[128];
   snprintf(hb, sizeof(hb), "DaysGone cSCAN %sCB%d size=%u", stage, slot, cbd.ByteWidth);
   reshade::log::message(reshade::log::level::info, hb);
   if (cbd.ByteWidth < 64)
      return;
   D3D11_BUFFER_DESC sd = {};
   sd.ByteWidth = cbd.ByteWidth;
   sd.Usage = D3D11_USAGE_STAGING;
   sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   ComPtr<ID3D11Buffer> st;
   if (FAILED(dev->CreateBuffer(&sd, nullptr, st.put())))
      return;
   ctx->CopySubresourceRegion(st.get(), 0, 0, 0, 0, cb, 0, nullptr);
   D3D11_MAPPED_SUBRESOURCE cm = {};
   if (FAILED(ctx->Map(st.get(), 0, D3D11_MAP_READ, 0, &cm)) || !cm.pData)
      return;
   const float* cf = (const float*)cm.pData;
   int nf = cbd.ByteWidth / 4;
   auto cnz = [](float v, float e) { return v > -e && v < e; };
   for (int o = 0; o + 16 <= nf; o += 16)
   {
      for (int tr = 0; tr < 2; tr++)
      {
         float m[16];
         for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
               m[r * 4 + c] = (tr == 0) ? cf[o + r * 4 + c] : cf[o + c * 4 + r];
         float m20 = m[8], m21 = m[9], m22 = m[10], m23 = m[11];
         float m30 = m[12], m31 = m[13], m32 = m[14], m33 = m[15];
         bool rows = cnz(m[1], 1e-3f) && cnz(m[2], 1e-3f) && cnz(m[3], 1e-3f)
            && cnz(m[4], 1e-3f) && cnz(m[6], 1e-3f) && cnz(m[7], 1e-3f);
         bool persp = (m23 > 0.95f || m23 < -0.95f);
         bool lastrow = cnz(m30, 1e-3f) && cnz(m31, 1e-3f) && (!cnz(m22, 1e-6f) || !cnz(m32, 1e-6f));
         if (rows && persp && lastrow)
         {
            char b[512];
            snprintf(b, sizeof(b), "DaysGone cSCAN %sCB%d+%d %s PROJ? m20=%.5f m21=%.5f m22=%.5f m23=%.3f m32=%.5f m33=%.5f",
               stage, slot, o * 4, (tr == 0) ? "ROW" : "COL", m20, m21, m22, m23, m32, m33);
            reshade::log::message(reshade::log::level::info, b);
         }
      }
   }
   ctx->Unmap(st.get(), 0);
}

class DaysGoneGame final : public Game
{
public:
   void OnInit(bool async) override
   {
      if (g_marker_module != nullptr)
         WriteLoadMarker(g_marker_module, "on-init");

      std::vector<ShaderDefineData> game_shader_defines_data = {
         {"TONEMAP_TYPE", '1', true, false, "0 - Vanilla SDR\n1 - Luma HDR (Vanilla+)", 1},
      };
      shader_defines_data.append_range(game_shader_defines_data);

      GetShaderDefineData(POST_PROCESS_SPACE_TYPE_HASH).SetDefaultValue('0');
      GetShaderDefineData(EARLY_DISPLAY_ENCODING_HASH).SetDefaultValue('0');
      GetShaderDefineData(VANILLA_ENCODING_TYPE_HASH).SetDefaultValue('0');
      GetShaderDefineData(GAMMA_CORRECTION_TYPE_HASH).SetDefaultValue('1');
      GetShaderDefineData(UI_DRAW_TYPE_HASH).SetDefaultValue('0');

      // M1: no custom passes, disable per-pass cbuffers.
      luma_settings_cbuffer_index = 13;
      luma_data_cbuffer_index = -1;
      luma_ui_cbuffer_index = -1;

#if TEST || DEVELOPMENT
      custom_shaders_enabled = false;
#endif
      auto_dump = false; // M43: dump OFF (set true for M2-style captures)

      cb_luma_global_settings.GameSettings.GameSetting01 = 0.5f;
      cb_luma_global_settings.GameSettings.GameSetting02 = 33;

      // M9: DLSS copy pixel shaders (RGBA + BGRA-swap variant for the
      // game's BGR-ordered data -- sky renders pink = R/B swapped).
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone DLSS Copy PS"),
         ShaderDefinition{"Luma_DaysGone_OutputCopy", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"SWAP_RB", "0"}}});
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone DLSS Copy BGRA PS"),
         ShaderDefinition{"Luma_DaysGone_OutputCopy", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"SWAP_RB", "1"}}});
      // M9f: sRGB-encode variant replicating the native TAAU tail.
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone DLSS sRGB PS"),
         ShaderDefinition{"Luma_DaysGone_OutputCopy", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"SRGB_ENCODE", "1"}}});
      // M10e: UI composite variant -- preserves alpha (opaque-scene copies
      // zero it for native parity, which made the alpha-blend composite a
      // silent no-op: src alpha 0 = 100% dest).
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone UI Copy PS"),
         ShaderDefinition{"Luma_DaysGone_OutputCopy", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"KEEP_ALPHA", "1"}}});
      // M11: keep-alpha + R/B swizzle (compute-TAA output copy triage).
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone UI Copy BGRA PS"),
         ShaderDefinition{"Luma_DaysGone_OutputCopy", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"KEEP_ALPHA", "1"}, {"SWAP_RB", "1"}}});
      // M20: keep-alpha + swizzle + sRGB (native-matching u0 output).
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone UI Encode BGRA PS"),
         ShaderDefinition{"Luma_DaysGone_OutputCopy", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"KEEP_ALPHA", "1"}, {"SWAP_RB", "1"}, {"SRGB_ENCODE", "1"}}});
      // M22: forced-opaque scene-copy variants (dark-gradient triage).
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone UI Copy AlphaOne PS"),
         ShaderDefinition{"Luma_DaysGone_OutputCopy", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"ALPHA_ONE", "1"}}});
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone UI Copy BGRA AlphaOne PS"),
         ShaderDefinition{"Luma_DaysGone_OutputCopy", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"ALPHA_ONE", "1"}, {"SWAP_RB", "1"}}});
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone UI Encode AlphaOne PS"),
         ShaderDefinition{"Luma_DaysGone_OutputCopy", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"ALPHA_ONE", "1"}, {"SRGB_ENCODE", "1"}}});
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone UI Encode BGRA AlphaOne PS"),
         ShaderDefinition{"Luma_DaysGone_OutputCopy", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"ALPHA_ONE", "1"}, {"SWAP_RB", "1"}, {"SRGB_ENCODE", "1"}}});
      // M10i: sRGB-decode variant -- t0 is sRGB LDR, DLSS output is linear HDR.
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone UI sRGB Decode PS"),
         ShaderDefinition{"Luma_DaysGone_OutputCopy", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"KEEP_ALPHA", "1"}, {"SRGB_DECODE", "1"}}});
      // M10j: tone-map variant -- Reinhard + sRGB encode for LDR composite.
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone Tonemap PS"),
         ShaderDefinition{"Luma_DaysGone_OutputCopy", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"KEEP_ALPHA", "1"}, {"UI_TM", "1"}, {"SRGB_ENCODE", "1"}}});
      // M17: ACES + Hable variants for the UI blend.
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone UI ACES PS"),
         ShaderDefinition{"Luma_DaysGone_OutputCopy", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"KEEP_ALPHA", "1"}, {"UI_TM", "2"}, {"SRGB_ENCODE", "1"}}});
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone UI Hable PS"),
         ShaderDefinition{"Luma_DaysGone_OutputCopy", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"KEEP_ALPHA", "1"}, {"UI_TM", "3"}, {"SRGB_ENCODE", "1"}}});
      // M18: encode-only variant (linear HUD buffer hypothesis).
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone UI Encode PS"),
         ShaderDefinition{"Luma_DaysGone_OutputCopy", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"KEEP_ALPHA", "1"}, {"SRGB_ENCODE", "1"}}});
      // M24/M30: MV decode variants. Bytecode proof (BE0130E5 immediates
      // 0.2495/0.499992/4.008016/-2.003978 = exact UE round-trip pair):
      // stock UE 0.5-centered encoding, same as upstream
      // Luma_MotionVec_UE4_Decode. DECODE 1 = upstream formula (DEFAULT);
      // DECODE 2 = 0.25-centered fallback (keep for A/B only).
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone MV Convert PS"),
         ShaderDefinition{"Luma_DaysGone_MVConvert", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"DECODE", "1"}}});
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone MV Convert 025 PS"),
         ShaderDefinition{"Luma_DaysGone_MVConvert", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"DECODE", "2"}}});
      // M32: zero-snap (cleared-bg) + negated (sign triage) variants.
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone MV Convert Zero PS"),
         ShaderDefinition{"Luma_DaysGone_MVConvert", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"DECODE", "3"}}});
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone MV Convert Neg PS"),
         ShaderDefinition{"Luma_DaysGone_MVConvert", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"DECODE", "4"}}});
      // M33: zero-snap + 0.25-centered (mode 3 fixed stationary, motion did
      // not follow -- motion content may be (v+1)*0.25 while clear is 0).
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone MV Convert Zero025 PS"),
         ShaderDefinition{"Luma_DaysGone_MVConvert", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"DECODE", "5"}}});
       // M34: negated + zero-snap (mode 4 could never test sign -- bg bias).
       native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone MV Convert NegZero PS"),
          ShaderDefinition{"Luma_DaysGone_MVConvert", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"DECODE", "6"}}});
       // M37: single-axis flips + zero-snap (modes 3/6 prove 0.5-centered
       // bulk but both full-sign options shimmer -- the sign matrix was
       // missing Y-only and X-only; 8 mirrors upstream -delta*(0.5,-0.5)).
       native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone MV Convert YNegZero PS"),
          ShaderDefinition{"Luma_DaysGone_MVConvert", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"DECODE", "7"}}});
       native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone MV Convert XNegZero PS"),
          ShaderDefinition{"Luma_DaysGone_MVConvert", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"DECODE", "8"}}});
       // Hybrid MVs (M11/compute only): game truth + own fill. Same DECODE
       // variants as MVConvert, mirrored key names.
       native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone MV Hybrid PS"),
          ShaderDefinition{"Luma_DaysGone_MVHybrid", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"DECODE", "1"}}});
       native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone MV Hybrid 025 PS"),
          ShaderDefinition{"Luma_DaysGone_MVHybrid", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"DECODE", "2"}}});
       native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone MV Hybrid Zero PS"),
          ShaderDefinition{"Luma_DaysGone_MVHybrid", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"DECODE", "3"}}});
       native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone MV Hybrid Neg PS"),
          ShaderDefinition{"Luma_DaysGone_MVHybrid", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"DECODE", "4"}}});
       native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone MV Hybrid Zero025 PS"),
          ShaderDefinition{"Luma_DaysGone_MVHybrid", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"DECODE", "5"}}});
       native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone MV Hybrid NegZero PS"),
          ShaderDefinition{"Luma_DaysGone_MVHybrid", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"DECODE", "6"}}});
       native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone MV Hybrid YNegZero PS"),
          ShaderDefinition{"Luma_DaysGone_MVHybrid", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"DECODE", "7"}}});
       native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone MV Hybrid XNegZero PS"),
          ShaderDefinition{"Luma_DaysGone_MVHybrid", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"DECODE", "8"}}});
       // M47: own rotation-exact camera MVs (stash-fed view matrices).
       native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone OwnMV PS"),
          ShaderDefinition{"Luma_DaysGone_OwnMV", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"OWN_NEG", "0"}, {"FULLMODE", "0"}}});
       native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone OwnMV Neg PS"),
          ShaderDefinition{"Luma_DaysGone_OwnMV", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"OWN_NEG", "1"}, {"FULLMODE", "0"}}});
       // M51: full-6DOF variants (FULLMODE 1 = V-row T, 2 = campos T).
       native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone OwnMV FullA PS"),
          ShaderDefinition{"Luma_DaysGone_OwnMV", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"OWN_NEG", "0"}, {"FULLMODE", "1"}}});
       native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone OwnMV FullB PS"),
          ShaderDefinition{"Luma_DaysGone_OwnMV", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"OWN_NEG", "0"}, {"FULLMODE", "2"}}});
      // M9h: single-channel viewer variants (raw, no encode).
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone View R PS"),
         ShaderDefinition{"Luma_DaysGone_View", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"VIEWCHAN", "0"}}});
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone View G PS"),
         ShaderDefinition{"Luma_DaysGone_View", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"VIEWCHAN", "1"}}});
      native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone View B PS"),
         ShaderDefinition{"Luma_DaysGone_View", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"VIEWCHAN", "2"}}});
       native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone View A PS"),
          ShaderDefinition{"Luma_DaysGone_View", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"VIEWCHAN", "3"}}});
       // M48: depth near-white (1-R) for reversed-Z buffers.
       native_shaders_definitions.emplace(CompileTimeStringHash("DaysGone View DepthN PS"),
          ShaderDefinition{"Luma_DaysGone_View", reshade::api::pipeline_subobject_type::pixel_shader, nullptr, nullptr, {{"VIEWCHAN", "4"}}});
   }

   void OnCreateDevice(ID3D11Device* native_device, DeviceData& device_data) override
   {
      device_data.game = new DaysGoneDeviceData;
   }

   void OnDestroyDeviceData(DeviceData& device_data) override
   {
      delete device_data.game;
      device_data.game = nullptr;
   }

   DrawOrDispatchOverrideType OnDrawOrDispatch(
      ID3D11Device* native_device,
      ID3D11DeviceContext* native_device_context,
      CommandListData& cmd_list_data,
      DeviceData& device_data,
      reshade::api::shader_stage stages,
      const ShaderHashesList<OneShaderPerPipeline>& original_shader_hashes,
      bool is_custom_pass,
      bool& updated_cbuffers,
      std::function<void()>* original_draw_dispatch_func) override
   {
      g_draws_this_frame.fetch_add(1, std::memory_order_relaxed);

      // Histogram every draw/dispatch (pixel + compute ??" 4.10 TAA may be either).
      // Split the unknowns: empty PS list (depth-only) vs FFFFFFFF (unresolved).
      // Unguarded: the freezer menu needs these in Publishing too.
      if (!is_custom_pass)
      {
         uint32_t h = 0;
         bool is_compute = (stages == reshade::api::shader_stage::all_compute);
         if (is_compute)
            h = original_shader_hashes.compute_shaders.empty() ? 0 : original_shader_hashes.compute_shaders[0];
         else
            h = original_shader_hashes.pixel_shaders.empty() ? 0 : original_shader_hashes.pixel_shaders[0];
         if (h == 0)
         {
            if (!is_compute)
               g_ps_empty.fetch_add(1, std::memory_order_relaxed);
         }
         else if (h == 0xFFFFFFFF)
         {
            if (!is_compute)
               g_ps_unresolved.fetch_add(1, std::memory_order_relaxed);
         }
         else
         {
            std::lock_guard<std::mutex> lk(g_hist_mutex);
            if (is_compute)
               g_cs_hist[h]++;
            else
               g_ps_hist[h]++;
         }
      }

#if TEST || DEVELOPMENT
      // M4 input capture (armed from menu, 3 presents, then auto-stops).
      if (!is_custom_pass && g_cap_frames_left.load(std::memory_order_relaxed) > 0 &&
          stages != reshade::api::shader_stage::all_compute)
      {
         uint32_t ps = original_shader_hashes.pixel_shaders.empty() ? 0 : original_shader_hashes.pixel_shaders[0];
         WriteCapLine(g_marker_module, g_hist_frame.load(std::memory_order_relaxed), ps, native_device_context);
      }
      // M7e: compute capture while armed (UAV outputs + SRV inputs).
      if (!is_custom_pass && g_cap_frames_left.load(std::memory_order_relaxed) > 0 &&
          stages == reshade::api::shader_stage::all_compute)
      {
         uint32_t cs = original_shader_hashes.compute_shaders.empty() ? 0 : original_shader_hashes.compute_shaders[0];
         WriteCapCSLine(g_marker_module, g_hist_frame.load(std::memory_order_relaxed), cs, native_device_context);
      }
#endif

       // M39: differential velocity-pass CB scan (masterless). The shape
       // matcher (below, kept) only finds animated per-object constants --
       // view matrices are constant across producers WITHIN one frame, so
       // capture VS CB0/CB1 at up to 8 distinct producers, then diff: a
       // 4x4 bit-identical across producers in the same frame is a
       // per-frame constant = view/viewproj candidate for the own-MV
       // generator. Disarms after 4 distinct producers (same-frame compare
       // is the goal; cross-frame pairs are flagged, camera may move).
       if (!is_custom_pass && stages != reshade::api::shader_stage::all_compute &&
           g_pscan_cb.load(std::memory_order_relaxed))
       {
          uint32_t wps = original_shader_hashes.pixel_shaders.empty() ? 0 : original_shader_hashes.pixel_shaders[0];
          bool is_mv = (wps == 0x1E94CABC || wps == 0x5846E9DA || wps == 0x6C6AD505 ||
             wps == 0xA1A256FA || wps == 0xA5CB30BF || wps == 0xBE0130E5);
          if (is_mv && native_device_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE)
          {
             bool finalize = false;
             {
                std::lock_guard<std::mutex> plk(g_pscan_mutex);
                bool seen = false;
                for (int i = 0; i < g_pscan_ncaps; i++)
                   if (g_pscan_caps[i].ps == wps) { seen = true; break; }
                if (!seen && g_pscan_ncaps < 8)
                {
                   PScanCap& c = g_pscan_caps[g_pscan_ncaps++];
                   c.ps = wps;
                   c.frame = g_hist_frame.load(std::memory_order_relaxed);
                   ID3D11Buffer* v0 = nullptr;
                   native_device_context->VSGetConstantBuffers(0, 1, &v0);
                   if (v0) { c.has_vs0 = ReadCB16(native_device, native_device_context, v0, c.vs0); v0->Release(); }
                   ID3D11Buffer* v1 = nullptr;
                   native_device_context->VSGetConstantBuffers(1, 1, &v1);
                   if (v1) { c.has_vs1 = ReadCB16(native_device, native_device_context, v1, c.vs1); v1->Release(); }
                   char pb[128];
                   snprintf(pb, sizeof(pb), "DaysGone pSCAN2 cap %d/4 producer %08X frame=%llu vs0=%d vs1=%d",
                      g_pscan_ncaps, wps, (unsigned long long)c.frame, (int)c.has_vs0, (int)c.has_vs1);
                   reshade::log::message(reshade::log::level::info, pb);
                }
                if (g_pscan_ncaps >= 4)
                {
                   finalize = true;
                   g_pscan_cb.store(false, std::memory_order_relaxed);
                }
             }
             if (finalize)
             {
                // Pairwise diff under the same lock scope: same-frame
                // identical 4x4s are the candidates; log full values.
                std::lock_guard<std::mutex> plk(g_pscan_mutex);
                for (int s = 0; s < 2; s++)
                {
                   const char* sname = (s == 0) ? "VS0" : "VS1";
                   char hb[512];
                   int pos = snprintf(hb, sizeof(hb), "DaysGone pSCAN2 %s hashes:", sname);
                   uint32_t hh[8] = {};
                   for (int i = 0; i < g_pscan_ncaps && pos < (int)sizeof(hb) - 32; i++)
                   {
                      bool has = (s == 0) ? g_pscan_caps[i].has_vs0 : g_pscan_caps[i].has_vs1;
                      const float* m = (s == 0) ? g_pscan_caps[i].vs0 : g_pscan_caps[i].vs1;
                      hh[i] = has ? Fnv1aBytes(m, 64) : 0;
                      pos += snprintf(hb + pos, sizeof(hb) - pos, " %08X=%08X%s",
                         g_pscan_caps[i].ps, hh[i], (g_pscan_caps[i].frame == g_pscan_caps[0].frame) ? "" : "(DIFF-FRAME)");
                   }
                   reshade::log::message(reshade::log::level::info, hb);
                   for (int i = 0; i < g_pscan_ncaps; i++)
                   {
                      if (!((s == 0) ? g_pscan_caps[i].has_vs0 : g_pscan_caps[i].has_vs1) || hh[i] == 0)
                         continue;
                      // Find all same-frame partners sharing this hash.
                      char grp[256];
                      int gp = snprintf(grp, sizeof(grp), "DaysGone pSCAN2 SHARED %s", sname);
                      int members = 0;
                      for (int j = i; j < g_pscan_ncaps && gp < (int)sizeof(grp) - 16; j++)
                      {
                         bool hasj = (s == 0) ? g_pscan_caps[j].has_vs0 : g_pscan_caps[j].has_vs1;
                         if (!hasj || hh[j] != hh[i] || g_pscan_caps[j].frame != g_pscan_caps[i].frame)
                            continue;
                         // Skip if already reported as part of an earlier group.
                         bool dup = false;
                         for (int k = i; k < j; k++)
                         {
                            bool hask = (s == 0) ? g_pscan_caps[k].has_vs0 : g_pscan_caps[k].has_vs1;
                            if (hask && hh[k] == hh[i] && g_pscan_caps[k].frame == g_pscan_caps[i].frame) { dup = true; break; }
                         }
                         if (dup)
                            continue;
                         gp += snprintf(grp + gp, sizeof(grp) - gp, " %08X", g_pscan_caps[j].ps);
                         members++;
                      }
                      if (members >= 2)
                      {
                         reshade::log::message(reshade::log::level::info, grp);
                         const float* m = (s == 0) ? g_pscan_caps[i].vs0 : g_pscan_caps[i].vs1;
                         char vb[512];
                         snprintf(vb, sizeof(vb),
                            "DaysGone pSCAN2 CANDIDATE %s hash=%08X frame=%llu [%.5f %.5f %.5f %.5f / %.5f %.5f %.5f %.5f / %.5f %.5f %.5f %.5f / %.5f %.5f %.5f %.5f]",
                            sname, hh[i], (unsigned long long)g_pscan_caps[i].frame,
                            m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                            m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
                         reshade::log::message(reshade::log::level::info, vb);
                      }
                   }
                }
                g_pscan_ncaps = 0;
             }
             // Legacy shape scan at the first producer only (sizes + PROJ?
             // reference); the differential pSCAN2 result is the real hunt.
             static bool legacy_done = false;
             if (!legacy_done)
             {
                legacy_done = true;
                char pb[96];
                snprintf(pb, sizeof(pb), "DaysGone pSCAN at producer %08X", wps);
                reshade::log::message(reshade::log::level::info, pb);
                for (int slot = 0; slot < 8; slot++)
                {
                   ID3D11Buffer* vcb = nullptr;
                   native_device_context->VSGetConstantBuffers(slot, 1, &vcb);
                   if (vcb)
                   {
                      ScanOneCBForProj(native_device, native_device_context, "VS", slot, vcb);
                      vcb->Release();
                   }
                   ID3D11Buffer* pcb = nullptr;
                   native_device_context->PSGetConstantBuffers(slot, 1, &pcb);
                   if (pcb)
                   {
                      ScanOneCBForProj(native_device, native_device_context, "PS", slot, pcb);
                      pcb->Release();
                   }
                }
             }
             else if (!g_pscan_cb.load(std::memory_order_relaxed))
                legacy_done = false; // re-arm with the next scan request
           }
        }

        // M41: one-shot VS-CB census (masterless). While armed, sniff small
        // VS CBs (256/512/1024/2048B) at pixel draws: read back the
        // first 256B, test each 64B-aligned 4x4 (shape-tagged), dedup by
        // exact hash. M44: spans ~240 presents (~4s) so the window covers
        // camera PANNING -- the user must pan while it runs; a still camera
        // leaves every matrix static and the view/proj pick is impossible.
        // Shared across 2+ shaders with 3+ sightings = view-matrix CANDIDATE;
        // per-location first/last (LOC lines) = animation proof.
        if (!is_custom_pass && stages != reshade::api::shader_stage::all_compute &&
            g_mscan_cb.load(std::memory_order_relaxed) &&
            native_device_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE)
        {
           uint32_t mps = original_shader_hashes.pixel_shaders.empty() ? 0 : original_shader_hashes.pixel_shaders[0];
           uint64_t mframe = g_hist_frame.load(std::memory_order_relaxed);
           if (mframe != g_mscan_lastframe) { g_mscan_lastframe = mframe; g_mscan_perframe = 0; }
           int drew = 0;
           // M46: throttle to 4 sniffs/frame so the ~240-frame window really
           // spans the pan (m44 burned all 2048 sniffs in frame 1, then the
           // span watched nothing: every entry froze at f0==f1).
           for (int slot = 0; slot < 4 && g_mscan_sniffs < 2048 && drew < 2 && g_mscan_perframe < 4; slot++)
          {
             ID3D11Buffer* mcb = nullptr;
             native_device_context->VSGetConstantBuffers(slot, 1, &mcb);
             if (!mcb)
                continue;
             D3D11_BUFFER_DESC mbd = {};
             mcb->GetDesc(&mbd);
              bool is_small_cb = (mbd.ByteWidth == 256 || mbd.ByteWidth == 512 || mbd.ByteWidth == 1024 || mbd.ByteWidth == 2048);
              if (!is_small_cb)
             {
                mcb->Release();
                continue;
             }
             D3D11_BUFFER_DESC msd = {};
             msd.ByteWidth = mbd.ByteWidth;
             msd.Usage = D3D11_USAGE_STAGING;
             msd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
             ComPtr<ID3D11Buffer> mst;
             bool ok = SUCCEEDED(native_device->CreateBuffer(&msd, nullptr, mst.put()));
             if (ok)
             {
                native_device_context->CopySubresourceRegion(mst.get(), 0, 0, 0, 0, mcb, 0, nullptr);
                D3D11_MAPPED_SUBRESOURCE mm = {};
                ok = SUCCEEDED(native_device_context->Map(mst.get(), 0, D3D11_MAP_READ, 0, &mm)) && mm.pData;
                if (ok)
                {
                    drew++;
                    g_mscan_sniffs++;
                    g_mscan_perframe++; // M46: throttle budget
                    const float* mf = (const float*)mm.pData;
                    int n4 = mbd.ByteWidth / 64; // M49: full CB (translation may live deep)
                    if (n4 > 32) n4 = 32; if (n4 < 0) n4 = 0;
                   for (int o = 0; o < n4; o++)
                   {
                      const float* m = mf + o * 16;
                      uint32_t h = Fnv1aBytes(m, 64);
                      if (h == 0)
                         continue; // all-zero padding, no signal
                      int tag = M44ShapeTag(m);
                      MScanEntry* e = nullptr;
                      for (int i = 0; i < g_mscan_n; i++)
                         if (g_mscan[i].hash == h) { e = &g_mscan[i]; break; }
                       if (!e)
                       {
                          if (g_mscan_n >= 256)
                             continue;
                         e = &g_mscan[g_mscan_n++];
                         e->hash = h;
                         e->eg_ps = mps; e->eg_slot = slot; e->eg_off = o * 64;
                         e->shape = tag;
                         memcpy(e->m, m, 64);
                         e->f0 = mframe;
                      }
                       e->count++;
                       e->f1 = mframe;
                       memcpy(e->m1, m, 64); // M43: track last values for pan-delta
                       if (mps != e->eg_ps && mps != e->ps_b && mps != e->ps_c)
                       {
                          if (e->ps_b == 0) e->ps_b = mps;
                          else if (e->ps_c == 0) e->ps_c = mps;
                          e->nps++;
                       }
                       // M43: location track (pan-proof): same slot+offset
                       // across sniffs, first vs last values.
                       {
                          MLocTrack& lt = g_mloc[slot][o];
                          if (!lt.seen) { lt.seen = true; memcpy(lt.first, m, 64); memcpy(lt.last, m, 64); lt.distinct = 1; lt.lasth = h; }
                          else
                          {
                             memcpy(lt.last, m, 64);
                             if (h != lt.lasth) { lt.lasth = h; lt.distinct++; }
                          }
                          lt.wlast = mbd.ByteWidth;
                       }
                   }
                   native_device_context->Unmap(mst.get(), 0);
                }
             }
             mcb->Release();
          }
           if (g_mscan_sniffs >= 48 && (mframe - g_mscan_frame0) >= 240)
           {
              g_mscan_cb.store(false, std::memory_order_relaxed);
             char mh[128];
             snprintf(mh, sizeof(mh), "DaysGone MSCAN done sniffs=%d distinct=%d", g_mscan_sniffs, g_mscan_n);
             reshade::log::message(reshade::log::level::info, mh);
             const char* shapes[] = { "other", "PROJ", "VIEW" };
             for (int i = 0; i < g_mscan_n; i++)
             {
                MScanEntry& e = g_mscan[i];
                if (e.count < 2)
                   continue;
                int sh = e.shape < 0 || e.shape > 2 ? 0 : e.shape;
                char ml[256];
                snprintf(ml, sizeof(ml), "DaysGone MSCAN %s hash=%08X n=%d nps=%d frames=%llu-%llu e.g. ps=%08X vs%d+%d",
                   shapes[sh], e.hash, e.count, e.nps + 1,
                   (unsigned long long)e.f0, (unsigned long long)e.f1,
                   e.eg_ps, e.eg_slot, e.eg_off);
                reshade::log::message(reshade::log::level::info, ml);
                if (e.nps >= 1 && e.count >= 3)
                 {
                    char mv[512];
                    snprintf(mv, sizeof(mv),
                       "DaysGone MSCAN CANDIDATE %s hash=%08X [%.5f %.5f %.5f %.5f / %.5f %.5f %.5f %.5f / %.5f %.5f %.5f %.5f / %.5f %.5f %.5f %.5f]",
                       shapes[sh], e.hash,
                       e.m[0], e.m[1], e.m[2], e.m[3], e.m[4], e.m[5], e.m[6], e.m[7],
                       e.m[8], e.m[9], e.m[10], e.m[11], e.m[12], e.m[13], e.m[14], e.m[15]);
                    reshade::log::message(reshade::log::level::info, mv);
                    // M43: last-seen values + max element delta first->last.
                    // PAN while sniffing: dmax>>0 = camera-animated (VIEW pick
                    // for the own-MV generator), dmax~0 = static (PROJ pick).
                    float dmax = 0.0f;
                    for (int k = 0; k < 16; k++)
                    {
                       float dd = e.m1[k] - e.m[k]; if (dd < 0) dd = -dd;
                       if (dd > dmax) dmax = dd;
                    }
                    char ml2[512];
                    snprintf(ml2, sizeof(ml2),
                       "DaysGone MSCAN CAND-LAST hash=%08X dmax=%.5f [%.5f %.5f %.5f %.5f / %.5f %.5f %.5f %.5f / %.5f %.5f %.5f %.5f / %.5f %.5f %.5f %.5f]",
                       e.hash, dmax,
                       e.m1[0], e.m1[1], e.m1[2], e.m1[3], e.m1[4], e.m1[5], e.m1[6], e.m1[7],
                       e.m1[8], e.m1[9], e.m1[10], e.m1[11], e.m1[12], e.m1[13], e.m1[14], e.m1[15]);
                    reshade::log::message(reshade::log::level::info, ml2);
                 }
              }
              // M43: per-location animation report (pan-proof view/proj pick).
              for (int ls = 0; ls < 4; ls++) for (int lo = 0; lo < 32; lo++)
              {
                 MLocTrack& lt = g_mloc[ls][lo];
                 if (!lt.seen)
                    continue;
                 float ldmax = 0.0f;
                 for (int k = 0; k < 16; k++)
                 {
                    float dd = lt.last[k] - lt.first[k]; if (dd < 0) dd = -dd;
                    if (dd > ldmax) ldmax = dd;
                 }
                 char ll[1024];
                 snprintf(ll, sizeof(ll),
                    "DaysGone MSCAN LOC vs%d+%d w=%u distinct=%d dmax=%.5f first=[%.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f] last=[%.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f]",
                    ls, lo * 64, (unsigned)lt.wlast, lt.distinct, ldmax,
                    lt.first[0], lt.first[1], lt.first[2], lt.first[3], lt.first[4], lt.first[5], lt.first[6], lt.first[7],
                    lt.first[8], lt.first[9], lt.first[10], lt.first[11], lt.first[12], lt.first[13], lt.first[14], lt.first[15],
                    lt.last[0], lt.last[1], lt.last[2], lt.last[3], lt.last[4], lt.last[5], lt.last[6], lt.last[7],
                    lt.last[8], lt.last[9], lt.last[10], lt.last[11], lt.last[12], lt.last[13], lt.last[14], lt.last[15]);
                 reshade::log::message(reshade::log::level::info, ll);
              }
           }
       }

       // M7b: depth cache -- grab any large DSV while DLSS master is on (the
       // TAAU slot binds no depth itself; the HDR post family does).
      // M7c: also stamp every RTV writer (handle -> present) for the
      // current-frame color pick at the slot.
      // M11: depth/MV/HDR caches also feed the compute path -- run when EITHER
      // DLSS master is on.
      if (!is_custom_pass && stages != reshade::api::shader_stage::all_compute &&
          (g_dlss_master.load(std::memory_order_relaxed) || g_cdlss_master.load(std::memory_order_relaxed)))
      {
         uint64_t cur_frame = g_hist_frame.load(std::memory_order_relaxed);
         ID3D11RenderTargetView* wrtv = nullptr;
         ID3D11DepthStencilView* dsv = nullptr;
         native_device_context->OMGetRenderTargets(1, &wrtv, &dsv);
         if (wrtv)
         {
            ID3D11Resource* tmp_rt = nullptr;
            wrtv->GetResource(&tmp_rt);
            if (tmp_rt)
            {
               ID3D11Texture2D* rtex = nullptr;
               if (SUCCEEDED(tmp_rt->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&rtex))) && rtex)
               {
                  D3D11_TEXTURE2D_DESC rd = {};
                  rtex->GetDesc(&rd);
                  rtex->Release();
                  auto& gd = *static_cast<DaysGoneDeviceData*>(device_data.game);
                  // M35: velocity cache -- format+depth gated. The 6-hash
                  // gate was mode-fragile: BE0130E5 is absent from entire
                  // captures and object-velocity shaders vary per scene,
                  // so ALL named sources read STALE in gameplay while DLSS
                  // runs on zero MVs (runs climb, motion shimmers).
                  // Velocity passes are the ONLY fullscreen fmt35
                  // (R16G16_UNORM) writers WITH the scene depth bound
                  // (capture 51950-52: every producer writes
                  // RT:1920x1080:35 + D:1920x1080:44). Bloom/LUT scratch
                  // never binds scene depth, so this can't mis-cache the
                  // way the deleted RG16-format fallback did. The per-hash
                  // map is still filled keyed by the ACTUAL writer, so the
                  // first-Draw mvhash line + manual selector stay truthful;
                  // unknown hashes simply feed Auto (latest-wins).
                  uint32_t wps = original_shader_hashes.pixel_shaders.empty() ? 0 : original_shader_hashes.pixel_shaders[0];
                  bool has_scene_depth = false;
                  if (dsv)
                  {
                     ID3D11Resource* dt = nullptr;
                     dsv->GetResource(&dt);
                     if (dt)
                     {
                        ID3D11Texture2D* dtex = nullptr;
                        if (SUCCEEDED(dt->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&dtex))) && dtex)
                        {
                           D3D11_TEXTURE2D_DESC dd = {};
                           dtex->GetDesc(&dd);
                           dtex->Release();
                           has_scene_depth = (dd.Width >= 400 && dd.Height >= 200); // M55: was 1000x500 (blocks 50% scale)
                        }
                        dt->Release();
                     }
                  }
                   bool is_mv_fmt = (rd.Width >= 400 && rd.Height >= 200 && // M55: was 1000x500
                      rd.Format == DXGI_FORMAT_R16G16_UNORM);
                  if (is_mv_fmt && has_scene_depth)
                  {
                     gd.cached_mvs.attach(tmp_rt);
                     tmp_rt = nullptr;
                     gd.cached_mvs_frame = cur_frame;
                     gd.cached_mvs_hash = wps;
                     std::lock_guard<std::mutex> mvlk(g_mvmap_mutex);
                     MvBuf& mvslot27 = g_mv_by_hash[wps];
                     mvslot27.res = gd.cached_mvs;
                     mvslot27.frame = cur_frame;
                  }
                  // M9i: HDR linear color (fmt10 RGBA16F) for the DLSS feed.
                   else if (rd.Width >= 400 && rd.Format == static_cast<DXGI_FORMAT>(10)) // M55: was 1000
                  {
                     gd.cached_hdr.attach(tmp_rt);
                     tmp_rt = nullptr;
                     gd.cached_hdr_w = rd.Width; gd.cached_hdr_h = rd.Height;
                     gd.cached_hdr_frame = cur_frame;
                  }
                  else
                  {
                     std::lock_guard<std::mutex> lk(g_writer_mutex);
                     g_rt_last_writer[(uint64_t)tmp_rt] = cur_frame;
                     if (g_rt_last_writer.size() > 4096)
                        g_rt_last_writer.clear(); // transient pool hygiene
                  }
               }
               if (tmp_rt) tmp_rt->Release();
            }
            wrtv->Release();
         }
         if (dsv)
         {
            ID3D11Resource* tmp = nullptr;
            dsv->GetResource(&tmp);
            if (tmp)
            {
               ID3D11Texture2D* tex = nullptr;
               if (SUCCEEDED(tmp->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex)
               {
                  D3D11_TEXTURE2D_DESC d = {};
                  tex->GetDesc(&d);
                  tex->Release();
                   if (d.Width >= 400 && d.Height >= 200) // M55: was 1000x500
                   {
                      // depthfilter: same principle as viewfilter -- only cache
                      // depth when a gameplay-sized color RT is bound ALONGSIDE
                      // the DSV. Depth-only (shadow/cascade) or small-RT draws
                      // leave the last fresh cache untouched (depth capture has
                      // no perfcap -- just skip the store).
                      bool depthGameplay = false;
                      {
                         ID3D11RenderTargetView* crtv = nullptr;
                         native_device_context->OMGetRenderTargets(1, &crtv, nullptr);
                         if (crtv)
                         {
                            ID3D11Resource* cres = nullptr;
                            crtv->GetResource(&cres);
                            if (cres)
                            {
                               ID3D11Texture2D* ctex = nullptr;
                               if (SUCCEEDED(cres->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&ctex))) && ctex)
                               {
                                  D3D11_TEXTURE2D_DESC crd = {};
                                  ctex->GetDesc(&crd);
                                  depthGameplay = (crd.Width >= 400 && crd.Height >= 200);
                                  ctex->Release();
                               }
                               cres->Release();
                            }
                            crtv->Release();
                         }
                      }
                      if (depthGameplay)
                      {
                      auto& gd = *static_cast<DaysGoneDeviceData*>(device_data.game);
                      gd.cached_depth.attach(tmp);
                     tmp = nullptr;
                     gd.cached_depth_frame = g_hist_frame.load(std::memory_order_relaxed);
                      }
                      else
                         g_depth_cache_skips.fetch_add(1, std::memory_order_relaxed);
                   }
               }
               if (tmp) tmp->Release();
            }
            dsv->Release();
         }
      }

       // M47: own-MV view stash (1 CB readback/frame, M11 master only).
       // Gameplay view rotation = VS CB1 (2048B) +128 (row-major,
       // orthonormal; VP at +0). M51: 768B incl. +640 translation row
       // (full 6DOF). Shape-validated; cur+prev frames feed the
       // generator at the compute slot.
       if (!is_custom_pass && g_cdlss_master.load(std::memory_order_relaxed) &&
           native_device_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE &&
           device_data.game)
       {
          auto& gdm = *static_cast<DaysGoneDeviceData*>(device_data.game);
          uint64_t vsframe = g_hist_frame.load(std::memory_order_relaxed);
          if (gdm.view_frame_cur != vsframe)
          {
             ID3D11Buffer* vcb = nullptr;
             native_device_context->VSGetConstantBuffers(1, 1, &vcb);
             if (vcb)
             {
                D3D11_BUFFER_DESC vbd = {};
                vcb->GetDesc(&vbd);
                 if (vbd.ByteWidth == 2048)
                 {
                     // viewfilter: RT-identity gate. The pool is built from the
                     // first qualifying draws, so shadow/cascade/static views
                     // (byte-identical across frames) fill it and the moving
                     // gameplay view drawn later never enters. Only pool draws
                     // targeting a gameplay-sized RT; the rest skip pooling
                     // (no cap counting, committed untouched).
                     bool isGameplay = false;
                     {
                        ID3D11RenderTargetView* rtv = nullptr;
                        ID3D11DepthStencilView* dsv = nullptr;
                        native_device_context->OMGetRenderTargets(1, &rtv, &dsv);
                        if (rtv) {
                           ID3D11Resource* res = nullptr;
                           rtv->GetResource(&res);
                           if (res) {
                              ID3D11Texture2D* tex = nullptr;
                              if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex) {
                                 D3D11_TEXTURE2D_DESC rd = {};
                                 tex->GetDesc(&rd);
                                 isGameplay = (rd.Width >= 400 && rd.Height >= 200);
                                 tex->Release();
                              }
                              res->Release();
                           }
                        }
                        if (rtv) rtv->Release();
                        if (dsv) dsv->Release();
                     }
                     if (!isGameplay)
                        g_view_pool_skips.fetch_add(1, std::memory_order_relaxed);
                     // perfcap: at most 8 staging reads per frame into the pool.
                     // Draws beyond the cap skip the staging read entirely
                     // (committed untouched, no counting, no logging).
                     if (gdm.view_pool_frame != vsframe) { gdm.view_npool = 0; gdm.view_pool_reads = 0; gdm.view_pool_frame = vsframe; }
                     if (isGameplay && gdm.view_pool_reads < 8)
                    {
                       ++gdm.view_pool_reads;
                    D3D11_BUFFER_DESC vsd = {};
                    vsd.ByteWidth = 768; // M51: +0 VP, +128 R, +640 T-row
                    vsd.Usage = D3D11_USAGE_STAGING;
                    vsd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    ComPtr<ID3D11Buffer> vst;
                    if (SUCCEEDED(native_device->CreateBuffer(&vsd, nullptr, vst.put())))
                    {
                       native_device_context->CopySubresourceRegion(vst.get(), 0, 0, 0, 0, vcb, 0, nullptr);
                       D3D11_MAPPED_SUBRESOURCE vsm = {};
                       if (SUCCEEDED(native_device_context->Map(vst.get(), 0, D3D11_MAP_READ, 0, &vsm)) && vsm.pData)
                       {
                          const float* vf = (const float*)vsm.pData;
                          if (M44ShapeTag(vf + 32) == 2)
                          {
                             // M50: continuity gate. The first qualifying draw
                             // of a frame can be a shadow/other view (also
                             // rotation-shaped) -- that poisoned own-rot.
                             // Camera motion is continuous; a wild jump vs the
                             // stashed view = wrong view -> reject (keep old).
                             bool okv = true;
                             if (gdm.view_frame_cur != 0)
                             {
                                float jd = 0.0f;
                                for (int q = 0; q < 16; q++)
                                {
                                   float dd = vf[32 + q] - gdm.view_cb_cur[32 + q];
                                   if (dd < 0) dd = -dd;
                                   if (dd > jd) jd = dd;
                                }
                                if (jd > 0.8f)
                                {
                                   g_view_rejects.fetch_add(1, std::memory_order_relaxed);
                                   if (g_view_rejrun.fetch_add(1, std::memory_order_relaxed) < 4)
                                      okv = false; // 5th straight reject = cut/teleport: re-anchor below
                                }
                             }
                              if (okv)
                              {
                                 // viewpick: pool the candidate, NO commit here.
                                 // Election at the first use-point commits
                                 // (fast path npool==1 via the legacy block).
                                 float cp00 = 0.0f, cp11 = 0.0f;
                                 if ((vf[32] > 1e-6f || vf[32] < -1e-6f)) cp00 = vf[0] / vf[32];
                                 if ((vf[37] > 1e-6f || vf[37] < -1e-6f)) cp11 = vf[5] / vf[37];
                                 if (cp00 > 0.5f && cp00 < 8.0f && cp11 > 0.5f && cp11 < 8.0f)
                                 {
                                    int mi = -1;
                                    for (int i = 0; i < gdm.view_npool; i++)
                                    {
                                       float mjd = 0.0f;
                                       for (int q = 0; q < 16; q++)
                                       {
                                          float dd = vf[32 + q] - gdm.view_pool[i].cb[32 + q];
                                          if (dd < 0) dd = -dd;
                                          if (dd > mjd) mjd = dd;
                                       }
                                       float b0 = gdm.view_pool[i].p00, b1 = gdm.view_pool[i].p11;
                                       float r0 = (cp00 - b0) / b0; if (r0 < 0) r0 = -r0;
                                       float r1 = (cp11 - b1) / b1; if (r1 < 0) r1 = -r1;
                                       if (mjd < 0.05f && r0 < 0.02f && r1 < 0.02f) { mi = i; break; }
                                    }
                                    if (mi >= 0)
                                       gdm.view_pool[mi].hits++;
                                    else if (gdm.view_npool < 4)
                                    {
                                       auto& nc = gdm.view_pool[gdm.view_npool];
                                       memcpy(nc.cb, vf, 768);
                                       nc.p00 = cp00; nc.p11 = cp11; nc.hits = 1;
                                       gdm.view_npool++;
                                    }
                                    // else: pool full, ignore
                                 }
                              }
                          }
                          native_device_context->Unmap(vst.get(), 0);
                       }
                    }
                    } // perfcap: end capped staging read
                 }
                vcb->Release();
             }
          }
       }

       // M7e: stamp COMPUTE UAV writes too (the color pair has no pixel
      // writer -- its producer may be a dispatch). Master-gated (either).
      if (!is_custom_pass && stages == reshade::api::shader_stage::all_compute &&
          (g_dlss_master.load(std::memory_order_relaxed) || g_cdlss_master.load(std::memory_order_relaxed)))
      {
         ID3D11UnorderedAccessView* uavs[4] = {};
         native_device_context->CSGetUnorderedAccessViews(0, 4, uavs);
         uint64_t cur_frame = g_hist_frame.load(std::memory_order_relaxed);
         for (int i = 0; i < 4; i++)
         {
            if (!uavs[i]) continue;
            ID3D11Resource* tmp = nullptr;
            uavs[i]->GetResource(&tmp);
            if (tmp)
            {
               std::lock_guard<std::mutex> lk(g_writer_mutex);
               g_rt_last_writer[(uint64_t)tmp] = cur_frame;
               if (g_rt_last_writer.size() > 8192)
                  g_rt_last_writer.clear();
               tmp->Release();
            }
            uavs[i]->Release();
         }
      }

        // M43: auto-diagnostic -- ONE press captures everything the own-MV
        // build needs. Arms all one-shots + a 4-frame slot-fire sequence
        // (dims/handles/CB0 keys/fidx/MV state/counters per frame). The user
        // MUST pan the camera while it runs so matrices animate.
        if (g_auto_diag.load(std::memory_order_relaxed))
        {
           g_auto_diag.store(false, std::memory_order_relaxed);
           g_cstats.store(true, std::memory_order_relaxed);
           g_cdump_cb.store(true, std::memory_order_relaxed);
           g_clog_slot.store(true, std::memory_order_relaxed);
           g_cscan_cb.store(true, std::memory_order_relaxed);
           g_pscan_cb.store(true, std::memory_order_relaxed);
           g_mscan_n = 0;
           g_mscan_sniffs = 0;
           MScanResetLoc();
           g_mscan_frame0 = g_hist_frame.load(std::memory_order_relaxed);
           g_mscan_cb.store(true, std::memory_order_relaxed);
           g_adiag_left.store(4, std::memory_order_relaxed);
           g_adiag_feed.store(4, std::memory_order_relaxed);
           {
              char ab[1024];
              char aages[6][32];
              uint64_t acur = g_hist_frame.load(std::memory_order_relaxed);
              {
                 std::lock_guard<std::mutex> mlk(g_mvmap_mutex);
                 for (int i = 0; i < 6; i++)
                 {
                    auto ait = g_mv_by_hash.find(kMvHashes[i]);
                    if (ait == g_mv_by_hash.end() || !ait->second.res)
                       sprintf_s(aages[i], "--");
                    else
                       sprintf_s(aages[i], "%lluf", (unsigned long long)(acur - ait->second.frame));
                 }
              }
              snprintf(ab, sizeof(ab),
                 "DaysGone ADIAG start sr=%s dec=%d msc=%d inv=%d jitmode=%d near=%.1f far=%.0f mvsrc=%d zero=%d mvsjit=%d aexp=%d "
                 "att=%llu runs=%llu resets=%llu real=%llu vzero=%llu capfail=%llu rtvfail=%llu "
                 "mvage=BE:%s,1E:%s,58:%s,6C:%s,A1:%s,A5:%s own=%d p=%.3f,%.3f",
                 SrTypeName(device_data.sr_type),
                 g_mv_decode.load(std::memory_order_relaxed),
                 g_mv_scale_mode.load(std::memory_order_relaxed), (int)g_inverted_depth.load(std::memory_order_relaxed),
                 g_jit_scale_mode.load(std::memory_order_relaxed),
                 g_near_plane.load(std::memory_order_relaxed), g_far_plane.load(std::memory_order_relaxed),
                 g_mv_src_mode.load(std::memory_order_relaxed), (int)g_zero_mv.load(std::memory_order_relaxed),
                 (int)g_mvs_jittered.load(std::memory_order_relaxed), (int)g_dlss_autoexp.load(std::memory_order_relaxed),
                 (unsigned long long)g_cdlss_attempts.load(), (unsigned long long)g_cdlss_runs.load(),
                 (unsigned long long)g_cdlss_resets.load(), (unsigned long long)g_cmv_frames_real.load(),
                 (unsigned long long)g_cmv_frames_zero.load(), (unsigned long long)g_cfail_capture.load(),
                 (unsigned long long)g_cfail_rtv.load(),
                 aages[0], aages[1], aages[2], aages[3], aages[4], aages[5],
                 g_own_used.load(std::memory_order_relaxed),
                 g_own_p00.load(std::memory_order_relaxed), g_own_p11.load(std::memory_order_relaxed));
              reshade::log::message(reshade::log::level::info, ab);
           }
        }

       // M11 DLSS at the COMPUTE gameplay TAA (CS 242D9D62, 1/frame).
       // Proved by the M10k frame.log: u0 = current (stable), u1/t3 ping-pong
       // history, t2 = fmt10 linear HDR, t1 = depth fmt42. The pixel TAAU slot
       // below only runs in showcase/upscale modes -- THIS is the gameplay DLAA
       // path. NGX renders into our FP16 target, then a plain (linear, alpha-
       // preserving) pixel copy writes it to the game's u0 (+u1) UAVs via a
       // runtime RTV. MVs come from the fmt35 velocity cache, not the slot's
       // unidentified fmt27 t0. No UI composite: output is pre-tonemap scene.
       if (!is_custom_pass && stages == reshade::api::shader_stage::all_compute &&
           (g_cdlss_master.load(std::memory_order_relaxed) || g_clog_slot.load(std::memory_order_relaxed) ||
            g_cdump_cb.load(std::memory_order_relaxed) || g_cview_src.load(std::memory_order_relaxed) >= 0))
      {
         uint32_t ccs = original_shader_hashes.compute_shaders.empty() ? 0 : original_shader_hashes.compute_shaders[0];
          // M29: cache-only viewer stash OUTSIDE the slot gate. Sources 6-10
          // are device caches (velocity/depth/HDR/DLSS_OUT/MV-feed), not slot
          // bindings, so they must work even when 242D9D62 doesn't fire
          // (menu/showcase).
          {
             int cve = g_cview_src.load(std::memory_order_relaxed);
             bool is_imm_e = (native_device_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE);
             if (cve >= 6 && cve <= 10 && is_imm_e && device_data.game)
            {
               auto& gdve = *static_cast<DaysGoneDeviceData*>(device_data.game);
               ComPtr<ID3D11ShaderResourceView> ve;
               bool veok = false;
                if (cve == 6 && gdve.cached_mvs)
                   veok = CreateStashSRV(native_device, gdve.cached_mvs.get(), ve);
                else if (cve == 7 && gdve.cached_depth)
                   veok = CreateDepthViewSRV(native_device, gdve.cached_depth.get(), ve);
                else if (cve == 8 && gdve.cached_hdr)
                   veok = CreateStashSRV(native_device, gdve.cached_hdr.get(), ve);
                 else if (cve == 9 && gdve.srv_dlss_out)
                 { ve = gdve.srv_dlss_out; veok = true; }
                 else if (cve == 10 && gdve.srv_mvs_conv)
                 { ve = gdve.srv_mvs_conv; veok = true; } // M48: MV feed to NGX
                if (veok && ve)
                {
                   std::lock_guard<std::mutex> vlke(g_cview_mutex);
                   g_cview_srv = ve;
                   g_cview_pending.store(true, std::memory_order_relaxed);
                   CViewStashReport(true, cve, "cache");
                }
                else
                {
                   CViewStashReport(false, cve, "empty/stale cache");
                }
            }
         }
         if (ccs == kDlssComputeSlotCS)
         {
#if ENABLE_SR
             g_cdlss_attempts.fetch_add(1, std::memory_order_relaxed);
             g_cdlss_last_fire.store(g_hist_frame.load(std::memory_order_relaxed), std::memory_order_relaxed); // M53: auto-unpark heartbeat
             bool is_imm_c = (native_device_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE);
            // One-shot I/O logger (masterless): u0/u1 + t0-t3 dims.
            if (g_clog_slot.exchange(false, std::memory_order_relaxed) && is_imm_c)
            {
               ID3D11UnorderedAccessView* lu[4] = {};
               native_device_context->CSGetUnorderedAccessViews(0, 4, lu);
               ID3D11ShaderResourceView* lr[8] = {};
               native_device_context->CSGetShaderResources(0, 8, lr);
               char cu[2][48] = {}, ct[4][48] = {};
               for (int i = 0; i < 2; i++)
               {
                  if (lu[i])
                  {
                     ID3D11Resource* tmp = nullptr;
                     lu[i]->GetResource(&tmp);
                     DescribeResHandle(tmp, cu[i], sizeof(cu[i]));
                     if (tmp) tmp->Release();
                  }
                  else strcpy_s(cu[i], sizeof(cu[i]), "null");
               }
               for (int i = 0; i < 4; i++)
               {
                  if (lr[i])
                  {
                     ID3D11Resource* tmp = nullptr;
                     lr[i]->GetResource(&tmp);
                     DescribeResHandle(tmp, ct[i], sizeof(ct[i]));
                     if (tmp) tmp->Release();
                  }
                  else strcpy_s(ct[i], sizeof(ct[i]), "null");
               }
               char b[448];
               // M23: fidx = core FrameIndex fed to NGX. Press twice seconds
               // apart: identical values = stuck frame counter = no history.
               snprintf(b, sizeof(b), "DaysGone cTAA 242D9D62 u0=%s u1=%s t0=%s t1=%s t2=%s t3=%s fidx=%llu",
                  cu[0], cu[1], ct[0], ct[1], ct[2], ct[3],
                  (unsigned long long)cb_luma_global_settings.FrameIndex);
               reshade::log::message(reshade::log::level::info, b);
               for (int i = 0; i < 4; i++) if (lu[i]) lu[i]->Release();
               for (int i = 0; i < 8; i++) if (lr[i]) lr[i]->Release();
            }
            // One-shot CB0 dump (masterless): find the compute-TAA jitter.
            if (g_cdump_cb.exchange(false, std::memory_order_relaxed) && is_imm_c && device_data.game)
            {
               auto& gd_dump = *static_cast<DaysGoneDeviceData*>(device_data.game);
               ID3D11Buffer* ccb0 = nullptr;
               native_device_context->CSGetConstantBuffers(0, 1, &ccb0);
               if (ccb0)
               {
                  D3D11_BUFFER_DESC bd = {};
                  ccb0->GetDesc(&bd);
                  if (bd.ByteWidth >= 16)
                  {
                     if (!gd_dump.staging_cb || gd_dump.staging_size != bd.ByteWidth)
                     {
                        gd_dump.staging_cb.reset();
                        D3D11_BUFFER_DESC sd = {};
                        sd.ByteWidth = bd.ByteWidth;
                        sd.Usage = D3D11_USAGE_STAGING;
                        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        if (SUCCEEDED(native_device->CreateBuffer(&sd, nullptr, gd_dump.staging_cb.put())))
                           gd_dump.staging_size = bd.ByteWidth;
                        else
                           gd_dump.staging_size = 0;
                     }
                     if (gd_dump.staging_cb)
                     {
                        native_device_context->CopySubresourceRegion(gd_dump.staging_cb.get(), 0, 0, 0, 0, ccb0, 0, nullptr);
                        D3D11_MAPPED_SUBRESOURCE m = {};
                        if (SUCCEEDED(native_device_context->Map(gd_dump.staging_cb.get(), 0, D3D11_MAP_READ, 0, &m)) && m.pData)
                        {
                           const float* f = (const float*)m.pData;
                            char b[2048];
                            int pos = snprintf(b, sizeof(b), "DaysGone cTAA CB0 dump (%u bytes):", bd.ByteWidth);
                            int nf = bd.ByteWidth / 4;
                            if (nf > 64) nf = 64; // M49: full 256B (params may live deep)
                           for (int i = 0; i < nf && pos < (int)sizeof(b) - 32; i++)
                              pos += snprintf(b + pos, sizeof(b) - pos, " [%d]%.5f", i, f[i]);
                           reshade::log::message(reshade::log::level::info, b);
                           native_device_context->Unmap(gd_dump.staging_cb.get(), 0);
                        }
                     }
                  }
                   ccb0->Release();
                }
             }
             // M32/M33/M34: one-shot projection-matrix scan (masterless,
             // own-camera-MV groundwork). Row + column layouts via helper.
             if (g_cscan_cb.exchange(false, std::memory_order_relaxed) && is_imm_c && device_data.game)
             {
                for (int cslot = 0; cslot < 8; cslot++)
                {
                   ID3D11Buffer* ccb = nullptr;
                   native_device_context->CSGetConstantBuffers(cslot, 1, &ccb);
                   if (!ccb)
                      continue;
                   ScanOneCBForProj(native_device, native_device_context, "CS", cslot, ccb);
                   ccb->Release();
                }
             }
            // M19: one-shot input signal stats (masterless). Proves per-mode
            // whether MV/color/depth buffers carry any signal at all: zero-
            // variance MVs (or constant jitter) make every scale sweep a no-op.
            if (g_cstats.exchange(false, std::memory_order_relaxed) && is_imm_c)
            {
               ID3D11ShaderResourceView* srs[4] = {};
               native_device_context->CSGetShaderResources(0, 4, srs);
               ID3D11Resource* rrs[4] = {};
               for (int i = 0; i < 4; i++) if (srs[i]) srs[i]->GetResource(&rrs[i]);
               // t2 = scene color, t1 = depth, t0 = HUD/velocity candidate.
               LogResStats(native_device, native_device_context, rrs[2], "t2-color");
               LogResStats(native_device, native_device_context, rrs[1], "t1-depth");
               LogResStats(native_device, native_device_context, rrs[0], "t0-hudvel");
               // The MV feed actually in use (cache or zero fallback).
               {
                  char b[128];
                  bool fresh = false;
                  ID3D11Resource* mvr = nullptr;
                  if (device_data.game)
                  {
                     auto& gds = *static_cast<DaysGoneDeviceData*>(device_data.game);
                     int smode = g_mv_src_mode.load(std::memory_order_relaxed);
                     if (smode >= 1 && smode <= 6)
                     {
                        std::lock_guard<std::mutex> mlk(g_mvmap_mutex);
                        auto it = g_mv_by_hash.find(kMvHashes[smode - 1]);
                        fresh = (it != g_mv_by_hash.end() && it->second.frame == g_hist_frame.load(std::memory_order_relaxed) && it->second.res);
                        mvr = fresh ? (ID3D11Resource*)it->second.res.get() : nullptr;
                     }
                     else
                     {
                        fresh = gds.cached_mvs && gds.cached_mvs_frame == g_hist_frame.load(std::memory_order_relaxed);
                        mvr = fresh ? (ID3D11Resource*)gds.cached_mvs.get() : nullptr;
                     }
                  }
                   snprintf(b, sizeof(b), "DaysGone cSTATS mvcache %s", fresh ? "FRESH" : "STALE");
                   reshade::log::message(reshade::log::level::info, b);
                   LogResStats(native_device, native_device_context, mvr, "mv-feed");
                }
                // M52: DLSS output brightness (darkness numbers: compare
                // dlss-out means against t2-color above; ratio < 1 =
                // NGX darkens, ~= 1 = output path/downstream does it).
                if (device_data.game)
                {
                   auto& gdo = *static_cast<DaysGoneDeviceData*>(device_data.game);
                   if (gdo.srv_dlss_out)
                   {
                      ID3D11Resource* dor = nullptr;
                      gdo.srv_dlss_out->GetResource(&dor);
                      LogResStats(native_device, native_device_context, dor, "dlss-out");
                      if (dor) dor->Release();
                   }
                   else
                   {
                      char nb[64];
                      snprintf(nb, sizeof(nb), "DaysGone cSTATS dlss-out=null");
                      reshade::log::message(reshade::log::level::info, nb);
                   }
                }
                // M49: history-pair CONTENT (settles black-t3 numerically:
                // all-zero t3+u1 = poisoned history; image = stale stash).
                {
                   ID3D11Resource* rrh = nullptr;
                   if (srs[3]) srs[3]->GetResource(&rrh);
                   LogResStats(native_device, native_device_context, rrh, "t3-hist");
                   if (rrh) rrh->Release();
                   ID3D11UnorderedAccessView* lhu[2] = {};
                   native_device_context->CSGetUnorderedAccessViews(0, 2, lhu);
                   ID3D11Resource* rru = nullptr;
                   if (lhu[1]) lhu[1]->GetResource(&rru);
                   LogResStats(native_device, native_device_context, rru, "u1-hist");
                   if (rru) rru->Release();
                   for (int i = 0; i < 2; i++) if (lhu[i]) lhu[i]->Release();
                }
                for (int i = 0; i < 4; i++) { if (rrs[i]) rrs[i]->Release(); }
                for (int i = 0; i < 4; i++) if (srs[i]) srs[i]->Release();
             }
             // M43: ADIAG per-frame line (4 consecutive slot fires). Own-MV
             // inputs per frame: slot I/O descs + CB0 anim keys ([0][1] vs
             // [22][23]/[26][27]) + fidx (history proof) + MV cache state.
             if (g_adiag_left.load(std::memory_order_relaxed) > 0 && is_imm_c && device_data.game)
             {
                int fnum = 4 - g_adiag_left.load(std::memory_order_relaxed);
                ID3D11UnorderedAccessView* fau[2] = {};
                native_device_context->CSGetUnorderedAccessViews(0, 2, fau);
                ID3D11ShaderResourceView* far_[4] = {};
                native_device_context->CSGetShaderResources(0, 4, far_);
                char fau0[48] = {}, fau1[48] = {}, fat[4][48] = {};
                strcpy_s(fau0, sizeof(fau0), "null"); strcpy_s(fau1, sizeof(fau1), "null");
                for (int i = 0; i < 4; i++) strcpy_s(fat[i], sizeof(fat[i]), "null");
                for (int i = 0; i < 2; i++)
                {
                   if (!fau[i]) continue;
                   ID3D11Resource* ftmp = nullptr;
                   fau[i]->GetResource(&ftmp);
                   DescribeResHandle(ftmp, i ? fau1 : fau0, 48);
                   if (ftmp) ftmp->Release();
                }
                for (int i = 0; i < 4; i++)
                {
                   if (!far_[i]) continue;
                   ID3D11Resource* ftmp = nullptr;
                   far_[i]->GetResource(&ftmp);
                   DescribeResHandle(ftmp, fat[i], sizeof(fat[i]));
                   if (ftmp) ftmp->Release();
                }
                float fk0 = 0, fk1 = 0, fk22 = 0, fk23 = 0, fk26 = 0, fk27 = 0;
                int fknf = 0;
                ID3D11Buffer* fkb = nullptr;
                native_device_context->CSGetConstantBuffers(0, 1, &fkb);
                if (fkb)
                {
                   D3D11_BUFFER_DESC fbd = {};
                   fkb->GetDesc(&fbd);
                   fknf = (int)(fbd.ByteWidth / 4);
                   if (fbd.ByteWidth >= 112)
                   {
                      D3D11_BUFFER_DESC fsd = {};
                      fsd.ByteWidth = fbd.ByteWidth;
                      fsd.Usage = D3D11_USAGE_STAGING;
                      fsd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                      ComPtr<ID3D11Buffer> fst;
                      if (SUCCEEDED(native_device->CreateBuffer(&fsd, nullptr, fst.put())))
                      {
                         native_device_context->CopySubresourceRegion(fst.get(), 0, 0, 0, 0, fkb, 0, nullptr);
                         D3D11_MAPPED_SUBRESOURCE fmm = {};
                         if (SUCCEEDED(native_device_context->Map(fst.get(), 0, D3D11_MAP_READ, 0, &fmm)) && fmm.pData)
                         {
                            const float* fff = (const float*)fmm.pData;
                            fk0 = fff[0]; fk1 = fff[1]; fk22 = fff[22]; fk23 = fff[23]; fk26 = fff[26]; fk27 = fff[27];
                            native_device_context->Unmap(fst.get(), 0);
                         }
                      }
                   }
                   fkb->Release();
                }
                auto& fgd = *static_cast<DaysGoneDeviceData*>(device_data.game);
                uint64_t fframe = g_hist_frame.load(std::memory_order_relaxed);
                bool ffresh = fgd.cached_mvs && fgd.cached_mvs_frame == fframe;
                char fb[1024];
                snprintf(fb, sizeof(fb),
                   "DaysGone ADIAG f%d frame=%llu fidx=%llu u0=%s u1=%s t0=%s t1=%s t2=%s t3=%s cb0n=%d k01=%.5f,%.5f k22=%.5f,%.5f k26=%.5f,%.5f mv=%s hash=%08X att=%llu runs=%llu resets=%llu",
                   fnum, (unsigned long long)fframe,
                   (unsigned long long)cb_luma_global_settings.FrameIndex,
                   fau0, fau1, fat[0], fat[1], fat[2], fat[3], fknf,
                   fk0, fk1, fk22, fk23, fk26, fk27,
                   ffresh ? "real" : "ZERO", (unsigned)fgd.cached_mvs_hash,
                   (unsigned long long)g_cdlss_attempts.load(),
                   (unsigned long long)g_cdlss_runs.load(), (unsigned long long)g_cdlss_resets.load());
                reshade::log::message(reshade::log::level::info, fb);
                for (int i = 0; i < 2; i++) if (fau[i]) fau[i]->Release();
                for (int i = 0; i < 4; i++) if (far_[i]) far_[i]->Release();
                if (g_adiag_left.fetch_sub(1, std::memory_order_relaxed) == 1)
                {
                   char fdone[256];
                   snprintf(fdone, sizeof(fdone),
                      "DaysGone ADIAG done att=%llu runs=%llu resets=%llu real=%llu vzero=%llu",
                      (unsigned long long)g_cdlss_attempts.load(), (unsigned long long)g_cdlss_runs.load(),
                      (unsigned long long)g_cdlss_resets.load(), (unsigned long long)g_cmv_frames_real.load(),
                      (unsigned long long)g_cmv_frames_zero.load());
                   reshade::log::message(reshade::log::level::info, fdone);
                }
             }
            // M27: compute viewer stash (masterless). Named TAA inputs/outputs
            // for real-gameplay viewing (pixel viewer is showcase-only).
            int cvsel = g_cview_src.load(std::memory_order_relaxed);
            if (cvsel < 0)
            {
               g_cview_pending.store(false, std::memory_order_relaxed);
               std::lock_guard<std::mutex> vlk0(g_cview_mutex);
               g_cview_srv.reset();
            }
             else if (cvsel <= 10 && is_imm_c)
             {
                ComPtr<ID3D11ShaderResourceView> vsrv;
                bool svok = false;
                const char* sdetail = "slot";
                if (cvsel >= 0 && cvsel <= 3)
                {
                   ID3D11ShaderResourceView* lr4[4] = {};
                   native_device_context->CSGetShaderResources(0, 4, lr4);
                   if (lr4[cvsel])
                   {
                      ID3D11Resource* tmp = nullptr;
                      lr4[cvsel]->GetResource(&tmp);
                      ComPtr<ID3D11Resource> h; h.attach(tmp);
                      if (h) svok = CreateStashSRV(native_device, h.get(), vsrv);
                      else sdetail = "null slot binding";
                   }
                   else sdetail = "null slot binding";
                   for (int i = 0; i < 4; i++) if (lr4[i]) lr4[i]->Release();
                }
                else if ((cvsel == 4 || cvsel == 5) && device_data.game)
                {
                   (void)device_data;
                   ID3D11UnorderedAccessView* lu4[4] = {};
                   native_device_context->CSGetUnorderedAccessViews(0, 4, lu4);
                   int ui = (cvsel == 4) ? 0 : 1;
                   if (lu4[ui])
                   {
                      ID3D11Resource* tmp = nullptr;
                      lu4[ui]->GetResource(&tmp);
                      ComPtr<ID3D11Resource> h; h.attach(tmp);
                      if (h) svok = CreateStashSRV(native_device, h.get(), vsrv);
                      else sdetail = "null UAV binding";
                   }
                   else sdetail = "null UAV binding";
                   for (int i = 0; i < 4; i++) if (lu4[i]) lu4[i]->Release();
                }
                else if (device_data.game)
                {
                   auto& gdv = *static_cast<DaysGoneDeviceData*>(device_data.game);
                   sdetail = "cache";
                   if (cvsel == 6 && gdv.cached_mvs)
                      svok = CreateStashSRV(native_device, gdv.cached_mvs.get(), vsrv);
                   else if (cvsel == 7 && gdv.cached_depth)
                      svok = CreateDepthViewSRV(native_device, gdv.cached_depth.get(), vsrv);
                   else if (cvsel == 8 && gdv.cached_hdr)
                      svok = CreateStashSRV(native_device, gdv.cached_hdr.get(), vsrv);
                    else if (cvsel == 9 && gdv.srv_dlss_out)
                    { vsrv = gdv.srv_dlss_out; svok = true; }
                    else if (cvsel == 10 && gdv.srv_mvs_conv)
                    { vsrv = gdv.srv_mvs_conv; svok = true; } // M48: MV feed to NGX
                    else sdetail = "empty/stale cache";
                }
                if (svok && vsrv)
                {
                   std::lock_guard<std::mutex> vlk(g_cview_mutex);
                   g_cview_srv = vsrv;
                   g_cview_pending.store(true, std::memory_order_relaxed);
                   CViewStashReport(true, cvsel, sdetail);
                }
                 else
                 {
                    CViewStashReport(false, cvsel, sdetail);
                 }
              }
             // All-passes slot stash (masterless, display-only): while armed,
             // hold refs to the slot's t0-t3/u0-u1 bindings for the Present-
             // time BMP saver. Refcounts only, no GPU copies, outside the
             // NGX-gated section below.
             if (g_pass_active.load(std::memory_order_relaxed) && is_imm_c)
             {
                ID3D11ShaderResourceView* pr[4] = {};
                native_device_context->CSGetShaderResources(0, 4, pr);
                ID3D11UnorderedAccessView* pu[2] = {};
                native_device_context->CSGetUnorderedAccessViews(0, 2, pu);
                ID3D11Resource* got[6] = {};
                for (int i = 0; i < 4; i++)
                   if (pr[i]) pr[i]->GetResource(&got[i]);
                for (int i = 0; i < 2; i++)
                   if (pu[i]) pu[i]->GetResource(&got[4 + i]);
                {
                   std::lock_guard<std::mutex> plk(g_pass_slot_mutex);
                   for (int i = 0; i < 6; i++)
                      if (got[i]) g_pass_slot[i] = got[i];
                }
                for (int i = 0; i < 6; i++) if (got[i]) got[i]->Release();
                for (int i = 0; i < 4; i++) if (pr[i]) pr[i]->Release();
                for (int i = 0; i < 2; i++) if (pu[i]) pu[i]->Release();
             }
             // Tools above run masterless; DLSS below stays gated.
            if (!g_cdlss_master.load(std::memory_order_relaxed))
               return DrawOrDispatchOverrideType::None;
            if (device_data.sr_type != SR::Type::None)
            {
               auto* sr_instance_data = device_data.GetSRInstanceData();
               bool is_immediate = (native_device_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE);
               if (sr_instance_data && is_immediate && device_data.game)
               {
                  ID3D11UnorderedAccessView* raw_uavs[4] = {};
                  native_device_context->CSGetUnorderedAccessViews(0, 4, raw_uavs);
                  ID3D11ShaderResourceView* raw_csrvs[8] = {};
                  native_device_context->CSGetShaderResources(0, 8, raw_csrvs);
                  ComPtr<ID3D11UnorderedAccessView> uavs[4] = {};
                  ComPtr<ID3D11ShaderResourceView> csrvs[8] = {};
                  for (int i = 0; i < 4; i++) uavs[i] = raw_uavs[i];
                  for (int i = 0; i < 8; i++) csrvs[i] = raw_csrvs[i];
                  // Color = t2 (fmt10 linear HDR, fed direct -- no unpack),
                  // depth = t1 (fmt42 SRV resource), out = u0 (+u1).
                  ComPtr<ID3D11Resource> c_color, c_depth, c_out0, c_out1;
                  uint32_t crw = 0, crh = 0, cow = 0, coh = 0;
                  DXGI_FORMAT cfmt = DXGI_FORMAT_UNKNOWN;
                  if (csrvs[2])
                  {
                     ID3D11Resource* tmp = nullptr;
                     csrvs[2]->GetResource(&tmp);
                     c_color.attach(tmp);
                     ComPtr<ID3D11Texture2D> t;
                     if (c_color && SUCCEEDED(c_color->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(t.put()))))
                     {
                        D3D11_TEXTURE2D_DESC d = {};
                        t->GetDesc(&d);
                        crw = d.Width; crh = d.Height; cfmt = d.Format;
                     }
                  }
                   if (csrvs[1])
                   {
                      ID3D11Resource* tmp = nullptr;
                      csrvs[1]->GetResource(&tmp);
                      c_depth.attach(tmp);
                   }
                   // M58: depth-source resolve. Cached DSV is guaranteed depth
                   // (bound as depth-stencil at a fullscreen draw this frame);
                   // slot t1 is only depth if it actually carries depth.
                   const char* c_depth_src = "slot-t1";
                   {
                      int dmode = g_cdepth_src.load(std::memory_order_relaxed);
                      auto& gdd = *static_cast<DaysGoneDeviceData*>(device_data.game);
                       bool cache_fresh = gdd.cached_depth &&
                          gdd.cached_depth_frame == g_hist_frame.load(std::memory_order_relaxed);
                       { // templog: depth age in frames (log-only, 0 = fresh).
                          uint64_t hfr = g_hist_frame.load(std::memory_order_relaxed);
                          g_clast_dage.store(!gdd.cached_depth ? 9999ULL : (cache_fresh ? 0ULL : (gdd.cached_depth_frame <= hfr ? hfr - gdd.cached_depth_frame : 0ULL)), std::memory_order_relaxed);
                       }
                      if ((dmode == 0 && cache_fresh) || dmode == 2)
                      {
                         if (gdd.cached_depth && (dmode == 2 || cache_fresh))
                         {
                            c_depth.reset();
                            c_depth = gdd.cached_depth;
                            c_depth_src = "cache-DSV";
                         }
                      }
                   }
                  if (uavs[0])
                  {
                     ID3D11Resource* tmp = nullptr;
                     uavs[0]->GetResource(&tmp);
                     c_out0.attach(tmp);
                     ComPtr<ID3D11Texture2D> t;
                     if (c_out0 && SUCCEEDED(c_out0->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(t.put()))))
                     {
                        D3D11_TEXTURE2D_DESC d = {};
                        t->GetDesc(&d);
                        cow = d.Width; coh = d.Height;
                     }
                  }
                  if (uavs[1])
                  {
                     ID3D11Resource* tmp = nullptr;
                     uavs[1]->GetResource(&tmp);
                     c_out1.attach(tmp);
                  }
                  for (int i = 0; i < 4; i++) { if (raw_uavs[i]) raw_uavs[i]->Release(); }
                  for (int i = 0; i < 8; i++) { if (raw_csrvs[i]) raw_csrvs[i]->Release(); }

                  auto& gd = *static_cast<DaysGoneDeviceData*>(device_data.game);
                   bool cinputs_ok = c_color && c_depth && c_out0 && crw >= 400 && cow >= 400 && // M55: was 1000
                      cfmt == DXGI_FORMAT_R16G16B16A16_FLOAT;
                   if (!cinputs_ok)
                   {
                      g_cfail_capture.fetch_add(1, std::memory_order_relaxed);
                      if (!gd.logged_ccapture_fail)
                     {
                        gd.logged_ccapture_fail = true;
                        char b[192];
                        snprintf(b, sizeof(b), "DaysGone cDLSS capture FAIL: color=%d depth=%d out=%d crw=%u cow=%u cfmt=%d (want fmt10)",
                           (int)(bool)c_color, (int)(bool)c_depth, (int)(bool)c_out0, crw, cow, (int)cfmt);
                        reshade::log::message(reshade::log::level::warning, b);
                     }
                     device_data.force_reset_sr = true;
                  }
                  else
                  {
                     // Own FP16 output target (shared with the pixel path).
                      // M64: size the output target to cover BOTH the game UAV
                      // and the render input. At some window sizes the game
                      // hands us render dims 1 row/col LARGER than the UAV
                      // (1920x1052 in, 1920x1051 out); NGX rejects render>out
                      // and Draw fails forever (silent native fallback). The
                      // downstream copies stay at cow/coh, so the extra
                      // row/col is harmless overscan.
                      uint32_t odw = cow > crw ? cow : crw, odh = coh > crh ? coh : crh;
                      if (!gd.tex_dlss_out || gd.dlss_out_w != odw || gd.dlss_out_h != odh)
                      {
                         gd.srv_dlss_out.reset();
                         gd.tex_dlss_out.reset();
                         D3D11_TEXTURE2D_DESC od = {};
                         od.Width = odw; od.Height = odh;
                        od.MipLevels = 1; od.ArraySize = 1;
                        od.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                        od.SampleDesc.Count = 1;
                        od.Usage = D3D11_USAGE_DEFAULT;
                        od.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                        ComPtr<ID3D11Texture2D> ot;
                        if (SUCCEEDED(native_device->CreateTexture2D(&od, nullptr, ot.put())))
                        {
                           if (SUCCEEDED(native_device->CreateShaderResourceView(ot.get(), nullptr, gd.srv_dlss_out.put())))
                           {
                              gd.tex_dlss_out = ot;
                               gd.dlss_out_w = odw; gd.dlss_out_h = odh;
                           }
                        }
                     }
                     // MVs: fmt35 velocity cache (BE0130E5 et al.) converted to
                     // signed pixels; stale/missing -> zero fallback.
                     // Zero texture first (independent of conversion below).
                     if (!gd.tex_dlss_zero_mvs || gd.zero_mvs_w != crw || gd.zero_mvs_h != crh)
                     {
                        gd.srv_dlss_zero_mvs.reset();
                        gd.tex_dlss_zero_mvs.reset();
                        D3D11_TEXTURE2D_DESC zd = {};
                        zd.Width = crw; zd.Height = crh;
                        zd.MipLevels = 1; zd.ArraySize = 1;
                        zd.Format = DXGI_FORMAT_R16G16_FLOAT;
                        zd.SampleDesc.Count = 1;
                        zd.Usage = D3D11_USAGE_DEFAULT;
                        zd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                        std::vector<uint8_t> zeros((size_t)crw * crh * 4, 0);
                        D3D11_SUBRESOURCE_DATA init = {};
                        init.pSysMem = zeros.data();
                        init.SysMemPitch = crw * 4;
                        ComPtr<ID3D11Texture2D> zt;
                        if (SUCCEEDED(native_device->CreateTexture2D(&zd, &init, zt.put())))
                        {
                           if (SUCCEEDED(native_device->CreateShaderResourceView(zt.get(), nullptr, gd.srv_dlss_zero_mvs.put())))
                           {
                              gd.tex_dlss_zero_mvs = zt;
                              gd.zero_mvs_w = crw; gd.zero_mvs_h = crh;
                           }
                        }
                     }
                     ID3D11Resource* c_mv_res = gd.tex_dlss_zero_mvs.get();
                      bool c_mv_real = false;
                      const char* c_mv_src = "zero";
                      int c_mv_code = 0; // M36: 0 zero, 1 auto-cache, 2 slot-t0, 3 named pick
                      ComPtr<ID3D11Resource> c_mv_hold; // owns extra refs below
                      // M27: resolve which velocity buffer feeds DLSS.
                      ID3D11Resource* mv_sel = nullptr;
                      const char* mv_sel_name = "cache";
                      long long mv_sel_frame = -1; // lookup frame of the pick (log-only)
                      {
                         int mvmode = g_mv_src_mode.load(std::memory_order_relaxed);
                         if (mvmode >= 1 && mvmode <= 6)
                         {
                            uint32_t want = kMvHashes[mvmode - 1];
                            std::lock_guard<std::mutex> mlk(g_mvmap_mutex);
                            auto it = g_mv_by_hash.find(want);
                            if (it != g_mv_by_hash.end() && it->second.frame == g_hist_frame.load(std::memory_order_relaxed) && it->second.res)
                            { mv_sel = it->second.res.get(); mv_sel_name = kMvNames[mvmode - 1]; c_mv_code = 3; mv_sel_frame = (long long)it->second.frame; }
                         }
                         else if (gd.cached_mvs && gd.cached_mvs_frame == g_hist_frame.load(std::memory_order_relaxed))
                         { mv_sel = gd.cached_mvs.get(); mv_sel_name = "cache"; c_mv_code = 1; mv_sel_frame = (long long)gd.cached_mvs_frame; }
                      }
                      // M47: own rotation-exact camera MVs (stash-fed). Takes
                      // priority when enabled with consecutive cur+prev stash;
                      // otherwise falls through to the game-MV paths below.
                      bool own_done_c = false;
                      {
                          int ownm_c = g_ownmv.load(std::memory_order_relaxed);
                          // M51: 1/2 rotation, 3 Full-A (V-row T), 4 Full-B (campos T).
                          bool own_full = (ownm_c == 3 || ownm_c == 4);
                          if ((ownm_c >= 1 && ownm_c <= 4) && device_data.game)
                         {
                             auto& godc = *static_cast<DaysGoneDeviceData*>(device_data.game);
                             uint64_t oframe_c = g_hist_frame.load(std::memory_order_relaxed);
                             ElectViewPick(godc, oframe_c); // viewpick: elect+commit before use
                             // Shake-diag pairing triple (log-only, M11 use-point).
                             g_clast_vfcur.store((unsigned long long)godc.view_frame_cur, std::memory_order_relaxed);
                             g_clast_vfprev.store((unsigned long long)godc.view_frame_prev, std::memory_order_relaxed);
                             g_clast_oframe.store((unsigned long long)oframe_c, std::memory_order_relaxed);
                             if (godc.view_frame_cur == oframe_c && godc.view_frame_prev != 0 &&
                                godc.view_frame_prev + 1 >= godc.view_frame_cur)
                            {
                               const float* vcur_c = godc.view_cb_cur;
                               const float* vprv_c = godc.view_cb_prev;
                               float op00_c = 0, op11_c = 0;
                               if ((vcur_c[32] > 1e-6f || vcur_c[32] < -1e-6f)) op00_c = vcur_c[0] / vcur_c[32];
                               if ((vcur_c[37] > 1e-6f || vcur_c[37] < -1e-6f)) op11_c = vcur_c[5] / vcur_c[37];
                               float op00_prev_c = 0, op11_prev_c = 0;
                               if ((vprv_c[32] > 1e-6f || vprv_c[32] < -1e-6f)) op00_prev_c = vprv_c[0] / vprv_c[32];
                               if ((vprv_c[37] > 1e-6f || vprv_c[37] < -1e-6f)) op11_prev_c = vprv_c[5] / vprv_c[37];
                               if (op00_c > 0.5f && op00_c < 8.0f && op11_c > 0.5f && op11_c < 8.0f &&
                                   op00_prev_c > 0.5f && op00_prev_c < 8.0f && op11_prev_c > 0.5f && op11_prev_c < 8.0f)
                               {
                                  if (!godc.tex_mvs_conv || godc.mvs_conv_w != crw || godc.mvs_conv_h != crh)
                                  {
                                     godc.srv_mvs_conv.reset();
                                     godc.rtv_mvs_conv.reset();
                                     godc.tex_mvs_conv.reset();
                                     D3D11_TEXTURE2D_DESC odc = {};
                                     odc.Width = crw; odc.Height = crh;
                                     odc.MipLevels = 1; odc.ArraySize = 1;
                                     odc.Format = DXGI_FORMAT_R16G16_FLOAT;
                                     odc.SampleDesc.Count = 1;
                                     odc.Usage = D3D11_USAGE_DEFAULT;
                                     odc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                                     ComPtr<ID3D11Texture2D> otc;
                                     if (SUCCEEDED(native_device->CreateTexture2D(&odc, nullptr, otc.put())))
                                     {
                                        ComPtr<ID3D11ShaderResourceView> osc;
                                        ComPtr<ID3D11RenderTargetView> orvc;
                                        if (SUCCEEDED(native_device->CreateShaderResourceView(otc.get(), nullptr, osc.put())) &&
                                            SUCCEEDED(native_device->CreateRenderTargetView(otc.get(), nullptr, orvc.put())))
                                        {
                                           godc.tex_mvs_conv = otc;
                                           godc.srv_mvs_conv = osc;
                                           godc.rtv_mvs_conv = orvc;
                                           godc.mvs_conv_w = crw; godc.mvs_conv_h = crh;
                                        }
                                     }
                                  }
                                  if (!godc.cb_ownmv)
                                  {
                                      D3D11_BUFFER_DESC cbdc = {};
                                       cbdc.ByteWidth = 208; // M51 192B R+T+Proj+Res+Near (48 floats) + DeltaC float3+pad (4 floats)
                                     cbdc.Usage = D3D11_USAGE_DYNAMIC;
                                     cbdc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                                     cbdc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                                     native_device->CreateBuffer(&cbdc, nullptr, godc.cb_ownmv.put());
                                  }
                                   // M51: full modes sample game depth (R24 view).
                                   // Without it they fall back to game MVs.
                                   ComPtr<ID3D11ShaderResourceView> owndepth;
                                   if (own_full && gd.cached_depth)
                                   {
                                      D3D11_SHADER_RESOURCE_VIEW_DESC dvd = {};
                                      dvd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                                      dvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                                      dvd.Texture2D.MipLevels = 1;
                                      if (FAILED(native_device->CreateShaderResourceView(gd.cached_depth.get(), &dvd, owndepth.put())))
                                         owndepth.reset();
                                   }
                                   uint32_t ownkey_c = (ownm_c == 2) ? CompileTimeStringHash("DaysGone OwnMV Neg PS")
                                      : (ownm_c == 3) ? CompileTimeStringHash("DaysGone OwnMV FullA PS")
                                      : (ownm_c == 4) ? CompileTimeStringHash("DaysGone OwnMV FullB PS")
                                      : CompileTimeStringHash("DaysGone OwnMV PS");
                                   ID3D11PixelShader* own_ps_c = device_data.native_pixel_shaders[ownkey_c].get();
                                   ID3D11VertexShader* copy_vs_c = device_data.native_vertex_shaders[CompileTimeStringHash("Copy VS")].get();
                                   if (own_ps_c && copy_vs_c && godc.rtv_mvs_conv && godc.cb_ownmv && (!own_full || owndepth))
                                  {
                                     D3D11_MAPPED_SUBRESOURCE cmoc = {};
                                     if (SUCCEEDED(native_device_context->Map(godc.cb_ownmv.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cmoc)) && cmoc.pData)
                                     {
                                         float* ofc = (float*)cmoc.pData;
                                         memcpy(ofc, vcur_c + 32, 64);
                                         memcpy(ofc + 16, vprv_c + 32, 64);
                                         memcpy(ofc + 32, vcur_c + 172, 16); // +640 row3 Tcur
                                         memcpy(ofc + 36, vprv_c + 172, 16); // +640 row3 Tprev
                                         ofc[40] = op00_c; ofc[41] = op11_c;
                                         ofc[42] = (float)crw; ofc[43] = (float)crh;
                                         ofc[44] = 10.0f; // TRUE near, UE cm (NOT the NGX slider)
                                         ofc[45] = op00_prev_c; ofc[46] = op11_prev_c; ofc[47] = 0.0f;
                                         // DeltaC (Full-B): Ccur-Cprev in DOUBLE on CPU --
                                         // |C| is absolute-world hundreds+, float subtraction
                                         // loses the small inter-frame delta (breaks at angles).
                                         ofc[48] = (float)((double)vcur_c[172] - (double)vprv_c[172]);
                                         ofc[49] = (float)((double)vcur_c[173] - (double)vprv_c[173]);
                                         ofc[50] = (float)((double)vcur_c[174] - (double)vprv_c[174]);
                                         ofc[51] = 0.0f;
                                        native_device_context->Unmap(godc.cb_ownmv.get(), 0);
                                        DrawStateStack<DrawStateStackType::FullGraphics> own_cs_c;
                                        own_cs_c.Cache(native_device_context, device_data.uav_max_count);
                                        ID3D11UnorderedAccessView* null_uavs_c[D3D11_1_UAV_SLOT_COUNT] = {};
                                        native_device_context->CSSetUnorderedAccessViews(0, device_data.uav_max_count, null_uavs_c, nullptr);
                                        D3D11_VIEWPORT vpc = {};
                                        vpc.TopLeftX = 0.0f; vpc.TopLeftY = 0.0f;
                                        vpc.Width = (float)crw; vpc.Height = (float)crh;
                                        vpc.MinDepth = 0.0f; vpc.MaxDepth = 1.0f;
                                        native_device_context->RSSetViewports(1, &vpc);
                                        native_device_context->RSSetState(godc.rs_copy.get());
                                        native_device_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
                                        native_device_context->OMSetDepthStencilState(nullptr, 0);
                                        native_device_context->OMSetRenderTargets(1, &godc.rtv_mvs_conv, nullptr);
                                         native_device_context->VSSetShader(copy_vs_c, nullptr, 0);
                                         native_device_context->PSSetShader(own_ps_c, nullptr, 0);
                                         ID3D11Buffer* owncb_c = godc.cb_ownmv.get();
                                         native_device_context->PSSetConstantBuffers(0, 1, &owncb_c);
                                         if (own_full)
                                         {
                                            ID3D11ShaderResourceView* odsv = owndepth.get();
                                            native_device_context->PSSetShaderResources(0, 1, &odsv);
                                         }
                                         native_device_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                                         native_device_context->Draw(4, 0);
                                         ID3D11Buffer* nullcb_c = nullptr;
                                         native_device_context->PSSetConstantBuffers(0, 1, &nullcb_c);
                                         if (own_full)
                                         {
                                            ID3D11ShaderResourceView* nullsrv_c = nullptr;
                                            native_device_context->PSSetShaderResources(0, 1, &nullsrv_c);
                                         }
                                         own_cs_c.Restore(native_device_context);
                                         c_mv_res = godc.tex_mvs_conv.get();
                                         c_mv_real = true;
                                         c_mv_src = (ownm_c == 2) ? "own-rot-neg" : (ownm_c == 3) ? "own-fullA" : (ownm_c == 4) ? "own-fullB" : "own-rot";
                                         c_mv_code = 4;
                                         own_done_c = true;
                                         g_clast_ownguard.store(0, std::memory_order_relaxed); // log-only: own-fed-ok
                                         g_own_p00.store(op00_c, std::memory_order_relaxed);
                                         g_own_p11.store(op11_c, std::memory_order_relaxed);
                                         g_own_used.store(ownm_c, std::memory_order_relaxed);
                                      }
                                   }
                                }
                                else // log-only: projection shape reject
                                   g_clast_ownguard.store(3, std::memory_order_relaxed);
                             }
                             else // log-only: election hold (1) vs pairing fail (2)
                             {
                                if (godc.view_frame_cur != oframe_c)
                                   g_clast_ownguard.store(1, std::memory_order_relaxed);
                                else
                                   g_clast_ownguard.store(2, std::memory_order_relaxed);
                             }
                          }
                          else // log-only: own-mode-off
                             g_clast_ownguard.store(4, std::memory_order_relaxed);
                       }
                       if (!own_done_c && !g_zero_mv.load(std::memory_order_relaxed) &&
                          gd.cached_mvs && gd.cached_mvs_frame == g_hist_frame.load(std::memory_order_relaxed))
                      {
                         // M13: raw fmt35 cache, direct to NGX (61c1f1a proven).
                        // The M10j UNORM->float conversion is deleted: the game
                        // buffer is UINT, so float-sampling it produced garbage
                        // MVs; the core reads the native format correctly itself.
                        // M14: slot-t0 alternative (fmt27 may be the PROCESSED
                        // velocity the native TAA actually consumes).
                        // Cleanup: slot-t0 MV branch deleted (t0 proven HUD, flag
                        // write-never). Decode chain below runs unconditionally.
                        {
                            // M24/M30: decode to zero-centered pixels (upstream UE
                            // formula; shader variant follows the menu mode).
                            // Raw feed makes DLSS read the 0.5 center as a
                            // constant full-screen motion vector.
                            if (g_mv_decode.load(std::memory_order_relaxed) != 0)
                            {
                               if (!gd.tex_mvs_conv || gd.mvs_conv_w != crw || gd.mvs_conv_h != crh)
                               {
                                  gd.srv_mvs_conv.reset();
                                  gd.rtv_mvs_conv.reset();
                                  gd.tex_mvs_conv.reset();
                                  D3D11_TEXTURE2D_DESC md = {};
                                  md.Width = crw; md.Height = crh;
                                  md.MipLevels = 1; md.ArraySize = 1;
                                  md.Format = DXGI_FORMAT_R16G16_FLOAT;
                                  md.SampleDesc.Count = 1;
                                  md.Usage = D3D11_USAGE_DEFAULT;
                                  md.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                                  ComPtr<ID3D11Texture2D> mt;
                                  if (SUCCEEDED(native_device->CreateTexture2D(&md, nullptr, mt.put())))
                                  {
                                     ComPtr<ID3D11ShaderResourceView> ms;
                                     ComPtr<ID3D11RenderTargetView> mr;
                                     if (SUCCEEDED(native_device->CreateShaderResourceView(mt.get(), nullptr, ms.put())) &&
                                         SUCCEEDED(native_device->CreateRenderTargetView(mt.get(), nullptr, mr.put())))
                                     {
                                        gd.tex_mvs_conv = mt;
                                        gd.srv_mvs_conv = ms;
                                        gd.rtv_mvs_conv = mr;
                                        gd.mvs_conv_w = crw; gd.mvs_conv_h = crh;
                                     }
                                  }
                               }
                               if (gd.rtv_mvs_conv)
                               {
                                   int mvdec = g_mv_decode.load(std::memory_order_relaxed);
                                    uint32_t mvkey = (mvdec == 2) ? CompileTimeStringHash("DaysGone MV Convert 025 PS")
                                       : (mvdec == 3) ? CompileTimeStringHash("DaysGone MV Convert Zero PS")
                                       : (mvdec == 4) ? CompileTimeStringHash("DaysGone MV Convert Neg PS")
                                       : (mvdec == 5) ? CompileTimeStringHash("DaysGone MV Convert Zero025 PS")
                                       : (mvdec == 6) ? CompileTimeStringHash("DaysGone MV Convert NegZero PS")
                                       : (mvdec == 7) ? CompileTimeStringHash("DaysGone MV Convert YNegZero PS")
                                       : (mvdec == 8) ? CompileTimeStringHash("DaysGone MV Convert XNegZero PS")
                                       : CompileTimeStringHash("DaysGone MV Convert PS");
                                   ID3D11PixelShader* mv_ps = device_data.native_pixel_shaders[mvkey].get();
                                  ID3D11VertexShader* copy_vs = device_data.native_vertex_shaders[CompileTimeStringHash("Copy VS")].get();
                                  ComPtr<ID3D11ShaderResourceView> cached_mv_srv;
                                  if (mv_ps && copy_vs && SUCCEEDED(native_device->CreateShaderResourceView(mv_sel, nullptr, cached_mv_srv.put())))
                                  {
                                     DrawStateStack<DrawStateStackType::FullGraphics> mv_cs;
                                     mv_cs.Cache(native_device_context, device_data.uav_max_count);
                                     ID3D11UnorderedAccessView* null_uavs[D3D11_1_UAV_SLOT_COUNT] = {};
                                     native_device_context->CSSetUnorderedAccessViews(0, device_data.uav_max_count, null_uavs, nullptr);
                                     D3D11_VIEWPORT vp = {};
                                     vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
                                     vp.Width = (float)crw; vp.Height = (float)crh;
                                     vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
                                     native_device_context->RSSetViewports(1, &vp);
                                     native_device_context->RSSetState(gd.rs_copy.get());
                                     native_device_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
                                     native_device_context->OMSetDepthStencilState(nullptr, 0);
                                     native_device_context->OMSetRenderTargets(1, &gd.rtv_mvs_conv, nullptr);
                                     native_device_context->VSSetShader(copy_vs, nullptr, 0);
                                     native_device_context->PSSetShader(mv_ps, nullptr, 0);
                                     native_device_context->PSSetShaderResources(0, 1, &cached_mv_srv);
                                     native_device_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                                     native_device_context->Draw(4, 0);
                                     mv_cs.Restore(native_device_context);
                                     c_mv_res = gd.tex_mvs_conv.get();
                                     c_mv_real = true;
                                     c_mv_src = mv_sel_name;
                                  }
                               }
                            }
                             else
                             {
                                c_mv_res = mv_sel;
                                c_mv_real = (mv_sel != nullptr);
                                c_mv_src = mv_sel_name;
                             }
                          }
                       }
                       // Hybrid MVs (M11/compute only, default OFF): per-pixel
                       // game truth + own fill. Own output (tex_mvs_conv) and
                       // the resolved game buffer (mv_sel) are both live here,
                       // so decode+select fuse into ONE pass into tex_mvs_hybrid
                       // (no second target, no feed restructuring). Toggle OFF
                       // or missing input: falls through untouched (game conv
                       // OR own OR zero fallback, codes unchanged).
                       if (g_hybrid_mv.load(std::memory_order_relaxed) && own_done_c && mv_sel && gd.srv_mvs_conv)
                       {
                          if (!gd.tex_mvs_hybrid || gd.mvs_hybrid_w != crw || gd.mvs_hybrid_h != crh)
                          {
                             gd.srv_mvs_hybrid.reset();
                             gd.rtv_mvs_hybrid.reset();
                             gd.tex_mvs_hybrid.reset();
                             D3D11_TEXTURE2D_DESC hd = {};
                             hd.Width = crw; hd.Height = crh;
                             hd.MipLevels = 1; hd.ArraySize = 1;
                             hd.Format = DXGI_FORMAT_R16G16_FLOAT;
                             hd.SampleDesc.Count = 1;
                             hd.Usage = D3D11_USAGE_DEFAULT;
                             hd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                             ComPtr<ID3D11Texture2D> ht;
                             if (SUCCEEDED(native_device->CreateTexture2D(&hd, nullptr, ht.put())))
                             {
                                ComPtr<ID3D11ShaderResourceView> hs;
                                ComPtr<ID3D11RenderTargetView> hr;
                                if (SUCCEEDED(native_device->CreateShaderResourceView(ht.get(), nullptr, hs.put())) &&
                                    SUCCEEDED(native_device->CreateRenderTargetView(ht.get(), nullptr, hr.put())))
                                {
                                   gd.tex_mvs_hybrid = ht;
                                   gd.srv_mvs_hybrid = hs;
                                   gd.rtv_mvs_hybrid = hr;
                                   gd.mvs_hybrid_w = crw; gd.mvs_hybrid_h = crh;
                                }
                             }
                          }
                          if (gd.rtv_mvs_hybrid)
                          {
                             int hdec = g_mv_decode.load(std::memory_order_relaxed);
                             uint32_t hybkey = (hdec == 2) ? CompileTimeStringHash("DaysGone MV Hybrid 025 PS")
                                : (hdec == 3) ? CompileTimeStringHash("DaysGone MV Hybrid Zero PS")
                                : (hdec == 4) ? CompileTimeStringHash("DaysGone MV Hybrid Neg PS")
                                : (hdec == 5) ? CompileTimeStringHash("DaysGone MV Hybrid Zero025 PS")
                                : (hdec == 6) ? CompileTimeStringHash("DaysGone MV Hybrid NegZero PS")
                                : (hdec == 7) ? CompileTimeStringHash("DaysGone MV Hybrid YNegZero PS")
                                : (hdec == 8) ? CompileTimeStringHash("DaysGone MV Hybrid XNegZero PS")
                                : CompileTimeStringHash("DaysGone MV Hybrid PS");
                             ID3D11PixelShader* hyb_ps = device_data.native_pixel_shaders[hybkey].get();
                             ID3D11VertexShader* hcopy_vs = device_data.native_vertex_shaders[CompileTimeStringHash("Copy VS")].get();
                             ComPtr<ID3D11ShaderResourceView> game_raw_srv;
                             if (hyb_ps && hcopy_vs && SUCCEEDED(native_device->CreateShaderResourceView(mv_sel, nullptr, game_raw_srv.put())))
                             {
                                DrawStateStack<DrawStateStackType::FullGraphics> hyb_cs;
                                hyb_cs.Cache(native_device_context, device_data.uav_max_count);
                                ID3D11UnorderedAccessView* hnull_uavs[D3D11_1_UAV_SLOT_COUNT] = {};
                                native_device_context->CSSetUnorderedAccessViews(0, device_data.uav_max_count, hnull_uavs, nullptr);
                                D3D11_VIEWPORT hvp = {};
                                hvp.TopLeftX = 0.0f; hvp.TopLeftY = 0.0f;
                                hvp.Width = (float)crw; hvp.Height = (float)crh;
                                hvp.MinDepth = 0.0f; hvp.MaxDepth = 1.0f;
                                native_device_context->RSSetViewports(1, &hvp);
                                native_device_context->RSSetState(gd.rs_copy.get());
                                native_device_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
                                native_device_context->OMSetDepthStencilState(nullptr, 0);
                                native_device_context->OMSetRenderTargets(1, &gd.rtv_mvs_hybrid, nullptr);
                                native_device_context->VSSetShader(hcopy_vs, nullptr, 0);
                                native_device_context->PSSetShader(hyb_ps, nullptr, 0);
                                ID3D11ShaderResourceView* hyb_srvs[2] = { game_raw_srv.get(), gd.srv_mvs_conv.get() };
                                native_device_context->PSSetShaderResources(0, 2, hyb_srvs);
                                native_device_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                                native_device_context->Draw(4, 0);
                                hyb_cs.Restore(native_device_context);
                                c_mv_res = gd.tex_mvs_hybrid.get();
                                c_mv_real = true;
                                c_mv_src = "hybrid";
                                c_mv_code = 5;
                             }
                          }
                       }
                      // M36: feed census (inputs were good -- this frame reaches NGX).
                      if (!c_mv_real) c_mv_code = 0;
                      if (c_mv_real)
                         g_cmv_frames_real.fetch_add(1, std::memory_order_relaxed);
                      else
                         g_cmv_frames_zero.fetch_add(1, std::memory_order_relaxed);
                      SR::SettingsData csettings_data;
                     csettings_data.output_width = cow;
                     csettings_data.output_height = coh;
                     csettings_data.render_width = crw;
                     csettings_data.render_height = crh;
                     csettings_data.dynamic_resolution = false;
                     csettings_data.hdr = true; // fmt10 linear input
                     csettings_data.inverted_depth = g_inverted_depth.load(std::memory_order_relaxed);
                     csettings_data.mvs_jittered = g_mvs_jittered.load(std::memory_order_relaxed);
                     csettings_data.auto_exposure = g_dlss_autoexp.load(std::memory_order_relaxed);
                     csettings_data.render_preset = dlss_render_preset;
                     {
                        float mvsx = 0.0f, mvsy = 0.0f;
                        GetMVScales((float)crw, (float)crh, mvsx, mvsy);
                        csettings_data.mvs_x_scale = mvsx;
                        csettings_data.mvs_y_scale = mvsy;
                     }
                      // M48: feed-identity line (stale-input verdict). Logs
                      // WHICH resources reach NGX + writer stamps over 4
                      // consecutive feeds. Healthy: same descs, stamps ==
                      // current frame every time. Stale pick: older stamps
                      // or flip-flopping identities.
                      if (g_adiag_feed.load(std::memory_order_relaxed) > 0)
                      {
                         char fcb[64] = {}, fdb[64] = {}, fmb[64] = {};
                         DescribeResHandle(c_color.get(), fcb, sizeof(fcb));
                         DescribeResHandle(c_depth.get(), fdb, sizeof(fdb));
                         DescribeResHandle(c_mv_res, fmb, sizeof(fmb));
                         uint64_t fframe = g_hist_frame.load(std::memory_order_relaxed);
                         char fl[768];
                         snprintf(fl, sizeof(fl),
                            "DaysGone ADIAG feed f%d frame=%llu color=%s@%llu depth=%s@%llu mv=%s(%s)@%llu",
                            4 - g_adiag_feed.load(std::memory_order_relaxed),
                            (unsigned long long)fframe,
                            fcb, (unsigned long long)WriterFrame(c_color.get()),
                            fdb, (unsigned long long)WriterFrame(c_depth.get()),
                            fmb, c_mv_src, (unsigned long long)WriterFrame(c_mv_res));
                         reshade::log::message(reshade::log::level::info, fl);
                         g_adiag_feed.fetch_sub(1, std::memory_order_relaxed);
                      }
                      sr_implementations[device_data.sr_type]->UpdateSettings(sr_instance_data, native_device_context, csettings_data);

                     // M62: window resize (game UAV changed size) => force NGX reset.
                     // M64: render-size change counts too (render-scale slider).
                     {
                        uint32_t pcw = g_c_last_cw.load(std::memory_order_relaxed);
                        uint32_t pch = g_c_last_ch.load(std::memory_order_relaxed);
                        uint32_t prw = g_c_last_crw.load(std::memory_order_relaxed);
                        uint32_t prh = g_c_last_crh.load(std::memory_order_relaxed);
                        if (pcw && pch && (cow != pcw || coh != pch || crw != prw || crh != prh))
                        {
                           device_data.force_reset_sr = true;
                           char rb[128];
                           snprintf(rb, sizeof(rb), "DaysGone cDLSS resize %ux%u -> %ux%u (render %ux%u): forcing NGX reset",
                              pcw, pch, cow, coh, crw, crh);
                           reshade::log::message(reshade::log::level::info, rb);
                        }
                        g_c_last_cw.store(cow, std::memory_order_relaxed);
                        g_c_last_ch.store(coh, std::memory_order_relaxed);
                        g_c_last_crw.store(crw, std::memory_order_relaxed);
                        g_c_last_crh.store(crh, std::memory_order_relaxed);
                     }
                     const bool creset = device_data.force_reset_sr || gd.first_cdlss_frame;
                     device_data.force_reset_sr = false;
                     if (!gd.tex_dlss_out || !c_mv_res)
                     {
                        device_data.force_reset_sr = true;
                     }
                     else
                     {
                        SR::SuperResolutionImpl::DrawData cdraw_data;
                        cdraw_data.source_color = c_color.get();
                        cdraw_data.output_color = gd.tex_dlss_out.get();
                        cdraw_data.motion_vectors = c_mv_res;
                        // Cleanup: g_no_depth branch retired (write-never flag;
                        // core requires depth -- null depth guarantees Draw fail).
                        cdraw_data.depth_buffer = c_depth.get();
                        cdraw_data.render_width = crw;
                        cdraw_data.render_height = crh;
                        cdraw_data.reset = creset;
                        cdraw_data.near_plane = g_near_plane.load(std::memory_order_relaxed);
                        cdraw_data.far_plane = g_far_plane.load(std::memory_order_relaxed);
                        cdraw_data.vert_fov = g_vert_fov.load(std::memory_order_relaxed);
                        // M12/M21: compute jitter from CS CB0[0]/[1], DIRECTLY in
                        // pixels. CB-dump proof: [0]/[1] animate (0.03-0.49px
                        // across dumps) while [26]/[27] are a CONSTANT 1px
                        // offset (texel bias, not jitter). Off by default
                        // (g_cjitter_on) until it proves itself.
                        float cjit_x = 0.0f, cjit_y = 0.0f;
                        {
                           ID3D11Buffer* ccb = nullptr;
                           native_device_context->CSGetConstantBuffers(0, 1, &ccb);
                           if (ccb)
                           {
                              D3D11_BUFFER_DESC bd = {};
                              ccb->GetDesc(&bd);
                              if (bd.ByteWidth >= 112)
                              {
                                 if (!gd.staging_cb || gd.staging_size != bd.ByteWidth)
                                 {
                                    gd.staging_cb.reset();
                                    D3D11_BUFFER_DESC sd = {};
                                    sd.ByteWidth = bd.ByteWidth;
                                    sd.Usage = D3D11_USAGE_STAGING;
                                    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                                    if (SUCCEEDED(native_device->CreateBuffer(&sd, nullptr, gd.staging_cb.put())))
                                       gd.staging_size = bd.ByteWidth;
                                    else
                                       gd.staging_size = 0;
                                 }
                                 if (gd.staging_cb)
                                 {
                                    native_device_context->CopySubresourceRegion(gd.staging_cb.get(), 0, 0, 0, 0, ccb, 0, nullptr);
                                    D3D11_MAPPED_SUBRESOURCE m = {};
                                    if (SUCCEEDED(native_device_context->Map(gd.staging_cb.get(), 0, D3D11_MAP_READ, 0, &m)) && m.pData)
                                    {
                                       const float* f = (const float*)m.pData;
                                       // M21: real jitter = CB0[0]/[1] DIRECTLY
                                       // in pixels (CB-dump proof: animated
                                       // 0.03-0.49px across dumps; [26]/[27]
                                       // are a CONSTANT 1px offset, not jitter).
                                       g_jit_rawx.store(f[0]);
                                       g_jit_rawy.store(f[1]);
                                       // templog: FULLAGG jitter accumulators (log-only).
                                       // quad<xs><ys>: 1 = non-negative axis.
                                       g_agg_jitn.fetch_add(1, std::memory_order_relaxed);
                                       g_agg_jitsumx.fetch_add((int64_t)(f[0] * 1000.0f), std::memory_order_relaxed);
                                       g_agg_jitsumy.fetch_add((int64_t)(f[1] * 1000.0f), std::memory_order_relaxed);
                                       if (f[0] >= 0.0f) { if (f[1] >= 0.0f) g_agg_q11.fetch_add(1, std::memory_order_relaxed); else g_agg_q10.fetch_add(1, std::memory_order_relaxed); }
                                       else { if (f[1] >= 0.0f) g_agg_q01.fetch_add(1, std::memory_order_relaxed); else g_agg_q00.fetch_add(1, std::memory_order_relaxed); }
                                       cjit_x = f[0];
                                       cjit_y = f[1];
                                       if (cjit_x > 32.0f || cjit_x < -32.0f || cjit_y > 32.0f || cjit_y < -32.0f)
                                       {
                                          cjit_x = 0.0f; cjit_y = 0.0f;
                                       }
                                       g_jit_pxx.store(cjit_x);
                                       g_jit_pxy.store(cjit_y);
                                       g_jitdbg_00.store(f[0]);
                                       g_jitdbg_01.store(f[1]);
                                       g_jitdbg_26.store(f[26]);
                                       g_jitdbg_27.store(f[27]);
                                       g_jitdbg_cjit.store(cjit_x);
                                       g_jitdbg_cjit_y.store(cjit_y);
                                       native_device_context->Unmap(gd.staging_cb.get(), 0);
                                    }
                                 }
                              }
                              ccb->Release();
                           }
                        }
                        cdraw_data.jitter_x = 0.0f;
                        cdraw_data.jitter_y = 0.0f;
                        if (g_cjitter_on.load(std::memory_order_relaxed))
                        {
                           float cjs = GetJitScale();
                           cdraw_data.jitter_x = (g_jit_flip_x.load(std::memory_order_relaxed) ? 1.0f : -1.0f) * cjit_x * cjs;
                           cdraw_data.jitter_y = (g_jit_flip_y.load(std::memory_order_relaxed) ? 1.0f : -1.0f) * cjit_y * cjs;
                        }
                         cdraw_data.frame_index = cb_luma_global_settings.FrameIndex;
                         // M59: store the exact NGX inputs (named feed viewer).
                         // M60: snapshot + mv_real + depth tag (frozen truth).
                         StoreFeed(0, c_color.get(), c_depth.get(), c_mv_res, gd.tex_dlss_out.get(),
                            native_device, native_device_context, c_mv_real, c_depth_src);
                         const bool cok = ([&]() {
                           DrawStateStack<DrawStateStackType::FullGraphics> draw_st;
                           draw_st.Cache(native_device_context, device_data.uav_max_count);
                           DrawStateStack<DrawStateStackType::Compute> st;
                           st.Cache(native_device_context, device_data.uav_max_count);
                           const bool r = sr_implementations[device_data.sr_type]->Draw(sr_instance_data, native_device_context, cdraw_data);
                           st.Restore(native_device_context);
                           draw_st.Restore(native_device_context);
                           return r;
                        })();
                         // M14: report the TRUE format of whatever feeds MVs
                         // (settles the fmt35 UNORM-vs-UINT question live).
                         // M36: computed EVERY frame for the menu snapshot;
                         // the log line below stays first-Draw-only.
                         int mvfmt = -1;
                         {
                            ComPtr<ID3D11Texture2D> q;
                            if (c_mv_res && SUCCEEDED(c_mv_res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(q.put()))))
                            {
                               D3D11_TEXTURE2D_DESC qd = {};
                               q->GetDesc(&qd);
                               mvfmt = (int)qd.Format;
                            }
                         }
                         g_clast_rw.store((int)crw); g_clast_rh.store((int)crh);
                         g_clast_ow.store((int)cow); g_clast_oh.store((int)coh);
                         g_clast_mvfmt.store(mvfmt); g_clast_code.store(c_mv_code);
                         g_clast_mvhash.store(gd.cached_mvs_hash);
                         g_clast_mvreal.store(c_mv_real ? 1 : 0);
                         // FED MV identity tap (log-only, relaxed like the rest).
                         g_clast_mvcode.store(c_mv_code, std::memory_order_relaxed);
                         snprintf(g_clast_mvsrc, sizeof(g_clast_mvsrc), "%s", c_mv_src ? c_mv_src : "?");
                         g_clast_mvselframe.store(mv_sel ? mv_sel_frame : -1, std::memory_order_relaxed);
                         // Own fallback outcome (log-only): game MVs (5) or zero (6).
                         if (!own_done_c)
                            g_clast_ownguard.store(c_mv_real ? 5 : 6, std::memory_order_relaxed);
                         // viewfilter snapshots (log-only): pool reads + skips this present.
                         g_clast_preads.store(gd.view_pool_reads, std::memory_order_relaxed);
                         g_clast_pskip.store(g_view_pool_skips.load(std::memory_order_relaxed), std::memory_order_relaxed);
                         g_clast_reset.store(creset ? 1 : 0);
                         g_clast_ok.store(cok ? 1 : 0);
                         g_clast_jitx.store(cdraw_data.jitter_x);
                         g_clast_jity.store(cdraw_data.jitter_y);
                         // M57: audit every requested feed (both paths share the sequence).
                         if (g_audit_left.load(std::memory_order_relaxed) > 0)
                         {
                            LogAuditFeed("M11", c_color.get(), c_depth.get(), c_mv_res, c_mv_src, mvfmt,
                               crw, crh, cow, coh, true, creset, cdraw_data.jitter_x, cdraw_data.jitter_y, cok, c_depth_src);
                            g_audit_left.fetch_sub(1, std::memory_order_relaxed);
                         }
                         if (!gd.logged_cdraw_result)
                         {
                            gd.logged_cdraw_result = true;
                            char b[320];
                           snprintf(b, sizeof(b), "DaysGone cDLSS first Draw ok=%d reset=%d render=%ux%u out=%ux%u sr=%s mv_real=%d mvsrc=%s mvfmt=%d mvhash=%08X msc=%d dec=%d inv=%d cso=%d dsrc=%s jit=%.2f,%.2f",
                              (int)cok, (int)creset, crw, crh, cow, coh, SrTypeName(device_data.sr_type), (int)c_mv_real, c_mv_src, mvfmt, gd.cached_mvs_hash,
                              g_mv_scale_mode.load(std::memory_order_relaxed), g_mv_decode.load(std::memory_order_relaxed), (int)g_inverted_depth.load(std::memory_order_relaxed),
                              (int)g_c_srgb_output.load(std::memory_order_relaxed), c_depth_src,
                              cdraw_data.jitter_x, cdraw_data.jitter_y);
                           reshade::log::message(reshade::log::level::info, b);
                        }
                        // M63: Draw failures after the first were silent (native
                        // runs instead, looks like "DLSS does nothing"). Log the
                        // first + every 600th with dims so a resize that breaks
                        // NGX (e.g. render>out) leaves a trail.
                        if (!cok)
                        {
                           uint64_t ndf = g_cfail_draw.fetch_add(1, std::memory_order_relaxed) + 1;
                           if (ndf == 1 || ndf % 600 == 0)
                           {
                              char db[256];
                              snprintf(db, sizeof(db), "DaysGone cDLSS Draw FAILED x%llu (ok=0) render=%ux%u out=%ux%u reset=%d mv=%d depth=%d -- native TAA runs, NGX retried with reset",
                                 (unsigned long long)ndf, crw, crh, cow, coh, (int)creset,
                                 (int)(bool)c_mv_res, (int)(bool)c_depth);
                              reshade::log::message(reshade::log::level::warning, db);
                           }
                        }
                        if (cok)
                        {
                           // M20: u0 gets the sRGB-encoded scene (native u0
                           // carries the TAA's sRGB tail -- bytecode proof), u1
                           // history stays LINEAR (written pre-tail natively).
                           // RTV creation fails if the UAV target isn't
                           // RTV-bindable -- then we stay native (log once).
                           // M13/M15/M20: HUD composite over u0 ONLY (u1 must
                           // stay scene-clean for next frame's history).
                           bool ccopied0 = false, ccopied1 = false;
                           bool c_srgb = g_c_srgb_output.load(std::memory_order_relaxed);
                           // M22: force-opaque scene alpha (dark-gradient triage).
                           bool c_alpha1 = (g_c_alpha_mode.load(std::memory_order_relaxed) == 1);
                           ComPtr<ID3D11RenderTargetView> c_rtv0, c_rtv1;
                           if (SUCCEEDED(native_device->CreateRenderTargetView(c_out0.get(), nullptr, c_rtv0.put())))
                           {
                              ccopied0 = RunCopyPass(native_device, native_device_context, device_data, gd,
                                 gd.srv_dlss_out.get(), c_rtv0.get(), cow, coh,
                                 g_swap_output.load(std::memory_order_relaxed), c_srgb, -1, true, c_alpha1);
                           }
                           if (c_out1 && SUCCEEDED(native_device->CreateRenderTargetView(c_out1.get(), nullptr, c_rtv1.put())))
                           {
                              ccopied1 = RunCopyPass(native_device, native_device_context, device_data, gd,
                                 gd.srv_dlss_out.get(), c_rtv1.get(), cow, coh,
                                 g_swap_output.load(std::memory_order_relaxed), false, -1, true, c_alpha1);
                           }
                           (void)ccopied1;
                            if (!ccopied0)
                            {
                               g_cfail_rtv.fetch_add(1, std::memory_order_relaxed);
                               if (!gd.logged_crtv_fail)
                              {
                                 gd.logged_crtv_fail = true;
                                 reshade::log::message(reshade::log::level::warning,
                                    "DaysGone cDLSS: u0 copy failed (RTV-bind?) -- native compute TAA.");
                              }
                              device_data.force_reset_sr = true;
                           }
                           else
                           {
                              // M13/M15/M20: HUD composite over u0 ONLY (u1 is the
                              // next frame's scene-only history -- compositing
                              // UI there burns it in on motion). Size-gated
                              // inside RunUIComposite (1x1 params ignored).
                              // Best effort: a failed composite still returns
                              // the scene frame.
                              if (g_ccomposite_ui.load(std::memory_order_relaxed))
                              {
                                 int uisrc = g_cui_src.load(std::memory_order_relaxed);
                                 ID3D11ShaderResourceView* cui = (uisrc >= 0 && uisrc <= 3) ? csrvs[uisrc].get() : nullptr;
                                 if (cui)
                                 {
                                    RunUIComposite(native_device, native_device_context, device_data, gd,
                                       cui, c_rtv0.get(), cow, coh);
                                 }
                              }
                              gd.first_cdlss_frame = false;
                              gd.cdlss_runs++;
                              g_cdlss_runs.fetch_add(1, std::memory_order_relaxed);
                               if (creset)
                               {
                                  g_cdlss_resets.fetch_add(1, std::memory_order_relaxed);
                                  g_agg_resets.fetch_add(1, std::memory_order_relaxed); // templog FULLAGG window
                               }
                              if (g_mark_sr.load(std::memory_order_relaxed))
                                 device_data.has_drawn_sr = true;
                              return DrawOrDispatchOverrideType::Skip;
                           }
                        }
                        else
                        {
                           device_data.force_reset_sr = true;
                        }
                     }
                  }
               }
            }
#else
            (void)native_device;
            (void)cmd_list_data;
#endif
         }
      }

      // M7 DLSS first light at the TAAU slot (PS 000573B8).
      // M10d: viewer/logger bypass the DLSS master gate (else "none showed").
      if (!is_custom_pass && stages != reshade::api::shader_stage::all_compute &&
          (g_dlss_master.load(std::memory_order_relaxed) || g_view_src.load(std::memory_order_relaxed) >= 0 ||
           g_log_slot.load(std::memory_order_relaxed)))
      {
         uint32_t ps = original_shader_hashes.pixel_shaders.empty() ? 0 : original_shader_hashes.pixel_shaders[0];
         if (ps == kDlssSlotPS)
         {
#if ENABLE_SR
            g_dlss_attempts.fetch_add(1, std::memory_order_relaxed);
            // M10d standalone viewer + slot logger: works with sr_type None
            // (the old viewer sat INSIDE the sr_type!=None gate, so with SR
            // unset every source "showed" native = "none showed UI").
            // Cleanup: pixel viewer/logger block retired (menu-unreachable since
            // menuclean -- g_view_src/g_log_slot have no stores, so vsel0 is
            // always -1 and want_log always false; the slot-dims logger and
            // 15-source viewer never fired). Compute viewer (g_cview_src) +
            // feed viewer (g_feedview) + one channel combo cover diagnosis.
            // Globals stay defined for compat; the M7 master gate below and
            // the DLSS path are untouched.
             // Viewer/logger above run masterless; DLSS below stays gated.
             if (!g_dlss_master.load(std::memory_order_relaxed))
                return DrawOrDispatchOverrideType::None;
             // M53 (was M16 park): M11 compute has priority while it fires,
             // but a silent compute slot (e.g. reduced render scale moves
             // the game to its TAAU upscale resolve) must NOT park M7 --
             // else DLSS idles on both paths. Unpark after 30 silent
             // presents (~0.5s); re-parks on the next compute fire, so the
             // two never fight over the shared NGX instance.
             {
                uint64_t cur_p = g_hist_frame.load(std::memory_order_relaxed);
                uint64_t last_c = g_cdlss_last_fire.load(std::memory_order_relaxed);
                bool silent = !g_cdlss_master.load(std::memory_order_relaxed) ? false
                   : (cur_p < last_c ? true : (cur_p - last_c) >= 30);
                // Note: M11 master off => M7 runs (old behavior, unchanged).
                if (g_cdlss_master.load(std::memory_order_relaxed) && !silent)
                   return DrawOrDispatchOverrideType::None;
                static bool logged_park = false;
                if (silent && !logged_park)
                {
                   logged_park = true;
                   reshade::log::message(reshade::log::level::info,
                      "DaysGone M53: compute slot silent 30+ presents -- M7 pixel path unparked.");
                }
                else if (!silent && logged_park)
                {
                   logged_park = false;
                   reshade::log::message(reshade::log::level::info,
                      "DaysGone M53: compute slot firing -- M7 pixel path parked.");
                }
             }
            if (device_data.sr_type != SR::Type::None)
            {
               auto* sr_instance_data = device_data.GetSRInstanceData();
               // Immediate context only ??" NGX work submission isn't safe on deferred lists.
               bool is_immediate = (native_device_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE);
               if (sr_instance_data && is_immediate)
               {
                  // Inputs: t1/t2 ping-pong color pair (1920x1080 fmt24).
                  // M7b: scan t0-t15 (depth hides above t3) + R32F depth.
                  ComPtr<ID3D11ShaderResourceView> srvs[16] = {};
                  ID3D11ShaderResourceView* raw_srvs[16] = {};
                  native_device_context->PSGetShaderResources(0, 16, raw_srvs);
                  for (int i = 0; i < 16; i++) srvs[i] = raw_srvs[i];
                  ComPtr<ID3D11Resource> res_color;
                  uint32_t rw = 0, rh = 0;
                  // M7c: among fmt24 render-size candidates, pick the one
                  // WRITTEN this frame (current); the ping-pong twin is stale
                  // history. Feeding them alternately = 30Hz flash (M7 bug).
                  // srv_color_pick survives for the M9b unpack pass below.
                  ComPtr<ID3D11ShaderResourceView> srv_color_pick;
                  {
                     struct Cand { ComPtr<ID3D11Resource> res; ComPtr<ID3D11ShaderResourceView> srv; uint32_t w, h; };
                     Cand cands[16];
                     int nc = 0;
                     for (int i = 1; i < 16; i++)
                     {
                        if (!srvs[i]) continue;
                        ID3D11Resource* tmp_res = nullptr;
                        srvs[i]->GetResource(&tmp_res);
                        ComPtr<ID3D11Resource> r;
                        r.attach(tmp_res);
                        if (!r) continue;
                        ComPtr<ID3D11Texture2D> t;
                        if (FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(t.put())))) continue;
                        D3D11_TEXTURE2D_DESC d = {};
                        t->GetDesc(&d);
                        if (d.Width >= 400 && d.Format == static_cast<DXGI_FORMAT>(24) && nc < 16) // M55: was 1000
                        {
                           cands[nc].res = r;
                           cands[nc].srv = srvs[i];
                           cands[nc].w = d.Width; cands[nc].h = d.Height;
                           nc++;
                        }
                     }
                     uint64_t cur = g_hist_frame.load(std::memory_order_relaxed);
                     int pick = -1;
                     {
                        std::lock_guard<std::mutex> lk(g_writer_mutex);
                        for (int i = 0; i < nc; i++)
                        {
                           auto it = g_rt_last_writer.find((uint64_t)cands[i].res.get());
                           if (it != g_rt_last_writer.end() && it->second == cur) { pick = i; break; }
                        }
                     }
                     if (pick < 0 && nc > 0) pick = 0; // fallback: first
                     if (pick >= 0)
                     {
                        res_color = cands[pick].res;
                        rw = cands[pick].w; rh = cands[pick].h;
                        // Freshness: count handle changes across runs.
                        uint64_t hnow = (uint64_t)res_color.get();
                        if (g_last_color_handle.exchange(hnow) != hnow)
                           g_color_changes.fetch_add(1, std::memory_order_relaxed);
                     }
                     // Keep the winning view for the M9b unpack pass.
                     if (pick >= 0)
                        srv_color_pick = cands[pick].srv;
                  }
                  // Depth: first depth-format SRV at any slot, else same-frame
                  // cache from the post family (the slot binds no depth).
                  ComPtr<ID3D11Resource> res_depth;
                  for (int i = 0; i < 16 && !res_depth; i++)
                  {
                     if (!srvs[i]) continue;
                     ID3D11Resource* tmp_res = nullptr;
                     srvs[i]->GetResource(&tmp_res);
                     ComPtr<ID3D11Resource> r;
                     r.attach(tmp_res);
                     if (!r) continue;
                     ComPtr<ID3D11Texture2D> t;
                     if (FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(t.put())))) continue;
                     D3D11_TEXTURE2D_DESC d = {};
                     t->GetDesc(&d);
                     switch (d.Format)
                     {
                     case DXGI_FORMAT_R24G8_TYPELESS:
                     case DXGI_FORMAT_D24_UNORM_S8_UINT:
                     case DXGI_FORMAT_R32G8X24_TYPELESS:
                     case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
                     case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
                     case DXGI_FORMAT_R32_FLOAT: // linear depth as texture
                     case DXGI_FORMAT_D16_UNORM:
                        res_depth = r;
                        break;
                     default: break;
                     }
                  }
                  bool depth_cached = false;
                  auto& gd_cache = *static_cast<DaysGoneDeviceData*>(device_data.game);
                  if (!res_depth && gd_cache.cached_depth &&
                      gd_cache.cached_depth_frame == g_hist_frame.load(std::memory_order_relaxed))
                  {
                     res_depth = gd_cache.cached_depth;
                     depth_cached = true;
                  }
                  // Game output RT (must be UAV-bound for direct DLSS draw).
                  ComPtr<ID3D11RenderTargetView> rtv;
                  ID3D11RenderTargetView* raw_rtv = nullptr;
                  native_device_context->OMGetRenderTargets(1, &raw_rtv, nullptr);
                  rtv = raw_rtv;
                  ComPtr<ID3D11Resource> res_out;
                  uint32_t ow = 0, oh = 0;
                  bool out_uav = false;
                   if (rtv)
                   {
                      ID3D11Resource* tmp_out = nullptr;
                      rtv->GetResource(&tmp_out);
                      res_out.attach(tmp_out);
                      ComPtr<ID3D11Texture2D> t;
                      if (res_out && SUCCEEDED(res_out->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(t.put()))))
                     {
                        D3D11_TEXTURE2D_DESC d = {};
                        t->GetDesc(&d);
                        ow = d.Width; oh = d.Height;
                        out_uav = (d.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
                     }
                  }
                  for (int i = 0; i < 16; i++) { if (raw_srvs[i]) raw_srvs[i]->Release(); }
                  if (raw_rtv) raw_rtv->Release();

                  auto& gd = *static_cast<DaysGoneDeviceData*>(device_data.game);
                  // (M10d: viewer moved to the standalone masterless block
                  // above -- plain copy, no sRGB wash. Removed here.)
                  bool inputs_ok = res_color && res_depth && res_out && rw && ow;
                   // M10k: DLAA (render==output, the M7 goal) is allowed by
                   // default. The M10 hard block on ow<=rw||oh<=rh made the M7
                   // toggle a no-op in normal 100%-scale gameplay. Only take
                   // the native path on a true downscale (ow<rw||oh<rh -- never
                   // a DLSS case), or when the upscale-only guard is ticked.
                   // Menu/map UI modes at render==output are handled by the
                   // t0 composite (M10d, on by default).
                   bool is_downscale = (ow < rw || oh < rh);
                   // Cleanup: upscale-only guard retired (write-never flag;
                   // DLAA render==output is the M7 goal, guard made it a no-op).
                   bool blocked_by_guard = false;
                   if (inputs_ok && (is_downscale || blocked_by_guard))
                   {
                      static bool logged_no_upscale = false;
                      if (!logged_no_upscale)
                      {
                         logged_no_upscale = true;
                         reshade::log::message(reshade::log::level::info,
                            "DaysGone DLSS: non-upscale frame -- native pass (upscale-only guard).");
                      }
                      device_data.force_reset_sr = true;
                      return DrawOrDispatchOverrideType::None;
                   }
                  if (!inputs_ok)
                  {
                     if (!gd.logged_capture_fail)
                     {
                        gd.logged_capture_fail = true;
                        char b[160];
                        snprintf(b, sizeof(b), "DaysGone DLSS capture FAIL: color=%d depth=%d out=%d rw=%u ow=%u",
                           (int)(bool)res_color, (int)(bool)res_depth, (int)(bool)res_out, rw, ow);
                        reshade::log::message(reshade::log::level::warning, b);
                     }
                     device_data.force_reset_sr = true;
                  }
                  // M9: game RT needs no UAV bind anymore (pixel copy via RTV).
                  {
                     // M9b: own FP16 color input (unpack fmt24 -> float).
                     if (!gd.tex_color_in || gd.color_in_w != rw || gd.color_in_h != rh)
                     {
                        gd.srv_color_in.reset();
                        gd.rtv_color_in.reset();
                        gd.tex_color_in.reset();
                        D3D11_TEXTURE2D_DESC cd = {};
                        cd.Width = rw; cd.Height = rh;
                        cd.MipLevels = 1; cd.ArraySize = 1;
                        cd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                        cd.SampleDesc.Count = 1;
                        cd.Usage = D3D11_USAGE_DEFAULT;
                        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                        ComPtr<ID3D11Texture2D> ct;
                        if (SUCCEEDED(native_device->CreateTexture2D(&cd, nullptr, ct.put())))
                        {
                           ComPtr<ID3D11ShaderResourceView> csrv;
                           ComPtr<ID3D11RenderTargetView> crtv;
                           if (SUCCEEDED(native_device->CreateShaderResourceView(ct.get(), nullptr, csrv.put())) &&
                               SUCCEEDED(native_device->CreateRenderTargetView(ct.get(), nullptr, crtv.put())))
                           {
                              gd.tex_color_in = ct;
                              gd.srv_color_in = csrv;
                              gd.rtv_color_in = crtv;
                              gd.color_in_w = rw; gd.color_in_h = rh;
                           }
                        }
                     }
                     // Zero-MV texture at render res (M7: real MVs at M8+).
                     if (!gd.tex_dlss_zero_mvs || gd.zero_mvs_w != rw || gd.zero_mvs_h != rh)
                     {
                        gd.srv_dlss_zero_mvs.reset();
                        gd.tex_dlss_zero_mvs.reset();
                        D3D11_TEXTURE2D_DESC zd = {};
                        zd.Width = rw; zd.Height = rh;
                        zd.MipLevels = 1; zd.ArraySize = 1;
                        zd.Format = DXGI_FORMAT_R16G16_FLOAT;
                        zd.SampleDesc.Count = 1;
                        zd.Usage = D3D11_USAGE_DEFAULT;
                        zd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                        std::vector<uint8_t> zeros((size_t)rw * rh * 4, 0);
                        D3D11_SUBRESOURCE_DATA init = {};
                        init.pSysMem = zeros.data();
                        init.SysMemPitch = rw * 4;
                        ComPtr<ID3D11Texture2D> zt;
                        if (SUCCEEDED(native_device->CreateTexture2D(&zd, &init, zt.put())))
                        {
                           if (SUCCEEDED(native_device->CreateShaderResourceView(zt.get(), nullptr, gd.srv_dlss_zero_mvs.put())))
                           {
                              gd.tex_dlss_zero_mvs = zt;
                              gd.zero_mvs_w = rw; gd.zero_mvs_h = rh;
                           }
                        }
                     }
                     // Cleanup: M9d bypass branch retired (write-never flag;
                     // show-input diagnostic superseded by the feed viewer).
                     // M9i: prefer cached linear HDR (unambiguous RGB in every
                     // mode) over the mode-dependent fmt24 pair. Decided here
                     // so render dims + hdr flag agree downstream.
                     // M10c: HDR only when preferred (pre-UI suspect) --
                     // default back to pair (may carry UI).
                     bool hdr_feed = false;
                      if (g_prefer_hdr.load(std::memory_order_relaxed) &&
                          gd.cached_hdr && gd.cached_hdr_frame == g_hist_frame.load(std::memory_order_relaxed) &&
                          gd.cached_hdr_w >= 400) // M55: was 1000
                     {
                        hdr_feed = true;
                     }
                     SR::SettingsData settings_data;
                     settings_data.output_width = ow;
                     settings_data.output_height = oh;
                     settings_data.render_width = rw;
                     settings_data.render_height = rh;
                     settings_data.dynamic_resolution = false;
                     settings_data.hdr = hdr_feed ? true : g_dlss_hdr.load(std::memory_order_relaxed);
                     settings_data.inverted_depth = g_inverted_depth.load(std::memory_order_relaxed);
                     settings_data.mvs_jittered = g_mvs_jittered.load(std::memory_order_relaxed);
                     settings_data.auto_exposure = g_dlss_autoexp.load(std::memory_order_relaxed);
                     settings_data.render_preset = dlss_render_preset;
                     {
                        float mvsx = 0.0f, mvsy = 0.0f;
                        GetMVScales((float)rw, (float)rh, mvsx, mvsy);
                        settings_data.mvs_x_scale = mvsx;
                        settings_data.mvs_y_scale = mvsy;
                     }
                     sr_implementations[device_data.sr_type]->UpdateSettings(sr_instance_data, native_device_context, settings_data);

                     // M62: window resize (game RT changed size) => force NGX reset.
                     // M64: render-size change counts too (render-scale slider).
                     {
                        uint32_t poww = g_m7_last_ow.load(std::memory_order_relaxed);
                        uint32_t powh = g_m7_last_oh.load(std::memory_order_relaxed);
                        uint32_t prw = g_m7_last_rw.load(std::memory_order_relaxed);
                        uint32_t prh = g_m7_last_rh.load(std::memory_order_relaxed);
                        if (poww && powh && (ow != poww || oh != powh || rw != prw || rh != prh))
                        {
                           device_data.force_reset_sr = true;
                           char rb[128];
                           snprintf(rb, sizeof(rb), "DaysGone M7 resize %ux%u -> %ux%u (render %ux%u): forcing NGX reset",
                              poww, powh, ow, oh, rw, rh);
                           reshade::log::message(reshade::log::level::info, rb);
                        }
                        g_m7_last_ow.store(ow, std::memory_order_relaxed);
                        g_m7_last_oh.store(oh, std::memory_order_relaxed);
                        g_m7_last_rw.store(rw, std::memory_order_relaxed);
                        g_m7_last_rh.store(rh, std::memory_order_relaxed);
                     }
                     const bool reset = device_data.force_reset_sr || gd.first_dlss_frame;
                     device_data.force_reset_sr = false;
                     // M9b: unpack fmt24 game color -> own FP16 (hardware does
                     // UNORM10 -> float on load; NGX misreads packed fmt24).
                     // Falls back to direct feed if anything is missing.
                     // M9c: swizzle variant selectable live (BGR-ordered data).
                     // M9e: via RunCopyPass (forced clean state).
                     ID3D11Resource* dlss_color = res_color.get();
                     if (hdr_feed)
                     {
                        dlss_color = gd.cached_hdr.get();
                        rw = gd.cached_hdr_w; rh = gd.cached_hdr_h;
                     }
                     else if (gd.tex_color_in && srv_color_pick)
                     {
                        if (RunCopyPass(native_device, native_device_context, device_data, gd,
                            srv_color_pick.get(), gd.rtv_color_in.get(), rw, rh,
                            g_swap_unpack.load(std::memory_order_relaxed), false))
                           dlss_color = gd.tex_color_in.get();
                     }
                     // M9: own FP16 output target (UAV for NGX) + SRV for copy.
                      // M64: same render>out overscan as the compute path (see
                      // above) -- output target covers max(render, output).
                      uint32_t odw = ow > rw ? ow : rw, odh = oh > rh ? oh : rh;
                      if (!gd.tex_dlss_out || gd.dlss_out_w != odw || gd.dlss_out_h != odh)
                      {
                         gd.srv_dlss_out.reset();
                         gd.tex_dlss_out.reset();
                         D3D11_TEXTURE2D_DESC od = {};
                         od.Width = odw; od.Height = odh;
                        od.MipLevels = 1; od.ArraySize = 1;
                        od.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                        od.SampleDesc.Count = 1;
                        od.Usage = D3D11_USAGE_DEFAULT;
                        od.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                        ComPtr<ID3D11Texture2D> ot;
                        if (SUCCEEDED(native_device->CreateTexture2D(&od, nullptr, ot.put())))
                        {
                           if (SUCCEEDED(native_device->CreateShaderResourceView(ot.get(), nullptr, gd.srv_dlss_out.put())))
                           {
                                 gd.tex_dlss_out = ot;
                                 gd.dlss_out_w = odw; gd.dlss_out_h = odh;
                           }
                        }
                     }
                     if (!gd.tex_dlss_out)
                     {
                        // No output target -- let the native TAAU run.
                        device_data.force_reset_sr = true;
                        return DrawOrDispatchOverrideType::None;
                     }
                     // M10b: sanity clamp. cb0[0] read 285x444px live -- that is
                     // the disasm's DITHER-hash offset (feeds sincos), NOT
                     // reprojection jitter. Real TAA jitter is a few px max.
                     // Feeding hundreds of px smeared the frame vertically and
                     // buried the HUD. Clamp kills the smear; true jitter
                     // still needs the camera-CB probe (parked-car test).
                     float jit_x = 0.0f, jit_y = 0.0f;
                     bool jit_clamped = false;
                     {
                        ID3D11Buffer* cb0 = nullptr;
                        native_device_context->PSGetConstantBuffers(0, 1, &cb0);
                        if (cb0)
                        {
                           D3D11_BUFFER_DESC bd = {};
                           cb0->GetDesc(&bd);
                           if (bd.ByteWidth >= 112)
                           {
                              if (!gd.staging_cb || gd.staging_size != bd.ByteWidth)
                              {
                                 gd.staging_cb.reset();
                                 D3D11_BUFFER_DESC sd = {};
                                 sd.ByteWidth = bd.ByteWidth;
                                 sd.Usage = D3D11_USAGE_STAGING;
                                 sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                                 if (SUCCEEDED(native_device->CreateBuffer(&sd, nullptr, gd.staging_cb.put())))
                                    gd.staging_size = bd.ByteWidth;
                                 else
                                    gd.staging_size = 0;
                              }
                               if (gd.staging_cb)
                               {
                                  native_device_context->CopySubresourceRegion(gd.staging_cb.get(), 0, 0, 0, 0, cb0, 0, nullptr);
                                  D3D11_MAPPED_SUBRESOURCE m = {};
                                  if (SUCCEEDED(native_device_context->Map(gd.staging_cb.get(), 0, D3D11_MAP_READ, 0, &m)) && m.pData)
                                  {
                                     const float* f = (const float*)m.pData;
                                     // Cleanup: M10f full-CB one-shot dump retired
                                     // (g_dump_cb has no stores since menuclean;
                                     // compute-side g_cdump_cb covers CB triage).
                                     // M10h: real jitter = cb0[26]/[27] (1-2px, from CB dump).
                                     // cb0[0]/[1] = dither offset (hundreds of px) -- logged only.
                                     g_jit_rawx.store(f[26]);
                                     g_jit_rawy.store(f[27]);
                                     jit_x = f[26] * (float)rw;
                                     jit_y = f[27] * (float)rh;
                                     if (jit_x > 32.0f || jit_x < -32.0f || jit_y > 32.0f || jit_y < -32.0f)
                                     {
                                        jit_clamped = true;
                                        jit_x = 0.0f; jit_y = 0.0f;
                                        static bool logged_clamp = false;
                                        if (!logged_clamp)
                                        {
                                           logged_clamp = true;
                                           char b[160];
                                           snprintf(b, sizeof(b), "DaysGone DLSS: cb0[26]/[27] jitter insane (%.1f,%.1f)px -- clamped to 0",
                                              jit_x, jit_y);
                                           reshade::log::message(reshade::log::level::warning, b);
                                        }
                                    }
                                    g_jit_pxx.store(jit_x);
                                    g_jit_pxy.store(jit_y);
                                    native_device_context->Unmap(gd.staging_cb.get(), 0);
                                 }
                              }
                           }
                           cb0->Release();
                        }
                     }
                     SR::SuperResolutionImpl::DrawData draw_data;
                     draw_data.source_color = dlss_color;
                     draw_data.output_color = gd.tex_dlss_out.get(); // own FP16, not game RT
                      // M56: unified MV feed (same as M11 compute): own-camera MVs
                      // (1/2 rot, 3/4 full with depth) take priority, then the
                      // named/auto velocity selector, then MVConvert decode to
                      // zero-centered pixels. The old raw fmt35 direct feed
                      // read the 0.5 center as constant motion = shimmer.
                      ID3D11Resource* mv_res = gd.tex_dlss_zero_mvs.get();
                      bool mv_real = false;
                      const char* mv_src = "zero";
                      bool own_done = false;
                      {
                         int ownm = g_ownmv.load(std::memory_order_relaxed);
                         bool own_full = (ownm == 3 || ownm == 4);
                         if ((ownm >= 1 && ownm <= 4) && device_data.game)
                         {
                             auto& god = *static_cast<DaysGoneDeviceData*>(device_data.game);
                             uint64_t oframe = g_hist_frame.load(std::memory_order_relaxed);
                             ElectViewPick(god, oframe); // viewpick: elect+commit before use
                             if (god.view_frame_cur == oframe && god.view_frame_prev != 0 &&
                                god.view_frame_prev + 1 >= god.view_frame_cur)
                            {
                               const float* vcur = god.view_cb_cur;
                               const float* vprv = god.view_cb_prev;
                               float op00 = 0, op11 = 0;
                               if ((vcur[32] > 1e-6f || vcur[32] < -1e-6f)) op00 = vcur[0] / vcur[32];
                               if ((vcur[37] > 1e-6f || vcur[37] < -1e-6f)) op11 = vcur[5] / vcur[37];
                               float op00_prev = 0, op11_prev = 0;
                               if ((vprv[32] > 1e-6f || vprv[32] < -1e-6f)) op00_prev = vprv[0] / vprv[32];
                               if ((vprv[37] > 1e-6f || vprv[37] < -1e-6f)) op11_prev = vprv[5] / vprv[37];
                               if (op00 > 0.5f && op00 < 8.0f && op11 > 0.5f && op11 < 8.0f &&
                                   op00_prev > 0.5f && op00_prev < 8.0f && op11_prev > 0.5f && op11_prev < 8.0f)
                               {
                                   if (!god.tex_mvs_conv || god.mvs_conv_w != rw || god.mvs_conv_h != rh)
                                  {
                                     god.srv_mvs_conv.reset();
                                     god.rtv_mvs_conv.reset();
                                     god.tex_mvs_conv.reset();
                                     D3D11_TEXTURE2D_DESC od = {};
                                      od.Width = rw; od.Height = rh;
                                     od.MipLevels = 1; od.ArraySize = 1;
                                     od.Format = DXGI_FORMAT_R16G16_FLOAT;
                                     od.SampleDesc.Count = 1;
                                     od.Usage = D3D11_USAGE_DEFAULT;
                                     od.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                                     ComPtr<ID3D11Texture2D> ot;
                                     if (SUCCEEDED(native_device->CreateTexture2D(&od, nullptr, ot.put())))
                                     {
                                        ComPtr<ID3D11ShaderResourceView> os;
                                        ComPtr<ID3D11RenderTargetView> orv;
                                        if (SUCCEEDED(native_device->CreateShaderResourceView(ot.get(), nullptr, os.put())) &&
                                            SUCCEEDED(native_device->CreateRenderTargetView(ot.get(), nullptr, orv.put())))
                                        {
                                           god.tex_mvs_conv = ot;
                                           god.srv_mvs_conv = os;
                                           god.rtv_mvs_conv = orv;
                                           god.mvs_conv_w = rw; god.mvs_conv_h = rh;
                                        }
                                     }
                                  }
                                  if (!god.cb_ownmv)
                                  {
                                     D3D11_BUFFER_DESC cbd = {};
                                      cbd.ByteWidth = 208; // M51 192B + DeltaC float3+pad (52 floats)
                                     cbd.Usage = D3D11_USAGE_DYNAMIC;
                                     cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                                     cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                                     native_device->CreateBuffer(&cbd, nullptr, god.cb_ownmv.put());
                                  }
                                  ComPtr<ID3D11ShaderResourceView> owndepth;
                                  if (own_full && res_depth)
                                  {
                                     D3D11_SHADER_RESOURCE_VIEW_DESC dvd = {};
                                     dvd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                                     dvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                                     dvd.Texture2D.MipLevels = 1;
                                     if (FAILED(native_device->CreateShaderResourceView(res_depth.get(), &dvd, owndepth.put())))
                                        owndepth.reset();
                                  }
                                  uint32_t ownkey = (ownm == 2) ? CompileTimeStringHash("DaysGone OwnMV Neg PS")
                                     : (ownm == 3) ? CompileTimeStringHash("DaysGone OwnMV FullA PS")
                                     : (ownm == 4) ? CompileTimeStringHash("DaysGone OwnMV FullB PS")
                                     : CompileTimeStringHash("DaysGone OwnMV PS");
                                  ID3D11PixelShader* own_ps = device_data.native_pixel_shaders[ownkey].get();
                                  ID3D11VertexShader* copy_vs2 = device_data.native_vertex_shaders[CompileTimeStringHash("Copy VS")].get();
                                  if (own_ps && copy_vs2 && god.rtv_mvs_conv && god.cb_ownmv && (!own_full || owndepth))
                                  {
                                     D3D11_MAPPED_SUBRESOURCE cmo = {};
                                     if (SUCCEEDED(native_device_context->Map(god.cb_ownmv.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cmo)) && cmo.pData)
                                     {
                                        float* of = (float*)cmo.pData;
                                        memcpy(of, vcur + 32, 64);
                                        memcpy(of + 16, vprv + 32, 64);
                                        memcpy(of + 32, vcur + 172, 16);
                                        memcpy(of + 36, vprv + 172, 16);
                                        of[40] = op00; of[41] = op11;
                                        of[42] = (float)rw; of[43] = (float)rh;
                                        of[44] = 10.0f; of[45] = op00_prev; of[46] = op11_prev; of[47] = 0.0f;
                                        // DeltaC (Full-B): Ccur-Cprev in DOUBLE on CPU (see compute site).
                                        of[48] = (float)((double)vcur[172] - (double)vprv[172]);
                                        of[49] = (float)((double)vcur[173] - (double)vprv[173]);
                                        of[50] = (float)((double)vcur[174] - (double)vprv[174]);
                                        of[51] = 0.0f;
                                        native_device_context->Unmap(god.cb_ownmv.get(), 0);
                                        DrawStateStack<DrawStateStackType::FullGraphics> own_cs;
                                        own_cs.Cache(native_device_context, device_data.uav_max_count);
                                        ID3D11UnorderedAccessView* null_uavs[D3D11_1_UAV_SLOT_COUNT] = {};
                                        native_device_context->CSSetUnorderedAccessViews(0, device_data.uav_max_count, null_uavs, nullptr);
                                        D3D11_VIEWPORT vp = {};
                                        vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
                                        vp.Width = (float)rw; vp.Height = (float)rh;
                                        vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
                                        native_device_context->RSSetViewports(1, &vp);
                                        native_device_context->RSSetState(god.rs_copy.get());
                                        native_device_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
                                        native_device_context->OMSetDepthStencilState(nullptr, 0);
                                        native_device_context->OMSetRenderTargets(1, &god.rtv_mvs_conv, nullptr);
                                        native_device_context->VSSetShader(copy_vs2, nullptr, 0);
                                        native_device_context->PSSetShader(own_ps, nullptr, 0);
                                        ID3D11Buffer* owncb = god.cb_ownmv.get();
                                        native_device_context->PSSetConstantBuffers(0, 1, &owncb);
                                        if (own_full)
                                        {
                                           ID3D11ShaderResourceView* odsv = owndepth.get();
                                           native_device_context->PSSetShaderResources(0, 1, &odsv);
                                        }
                                        native_device_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                                        native_device_context->Draw(4, 0);
                                        ID3D11Buffer* nullcb = nullptr;
                                        native_device_context->PSSetConstantBuffers(0, 1, &nullcb);
                                        if (own_full)
                                        {
                                           ID3D11ShaderResourceView* nullsrv = nullptr;
                                           native_device_context->PSSetShaderResources(0, 1, &nullsrv);
                                        }
                                        own_cs.Restore(native_device_context);
                                        mv_res = god.tex_mvs_conv.get();
                                        mv_real = true;
                                        mv_src = (ownm == 2) ? "own-rot-neg" : (ownm == 3) ? "own-fullA" : (ownm == 4) ? "own-fullB" : "own-rot";
                                        own_done = true;
                                        g_clast_ownguard.store(0, std::memory_order_relaxed); // log-only: own-fed-ok
                                        g_own_p00.store(op00, std::memory_order_relaxed);
                                        g_own_p11.store(op11, std::memory_order_relaxed);
                                        g_own_used.store(ownm, std::memory_order_relaxed);
                                     }
                                  }
                               }
                               else // log-only: projection shape reject
                                  g_clast_ownguard.store(3, std::memory_order_relaxed);
                            }
                            else // log-only: election hold (1) vs pairing fail (2)
                            {
                               if (god.view_frame_cur != oframe)
                                  g_clast_ownguard.store(1, std::memory_order_relaxed);
                               else
                                  g_clast_ownguard.store(2, std::memory_order_relaxed);
                            }
                         }
                         else // log-only: own-mode-off
                            g_clast_ownguard.store(4, std::memory_order_relaxed);
                      }
                      if (!own_done && !g_zero_mv.load(std::memory_order_relaxed))
                      {
                         ID3D11Resource* mv_sel = nullptr;
                         const char* mv_sel_name = "cache";
                         int mvmode = g_mv_src_mode.load(std::memory_order_relaxed);
                         if (mvmode >= 1 && mvmode <= 6)
                         {
                            uint32_t want = kMvHashes[mvmode - 1];
                            std::lock_guard<std::mutex> mlk(g_mvmap_mutex);
                            auto it = g_mv_by_hash.find(want);
                            if (it != g_mv_by_hash.end() && it->second.frame == g_hist_frame.load(std::memory_order_relaxed) && it->second.res)
                            { mv_sel = it->second.res.get(); mv_sel_name = kMvNames[mvmode - 1]; }
                         }
                         else if (gd.cached_mvs && gd.cached_mvs_frame == g_hist_frame.load(std::memory_order_relaxed))
                         { mv_sel = gd.cached_mvs.get(); mv_sel_name = "cache"; }
                         if (mv_sel)
                         {
                            int mvdec = g_mv_decode.load(std::memory_order_relaxed);
                            if (mvdec != 0)
                            {
                               if (!gd.tex_mvs_conv || gd.mvs_conv_w != rw || gd.mvs_conv_h != rh)
                               {
                                  gd.srv_mvs_conv.reset();
                                  gd.rtv_mvs_conv.reset();
                                  gd.tex_mvs_conv.reset();
                                  D3D11_TEXTURE2D_DESC md = {};
                                  md.Width = rw; md.Height = rh;
                                  md.MipLevels = 1; md.ArraySize = 1;
                                  md.Format = DXGI_FORMAT_R16G16_FLOAT;
                                  md.SampleDesc.Count = 1;
                                  md.Usage = D3D11_USAGE_DEFAULT;
                                  md.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                                  ComPtr<ID3D11Texture2D> mt;
                                  if (SUCCEEDED(native_device->CreateTexture2D(&md, nullptr, mt.put())))
                                  {
                                     ComPtr<ID3D11ShaderResourceView> ms;
                                     ComPtr<ID3D11RenderTargetView> mr;
                                     if (SUCCEEDED(native_device->CreateShaderResourceView(mt.get(), nullptr, ms.put())) &&
                                         SUCCEEDED(native_device->CreateRenderTargetView(mt.get(), nullptr, mr.put())))
                                     {
                                        gd.tex_mvs_conv = mt;
                                        gd.srv_mvs_conv = ms;
                                        gd.rtv_mvs_conv = mr;
                                        gd.mvs_conv_w = rw; gd.mvs_conv_h = rh;
                                     }
                                  }
                               }
                               if (gd.rtv_mvs_conv)
                               {
                                  uint32_t mvkey = (mvdec == 2) ? CompileTimeStringHash("DaysGone MV Convert 025 PS")
                                     : (mvdec == 3) ? CompileTimeStringHash("DaysGone MV Convert Zero PS")
                                     : (mvdec == 4) ? CompileTimeStringHash("DaysGone MV Convert Neg PS")
                                     : (mvdec == 5) ? CompileTimeStringHash("DaysGone MV Convert Zero025 PS")
                                     : (mvdec == 6) ? CompileTimeStringHash("DaysGone MV Convert NegZero PS")
                                     : (mvdec == 7) ? CompileTimeStringHash("DaysGone MV Convert YNegZero PS")
                                     : (mvdec == 8) ? CompileTimeStringHash("DaysGone MV Convert XNegZero PS")
                                     : CompileTimeStringHash("DaysGone MV Convert PS");
                                  ID3D11PixelShader* mv_ps = device_data.native_pixel_shaders[mvkey].get();
                                  ID3D11VertexShader* copy_vs = device_data.native_vertex_shaders[CompileTimeStringHash("Copy VS")].get();
                                  ComPtr<ID3D11ShaderResourceView> cached_mv_srv;
                                  if (mv_ps && copy_vs && SUCCEEDED(native_device->CreateShaderResourceView(mv_sel, nullptr, cached_mv_srv.put())))
                                  {
                                     DrawStateStack<DrawStateStackType::FullGraphics> mv_cs;
                                     mv_cs.Cache(native_device_context, device_data.uav_max_count);
                                     ID3D11UnorderedAccessView* null_uavs[D3D11_1_UAV_SLOT_COUNT] = {};
                                     native_device_context->CSSetUnorderedAccessViews(0, device_data.uav_max_count, null_uavs, nullptr);
                                     D3D11_VIEWPORT vp = {};
                                     vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
                                     vp.Width = (float)rw; vp.Height = (float)rh;
                                     vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
                                     native_device_context->RSSetViewports(1, &vp);
                                     native_device_context->RSSetState(gd.rs_copy.get());
                                     native_device_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
                                     native_device_context->OMSetDepthStencilState(nullptr, 0);
                                     native_device_context->OMSetRenderTargets(1, &gd.rtv_mvs_conv, nullptr);
                                     native_device_context->VSSetShader(copy_vs, nullptr, 0);
                                     native_device_context->PSSetShader(mv_ps, nullptr, 0);
                                     native_device_context->PSSetShaderResources(0, 1, &cached_mv_srv);
                                     native_device_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                                     native_device_context->Draw(4, 0);
                                     mv_cs.Restore(native_device_context);
                                     mv_res = gd.tex_mvs_conv.get();
                                     mv_real = true;
                                     mv_src = mv_sel_name;
                                  }
                               }
                            }
                            else
                            {
                               mv_res = mv_sel;
                               mv_real = true;
                               mv_src = mv_sel_name;
                            }
                          }
                       }
                       // log-only: fell back to game MVs (5) or zero (6)
                       if (!own_done)
                          g_clast_ownguard.store(mv_real ? 5 : 6, std::memory_order_relaxed);
                       draw_data.motion_vectors = mv_res;
                       // Cleanup: g_no_depth branch retired (write-never flag).
                       draw_data.depth_buffer = res_depth.get();
                     draw_data.render_width = rw;
                     draw_data.render_height = rh;
                     draw_data.reset = reset;
                      draw_data.near_plane = g_near_plane.load(std::memory_order_relaxed);
                      draw_data.far_plane = g_far_plane.load(std::memory_order_relaxed);
                      draw_data.vert_fov = g_vert_fov.load(std::memory_order_relaxed);
                     draw_data.jitter_x = 0.0f; // replaced below (M10)
                     draw_data.jitter_y = 0.0f;
                      // Cleanup: g_jitter_off conjunct retired (write-never flag).
                      if (g_jitter_on.load(std::memory_order_relaxed))
                      {
                        float pjs = GetJitScale();
                        draw_data.jitter_x = (g_jit_flip_x.load(std::memory_order_relaxed) ? 1.0f : -1.0f) * jit_x * pjs;
                        draw_data.jitter_y = (g_jit_flip_y.load(std::memory_order_relaxed) ? 1.0f : -1.0f) * jit_y * pjs;
                      }
                     draw_data.frame_index = cb_luma_global_settings.FrameIndex;
                     // M59: store the exact NGX inputs (named feed viewer).
                     // M60: snapshot + mv_real + depth tag (frozen truth).
                     const char* m7dsrc = (res_depth && gd.cached_depth && res_depth.get() == gd.cached_depth.get())
                        ? "cache-DSV" : "slot-depth";
                     StoreFeed(1, dlss_color, res_depth.get(), mv_res, gd.tex_dlss_out.get(),
                        native_device, native_device_context, mv_real, m7dsrc);
                     const bool ok = ([&]() {
                        // M7f: match upstream Unreal DLSS path -- full graphics
                        // + compute state cover, Replaced (not Skip). M7's
                        // Skip left core bookkeeping/state dangling behind a
                        // cancelled pixel draw = frozen/garbage presentation.
                        DrawStateStack<DrawStateStackType::FullGraphics> draw_st;
                        draw_st.Cache(native_device_context, device_data.uav_max_count);
                        DrawStateStack<DrawStateStackType::Compute> st;
                        st.Cache(native_device_context, device_data.uav_max_count);
                        const bool r = sr_implementations[device_data.sr_type]->Draw(sr_instance_data, native_device_context, draw_data);
                        st.Restore(native_device_context);
                        draw_st.Restore(native_device_context);
                        return r;
                     })();
                      if (!gd.logged_draw_result)
                      {
                         gd.logged_draw_result = true;
                         int mvfmt0 = -1;
                         {
                            ComPtr<ID3D11Texture2D> q0;
                            if (mv_res && SUCCEEDED(mv_res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(q0.put()))))
                            {
                               D3D11_TEXTURE2D_DESC qd0 = {};
                               q0->GetDesc(&qd0);
                               mvfmt0 = (int)qd0.Format;
                            }
                         }
                         char b[320];
                         snprintf(b, sizeof(b), "DaysGone DLSS first Draw ok=%d reset=%d render=%ux%u out=%ux%u sr=%s depth_cached=%d mv_real=%d mvsrc=%s mvfmt=%d dec=%d msc=%d inv=%d hdr_feed=%d (M56 unified)",
                            (int)ok, (int)reset, rw, rh, ow, oh, SrTypeName(device_data.sr_type), (int)depth_cached, (int)mv_real, mv_src, mvfmt0,
                            g_mv_decode.load(std::memory_order_relaxed), g_mv_scale_mode.load(std::memory_order_relaxed), (int)g_inverted_depth.load(std::memory_order_relaxed), (int)hdr_feed);
                         reshade::log::message(reshade::log::level::info, b);
                      }
                      // Cleanup: M56 M7 feed-identity + M57 M7 stats one-shots
                      // retired (g_m7feed/g_m7stats have no stores since
                      // menuclean; shared AUDIT + compute cSTATS below cover
                      // both paths).
                      // M57: audit every requested feed (both paths share the sequence).
                      {
                         int mvfmtA = -1;
                         {
                            ComPtr<ID3D11Texture2D> qA;
                            if (mv_res && SUCCEEDED(mv_res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(qA.put()))))
                            {
                               D3D11_TEXTURE2D_DESC qdA = {};
                               qA->GetDesc(&qdA);
                               mvfmtA = (int)qdA.Format;
                            }
                         }
                         if (g_audit_left.load(std::memory_order_relaxed) > 0)
                         {
                            LogAuditFeed("M7", dlss_color, res_depth.get(), mv_res, mv_src, mvfmtA,
                               rw, rh, ow, oh, hdr_feed, reset, draw_data.jitter_x, draw_data.jitter_y, ok);
                            g_audit_left.fetch_sub(1, std::memory_order_relaxed);
                         }
                      }
                      if (ok)
                      {
                         // M9 copy: FP16 DLSS result -> game RTV (M9e: forced
                         // clean state inside RunCopyPass).
                         // M57: keep_alpha=true (M11 parity; M7 wrote alpha 0).
                         bool copied = false;
                         if (rtv)
                         {
                            copied = RunCopyPass(native_device, native_device_context, device_data, gd,
                               gd.srv_dlss_out.get(), rtv.get(), ow, oh,
                               g_swap_output.load(std::memory_order_relaxed), !g_plain_output.load(std::memory_order_relaxed),
                               -1, true);
                            if (reset)
                               g_dlss_resets.fetch_add(1, std::memory_order_relaxed);
                           if (!copied)
                           {
                              static bool logged_no_copy = false;
                              if (!logged_no_copy)
                              {
                                 logged_no_copy = true;
                                 reshade::log::message(reshade::log::level::warning,
                                    "DaysGone DLSS: copy failed -- native TAAU.");
                              }
                           }
                        }
                        if (!copied)
                        {
                           // Copy failed -- native TAAU runs below.
                           device_data.force_reset_sr = true;
                        }
                        else
                        {
                           // M10d: t0 UI composite (user: UI is t0). DLSS feeds
                           // t1/t2 scene color, so t0 UI would be wiped -- draw
                           // it back over the copy. Size-gated inside (1x1
                           // params t0 = no-op). Best effort: a failed composite
                           // still returns Skip with the scene frame.
                           if (g_composite_ui.load(std::memory_order_relaxed) && srvs[0])
                              RunUIComposite(native_device, native_device_context, device_data, gd,
                                 srvs[0].get(), rtv.get(), ow, oh);
                           gd.first_dlss_frame = false;
                           gd.dlss_runs++;
                           g_dlss_runs.fetch_add(1, std::memory_order_relaxed);
                           if (g_mark_sr.load(std::memory_order_relaxed))
                              device_data.has_drawn_sr = true;
                           // M8c: Skip cancels the native pixel draw (verified:
                           // Replaced re-runs native here). Full state guards stay.
                           return DrawOrDispatchOverrideType::Skip;
                        }
                     }
                     device_data.force_reset_sr = true;
                  }
               }
            }
#else
            (void)native_device;
            (void)cmd_list_data;
#endif
         }
      }

      // M59: exact-feed viewer refresh (masterless). When selected, resolve
      // the stored NGX input to an SRV here so it overwrites any slot-viewer
      // stash below (feed view wins). Rebuilt when the feed frame advances.
      {
         int fsel = g_feedview.load(std::memory_order_relaxed);
         if (fsel >= 0 && fsel <= 7)
         {
            static int last_sel = -2;
            static uint64_t last_ff = 0;
            ComPtr<ID3D11Resource> fr;
            bool is_depth = (fsel == 1 || fsel == 5);
            {
               std::lock_guard<std::mutex> flk(g_feed_mutex);
               if (fsel == 0) fr = g_feed_m11_color;
               else if (fsel == 1) fr = g_feed_m11_depth;
               else if (fsel == 2) fr = g_feed_m11_mv;
               else if (fsel == 3) fr = g_feed_m11_out;
               else if (fsel == 4) fr = g_feed_m7_color;
               else if (fsel == 5) fr = g_feed_m7_depth;
               else if (fsel == 6) fr = g_feed_m7_mv;
               else if (fsel == 7) fr = g_feed_m7_out;
            }
            uint64_t ff = (fsel <= 3 ? g_feed_m11_frame.load(std::memory_order_relaxed)
               : g_feed_m7_frame.load(std::memory_order_relaxed));
            uint64_t curh = g_hist_frame.load(std::memory_order_relaxed);
            if (!fr || (ff + 30 < curh && curh > 30))
            {
               // No feed yet (or path idle >30 frames): honest empty, game intact.
               bool was_empty = !fr;
               {
                  std::lock_guard<std::mutex> vlk(g_cview_mutex);
                  g_cview_srv.reset();
                  g_cview_pending.store(false, std::memory_order_relaxed);
               }
               last_sel = fsel; last_ff = 0;
               CViewStashReport(false, 100 + fsel, was_empty ? "feed empty (path idle?)" : "feed stale >30f");
            }
            else if (fsel != last_sel || ff != last_ff)
            {
               ComPtr<ID3D11ShaderResourceView> fv;
               bool okv = is_depth ? CreateDepthViewSRV(native_device, fr.get(), fv)
                  : CreateStashSRV(native_device, fr.get(), fv);
               if (okv && fv)
               {
                  {
                     std::lock_guard<std::mutex> vlk(g_cview_mutex);
                     g_cview_srv = fv;
                     g_cview_pending.store(true, std::memory_order_relaxed);
                  }
                  last_sel = fsel; last_ff = ff;
                  CViewStashReport(true, 100 + fsel, "feed");
               }
               else
               {
                  last_sel = fsel; last_ff = ff;
                  CViewStashReport(false, 100 + fsel, "feed SRV creation");
               }
            }
            else
            {
               std::lock_guard<std::mutex> vlk(g_cview_mutex);
               if (g_cview_srv) g_cview_pending.store(true, std::memory_order_relaxed);
            }
         }
      }

      // M27/M31: compute-viewer blit (gameplay) over large pixel draws.
      // Stashed at the compute slot (or from caches, any mode). M31: the
      // blit is PERSISTENT -- every large draw is replaced until the source
      // is set OFF -- so the LAST fullscreen draw before present wins and
      // the view is always visible. (One-shot blits landed on arbitrary
      // mid-frame passes: invisible when buried, fullscreen-black when the
      // final draw was hit -- exactly the "black / no change" symptoms.)
      // Never replace the DLSS slots themselves while masters are on.
       if (!is_custom_pass && stages != reshade::api::shader_stage::all_compute &&
           g_cview_pending.load(std::memory_order_relaxed))
       {
           // M40: OFF must work masterlessly. The compute-side cleanup only
           // runs inside the master-gated slot block, so with masters OFF a
           // stale pending flag survived forever (stuck viewer, game looks
           // broken until reboot). Clear it here -- this path is masterless.
           // M59: feed viewer shares the same backend; OFF means both off.
           if (g_cview_src.load(std::memory_order_relaxed) < 0 &&
               g_feedview.load(std::memory_order_relaxed) < 0)
          {
             g_cview_pending.store(false, std::memory_order_relaxed);
             std::lock_guard<std::mutex> vlk(g_cview_mutex);
             g_cview_srv.reset();
          }
          else
          {
          uint32_t vps = original_shader_hashes.pixel_shaders.empty() ? 0 : original_shader_hashes.pixel_shaders[0];
         bool vslot = (vps == kDlssSlotPS);
         ComPtr<ID3D11ShaderResourceView> vsrc;
         { std::lock_guard<std::mutex> vlk(g_cview_mutex); vsrc = g_cview_srv; }
         if (!vsrc)
            g_cview_pending.store(false, std::memory_order_relaxed);
         else if (vslot)
         {
            // Park the viewer on DLSS slot draws -- replacing them would eat
            // the DLSS input/output while masters are on. Pending survives
            // (M29) so the next non-slot draw still shows the view.
         }
         else if (vsrc && device_data.game)
         {
            // Blit-guard: our own copy Draw re-enters here -- never blit
            // re-entrantly, fall through to the normal draw path instead.
            if (g_in_copy_blit)
            {
               // fall through (no viewer work, game draw proceeds untouched)
            }
            else
            {
            ID3D11RenderTargetView* wrtv2 = nullptr;
            native_device_context->OMGetRenderTargets(1, &wrtv2, nullptr);
            if (wrtv2)
            {
               bool vdone2 = false;
               ID3D11Resource* vt2 = nullptr;
               wrtv2->GetResource(&vt2);
               if (vt2)
               {
                  ComPtr<ID3D11Texture2D> vx2;
                  if (SUCCEEDED(vt2->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(vx2.put()))))
                  {
                      D3D11_TEXTURE2D_DESC vd2 = {};
                      vx2->GetDesc(&vd2);
                      // Viewfix: persistent blit -- EVERY large draw is replaced
                       // until the source is OFF (last fullscreen draw wins, so
                       // the view is always visible). DLSS slots are parked
                       // above; velocity (R16G16) is never overwritten (eating
                       // its writer would starve the MV feed).
                       if (vd2.Width >= 400 && vd2.Height >= 200 && // M55: was 1000x500
                          vd2.Format != DXGI_FORMAT_R16G16_UNORM)
                      {
                          auto& gdview = *static_cast<DaysGoneDeviceData*>(device_data.game);
                          g_in_copy_blit = true;
                          CopyBlitScope blit_scope; // clears guard even on early return below
                          vdone2 = RunCopyPass(native_device, native_device_context, device_data, gdview,
                             vsrc.get(), wrtv2, vd2.Width, vd2.Height,
                             g_swap_output.load(std::memory_order_relaxed), false,
                             ViewChanForBlit(g_view_chan.load(std::memory_order_relaxed)), false, false, true);
                         if (vdone2)
                         {
                            g_cview_blits.fetch_add(1, std::memory_order_relaxed);
                         }
                      }
                  }
                  vt2->Release();
               }
               wrtv2->Release();
                if (vdone2)
               {
                   // M31: persistent -- do NOT clear pending (last draw wins).
                   // Cleared only when the source is set OFF or the SRV dies.
                   if (g_mark_sr.load(std::memory_order_relaxed))
                      device_data.has_drawn_sr = true;
                   return DrawOrDispatchOverrideType::Skip;
                }
             }
            }
         }
          } // M40 else (viewer source ON)
       }
      // M2 freezer: skip checked hashes live (master + per-hash set).
      // UI composes later under different hashes, so freezing a scene-only
      // pass leaves HUD drawing ??" the TAA-off-with-HUD test.
      if (!is_custom_pass && g_freeze_master.load(std::memory_order_relaxed))
      {
         uint32_t h = 0;
         bool is_compute = (stages == reshade::api::shader_stage::all_compute);
         if (is_compute)
            h = original_shader_hashes.compute_shaders.empty() ? 0 : original_shader_hashes.compute_shaders[0];
         else
            h = original_shader_hashes.pixel_shaders.empty() ? 0 : original_shader_hashes.pixel_shaders[0];
         if (h != 0)
         {
            std::lock_guard<std::mutex> lk(g_freeze_mutex);
            if (g_freeze_skips.find(h) != g_freeze_skips.end())
            {
               g_frozen_skipped_this_frame.fetch_add(1, std::memory_order_relaxed);
               return DrawOrDispatchOverrideType::Skip;
            }
         }
      }

      return DrawOrDispatchOverrideType::None;
   }

   // M7e: copy observation -- observe only, NEVER block (return false).
    bool OverrideCopyResource(ID3D11Device* native_device, DeviceData& device_data, uint64_t& dst_resource, uint64_t& src_resource) override
    {
       (void)native_device; (void)device_data;
       // M52: stamp copy destinations (HDR color is copy-fed: closes the
       // writer-map blind spot so feed stamps mean something).
       if (dst_resource != 0 && src_resource != 0 && dst_resource != src_resource)
       {
          std::lock_guard<std::mutex> lk(g_writer_mutex);
          g_rt_last_writer[dst_resource] = g_hist_frame.load(std::memory_order_relaxed);
          if (g_rt_last_writer.size() > 8192)
             g_rt_last_writer.clear();
       }
#if TEST || DEVELOPMENT
      if (g_cap_frames_left.load(std::memory_order_relaxed) > 0 && dst_resource != 0 && src_resource != 0 && dst_resource != src_resource)
      {
         char dd[64] = {}, sd[64] = {};
         CopyTexDesc(dst_resource, dd, sizeof(dd));
         CopyTexDesc(src_resource, sd, sizeof(sd));
         WriteCopyLine(g_marker_module, dst_resource, src_resource, dd, sd);
      }
#else
      (void)dst_resource; (void)src_resource;
#endif
      return false;
   }
    bool OverrideCopyTextureRegion(ID3D11Device* native_device, DeviceData& device_data, uint64_t& dst_resource, uint32_t dst_subresource, const D3D11_BOX* dst_box, uint64_t& src_resource, uint32_t src_subresource, const D3D11_BOX* src_box) override
    {
       (void)native_device; (void)device_data; (void)dst_box; (void)src_box;
       // M52: same copy-destination stamping as above.
       if (dst_resource != 0 && src_resource != 0 && dst_resource != src_resource)
       {
          std::lock_guard<std::mutex> lk(g_writer_mutex);
          g_rt_last_writer[dst_resource] = g_hist_frame.load(std::memory_order_relaxed);
          if (g_rt_last_writer.size() > 8192)
             g_rt_last_writer.clear();
       }
#if TEST || DEVELOPMENT
      if (g_cap_frames_left.load(std::memory_order_relaxed) > 0 && dst_resource != 0 && src_resource != 0)
      {
         char dd[64] = {}, sd[64] = {}, ex[128] = {};
         CopyTexDesc(dst_resource, dd, sizeof(dd));
         CopyTexDesc(src_resource, sd, sizeof(sd));
         sprintf_s(ex, " sub d%u s%u", dst_subresource, src_subresource);
         WriteCopyLine(g_marker_module, dst_resource, src_resource, dd, sd);
         (void)ex;
      }
#else
      (void)dst_resource; (void)dst_subresource; (void)src_resource; (void)src_subresource;
#endif
      return false;
   }

   void OnPresent(ID3D11Device* native_device, DeviceData& device_data) override
   {
      device_data.has_drawn_sr = false; // per-frame; core snapshots it before this
      // M29: do NOT clear g_cview_pending here. The compute TAA slot fires
      // late (post->TAA->tonemap->UI); wiping per-present killed the stash
      // whenever no large pixel draw remained after the slot that frame.
      // Pending now lives until consumed by the blit or the source is set OFF
      // (see the cvsel<0 reset) -- at most 1 frame stale, and the blit
      // re-stashes every slot fire while active.
      (void)native_device;
      g_draws_last_frame.store(g_draws_this_frame.exchange(0, std::memory_order_relaxed));
      uint64_t frame = g_hist_frame.fetch_add(1, std::memory_order_relaxed) + 1;
      g_frozen_skipped_last_frame.store(g_frozen_skipped_this_frame.exchange(0, std::memory_order_relaxed));
      // Hotkeys: minimal per-present edge-triggered check (no core keybind
      // mechanism exists in this overlay). Mirrors the menu actions exactly.
      {
         static bool prev_dlss = false, prev_trace = false;
         bool down_dlss = (GetAsyncKeyState(DG_HOTKEY_DLSS) & 0x8000) != 0;
         bool down_trace = (GetAsyncKeyState(DG_HOTKEY_TRACE) & 0x8000) != 0;
         if (down_dlss && !prev_dlss)
         {
            bool on = !g_cdlss_master.load(std::memory_order_relaxed);
            g_cdlss_master.store(on);
            device_data.force_reset_sr = true;
#if ENABLE_SR
            if (device_data.game)
               static_cast<DaysGoneDeviceData*>(device_data.game)->first_cdlss_frame = true;
#endif
            char hb[64] = {};
            snprintf(hb, sizeof(hb), "DaysGone HOTKEY dlss=%s", on ? "on" : "off");
            reshade::log::message(reshade::log::level::info, hb);
         }
         if (down_trace && !prev_trace)
         {
            // Tracefix: never re-arm while running; never arm with viewers on
            // (same gate as the bundle buttons); clamp N to 30..300.
            if (g_fulltrace_frames_left.load(std::memory_order_relaxed) > 0)
               reshade::log::message(reshade::log::level::info, "DaysGone HOTKEY trace already running");
            else if (g_view_src.load(std::memory_order_relaxed) >= 0 ||
               g_cview_src.load(std::memory_order_relaxed) >= 0 ||
               g_feedview.load(std::memory_order_relaxed) >= 0)
               reshade::log::message(reshade::log::level::info,
                  "DaysGone HOTKEY trace refused viewer-armed (turn viewers OFF first)");
            else
            {
               int ttn = g_fulltrace_n.load(std::memory_order_relaxed);
               if (ttn < 30) ttn = 30;
               if (ttn > 300) ttn = 300;
               g_fulltrace_frames_left.store(ttn, std::memory_order_relaxed);
               g_fulltrace.store(true, std::memory_order_relaxed);
               char hb[64] = {};
               snprintf(hb, sizeof(hb), "DaysGone HOTKEY trace start %d", ttn);
               reshade::log::message(reshade::log::level::info, hb);
            }
         }
         prev_dlss = down_dlss;
         prev_trace = down_trace;
      }
#if TEST || DEVELOPMENT
       if (g_cap_frames_left.load(std::memory_order_relaxed) > 0)
          g_cap_frames_left.fetch_sub(1, std::memory_order_relaxed);
#endif
       // Pictures: copy-only backbuffer BMP, next 3 presents while armed.
       // Bundle-hang guard: per-Present staging work stalls against an armed
       // viewer blit (~150 copies/frame). Refuse while any viewer is armed
       // (same condition as the blit gate); count down normally so the
       // bundle still completes and prints BUNDLE done.
       if (g_pic_frames_left.load(std::memory_order_relaxed) > 0)
       {
          if (g_cview_src.load(std::memory_order_relaxed) >= 0 ||
              g_feedview.load(std::memory_order_relaxed) >= 0)
             reshade::log::message(reshade::log::level::info,
                "DaysGone PIC SKIP viewer-armed (turn viewers OFF first)");
          else
             WritePicFile(g_marker_module, native_device, frame);
          g_pic_frames_left.fetch_sub(1, std::memory_order_relaxed);
       }
       // All-passes: one pass BMP per present while armed (auto-disarms).
       // Same viewer-armed refusal; advance replicates WritePassOne's tail
       // so progress/disarm/BUNDLE-done behave identically.
       if (g_pass_active.load(std::memory_order_relaxed))
       {
          if (g_cview_src.load(std::memory_order_relaxed) >= 0 ||
              g_feedview.load(std::memory_order_relaxed) >= 0)
          {
             reshade::log::message(reshade::log::level::info,
                "DaysGone PIC SKIP viewer-armed (turn viewers OFF first)");
             int pdone = g_pass_pos.fetch_add(1, std::memory_order_relaxed) + 1;
             if (pdone >= kPassCount)
             {
                g_pass_active.store(false, std::memory_order_relaxed);
                {
                   std::lock_guard<std::mutex> plk(g_pass_slot_mutex);
                   for (int i = 0; i < 6; i++)
                      g_pass_slot[i].reset();
                }
                reshade::log::message(reshade::log::level::info, "DaysGone PIC passes done 19/19");
             }
          }
          else
             WritePassOne(g_marker_module, native_device, device_data, frame);
       }
       // Full-trace: one summary line per present while armed; auto-OFF at 0.
       if (g_fulltrace.load(std::memory_order_relaxed) &&
           g_fulltrace_frames_left.load(std::memory_order_relaxed) > 0)
       {
          WriteFullLine(g_marker_module, frame);
          if (g_fulltrace_frames_left.fetch_sub(1, std::memory_order_relaxed) <= 1)
             g_fulltrace.store(false, std::memory_order_relaxed);
       }
       // Full bundle: done when every armed sub-path disarmed itself.
       if (g_bundle_active.load(std::memory_order_relaxed) &&
           g_pic_frames_left.load(std::memory_order_relaxed) <= 0 &&
           !g_pass_active.load(std::memory_order_relaxed) &&
#if TEST || DEVELOPMENT
           g_cap_frames_left.load(std::memory_order_relaxed) <= 0 &&
#endif
           (!g_fulltrace.load(std::memory_order_relaxed) ||
            g_fulltrace_frames_left.load(std::memory_order_relaxed) <= 0))
       {
          g_bundle_active.store(false, std::memory_order_relaxed);
          char bb[192] = {};
          snprintf(bb, sizeof(bb), "DaysGone BUNDLE done pics=%d passes=%d/%d cap=%d trace=%d",
             g_bundle_pics.load(std::memory_order_relaxed),
             g_bundle_passes.load(std::memory_order_relaxed), kPassCount,
             g_bundle_cap.load(std::memory_order_relaxed),
             g_bundle_trace.load(std::memory_order_relaxed));
          reshade::log::message(reshade::log::level::info, bb);
       }

      static bool first = true;
      if (first)
      {
         first = false;
         if (g_marker_module != nullptr)
            WriteLoadMarker(g_marker_module, "first-present");
      }
      // Publish freezer candidates: first at present 30 (menu populated ~1s
      // after boot in ANY config), then every 600 presents.
      // NOTE: file flush runs AFTER publishing ??" flushing clears the maps,
      // so flushing first left the menu permanently empty (M5b bug).
      uint64_t pf = g_hist_frame.load(std::memory_order_relaxed);
       if (pf == 30 || (pf % 600) == 0)
       {
          // M52: prune stale MV producers (unbounded map pins stale
          // fullscreen textures = slow GPU leak across scenes; readers
          // already treat missing entries as stale, so this is safe).
          {
             std::lock_guard<std::mutex> mlk(g_mvmap_mutex);
             uint64_t cutoff = (pf > 600) ? (pf - 600) : 0;
             for (auto it = g_mv_by_hash.begin(); it != g_mv_by_hash.end(); )
                it = (it->second.frame < cutoff) ? g_mv_by_hash.erase(it) : std::next(it);
          }
          std::vector<std::pair<uint32_t, uint64_t>> top_ps, top_cs;
         {
            std::lock_guard<std::mutex> lk(g_hist_mutex);
            top_ps.assign(g_ps_hist.begin(), g_ps_hist.end());
            top_cs.assign(g_cs_hist.begin(), g_cs_hist.end());
         }
         auto cmp = [](auto& a, auto& b) { return a.second > b.second; };
         std::sort(top_ps.begin(), top_ps.end(), cmp);
         std::sort(top_cs.begin(), top_cs.end(), cmp);
         std::lock_guard<std::mutex> flk(g_freeze_mutex);
         g_freeze_candidates.clear();
         // M5d: slowest-FIRST over the FULL list (not bottom of top-50 ??" a
         // 1/frame pass in a 7k-draw game ranks below any top-50 cut).
         // Slowest 16 with count>=10 (filters one-off blips) + top 8 ref.
         auto push_band = [&](std::vector<std::pair<uint32_t, uint64_t>>& top, bool is_compute) {
            size_t slow = 0;
            for (size_t i = top.size(); i-- > 0 && slow < 16;)
            {
               if (top[i].second < 10)
                  continue;
               g_freeze_candidates.push_back({ top[i].first, top[i].second, is_compute });
               slow++;
            }
            size_t n = top.size() > 8 ? 8 : top.size();
            for (size_t i = 0; i < n; i++)
               g_freeze_candidates.push_back({ top[i].first, top[i].second, is_compute });
         };
         push_band(top_ps, false);
         push_band(top_cs, true);
         // NOTE: no pruning ??" a hash missing from one window (scene change)
         // is a harmless no-op in the skip set; pruning ate manual entries.
      }
#if TEST || DEVELOPMENT
      // File flush AFTER publishing (it clears the maps ??" see note above).
      if ((pf % 600) == 0)
         WriteDrawHistogram(g_marker_module);
#endif
   }

    void DrawImGuiSettings(DeviceData& device_data) override
    {
       ImGui::Text("Days Gone DLSS: %s", DG_BUILD_ID);
       ImGui::Text("Draws: %llu  Presents: %llu  Frozen: %llu",
          (unsigned long long)g_draws_last_frame.load(),
          (unsigned long long)g_hist_frame.load(),
          (unsigned long long)g_frozen_skipped_last_frame.load());
       ImGui::Separator();

       if (ImGui::CollapsingHeader("Freezer (master OFF = zero changes)"))
       {
          bool master = g_freeze_master.load();
          if (ImGui::Checkbox("Freezer master", &master))
             g_freeze_master.store(master);
          if (ImGui::Button("Clear all skips"))
          {
             std::lock_guard<std::mutex> lk(g_freeze_mutex);
             g_freeze_skips.clear();
          }
          static char manual_hex[16] = "242D9D62";
          ImGui::InputText("hash hex", manual_hex, sizeof(manual_hex));
          if (ImGui::Button("Freeze typed hash"))
          {
             unsigned int h = 0;
             if (sscanf_s(manual_hex, "%X", &h) == 1 && h != 0 && h != 0xFFFFFFFF)
             {
                std::lock_guard<std::mutex> lk(g_freeze_mutex);
                g_freeze_skips.insert((uint32_t)h);
             }
          }
          ImGui::TextWrapped("Freeze ONE candidate at a time. TAA resolve = instant aliasing, HUD stays.");
          if (ImGui::CollapsingHeader("Freezer candidates (advanced)"))
          {
             std::lock_guard<std::mutex> lk(g_freeze_mutex);
             for (auto& c : g_freeze_candidates)
             {
                char label[64];
                sprintf_s(label, "%s %08X x%llu", c.is_compute ? "CS" : "PS", c.hash, (unsigned long long)c.count);
                bool checked = g_freeze_skips.find(c.hash) != g_freeze_skips.end();
                if (ImGui::Checkbox(label, &checked))
                {
                   if (checked)
                      g_freeze_skips.insert(c.hash);
                   else
                      g_freeze_skips.erase(c.hash);
                }
             }
             for (auto it = g_freeze_skips.begin(); it != g_freeze_skips.end();)
             {
                uint32_t h = *it;
                bool listed = false;
                for (auto& c : g_freeze_candidates)
                   if (c.hash == h) { listed = true; break; }
                if (!listed)
                {
                   char label[64];
                   sprintf_s(label, "PINNED %08X", h);
                   bool checked = true;
                   if (ImGui::Checkbox(label, &checked) && !checked)
                      it = g_freeze_skips.erase(it);
                   else
                      ++it;
                }
                else
                   ++it;
             }
          }
       }

       ImGui::Separator();
       ImGui::Text("DLSS Masters");
       bool dlss = g_dlss_master.load();
       if (ImGui::Checkbox("M7 DLSS at TAAU 000573B8 (showcase/upscale)", &dlss))
       {
          g_dlss_master.store(dlss);
          device_data.force_reset_sr = true;
#if ENABLE_SR
          if (device_data.game)
             static_cast<DaysGoneDeviceData*>(device_data.game)->first_dlss_frame = true;
#endif
       }
       bool cdlss = g_cdlss_master.load();
       if (ImGui::Checkbox("M11 DLSS at COMPUTE TAA 242D9D62 (gameplay DLAA)", &cdlss))
       {
          g_cdlss_master.store(cdlss);
          device_data.force_reset_sr = true;
#if ENABLE_SR
          if (device_data.game)
             static_cast<DaysGoneDeviceData*>(device_data.game)->first_cdlss_frame = true;
#endif
       }
       ImGui::Text("Hotkeys: F9 = M11 DLSS toggle, F10 = start full-trace (DG_HOTKEY_* defines at top of main.cpp).");
        if (g_dlss_master.load(std::memory_order_relaxed) && g_cdlss_master.load(std::memory_order_relaxed))
           ImGui::TextWrapped("Both ON -- M11 while the compute slot fires, M7 takes over when it goes silent 30+ presents (e.g. reduced render scale).");
       if (g_cdlss_master.load(std::memory_order_relaxed) && g_cdlss_attempts.load(std::memory_order_relaxed) == 0)
          ImGui::TextWrapped("M11 ON but CS 242D9D62 hasn't fired: needs DLSS SR in Luma menu + real gameplay.");
       if (g_dlss_master.load(std::memory_order_relaxed) && g_dlss_attempts.load(std::memory_order_relaxed) == 0)
          ImGui::TextWrapped("M7 ON but slot 000573B8 hasn't fired: pick DLSS SR in Luma menu, reboot once if PS-unresolved climbs.");
#if ENABLE_SR
       bool sr_on = (device_data.sr_type != SR::Type::None);
       ImGui::Text("SR active: %s (%s)", sr_on ? "YES" : "no", SrTypeName(device_data.sr_type));
       if (!sr_on)
          ImGui::TextWrapped("Open Luma menu and set Super Resolution to DLSS.");
#endif

       ImGui::Separator();
       ImGui::Text("Live Diagnostics");
       {
          uint64_t real = g_cmv_frames_real.load(), zero = g_cmv_frames_zero.load();
          ImGui::Text("MV feeds: real %llu / zero %llu%s", real, zero,
             (zero > real && zero > 100) ? " (BLIND -- fix MVs)" : "");
          ImGui::Text("Fallbacks: capture %llu / rtv %llu",
             (unsigned long long)g_cfail_capture.load(), (unsigned long long)g_cfail_rtv.load());
          uint64_t att = g_cdlss_attempts.load(), pres = g_hist_frame.load();
          ImGui::Text("Slot fire rate: %.2f/present (1.0 = gameplay)",
             pres ? (double)att / (double)pres : 0.0);
           static const char* code_names[] = { "zero", "auto-cache", "slot-t0", "named", "own", "hybrid" };
           int cc = g_clast_code.load(); if (cc < 0 || cc > 5) cc = 0;
          ImGui::Text("Last: ok=%d reset=%d %dx%d->%dx%d mv=%s(%s) fmt=%d hash=%08X jit=%.2f,%.2f",
             g_clast_ok.load(), g_clast_reset.load(),
             g_clast_rw.load(), g_clast_rh.load(), g_clast_ow.load(), g_clast_oh.load(),
             g_clast_mvreal.load() ? "real" : "ZERO", code_names[cc],
             g_clast_mvfmt.load(), (unsigned)g_clast_mvhash.load(),
             g_clast_jitx.load(), g_clast_jity.load());
          uint64_t cur = g_hist_frame.load(std::memory_order_relaxed);
          char ages[6][32];
          {
             std::lock_guard<std::mutex> mlk(g_mvmap_mutex);
             for (int i = 0; i < 6; i++)
             {
                auto it = g_mv_by_hash.find(kMvHashes[i]);
                if (it == g_mv_by_hash.end() || !it->second.res)
                   sprintf_s(ages[i], "--");
                else
                   sprintf_s(ages[i], "%lluf", (unsigned long long)(cur - it->second.frame));
             }
          }
          ImGui::Text("MV age: BE:%s 1E:%s 58:%s 6C:%s A1:%s A5:%s (-- = never)",
             ages[0], ages[1], ages[2], ages[3], ages[4], ages[5]);
           ImGui::Text("cDLSS attempts: %llu runs: %llu resets: %llu",
              (unsigned long long)g_cdlss_attempts.load(),
              (unsigned long long)g_cdlss_runs.load(),
              (unsigned long long)g_cdlss_resets.load());
           ImGui::Text("DLSS attempts: %llu runs: %llu resets: %llu  Color changes: %llu",
              (unsigned long long)g_dlss_attempts.load(),
              (unsigned long long)g_dlss_runs.load(),
              (unsigned long long)g_dlss_resets.load(),
              (unsigned long long)g_color_changes.load());
           // M54: path state (answers "attempts 0" live: parked vs silent).
           {
              uint64_t lf = g_cdlss_last_fire.load(std::memory_order_relaxed);
              uint64_t sage = (cur >= lf) ? (cur - lf) : 0;
              if (!g_cdlss_master.load(std::memory_order_relaxed))
                 ImGui::Text("M7 pixel: M11 master OFF (M7 on its own master)");
              else
                 ImGui::Text("M7 pixel: %s (compute silent %lluf)",
                    sage >= 30 ? "UNPARKED" : "PARKED", (unsigned long long)sage);
           }
           // M54: full feed-settings mirror (no menu hunting).
           ImGui::Text("Feed: dec=%d src=%d own=%d scale=%d inv=%d jitmode=%d mvsjit=%d zero=%d aexp=%d",
              g_mv_decode.load(), g_mv_src_mode.load(), g_ownmv.load(),
              g_mv_scale_mode.load(), (int)g_inverted_depth.load(),
              g_jit_scale_mode.load(), (int)g_mvs_jittered.load(),
              (int)g_zero_mv.load(), (int)g_dlss_autoexp.load());
           ImGui::Text("DepthRange: near=%.2f far=%.0f | UI: blend=%d tm=%d alpha=%d uisrc=%d cui=%d",
              g_near_plane.load(), g_far_plane.load(),
              g_blend_mode.load(), g_ui_tonemap_mode.load(), g_c_alpha_mode.load(),
              g_cui_src.load(), (int)g_ccomposite_ui.load());
          ImGui::Text("PS unresolved (FFFFFFFF): %llu", (unsigned long long)g_ps_unresolved.load());
#if ENABLE_SR
          if (device_data.game)
          {
             auto& gdc = *static_cast<DaysGoneDeviceData*>(device_data.game);
             uint64_t cur2 = g_hist_frame.load(std::memory_order_relaxed);
              if (gdc.cached_mvs)
                 ImGui::Text("Auto MV cache age: %llu frames (0 = this frame)",
                    (unsigned long long)(cur2 - gdc.cached_mvs_frame));
              else
                 ImGui::Text("Auto MV cache: EMPTY");
              // M54: depth/HDR cache ages (depth-view ghost question) +
              // own-MV stash state mirror.
              if (gdc.cached_depth)
                 ImGui::Text("Depth cache age: %lluf",
                    (unsigned long long)(gdc.cached_depth_frame <= cur2 ? cur2 - gdc.cached_depth_frame : 0));
              else
                 ImGui::Text("Depth cache: EMPTY");
              if (gdc.cached_hdr)
                 ImGui::Text("HDR cache age: %lluf",
                    (unsigned long long)(gdc.cached_hdr_frame <= cur2 ? cur2 - gdc.cached_hdr_frame : 0));
              else
                 ImGui::Text("HDR cache: EMPTY");
              ImGui::Text("OwnMV stash: cur %lluf ago / prev %lluf ago (rej=%llu)",
                 (unsigned long long)(gdc.view_frame_cur ? cur2 - gdc.view_frame_cur : 9999),
                 (unsigned long long)(gdc.view_frame_prev ? cur2 - gdc.view_frame_prev : 9999),
                 (unsigned long long)g_view_rejects.load());
          }
#endif
       }

       ImGui::Separator();
       if (ImGui::Button("AUTO-DIAGNOSE (arm all, auto-fire on next frame)"))
       {
          g_auto_diag.store(true, std::memory_order_relaxed);
          ImGui::OpenPopup("Auto-Diag Armed");
       }
       if (ImGui::BeginPopupModal("Auto-Diag Armed", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
       {
           ImGui::Text("All diagnostics armed.\nPAN THE CAMERA slowly for ~5 seconds (matrices must animate).\nCheck ReShade.log afterwards, then close the game.");
          ImGui::Separator();
          if (ImGui::Button("OK"))
             ImGui::CloseCurrentPopup();
          ImGui::EndPopup();
       }

       ImGui::Separator();
       ImGui::Text("MV Feed");
       {
          const char* mvsrc_names[] = { "Auto (latest cache)", "BE0130E5", "1E94CABC", "5846E9DA", "6C6AD505", "A1A256FA", "A5CB30BF" };
          int msm = g_mv_src_mode.load();
          if (ImGui::Combo("MV source (velocity pass)", &msm, mvsrc_names, 7))
             g_mv_src_mode.store(msm);
           int msm2 = g_mv_src_mode.load();
           if (msm2 >= 1 && msm2 <= 6)
           {
              bool mfr = false;
              {
                 std::lock_guard<std::mutex> mlk(g_mvmap_mutex);
                 auto it2 = g_mv_by_hash.find(kMvHashes[msm2 - 1]);
                 mfr = (it2 != g_mv_by_hash.end() && it2->second.frame == g_hist_frame.load(std::memory_order_relaxed));
              }
              ImGui::Text("Selected %s: %s", kMvNames[msm2 - 1], mfr ? "FRESH" : "STALE (native fallback)");
           }
           bool hyb = g_hybrid_mv.load();
           if (ImGui::Checkbox("Hybrid MVs (game truth + own fill)", &hyb))
              g_hybrid_mv.store(hyb);
       }
       {
          const char* dc_names[] = { "Raw", "Upstream UE (default)", "0.25-cent", "UE+zero-snap", "Negated", "Snap+0.25", "Neg+zero-snap", "Y-neg+snap", "X-neg+snap" };
          int dc = g_mv_decode.load();
          if (dc < 0) dc = 0; if (dc > 8) dc = 8;
           if (ImGui::Combo("MV decode", &dc, dc_names, 9))
              g_mv_decode.store(dc);
        }
        {
           const char* om_names[] = { "Off (game MVs)", "Rotation", "Rotation-neg", "Full-A (V-row T)", "Full-B (campos T)" };
           int omm = g_ownmv.load();
           if (omm < 0) omm = 0; if (omm > 4) omm = 4;
           if (ImGui::Combo("Own camera MVs (M47/M51)", &omm, om_names, 5))
              g_ownmv.store(omm);
        }
#if ENABLE_SR
        if (device_data.game)
        {
           auto& god = *static_cast<DaysGoneDeviceData*>(device_data.game);
           uint64_t ocur = g_hist_frame.load(std::memory_order_relaxed);
            ImGui::Text("OwnMV stash: cur %lluf ago / prev %lluf ago (p00=%.3f p11=%.3f rej=%llu pick=%d/%d projrej=%llu)",
               (unsigned long long)(god.view_frame_cur ? ocur - god.view_frame_cur : 9999),
               (unsigned long long)(god.view_frame_prev ? ocur - god.view_frame_prev : 9999),
               g_own_p00.load(), g_own_p11.load(),
               (unsigned long long)g_view_rejects.load(),
               god.view_pick, god.view_pick_n,
               (unsigned long long)g_view_projrej.load());
        }
#endif
       bool zmv = g_zero_mv.load();
       if (ImGui::Checkbox("Zero motion vectors (flicker test)", &zmv))
          g_zero_mv.store(zmv);

       ImGui::Separator();
       ImGui::Text("UI Composite");
       bool ccui = g_ccomposite_ui.load();
       if (ImGui::Checkbox("Composite t0 HUD over compute DLSS", &ccui))
          g_ccomposite_ui.store(ccui);
       {
          const char* blend_names[] = { "Premult", "Straight", "Additive", "Multiplicative", "Alpha-additive", "Premult RGB-only", "Replace" };
          int bm = g_blend_mode.load();
          if (ImGui::Combo("UI blend mode", &bm, blend_names, 7))
             g_blend_mode.store(bm);
       }
       {
          const char* tm_names[] = { "None", "Reinhard+sRGB", "ACES+sRGB", "Hable+sRGB", "sRGB decode", "sRGB encode" };
          int tmm = g_ui_tonemap_mode.load();
          if (ImGui::Combo("UI tonemap mode", &tmm, tm_names, 6))
             g_ui_tonemap_mode.store(tmm);
       }
       {
          const char* sa_names[] = { "Keep NGX alpha", "Force opaque" };
          int sam = g_c_alpha_mode.load();
          if (ImGui::Combo("Scene alpha mode", &sam, sa_names, 2))
             g_c_alpha_mode.store(sam);
       }
       {
          const char* us_names[] = { "t0 (default)", "t1", "t2", "t3", "OFF" };
          int us = g_cui_src.load();
          if (ImGui::Combo("Compute UI source", &us, us_names, 5))
             g_cui_src.store(us);
       }

        if (ImGui::CollapsingHeader("Motion tuning (advanced)"))
        {
           // Menuclean: only proven-live knobs here (near/far/FOV/depth-src/inv
           // + M11 jitter). Legacy/scale/sign/sRGB controls moved to the
           // "Legacy (proven wrong, kept for A/B)" header below.
           bool invd = g_inverted_depth.load();
           if (ImGui::Checkbox("Inverted depth", &invd))
              g_inverted_depth.store(invd);
            // M61: the compute path reads CB0[0]/[1] into cjit but only
            // feeds it to NGX when this is on (audit proved jit=0).
            bool cjon = g_cjitter_on.load();
            if (ImGui::Checkbox("M11 jitter from slot CB0 (audit must show jit!=0)", &cjon))
               g_cjitter_on.store(cjon);
           float npl = g_near_plane.load();
           if (ImGui::SliderFloat("Near plane", &npl, 0.01f, 200.0f, "%.2f"))
              g_near_plane.store(npl);
           float fpl = g_far_plane.load();
           if (ImGui::SliderFloat("Far plane", &fpl, 5000.0f, 2000000.0f, "%.0f"))
              g_far_plane.store(fpl);
           float vfov = g_vert_fov.load();
           if (ImGui::SliderFloat("Vert FOV rad (60deg=1.047, proven 39.4deg=0.688)", &vfov, 0.4f, 1.2f, "%.3f"))
              g_vert_fov.store(vfov);
           {
              const char* ds_names[] = { "Auto: cache-DSV if fresh", "Slot t1 only", "Cache-DSV only" };
              int dsm = g_cdepth_src.load();
              if (dsm < 0) dsm = 0; if (dsm > 2) dsm = 2;
              if (ImGui::Combo("M11 depth source (t1=accum? use cache)", &dsm, ds_names, 3))
                 g_cdepth_src.store(dsm);
              ImGui::TextWrapped("Compare Viewer: TAA t1 vs Depth cache. Whichever shows the depth silhouette is depth; scene content = accumulation.");
           }
           bool aexp = g_dlss_autoexp.load();
           if (ImGui::Checkbox("Auto exposure (default ON)", &aexp))
              g_dlss_autoexp.store(aexp);
        }

        if (ImGui::CollapsingHeader("Legacy (proven wrong, kept for A/B)"))
        {
           // Menuclean: M7 pixel path parks while the M11 compute slot fires
           // (M53 silence-gated unpark). While PARKED the controls below do
           // nothing -- see the parked note. All globals stay defined for
           // compat; only the ImGui controls moved here.
           {
              uint64_t cur = g_hist_frame.load(std::memory_order_relaxed);
              uint64_t lf = g_cdlss_last_fire.load(std::memory_order_relaxed);
              uint64_t sage = (cur >= lf) ? (cur - lf) : 0;
              bool parked = g_cdlss_master.load(std::memory_order_relaxed) && sage < 30;
              if (parked)
                 ImGui::TextWrapped("M7 PARKED (compute slot fired %lluf ago): controls below do nothing until compute goes silent 30+ presents.", (unsigned long long)sage);
              else
                 ImGui::TextWrapped("M7 UNPARKED: pixel path live (compute silent %lluf).", (unsigned long long)sage);
           }
           const char* sc_names[] = { "1.0 pass-through (default)", "0.5*res (legacy)", "0.5", "2.0 (half-gain compensation test)" };
           int sm = g_mv_scale_mode.load();
           if (ImGui::Combo("MV scale mode", &sm, sc_names, 4))
              g_mv_scale_mode.store(sm);
           const char* js_names[] = { "x1 (default)", "x2 (NDC)", "x0.5" };
           int jm = g_jit_scale_mode.load();
           if (ImGui::Combo("Jitter scale", &jm, js_names, 3))
              g_jit_scale_mode.store(jm);
           bool cso = g_c_srgb_output.load();
           if (ImGui::Checkbox("sRGB scene output (keep OFF, #25 proven wrong)", &cso))
              g_c_srgb_output.store(cso);
            bool mvj = g_mvs_jittered.load();
            if (ImGui::Checkbox("Game MVs include jitter", &mvj))
               g_mvs_jittered.store(mvj);
           bool hdr = g_dlss_hdr.load();
           if (ImGui::Checkbox("DLSS hdr flag (fmt24)", &hdr))
              g_dlss_hdr.store(hdr);
           bool swu = g_swap_unpack.load();
           if (ImGui::Checkbox("Swap R/B on unpack", &swu))
              g_swap_unpack.store(swu);
           bool bypass = g_dlss_bypass.load();
           if (ImGui::Checkbox("BYPASS NGX (show input only)", &bypass))
              g_dlss_bypass.store(bypass);
           bool plain = g_plain_output.load();
           if (ImGui::Checkbox("PLAIN output (no sRGB encode)", &plain))
              g_plain_output.store(plain);
           bool phdr = g_prefer_hdr.load();
           if (ImGui::Checkbox("HDR color feed", &phdr))
              g_prefer_hdr.store(phdr);
           bool rqu = g_require_upscale.load();
           if (ImGui::Checkbox("Upscale-only guard", &rqu))
              g_require_upscale.store(rqu);
           bool cui = g_composite_ui.load();
           if (ImGui::Checkbox("Composite t0 UI over DLSS (M7, use compute one)", &cui))
              g_composite_ui.store(cui);
           bool swo = g_swap_output.load();
           if (ImGui::Checkbox("Swap R/B on output", &swo))
              g_swap_output.store(swo);
           bool msr = g_mark_sr.load();
           if (ImGui::Checkbox("Mark SR drawn for core", &msr))
              g_mark_sr.store(msr);
           bool jon = g_jitter_on.load();
           if (ImGui::Checkbox("Game jitter from slot cb0 (M7, use M11 one)", &jon))
              g_jitter_on.store(jon);
           ImGui::Text("cb0[0] raw: %.5f %.5f  px: %.3f %.3f",
              g_jit_rawx.load(), g_jit_rawy.load(), g_jit_pxx.load(), g_jit_pxy.load());
           bool jfx = g_jit_flip_x.load();
           if (ImGui::Checkbox("Jitter flip X", &jfx))
              g_jit_flip_x.store(jfx);
           bool jfy = g_jit_flip_y.load();
           if (ImGui::Checkbox("Jitter flip Y", &jfy))
              g_jit_flip_y.store(jfy);
        }

       if (ImGui::CollapsingHeader("Viewer"))
       {
           const char* cv_names[] = { "OFF", "TAA t0 (HUD?)", "TAA t1 (depth)", "TAA t2 (HDR color)", "TAA t3 (history)", "TAA u0 (current)", "TAA u1 (hist scratch)", "Velocity cache", "Depth cache", "HDR cache", "DLSS output", "MV feed (NGX input)" };
           int cvv = g_cview_src.load() + 1;
           if (cvv < 0) cvv = 0; if (cvv > 11) cvv = 11;
           if (ImGui::Combo("Compute viewer source", &cvv, cv_names, 12))
              g_cview_src.store(cvv - 1);
           // M59: exact NGX feeds (what DLSS really got, per path, named).
           {
              const char* fv_names[] = { "OFF", "M11 color (NGX src)", "M11 depth (NGX depth)", "M11 MV (NGX MV)", "M11 output (NGX out)", "M7 color (NGX src)", "M7 depth (NGX depth)", "M7 MV (NGX MV)", "M7 output (NGX out)" };
              int fv = g_feedview.load() + 1;
              if (fv < 0) fv = 0; if (fv > 8) fv = 8;
              if (ImGui::Combo("DLSS feed (exact NGX input)", &fv, fv_names, 9))
                 g_feedview.store(fv - 1);
              uint64_t curf = g_hist_frame.load(std::memory_order_relaxed);
              uint64_t f11 = g_feed_m11_frame.load(std::memory_order_relaxed);
              uint64_t f7 = g_feed_m7_frame.load(std::memory_order_relaxed);
              uint64_t m11 = g_feed_m11_mvframe.load(std::memory_order_relaxed);
              uint64_t m7 = g_feed_m7_mvframe.load(std::memory_order_relaxed);
              char d11[4][48] = {}, d7[4][48] = {};
              char s11[24] = {}, s7[24] = {};
              {
                 std::lock_guard<std::mutex> flk(g_feed_mutex);
                 if (g_feed_m11_color) DescribeResHandle(g_feed_m11_color.get(), d11[0], sizeof(d11[0]));
                 if (g_feed_m11_depth) DescribeResHandle(g_feed_m11_depth.get(), d11[1], sizeof(d11[1]));
                 if (g_feed_m11_mv) DescribeResHandle(g_feed_m11_mv.get(), d11[2], sizeof(d11[2]));
                 if (g_feed_m11_out) DescribeResHandle(g_feed_m11_out.get(), d11[3], sizeof(d11[3]));
                 if (g_feed_m7_color) DescribeResHandle(g_feed_m7_color.get(), d7[0], sizeof(d7[0]));
                 if (g_feed_m7_depth) DescribeResHandle(g_feed_m7_depth.get(), d7[1], sizeof(d7[1]));
                 if (g_feed_m7_mv) DescribeResHandle(g_feed_m7_mv.get(), d7[2], sizeof(d7[2]));
                 if (g_feed_m7_out) DescribeResHandle(g_feed_m7_out.get(), d7[3], sizeof(d7[3]));
                 snprintf(s11, sizeof(s11), "%s", g_feed_m11_dsrc);
                 snprintf(s7, sizeof(s7), "%s", g_feed_m7_dsrc);
              }
              ImGui::Text("M11 age %lluf [%s]: C %s D %s MV(%lluf) %s OUT %s",
                 (unsigned long long)(curf >= f11 ? curf - f11 : 0), s11, d11[0], d11[1],
                 (unsigned long long)(curf >= m11 ? curf - m11 : 0), d11[2], d11[3]);
              ImGui::Text("M7 age %lluf [%s]: C %s D %s MV(%lluf) %s OUT %s",
                 (unsigned long long)(curf >= f7 ? curf - f7 : 0), s7, d7[0], d7[1],
                 (unsigned long long)(curf >= m7 ? curf - m7 : 0), d7[2], d7[3]);
              ImGui::TextWrapped("M60: snapshots = exactly what DLSS got (frozen at feed time). MV keeps last-good (zero skipped) -- MV age shows staleness. [cache-DSV] vs [slot-t1/slot-depth] tells which depth fed NGX. Scene content in depth/MV = wrong buffer.");
           }
            ImGui::Text("Blit pending: %s", g_cview_pending.load(std::memory_order_relaxed) ? "PENDING" : "CONSUMED");
            {
               int sok = g_cview_stash_ok.load(std::memory_order_relaxed);
               int ssrc = g_cview_stash_src.load(std::memory_order_relaxed);
               uint64_t sframe = g_cview_stash_frame.load(std::memory_order_relaxed);
               uint64_t curf2 = g_hist_frame.load(std::memory_order_relaxed);
               uint64_t age = (curf2 >= sframe) ? (curf2 - sframe) : 0;
               char rs[96] = {};
               { std::lock_guard<std::mutex> slk(g_cview_mutex); snprintf(rs, sizeof(rs), "%s", g_cview_stash_reason); }
               const char* st = (sok < 0) ? "STASH-NONE" : (sok ? "STASH-OK" : "STASH-FAIL");
               const char* fr = (sok >= 0 && age <= 1) ? "FRESH" : (sok >= 0 ? "STALE" : "-");
               ImGui::Text("Viewer: %s src=%d (%s) %s (age %lluf, blits %llu)",
                  st, ssrc, rs[0] ? rs : "-", fr,
                  (unsigned long long)age, (unsigned long long)g_cview_blits.load(std::memory_order_relaxed));
               ImGui::TextWrapped("Pick a source above; every large draw is replaced until OFF. DLSS slots are parked, never eaten. FAIL names the reason (see ReShade.log DaysGone cVIEW) -- never silently black.");
            }
           // M52: channel toggle lives with the views (was buried in the
           // pixel-legacy header; applies to every view, both viewers).
           {
              const char* vc_names[] = { "R", "G", "B", "A", "ALL", "DepthN" };
              int vc = g_view_chan.load();
              if (vc < 0) vc = 0; if (vc > 5) vc = 5;
              if (ImGui::Combo("Viewer channel (all views)", &vc, vc_names, 6))
                 g_view_chan.store(vc);
           }
       }

        if (ImGui::CollapsingHeader("Tools (one-shot diagnostics)"))
        {
           if (ImGui::Button("Audit DLSS inputs (10 feeds, BOTH paths)"))
           {
              g_audit_seq.store(0, std::memory_order_relaxed);
              g_audit_left.store(10, std::memory_order_relaxed);
              char ab[256];
              snprintf(ab, sizeof(ab),
                 "DaysGone AUDIT start: move + pan for the next 10 DLSS feeds (*=handle changed, !=fidx skip; *=stale risk)");
              reshade::log::message(reshade::log::level::info, ab);
           }
           if (ImGui::Button("Stats: compute inputs (signal check)"))
              g_cstats.store(true, std::memory_order_relaxed);
           if (ImGui::Button("Log compute TAA u/t dims"))
              g_clog_slot.store(true, std::memory_order_relaxed);
           if (ImGui::Button("Dump compute TAA CB0 floats"))
              g_cdump_cb.store(true, std::memory_order_relaxed);
           if (ImGui::CollapsingHeader("Advanced scans (may hitch)"))
           {
              ImGui::TextWrapped("WARNING: these stall the GPU for a scan during gameplay. Press once in gameplay, then check ReShade.log.");
              if (ImGui::Button("Scan TAA CBs for projection matrices"))
                 g_cscan_cb.store(true, std::memory_order_relaxed);
              if (ImGui::Button("Scan velocity-pass CBs (VS+PS)"))
                 g_pscan_cb.store(true, std::memory_order_relaxed);
              if (ImGui::Button("Sniff VS CBs for shared matrices (brief hitch)"))
              {
                 g_mscan_n = 0;
                 g_mscan_sniffs = 0;
                 MScanResetLoc();
                 g_mscan_frame0 = g_hist_frame.load(std::memory_order_relaxed);
                 g_mscan_cb.store(true, std::memory_order_relaxed);
              }
           }
            // Full-trace master switch (all configs). Bounded: 1 line/present
            // to full.log + ReShade.log mirror; never per-draw (5700 draws
            // per frame would explode the file). Tracefix: N clamped 30..300
            // (~5s max), no re-arm while running, viewers-OFF gate like the
            // bundle buttons (FULL itself is atomics-only, but the bundle
            // pairs it with pics/passes that SKIP while viewers are armed).
            {
               bool ft = g_fulltrace.load(std::memory_order_relaxed);
               int left = g_fulltrace_frames_left.load(std::memory_order_relaxed);
               bool running = (left > 0);
               bool troff = (g_view_src.load(std::memory_order_relaxed) < 0 &&
                  g_cview_src.load(std::memory_order_relaxed) < 0 &&
                  g_feedview.load(std::memory_order_relaxed) < 0);
               if (ImGui::Checkbox("LOG EVERYTHING (full-trace, next N frames)", &ft))
               {
                  if (ft && !running)
                  {
                     if (!troff)
                        ft = false; // refuse: checkbox falls back unchecked next frame
                     else
                     {
                        int cn = g_fulltrace_n.load(std::memory_order_relaxed);
                        if (cn < 30) cn = 30;
                        if (cn > 300) cn = 300;
                        g_fulltrace_frames_left.store(cn, std::memory_order_relaxed);
                     }
                  }
                  if (!ft)
                     g_fulltrace_frames_left.store(0, std::memory_order_relaxed);
                  g_fulltrace.store(ft, std::memory_order_relaxed);
               }
               int nn = g_fulltrace_n.load(std::memory_order_relaxed);
               if (ImGui::InputInt("full-trace N presents (30-300, ~5s=300)", &nn))
               {
                  if (nn < 30) nn = 30;
                  if (nn > 300) nn = 300;
                  g_fulltrace_n.store(nn, std::memory_order_relaxed);
               }
               if (running)
                  ImGui::Text("FULL-TRACE ACTIVE: %d presents left (full.log + DaysGone FULL mirror)", left);
               else if (!troff)
                  ImGui::TextWrapped("Full-trace unavailable: turn Viewers OFF first.");
               else if (ImGui::Button("Start full-trace now"))
               {
                  int cn2 = g_fulltrace_n.load(std::memory_order_relaxed);
                  if (cn2 < 30) cn2 = 30;
                  if (cn2 > 300) cn2 = 300;
                  g_fulltrace_frames_left.store(cn2, std::memory_order_relaxed);
                  g_fulltrace.store(true, std::memory_order_relaxed);
#if !(TEST || DEVELOPMENT)
                  reshade::log::message(reshade::log::level::info,
                     "DaysGone FULL note: file logging disabled in Publishing config; ReShade.log mirror only.");
#endif
               }
               ImGui::Text("Hotkey: F10 starts full-trace with the N above.");
            }
#if TEST || DEVELOPMENT
             int cap_left = g_cap_frames_left.load();
            if (cap_left > 0)
               ImGui::Text("CAPTURING... %d presents left", cap_left);
            else if (g_view_src.load(std::memory_order_relaxed) >= 0 || g_cview_src.load(std::memory_order_relaxed) >= 0 || g_feedview.load(std::memory_order_relaxed) >= 0)
               ImGui::TextWrapped("Capture unavailable: turn Viewers OFF first.");
            else if (ImGui::Button("Capture 3 frames (TAA hunt)"))
               g_cap_frames_left.store(3);
            ImGui::TextWrapped("Read Luma-DaysGone-frame.log: TAA = fullscreen RT + 2x HDR color + depth + velocity.");
#endif
            bool viewers_off = (g_view_src.load(std::memory_order_relaxed) < 0 && g_cview_src.load(std::memory_order_relaxed) < 0 && g_feedview.load(std::memory_order_relaxed) < 0);
            int pic_left = g_pic_frames_left.load();
            if (pic_left > 0)
               ImGui::Text("SAVING PICTURES... %d presents left", pic_left);
            else if (!viewers_off)
               ImGui::TextWrapped("Pictures unavailable: turn Viewers OFF first.");
            else if (ImGui::Button("Save 3 pictures now"))
               g_pic_frames_left.store(3);
           ImGui::TextWrapped("Writes Luma-DaysGone-pic-<present>.bmp + DaysGone PIC mirror in ReShade.log (turn Viewers OFF first).");
            bool pass_on = g_pass_active.load(std::memory_order_relaxed);
            int pass_pos = g_pass_pos.load(std::memory_order_relaxed);
            if (pass_on)
               ImGui::Text("SAVING PASSES... %d/%d", pass_pos, kPassCount);
            else if (!viewers_off)
               ImGui::TextWrapped("Passes unavailable: turn Viewers OFF first.");
            else if (ImGui::Button("Save all passes now"))
           {
              g_pass_pos.store(0, std::memory_order_relaxed);
              g_pass_active.store(true, std::memory_order_relaxed);
           }
           ImGui::TextWrapped("One BMP per debug pass (~19 presents): Luma-DaysGone-pass-<src>-<present>.bmp + DaysGone PIC mirror (turn Viewers OFF first).");
           bool bundle_on = g_bundle_active.load(std::memory_order_relaxed);
           if (bundle_on)
           {
              int bp = g_pic_frames_left.load() > 0 ? g_pic_frames_left.load() : 0;
              int bm = g_pass_pos.load(std::memory_order_relaxed);
              if (bm < 0) bm = 0; if (bm > kPassCount) bm = kPassCount;
#if TEST || DEVELOPMENT
              int bc = g_cap_frames_left.load() > 0 ? g_cap_frames_left.load() : 0;
#else
              int bc = 0;
#endif
              int bt = g_fulltrace_frames_left.load(std::memory_order_relaxed);
              if (bt < 0) bt = 0;
              ImGui::Text("BUNDLE... pics %d/%d passes %d/%d cap %d/%d trace %d left",
                 bp, g_bundle_pics.load(std::memory_order_relaxed),
                 bm, kPassCount,
                 bc, g_bundle_cap.load(std::memory_order_relaxed), bt);
           }
            else if (!viewers_off)
               ImGui::TextWrapped("Bundle unavailable: turn Viewers OFF first.");
            else if (ImGui::Button("Save full bundle now"))
           {
              g_pic_frames_left.store(3);
              g_pass_pos.store(0, std::memory_order_relaxed);
              g_pass_active.store(true, std::memory_order_relaxed);
#if TEST || DEVELOPMENT
              g_cap_frames_left.store(3);
#endif
              int tn = g_fulltrace_n.load(std::memory_order_relaxed);
               if (tn < 30) tn = 30;
               if (tn > 300) tn = 300;
               // Tracefix: bundle button already requires viewers-OFF to press;
               // still never re-arm a running trace (keeps its countdown).
               if (g_fulltrace_frames_left.load(std::memory_order_relaxed) <= 0)
               {
                  g_fulltrace_frames_left.store(tn, std::memory_order_relaxed);
                  g_fulltrace.store(true, std::memory_order_relaxed);
               }
               else
                  reshade::log::message(reshade::log::level::info,
                     "DaysGone BUNDLE note: full-trace already running, keeping its countdown");
              g_bundle_pics.store(3, std::memory_order_relaxed);
              g_bundle_passes.store(kPassCount, std::memory_order_relaxed);
#if TEST || DEVELOPMENT
              g_bundle_cap.store(3, std::memory_order_relaxed);
#else
              g_bundle_cap.store(0, std::memory_order_relaxed);
#endif
              g_bundle_trace.store(tn, std::memory_order_relaxed);
              g_bundle_active.store(true, std::memory_order_relaxed);
              reshade::log::message(reshade::log::level::info,
                 "DaysGone BUNDLE start: pics + passes + cap + full-trace armed");
           }
           ImGui::TextWrapped("Arms pics(3) + passes(19) + cap(3) + full-trace(N); 'DaysGone BUNDLE done ...' mirrors at the end (turn Viewers OFF first).");
       }
    }
};

// The game instance is owned by Luma core (extern `game` in core.hpp).
// It MUST be heap-allocated in DllMain before CoreMain registers the addon
// (same pattern as every upstream game project, e.g. NFS Heat).
static DaysGoneGame* g_daysGoneGame = nullptr;

static void WriteLoadMarker(HMODULE hModule, const char* phase)
{
   char path[MAX_PATH] = {};
   DWORD len = GetModuleFileNameA(hModule, path, MAX_PATH - 32);
   if (len == 0 || len >= MAX_PATH - 32)
      return;
   char* slash = strrchr(path, '\\');
   if (!slash)
      return;
   strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "Luma-DaysGone-load-marker.txt");
   HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
   if (f == INVALID_HANDLE_VALUE)
      return;
   char line[256] = {};
   SYSTEMTIME st = {};
   GetLocalTime(&st);
   snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u %s\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, phase);
   DWORD written = 0;
   WriteFile(f, line, (DWORD)strlen(line), &written, nullptr);
   CloseHandle(f);
}

#if TEST || DEVELOPMENT
// templog: shared full.log appender (FULL + FULLAGG, behavior identical).
static void AppendFullLogLine(HMODULE hModule, const char* text)
{
   if (hModule == nullptr) return;
   char path[MAX_PATH] = {};
   DWORD len = GetModuleFileNameA(hModule, path, MAX_PATH - 32);
   char* slash = (len == 0 || len >= MAX_PATH - 32) ? nullptr : strrchr(path, '\\');
   if (slash != nullptr)
   {
      strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "Luma-DaysGone-full.log");
      HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (f != INVALID_HANDLE_VALUE)
      {
         // Tracefix: FULL line is up to 1408B (line[1408] at the call site) +
         // CRLF; the old 1024B buffer made sprintf_s hit its invalid-parameter
         // handler (fail-fast crash, worse with every BUILD_ID suffix bump).
         char fline[2048] = {};
         sprintf_s(fline, "%s\r\n", text);
         DWORD written = 0;
         WriteFile(f, fline, (DWORD)strlen(fline), &written, nullptr);
         CloseHandle(f);
      }
   }
}
#endif
// Pictures: copy the currently-bound backbuffer RTV into a staging texture,
// Map it, and hand-write a 24-bit .BMP (bottom-up, no libs). R8G8B8A8_UNORM
// (+SRGB/BGRA variants) go direct; float16 does x/(1+x) tonemap to 8-bit.
// File write is TEST||DEVELOPMENT-gated; the DaysGone PIC mirror always logs.
// Copy-only: never touches the real backbuffer; every failure logs FAIL and
// returns (caller still decrements/auto-disarms).
static void WritePicFile(HMODULE hModule, ID3D11Device* dev, uint64_t present)
{
   char fail[128] = {};
   ID3D11DeviceContext* ctx = nullptr;
   ID3D11RenderTargetView* rtv = nullptr;
   ID3D11Resource* bbres = nullptr;
   ID3D11Texture2D* bbtex = nullptr;
   ID3D11Texture2D* stag = nullptr;
   if (!dev)
      snprintf(fail, sizeof(fail), "no-device");
   else
   {
      dev->GetImmediateContext(&ctx);
      if (!ctx)
         snprintf(fail, sizeof(fail), "no-context");
      else
      {
         ctx->OMGetRenderTargets(1, &rtv, nullptr);
         if (!rtv)
            snprintf(fail, sizeof(fail), "no-rtv");
         else
         {
            rtv->GetResource(&bbres);
            if (!bbres)
               snprintf(fail, sizeof(fail), "no-resource");
            else if (FAILED(bbres->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bbtex))) || !bbtex)
               snprintf(fail, sizeof(fail), "not-tex2d");
            else
            {
               D3D11_TEXTURE2D_DESC d = {};
               bbtex->GetDesc(&d);
               bool is8 = (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                  d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
               bool is16f = (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
               if (d.SampleDesc.Count != 1)
                  snprintf(fail, sizeof(fail), "msaa-unsupported");
               else if (!is8 && !is16f)
                  snprintf(fail, sizeof(fail), "unsupported-fmt-%d", (int)d.Format);
               else if (d.Width == 0 || d.Height == 0 || d.Width > 8192 || d.Height > 8192)
                  snprintf(fail, sizeof(fail), "bad-dims-%ux%u", d.Width, d.Height);
               else
               {
                  D3D11_TEXTURE2D_DESC sd = {};
                  sd.Width = d.Width; sd.Height = d.Height;
                  sd.MipLevels = 1; sd.ArraySize = 1;
                  sd.Format = d.Format; sd.SampleDesc.Count = 1;
                  sd.Usage = D3D11_USAGE_STAGING;
                  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                  if (FAILED(dev->CreateTexture2D(&sd, nullptr, &stag)) || !stag)
                     snprintf(fail, sizeof(fail), "staging-fail");
                  else
                  {
                     ctx->CopySubresourceRegion(stag, 0, 0, 0, 0, bbtex, 0, nullptr);
                     D3D11_MAPPED_SUBRESOURCE m = {};
                     if (FAILED(ctx->Map(stag, 0, D3D11_MAP_READ, 0, &m)) || !m.pData)
                        snprintf(fail, sizeof(fail), "map-fail");
                     else
                     {
                        uint32_t W = d.Width, H = d.Height;
                        uint32_t rowPad = (4 - (W * 3u % 4u)) % 4u;
                        uint32_t rowBytes = W * 3u + rowPad;
                        std::vector<uint8_t> bmp;
                        bmp.resize(54 + (size_t)rowBytes * H);
                        uint8_t* hd = bmp.data();
                        hd[0] = 'B'; hd[1] = 'M';
                        uint32_t fsize = (uint32_t)bmp.size();
                        memcpy(hd + 2, &fsize, 4);
                        uint32_t off = 54;
                        memcpy(hd + 10, &off, 4);
                        uint32_t bis = 40;
                        memcpy(hd + 14, &bis, 4);
                        memcpy(hd + 18, &W, 4);
                        memcpy(hd + 22, &H, 4);
                        uint16_t planes = 1;
                        memcpy(hd + 26, &planes, 2);
                        uint16_t bpp = 24;
                        memcpy(hd + 28, &bpp, 2);
                        bool bgra = (d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
                        for (uint32_t y = 0; y < H; y++)
                        {
                           const uint8_t* src = (const uint8_t*)m.pData + (size_t)(H - 1 - y) * m.RowPitch;
                           uint8_t* dst = bmp.data() + 54 + (size_t)y * rowBytes;
                           for (uint32_t x = 0; x < W; x++)
                           {
                              uint8_t R, G, B;
                              if (is8)
                              {
                                 const uint8_t* px = src + (size_t)x * 4;
                                 if (bgra) { B = px[0]; G = px[1]; R = px[2]; }
                                 else { R = px[0]; G = px[1]; B = px[2]; }
                              }
                              else
                              {
                                 const uint16_t* px = (const uint16_t*)(src + (size_t)x * 8);
                                 float fr = HalfToFloat(px[0]), fg = HalfToFloat(px[1]), fb = HalfToFloat(px[2]);
                                 if (fr < 0) fr = 0; if (fg < 0) fg = 0; if (fb < 0) fb = 0;
                                 fr = fr / (1.0f + fr); fg = fg / (1.0f + fg); fb = fb / (1.0f + fb);
                                 R = (uint8_t)(fr * 255.0f + 0.5f); G = (uint8_t)(fg * 255.0f + 0.5f); B = (uint8_t)(fb * 255.0f + 0.5f);
                              }
                              dst[x * 3u + 0] = B; dst[x * 3u + 1] = G; dst[x * 3u + 2] = R;
                           }
                           for (uint32_t p = 0; p < rowPad; p++)
                              dst[W * 3u + p] = 0;
                        }
                        ctx->Unmap(stag, 0);
                        char fname[64] = {};
                        snprintf(fname, sizeof(fname), "Luma-DaysGone-pic-%llu.bmp", (unsigned long long)present);
#if TEST || DEVELOPMENT
                        if (hModule != nullptr)
                        {
                           char path[MAX_PATH] = {};
                           DWORD len = GetModuleFileNameA(hModule, path, MAX_PATH - 32);
                           char* slash = (len == 0 || len >= MAX_PATH - 32) ? nullptr : strrchr(path, '\\');
                           if (slash == nullptr)
                              snprintf(fail, sizeof(fail), "path-fail");
                           else
                           {
                              strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), fname);
                              HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                              if (f == INVALID_HANDLE_VALUE)
                                 snprintf(fail, sizeof(fail), "create-fail");
                              else
                              {
                                 DWORD written = 0;
                                 WriteFile(f, bmp.data(), (DWORD)bmp.size(), &written, nullptr);
                                 CloseHandle(f);
                                 if (written != (DWORD)bmp.size())
                                    snprintf(fail, sizeof(fail), "write-fail");
                              }
                           }
                        }
                        else
                           snprintf(fail, sizeof(fail), "no-hmodule");
#else
                        (void)hModule;
#endif
                        if (!fail[0])
                        {
                           char ok[128] = {};
                           snprintf(ok, sizeof(ok), "DaysGone PIC saved %s %ux%u", fname, W, H);
                           reshade::log::message(reshade::log::level::info, ok);
                        }
                     }
                  }
               }
            }
         }
      }
   }
   if (fail[0])
   {
      char fl[192] = {};
      snprintf(fl, sizeof(fl), "DaysGone PIC FAIL %s", fail);
      reshade::log::message(reshade::log::level::warning, fl);
   }
   if (stag) stag->Release();
   if (bbtex) bbtex->Release();
   if (bbres) bbres->Release();
   if (rtv) rtv->Release();
   if (ctx) ctx->Release();
}
// All-passes pixel writer: staging copy + Map of ONE source resource, then a
// hand-written 24-bit .BMP (bottom-up, no libs) named
// Luma-DaysGone-pass-<filetag>-<present>.bmp. Shares the WritePicFile path
// (same staging/Map/BMP/header code shape). Color/float use the same
// x/(1+x) tonemap as pictures; depth sources (is_depth) save RAW linear
// (clamp 0..1, no curve -- they look dark, fine for debug); 2-lane velocity
// maps ch0->R, ch1->G. File write is TEST||DEVELOPMENT-gated; the DaysGone
// PIC mirror always logs. Copy-only; failures log SKIP and return.
static void WritePicRes(HMODULE hModule, ID3D11Device* dev, ID3D11DeviceContext* ctx,
   ID3D11Resource* src, const char* filetag, const char* logsrc, uint64_t present, bool is_depth)
{
   char skip[128] = {};
   ID3D11Texture2D* tex = nullptr;
   ID3D11Texture2D* stag = nullptr;
   if (!dev || !ctx || !src || !filetag || !logsrc)
      snprintf(skip, sizeof(skip), "bad-arg");
   else if (FAILED(src->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) || !tex)
      snprintf(skip, sizeof(skip), "not-tex2d");
   else
   {
      D3D11_TEXTURE2D_DESC d = {};
      tex->GetDesc(&d);
      enum Kind { K8, K16F4, K16F2, K16U2, K32F2, KDF, KD24, KD32S8, KG16, KBad };
      Kind k = KBad;
      bool bgra = false;
      switch (d.Format)
      {
      case DXGI_FORMAT_R8G8B8A8_TYPELESS: // fmt27: plain 8-bit RGBA (same 4B/px layout
         k = K8; break;                   // as UNORM -- direct copy, no tonemap).
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: k = K8; break;
      case DXGI_FORMAT_B8G8R8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: k = K8; bgra = true; break;
      case DXGI_FORMAT_R16G16B16A16_FLOAT: k = K16F4; break;
      case DXGI_FORMAT_R16G16_FLOAT: k = K16F2; break;
      case DXGI_FORMAT_R16G16_UNORM:
      case DXGI_FORMAT_R16G16_UINT: k = K16U2; break;
      case DXGI_FORMAT_R32G32_FLOAT: k = K32F2; break;
      case DXGI_FORMAT_R32_FLOAT:
      case DXGI_FORMAT_D32_FLOAT:
      case DXGI_FORMAT_R16_FLOAT: k = KDF; break;
      case DXGI_FORMAT_D24_UNORM_S8_UINT:
      case DXGI_FORMAT_R24G8_TYPELESS:
      case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: k = KD24; break;
      case DXGI_FORMAT_R32G8X24_TYPELESS:
      case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: k = KD32S8; break;
      case DXGI_FORMAT_R16_UNORM:
      case DXGI_FORMAT_R16_TYPELESS: k = KG16; break;
      default: break;
      }
      if (d.SampleDesc.Count != 1)
         snprintf(skip, sizeof(skip), "msaa-unsupported");
      else if (k == KBad)
         snprintf(skip, sizeof(skip), "unsupported-fmt-%d", (int)d.Format);
      else if (d.Width == 0 || d.Height == 0 || d.Width > 8192 || d.Height > 8192)
         snprintf(skip, sizeof(skip), "bad-dims-%ux%u", d.Width, d.Height);
      else
      {
         D3D11_TEXTURE2D_DESC sd = {};
         sd.Width = d.Width; sd.Height = d.Height;
         sd.MipLevels = 1; sd.ArraySize = 1;
         sd.Format = d.Format; sd.SampleDesc.Count = 1;
         sd.Usage = D3D11_USAGE_STAGING;
         sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
         if (FAILED(dev->CreateTexture2D(&sd, nullptr, &stag)) || !stag)
            snprintf(skip, sizeof(skip), "staging-fail");
         else
         {
            ctx->CopySubresourceRegion(stag, 0, 0, 0, 0, tex, 0, nullptr);
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (FAILED(ctx->Map(stag, 0, D3D11_MAP_READ, 0, &m)) || !m.pData)
               snprintf(skip, sizeof(skip), "map-fail");
            else
            {
               uint32_t W = d.Width, H = d.Height;
               uint32_t rowPad = (4 - (W * 3u % 4u)) % 4u;
               uint32_t rowBytes = W * 3u + rowPad;
               std::vector<uint8_t> bmp;
               bmp.resize(54 + (size_t)rowBytes * H);
               uint8_t* hd = bmp.data();
               hd[0] = 'B'; hd[1] = 'M';
               uint32_t fsize = (uint32_t)bmp.size();
               memcpy(hd + 2, &fsize, 4);
               uint32_t off = 54;
               memcpy(hd + 10, &off, 4);
               uint32_t bis = 40;
               memcpy(hd + 14, &bis, 4);
               memcpy(hd + 18, &W, 4);
               memcpy(hd + 22, &H, 4);
               uint16_t planes = 1;
               memcpy(hd + 26, &planes, 2);
               uint16_t bpp = 24;
               memcpy(hd + 28, &bpp, 2);
               auto tone = [](float v) -> uint8_t {
                  if (v < 0) v = 0;
                  v = v / (1.0f + v);
                  return (uint8_t)(v * 255.0f + 0.5f);
               };
               auto rawb = [](float v) -> uint8_t {
                  if (v < 0) v = 0; if (v > 1) v = 1;
                  return (uint8_t)(v * 255.0f + 0.5f);
               };
               for (uint32_t y = 0; y < H; y++)
               {
                  const uint8_t* srow = (const uint8_t*)m.pData + (size_t)(H - 1 - y) * m.RowPitch;
                  uint8_t* drow = bmp.data() + 54 + (size_t)y * rowBytes;
                  for (uint32_t x = 0; x < W; x++)
                  {
                     uint8_t R = 0, G = 0, B = 0;
                     if (k == K8)
                     {
                        const uint8_t* px = srow + (size_t)x * 4;
                        if (bgra) { B = px[0]; G = px[1]; R = px[2]; }
                        else { R = px[0]; G = px[1]; B = px[2]; }
                     }
                     else if (k == K16F4)
                     {
                        const uint16_t* px = (const uint16_t*)(srow + (size_t)x * 8);
                        if (is_depth)
                        {
                           R = G = B = rawb(HalfToFloat(px[0]));
                        }
                        else
                        {
                           R = tone(HalfToFloat(px[0])); G = tone(HalfToFloat(px[1])); B = tone(HalfToFloat(px[2]));
                        }
                     }
                     else if (k == K16F2)
                     {
                        const uint16_t* px = (const uint16_t*)(srow + (size_t)x * 4);
                        R = tone(HalfToFloat(px[0])); G = tone(HalfToFloat(px[1])); B = 0;
                     }
                     else if (k == K16U2)
                     {
                        const uint16_t* px = (const uint16_t*)(srow + (size_t)x * 4);
                        R = (uint8_t)(((uint32_t)px[0] * 255u + 32767u) / 65535u);
                        G = (uint8_t)(((uint32_t)px[1] * 255u + 32767u) / 65535u);
                        B = 0;
                     }
                     else if (k == K32F2)
                     {
                        const float* px = (const float*)(srow + (size_t)x * 8);
                        R = tone(px[0]); G = tone(px[1]); B = 0;
                     }
                     else if (k == KDF)
                     {
                        float v = 0.0f;
                        if (d.Format == DXGI_FORMAT_R16_FLOAT)
                           v = HalfToFloat(((const uint16_t*)(srow + (size_t)x * 2))[0]);
                        else
                           v = ((const float*)(srow + (size_t)x * 4))[0];
                        R = G = B = rawb(v);
                     }
                     else if (k == KD24)
                     {
                        uint32_t dw = ((const uint32_t*)(srow + (size_t)x * 4))[0] & 0x00FFFFFFu;
                        float v = (float)dw / 16777215.0f;
                        R = G = B = rawb(v);
                     }
                     else if (k == KD32S8)
                     {
                        float v = ((const float*)(srow + (size_t)x * 8))[0];
                        R = G = B = rawb(v);
                     }
                     else if (k == KG16)
                     {
                        uint16_t w = ((const uint16_t*)(srow + (size_t)x * 2))[0];
                        uint8_t g = (uint8_t)(((uint32_t)w * 255u + 32767u) / 65535u);
                        R = G = B = g;
                     }
                     drow[x * 3u + 0] = B; drow[x * 3u + 1] = G; drow[x * 3u + 2] = R;
                  }
                  for (uint32_t p = 0; p < rowPad; p++)
                     drow[W * 3u + p] = 0;
               }
               ctx->Unmap(stag, 0);
               char fname[96] = {};
               snprintf(fname, sizeof(fname), "Luma-DaysGone-pass-%s-%llu.bmp", filetag, (unsigned long long)present);
#if TEST || DEVELOPMENT
               if (hModule != nullptr)
               {
                  char path[MAX_PATH] = {};
                  DWORD len = GetModuleFileNameA(hModule, path, MAX_PATH - 32);
                  char* slash = (len == 0 || len >= MAX_PATH - 32) ? nullptr : strrchr(path, '\\');
                  if (slash == nullptr)
                     snprintf(skip, sizeof(skip), "path-fail");
                  else
                  {
                     strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), fname);
                     HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                     if (f == INVALID_HANDLE_VALUE)
                        snprintf(skip, sizeof(skip), "create-fail");
                     else
                     {
                        DWORD written = 0;
                        WriteFile(f, bmp.data(), (DWORD)bmp.size(), &written, nullptr);
                        CloseHandle(f);
                        if (written != (DWORD)bmp.size())
                           snprintf(skip, sizeof(skip), "write-fail");
                     }
                  }
               }
               else
                  snprintf(skip, sizeof(skip), "no-hmodule");
#else
               (void)hModule;
#endif
               if (!skip[0])
               {
                  char ok[192] = {};
                  snprintf(ok, sizeof(ok), "DaysGone PIC saved %s %ux%u %s", fname, W, H, logsrc);
                  reshade::log::message(reshade::log::level::info, ok);
               }
            }
         }
      }
   }
   if (skip[0])
   {
      char fl[256] = {};
      snprintf(fl, sizeof(fl), "DaysGone PIC SKIP %s %s", logsrc ? logsrc : "?", skip);
      reshade::log::message(reshade::log::level::info, fl);
   }
   if (stag) stag->Release();
   if (tex) tex->Release();
}
// All-passes dispatcher: resolve ONE source per present from its existing
// stored ref (slot stash / device caches / feed refs), save it via
// WritePicRes (max 1 staging copy + Map per present), then advance. Missing
// sources log SKIP and advance -- never stalls. Auto-disarms + releases slot
// refs when the full cycle completes.
static void WritePassOne(HMODULE hModule, ID3D11Device* dev, DeviceData& dd, uint64_t present)
{
   int pos = g_pass_pos.load(std::memory_order_relaxed);
   if (pos < 0) pos = 0;
   if (pos >= kPassCount)
   {
      g_pass_active.store(false, std::memory_order_relaxed);
      return;
   }
   const PassSrc& ps = kPassSrcs[pos];
   bool is_depth = (ps.id == 1 || ps.id == 7 || ps.id == 101 || ps.id == 105);
   ComPtr<ID3D11Resource> held;
   char nosrc[96] = {};
   if (!dev)
      snprintf(nosrc, sizeof(nosrc), "no-device");
   else if (ps.id >= 0 && ps.id <= 5)
   {
      std::lock_guard<std::mutex> plk(g_pass_slot_mutex);
      held = g_pass_slot[ps.id];
      if (!held)
         snprintf(nosrc, sizeof(nosrc), "no-slot-yet");
   }
   else if (ps.id >= 6 && ps.id <= 10)
   {
#if ENABLE_SR
      DaysGoneDeviceData* gd = dd.game ? static_cast<DaysGoneDeviceData*>(dd.game) : nullptr;
      if (!gd)
         snprintf(nosrc, sizeof(nosrc), "no-device-data");
      else if (ps.id == 6) { if (gd->cached_mvs) held = gd->cached_mvs.get(); else snprintf(nosrc, sizeof(nosrc), "empty-cache"); }
      else if (ps.id == 7) { if (gd->cached_depth) held = gd->cached_depth.get(); else snprintf(nosrc, sizeof(nosrc), "empty-cache"); }
      else if (ps.id == 8) { if (gd->cached_hdr) held = gd->cached_hdr.get(); else snprintf(nosrc, sizeof(nosrc), "empty-cache"); }
      else if (ps.id == 9) { if (gd->tex_dlss_out) held = static_cast<ID3D11Resource*>(gd->tex_dlss_out.get()); else snprintf(nosrc, sizeof(nosrc), "empty-cache"); }
      else { if (gd->tex_mvs_conv) held = static_cast<ID3D11Resource*>(gd->tex_mvs_conv.get()); else snprintf(nosrc, sizeof(nosrc), "empty-cache"); }
#else
      snprintf(nosrc, sizeof(nosrc), "no-sr-build");
#endif
   }
   else
   {
      std::lock_guard<std::mutex> flk(g_feed_mutex);
      if (ps.id == 100) held = g_feed_m11_color;
      else if (ps.id == 101) held = g_feed_m11_depth;
      else if (ps.id == 102) held = g_feed_m11_mv;
      else if (ps.id == 103) held = g_feed_m11_out;
      else if (ps.id == 104) held = g_feed_m7_color;
      else if (ps.id == 105) held = g_feed_m7_depth;
      else if (ps.id == 106) held = g_feed_m7_mv;
      else if (ps.id == 107) held = g_feed_m7_out;
      if (!held)
         snprintf(nosrc, sizeof(nosrc), "feed-empty");
   }
   if (nosrc[0])
   {
      char fl[256] = {};
      snprintf(fl, sizeof(fl), "DaysGone PIC SKIP %s %s", ps.tag, nosrc);
      reshade::log::message(reshade::log::level::info, fl);
   }
   else
   {
      ID3D11DeviceContext* ctx = nullptr;
      dev->GetImmediateContext(&ctx);
      if (!ctx)
      {
         char fl[256] = {};
         snprintf(fl, sizeof(fl), "DaysGone PIC SKIP %s no-context", ps.tag);
         reshade::log::message(reshade::log::level::info, fl);
      }
      else
      {
         WritePicRes(hModule, dev, ctx, held.get(), ps.tag, ps.tag, present, is_depth);
         ctx->Release();
      }
   }
   int done = g_pass_pos.fetch_add(1, std::memory_order_relaxed) + 1;
   if (done >= kPassCount)
   {
      g_pass_active.store(false, std::memory_order_relaxed);
      {
         std::lock_guard<std::mutex> plk(g_pass_slot_mutex);
         for (int i = 0; i < 6; i++)
            g_pass_slot[i].reset();
      }
      reshade::log::message(reshade::log::level::info, "DaysGone PIC passes done 19/19");
   }
}
// Full-trace: ONE line per present while armed (never per-draw). Reuses live
// atomics only. File append is TEST||DEVELOPMENT-gated (same as frame/draws
// logs); the ReShade.log mirror runs in ALL configs with prefix DaysGone FULL.
static void WriteFullLine(HMODULE hModule, uint64_t present)
{
   uint64_t draws = g_draws_last_frame.load(std::memory_order_relaxed);
   uint64_t frozen = g_frozen_skipped_last_frame.load(std::memory_order_relaxed);
   uint64_t att = g_cdlss_attempts.load(std::memory_order_relaxed);
   uint64_t runs = g_cdlss_runs.load(std::memory_order_relaxed);
   uint64_t resets = g_cdlss_resets.load(std::memory_order_relaxed);
   int mvreal = g_clast_mvreal.load(std::memory_order_relaxed);
   uint32_t mvhash = g_clast_mvhash.load(std::memory_order_relaxed);
   int mvfmt = g_clast_mvfmt.load(std::memory_order_relaxed);
   int mvcode = g_clast_mvcode.load(std::memory_order_relaxed);
   int owng = g_clast_ownguard.load(std::memory_order_relaxed);
   unsigned long long vfc = g_clast_vfcur.load(std::memory_order_relaxed);
   unsigned long long vfp2 = g_clast_vfprev.load(std::memory_order_relaxed);
   unsigned long long vfo = g_clast_oframe.load(std::memory_order_relaxed);
   char mvsrc[32] = {};
   snprintf(mvsrc, sizeof(mvsrc), "%s", g_clast_mvsrc);
   int ok = g_clast_ok.load(std::memory_order_relaxed);
   int rst = g_clast_reset.load(std::memory_order_relaxed);
   float jx = g_clast_jitx.load(std::memory_order_relaxed);
   float jy = g_clast_jity.load(std::memory_order_relaxed);
   float jd00 = g_jitdbg_00.load(std::memory_order_relaxed);
   float jd01 = g_jitdbg_01.load(std::memory_order_relaxed);
   float jd26 = g_jitdbg_26.load(std::memory_order_relaxed);
   float jd27 = g_jitdbg_27.load(std::memory_order_relaxed);
   float jdcx = g_jitdbg_cjit.load(std::memory_order_relaxed);
   float jdcy = g_jitdbg_cjit_y.load(std::memory_order_relaxed);
   // templog: view election snapshot + per-present deltas (log-only).
   int vpick = g_view_pick_snap.load(std::memory_order_relaxed);
   int vnpool = g_view_npool_snap.load(std::memory_order_relaxed);
   int preads = g_clast_preads.load(std::memory_order_relaxed);
   unsigned long long pskip = g_clast_pskip.load(std::memory_order_relaxed);
   uint32_t pickck = g_view_pick_cksum.load(std::memory_order_relaxed);
   uint64_t rej = g_view_rejects.load(std::memory_order_relaxed);
   uint64_t prj = g_view_projrej.load(std::memory_order_relaxed);
   static uint64_t last_rej = 0, last_prj = 0;
   uint64_t drej = (rej >= last_rej) ? rej - last_rej : rej; last_rej = rej;
   uint64_t dproj = (prj >= last_prj) ? prj - last_prj : prj; last_prj = prj;
   float vp00 = g_own_p00.load(std::memory_order_relaxed);
   float vp11 = g_own_p11.load(std::memory_order_relaxed);
   float rotmag = g_view_rotmag.load(std::memory_order_relaxed);
   int cown = g_own_used.load(std::memory_order_relaxed);
   int cmsc = g_mv_scale_mode.load(std::memory_order_relaxed);
   int cmvsjit = g_mvs_jittered.load(std::memory_order_relaxed) ? 1 : 0;
   int cdec = g_mv_decode.load(std::memory_order_relaxed);
   int cfx = g_jit_flip_x.load(std::memory_order_relaxed) ? 1 : 0;
   int cfy = g_jit_flip_y.load(std::memory_order_relaxed) ? 1 : 0;
   int csc = g_jit_scale_mode.load(std::memory_order_relaxed);
   int con = g_cjitter_on.load(std::memory_order_relaxed) ? 1 : 0;
   float fovsl = g_vert_fov.load(std::memory_order_relaxed);
   float fovtrue = 0.0f;
   if (vp11 > 1e-6f || vp11 < -1e-6f) fovtrue = 2.0f * atanf(1.0f / vp11);
   uint64_t fi = (uint64_t)cb_luma_global_settings.FrameIndex;
   static uint64_t last_fi = 0;
   static bool firstfi = true;
   char fibang = '=';
   if (!firstfi && fi != last_fi + 1) fibang = '!';
   firstfi = false; last_fi = fi;
   uint64_t dage = g_clast_dage.load(std::memory_order_relaxed);
   uint64_t dskip = g_depth_cache_skips.load(std::memory_order_relaxed);
   uint64_t ccv = g_color_changes.load(std::memory_order_relaxed);
   int cvsrc = g_cview_stash_src.load(std::memory_order_relaxed);
   int cvok = g_cview_stash_ok.load(std::memory_order_relaxed);
   char cvreason[96] = {};
   // Bundle-hang guard: never block Present on a draw-thread mutex. If the
   // stash lock is contested, emit the busy sentinel and continue; every
   // other field below stays byte-identical.
   bool cvbusy = false;
   {
      std::unique_lock<std::mutex> lk(g_cview_mutex, std::try_to_lock);
      if (lk.owns_lock())
         snprintf(cvreason, sizeof(cvreason), "%s", g_cview_stash_reason);
      else
         cvbusy = true;
   }
   for (char* p = cvreason; *p; ++p) { if (*p == ' ' || *p == ':') *p = '_'; }
   int fmaster = g_freeze_master.load(std::memory_order_relaxed) ? 1 : 0;
   // real vs zero vs stale: stale = last MV was real but the compute slot has
   // been silent 30+ presents (feed not refreshed; same 30f rule as M53/feed).
   const char* mvst = mvreal ? "real" : "zero";
   {
      uint64_t cur = g_hist_frame.load(std::memory_order_relaxed);
      uint64_t lf = g_cdlss_last_fire.load(std::memory_order_relaxed);
      if (mvreal && cur >= lf && (cur - lf) >= 30)
         mvst = "stale";
   }
   char cvst[128] = {};
   if (cvbusy)
      snprintf(cvst, sizeof(cvst), "-99:busy");
   else if (cvok < 0)
      snprintf(cvst, sizeof(cvst), "%d:none", cvsrc);
   else
      snprintf(cvst, sizeof(cvst), "%d:%s%s%s", cvsrc, cvok ? "ok" : "fail:",
         cvok ? "" : (cvreason[0] ? cvreason : "unknown"));
   // Worst-case growth of the new view-filter fields is ~60 chars; buffers
   // sized so the margin over the true worst case stays >= 128 (snprintf
   // stays sizeof-bounded regardless -- no repeat of the old overflow).
   char line[1408] = {};
   snprintf(line, sizeof(line),
      "full %llu build=%s draws=%llu frozen=%llu slot_att/slot_runs/slot_resets=%llu/%llu/%llu mv=%s mvhash=%08X mvfmt=%d mvsrc=%s mvcode=%d owng=%d vfp=%llu/%llu/%llu dlss_ok=%d dlss_reset=%d jit=%.2f,%.2f jitraw=%.2f,%.2f,%.2f,%.2f,%.2f,%.2f cview=%s freeze_master=%d view=%d/%d preads=%d pskip=%llu pickck=%08X rejD+%llu projrejD+%llu p%.3f,%.3f rot=%.4f cfg=%d.%d.%d.%d jm=%d.%d.%d.%d fov=%.3f:%.3f fidx=%llu%c dage=%llu dskip=%llu cc=%llu",
      (unsigned long long)present, DG_BUILD_ID,
      (unsigned long long)draws, (unsigned long long)frozen,
      (unsigned long long)att, (unsigned long long)runs, (unsigned long long)resets,
      mvst, (unsigned)mvhash, mvfmt, mvsrc, mvcode, owng, vfc, vfp2, vfo, ok ? 1 : 0, rst ? 1 : 0, jx, jy, jd00, jd01, jd26, jd27, jdcx, jdcy, cvst, fmaster,
      vpick, vnpool, preads, pskip, (unsigned)pickck, (unsigned long long)drej, (unsigned long long)dproj, vp00, vp11, rotmag,
      cown, cmsc, cmvsjit, cdec, cfx, cfy, csc, con, fovsl, fovtrue, (unsigned long long)fi, fibang,
      (unsigned long long)dage, (unsigned long long)dskip, (unsigned long long)ccv);
#if TEST || DEVELOPMENT
   AppendFullLogLine(hModule, line);
#else
   (void)hModule;
#endif
   {
      char rline[1472] = {};
      snprintf(rline, sizeof(rline), "DaysGone FULL %s", line);
      reshade::log::message(reshade::log::level::info, rline);
   }
   // templog: FULLAGG window aggregate every 600 presents (log-only,
   // read-and-reset; accumulators update in feed/commit paths only).
   if (present % 600 == 0)
   {
      uint64_t wstart = (present >= 599) ? present - 599 : 0;
      uint64_t jn = g_agg_jitn.exchange(0, std::memory_order_relaxed);
      int64_t jsx = g_agg_jitsumx.exchange(0, std::memory_order_relaxed);
      int64_t jsy = g_agg_jitsumy.exchange(0, std::memory_order_relaxed);
      uint64_t q00 = g_agg_q00.exchange(0, std::memory_order_relaxed);
      uint64_t q01 = g_agg_q01.exchange(0, std::memory_order_relaxed);
      uint64_t q10 = g_agg_q10.exchange(0, std::memory_order_relaxed);
      uint64_t q11 = g_agg_q11.exchange(0, std::memory_order_relaxed);
      uint64_t rn = g_agg_rotn.exchange(0, std::memory_order_relaxed);
      int64_t rsum = g_agg_rotsum.exchange(0, std::memory_order_relaxed);
      int64_t rmax = g_agg_rotmax.exchange(0, std::memory_order_relaxed);
      uint64_t ars = g_agg_resets.exchange(0, std::memory_order_relaxed);
      uint64_t apr = g_agg_projrej.exchange(0, std::memory_order_relaxed);
      double jmx = jn ? (double)jsx / 1000.0 / (double)jn : 0.0;
      double jmy = jn ? (double)jsy / 1000.0 / (double)jn : 0.0;
      double rmean = rn ? (double)rsum / 1000.0 / (double)rn : 0.0;
      double rmaxv = (double)rmax / 1000.0;
      char aline[512] = {};
      snprintf(aline, sizeof(aline),
         "DaysGone FULLAGG presents=%llu-%llu jit_n=%llu mean=%.3f,%.3f quad00/01/10/11=%llu/%llu/%llu/%llu rot_mean=%.4f rot_max=%.4f resets=%llu projrej=%llu cfg=%d.%d.%d.%d",
         (unsigned long long)wstart, (unsigned long long)present,
         (unsigned long long)jn, jmx, jmy,
         (unsigned long long)q00, (unsigned long long)q01, (unsigned long long)q10, (unsigned long long)q11,
         rmean, rmaxv, (unsigned long long)ars, (unsigned long long)apr,
         cown, cmsc, cmvsjit, cdec);
      reshade::log::message(reshade::log::level::info, aline);
#if TEST || DEVELOPMENT
      AppendFullLogLine(hModule, aline);
#endif
   }
}

#if TEST || DEVELOPMENT
// Describe one texture resource as handle:WxH:fmt. The handle (raw
// ID3D11Resource pointer) is the identity: a handle recurring as an INPUT
// across frames is a persistent buffer (the TAA history), and its consumer
// is the resolve. M5 logs t0-t7 (history/velocity hide above t3) + handles.
static void AppendResDesc(std::string& out, ID3D11Resource* res)
{
   if (res == nullptr) { out += "null"; return; }
   char e[64] = {};
   sprintf_s(e, "%016llX:", (unsigned long long)res);
   out += e;
   D3D11_RESOURCE_DIMENSION type = D3D11_RESOURCE_DIMENSION_UNKNOWN;
   res->GetType(&type);
   if (type != D3D11_RESOURCE_DIMENSION_TEXTURE2D) { out += "n2d"; return; }
   ID3D11Texture2D* tex = nullptr;
   if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) || tex == nullptr) { out += "qi-fail"; return; }
   D3D11_TEXTURE2D_DESC d = {};
   tex->GetDesc(&d);
   tex->Release();
   sprintf_s(e, "%ux%u:%d", d.Width, d.Height, (int)d.Format);
   out += e;
}

static void WriteCapLine(HMODULE hModule, uint64_t frame, uint32_t ps, ID3D11DeviceContext* ctx)
{
   if (hModule == nullptr || ctx == nullptr)
      return;
   // Output target
   ID3D11RenderTargetView* rtv = nullptr;
   ID3D11DepthStencilView* dsv = nullptr;
   ctx->OMGetRenderTargets(1, &rtv, &dsv);
   // First 8 pixel-shader inputs (M5: history/velocity hide above t3)
   ID3D11ShaderResourceView* srvs[8] = {};
   ctx->PSGetShaderResources(0, 8, srvs);

   std::string line;
   char head[64] = {};
   sprintf_s(head, "cap %llu PS %08X ", (unsigned long long)frame, ps);
   line += head;
   ID3D11Resource* res = nullptr;
   if (rtv != nullptr) { rtv->GetResource(&res); line += "RT:"; AppendResDesc(line, res); if (res) res->Release(); res = nullptr; }
   else line += "RT:null";
   if (dsv != nullptr) { dsv->GetResource(&res); line += " D:"; AppendResDesc(line, res); if (res) res->Release(); res = nullptr; }
   else line += " D:null";
   for (int i = 0; i < 8; i++)
   {
      char tag[8] = {};
      sprintf_s(tag, " t%d:", i);
      line += tag;
      if (srvs[i] != nullptr) { srvs[i]->GetResource(&res); AppendResDesc(line, res); if (res) res->Release(); res = nullptr; }
      else line += "null";
   }
   line += "\r\n";

   if (rtv) rtv->Release();
   if (dsv) dsv->Release();
   for (int i = 0; i < 8; i++) if (srvs[i]) srvs[i]->Release();

   char path[MAX_PATH] = {};
   DWORD len = GetModuleFileNameA(hModule, path, MAX_PATH - 32);
   if (len == 0 || len >= MAX_PATH - 32 || strrchr(path, '\\') == nullptr)
      return;
   strcpy_s(strrchr(path, '\\') + 1, MAX_PATH - (strrchr(path, '\\') + 1 - path), "Luma-DaysGone-frame.log");
   HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
   if (f == INVALID_HANDLE_VALUE)
      return;
   DWORD written = 0;
   WriteFile(f, line.c_str(), (DWORD)line.size(), &written, nullptr);
   CloseHandle(f);
}

static void WriteCapCSLine(HMODULE hModule, uint64_t frame, uint32_t cs, ID3D11DeviceContext* ctx)
{
   if (hModule == nullptr || ctx == nullptr)
      return;
   ID3D11UnorderedAccessView* uavs[4] = {};
   ctx->CSGetUnorderedAccessViews(0, 4, uavs);
   ID3D11ShaderResourceView* srvs[4] = {};
   ctx->CSGetShaderResources(0, 4, srvs);

   std::string line;
   char head[64] = {};
   sprintf_s(head, "capCS %llu CS %08X ", (unsigned long long)frame, cs);
   line += head;
   ID3D11Resource* res = nullptr;
   for (int i = 0; i < 4; i++)
   {
      char tag[8] = {};
      sprintf_s(tag, " u%d:", i);
      line += tag;
      if (uavs[i] != nullptr) { uavs[i]->GetResource(&res); AppendResDesc(line, res); if (res) res->Release(); res = nullptr; }
      else line += "null";
   }
   for (int i = 0; i < 4; i++)
   {
      char tag[8] = {};
      sprintf_s(tag, " t%d:", i);
      line += tag;
      if (srvs[i] != nullptr) { srvs[i]->GetResource(&res); AppendResDesc(line, res); if (res) res->Release(); res = nullptr; }
      else line += "null";
   }
   line += "\r\n";

   for (int i = 0; i < 4; i++) if (uavs[i]) uavs[i]->Release();
   for (int i = 0; i < 4; i++) if (srvs[i]) srvs[i]->Release();

   char path[MAX_PATH] = {};
   DWORD len = GetModuleFileNameA(hModule, path, MAX_PATH - 32);
   if (len == 0 || len >= MAX_PATH - 32 || strrchr(path, '\\') == nullptr)
      return;
   strcpy_s(strrchr(path, '\\') + 1, MAX_PATH - (strrchr(path, '\\') + 1 - path), "Luma-DaysGone-frame.log");
   HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
   if (f == INVALID_HANDLE_VALUE)
      return;
   DWORD written = 0;
   WriteFile(f, line.c_str(), (DWORD)line.size(), &written, nullptr);
   CloseHandle(f);
}

// M7e: copy observation (Heat M4g pattern). History ping-pong / resolve
// maintenance often happens via copies with no shader -- invisible to draw
// logs. Observe only, never block.
static void CopyTexDesc(uint64_t handle, char* out, size_t out_size)
{
   ID3D11Resource* res = reinterpret_cast<ID3D11Resource*>(handle);
   ID3D11Texture2D* tex = nullptr;
   if (res != nullptr && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex != nullptr)
   {
      D3D11_TEXTURE2D_DESC d = {};
      tex->GetDesc(&d);
      sprintf_s(out, out_size, "%ux%u f%d", d.Width, d.Height, (int)d.Format);
      tex->Release();
   }
   else
   {
      strcpy_s(out, out_size, "buf");
   }
}

static void WriteCopyLine(HMODULE hModule, uint64_t dst, uint64_t src, const char* dd, const char* sd)
{
   if (hModule == nullptr)
      return;
   char path[MAX_PATH] = {};
   DWORD len = GetModuleFileNameA(hModule, path, MAX_PATH - 32);
   if (len == 0 || len >= MAX_PATH - 32 || strrchr(path, '\\') == nullptr)
      return;
   strcpy_s(strrchr(path, '\\') + 1, MAX_PATH - (strrchr(path, '\\') + 1 - path), "Luma-DaysGone-frame.log");
   HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
   if (f == INVALID_HANDLE_VALUE)
      return;
   char line[256] = {};
   sprintf_s(line, "copy %llu dst=%016llX %s src=%016llX %s\r\n",
      (unsigned long long)g_hist_frame.load(std::memory_order_relaxed),
      (unsigned long long)dst, dd, (unsigned long long)src, sd);
   DWORD written = 0;
   WriteFile(f, line, (DWORD)strlen(line), &written, nullptr);
   CloseHandle(f);
}

static void WriteDrawHistogram(HMODULE hModule)
{
   if (hModule == nullptr)
      return;
   char path[MAX_PATH] = {};
   DWORD len = GetModuleFileNameA(hModule, path, MAX_PATH - 32);
   if (len == 0 || len >= MAX_PATH - 32 || strrchr(path, '\\') == nullptr)
      return;
   strcpy_s(strrchr(path, '\\') + 1, MAX_PATH - (strrchr(path, '\\') + 1 - path), "Luma-DaysGone-draws.log");
   HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
   if (f == INVALID_HANDLE_VALUE)
      return;
   std::vector<std::pair<uint32_t, uint64_t>> top;
   {
      std::lock_guard<std::mutex> lk(g_hist_mutex);
      top.assign(g_ps_hist.begin(), g_ps_hist.end());
      g_ps_hist.clear();
   }
   std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
   char line[256] = {};
   SYSTEMTIME st = {};
   GetLocalTime(&st);
   snprintf(line, sizeof(line), "session %s %04u-%02u-%02u %02u:%02u:%02u frame %llu\r\n", DG_BUILD_ID, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, (unsigned long long)g_hist_frame.load());
   DWORD written = 0;
   WriteFile(f, line, (DWORD)strlen(line), &written, nullptr);
   // Compute passes + the unknowns, so the log shows ALL passes, not just PS.
   {
      std::vector<std::pair<uint32_t, uint64_t>> top;
      {
         std::lock_guard<std::mutex> lk(g_hist_mutex);
         top.assign(g_cs_hist.begin(), g_cs_hist.end());
         g_cs_hist.clear();
      }
      std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
      size_t n = top.size() > 30 ? 30 : top.size();
      for (size_t i = 0; i < n; i++)
      {
         sprintf_s(line, "CS %08X x%llu\r\n", top[i].first, (unsigned long long)top[i].second);
         WriteFile(f, line, (DWORD)strlen(line), &written, nullptr);
      }
   }
   sprintf_s(line, "PS-none x%llu (depth-only etc)\r\n", (unsigned long long)g_ps_empty.exchange(0));
   WriteFile(f, line, (DWORD)strlen(line), &written, nullptr);
   sprintf_s(line, "PS-unresolved x%llu (FFFFFFFF)\r\n", (unsigned long long)g_ps_unresolved.exchange(0));
   WriteFile(f, line, (DWORD)strlen(line), &written, nullptr);
   CloseHandle(f);
}
#endif

// This is where everything starts from, the very first call to the dll.
// Mirrors the load-only DllMain of the working NFS Heat M1:
// SetGlobals + CoreMain are what actually register the ReShade addon.
// Without them ReShade logs "No add-on was registered" and unloads us.
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
   if (ul_reason_for_call == DLL_PROCESS_ATTACH)
   {
      g_marker_module = hModule;
      WriteLoadMarker(hModule, DG_BUILD_ID);
      WriteLoadMarker(hModule, "attached");
      // Setup the globals (e.g. name etc) before registering the ReShade addon.
      const char* project_name = PROJECT_NAME;
      const char* cleared_project_name = (project_name[0] == '_') ? (project_name + 1) : project_name;
      uint32_t mod_version = 1; // Increase to reset settings/shader binaries after large changes
      // NOTE: website arg must be non-null ??" SetGlobals does an unchecked
      // strncpy into a fixed buffer, so nullptr AVs here (loader error 1114).
      Globals::SetGlobals(cleared_project_name, "Days Gone Luma mod", "" /*E.g. Nexus link*/, mod_version);
      WriteLoadMarker(hModule, "post-globals");

      // M1 load-only: keep EVERYTHING off for a true zero-changes test.
      swapchain_format_upgrade_type = TextureFormatUpgradesType::None;
      swapchain_upgrade_type = SwapchainUpgradeType::None;
      texture_format_upgrades_type = TextureFormatUpgradesType::None;
      texture_upgrade_formats = {
            reshade::api::format::r8g8b8a8_unorm,
            reshade::api::format::r8g8b8a8_unorm_srgb,
            reshade::api::format::r8g8b8a8_typeless,
            reshade::api::format::r8g8b8x8_unorm,
            reshade::api::format::r8g8b8x8_unorm_srgb,
            reshade::api::format::b8g8r8a8_unorm,
            reshade::api::format::b8g8r8a8_unorm_srgb,
            reshade::api::format::r11g11b10_float,
      };
      texture_format_upgrades_lut_size = 32;
      texture_format_upgrades_lut_dimensions = LUTDimensions::_2D;
      texture_format_upgrades_2d_size_filters = 0 | (uint32_t)TextureFormatUpgrades2DSizeFilters::SwapchainResolution | (uint32_t)TextureFormatUpgrades2DSizeFilters::SwapchainAspectRatio;
      enable_samplers_upgrade = false;

      // M1: skip Luma's final display-composition pass entirely. Without this,
      // core asserts the backbuffer is FP16/R10G10 on Days Gone's R8G8B8A8
      // SDR swapchain (and would re-encode the image anyway ??" not load-only).
      force_disable_display_composition = true;

#if TEST || DEVELOPMENT
      custom_shaders_enabled = false;
#endif
      auto_dump = false; // M43: dump OFF (set true for M2-style captures);

      // M1: never veto display-mode changes. Luma defaults to blocking ALL
      // SetFullscreenState transitions for HDR swapchain management ??" that
      // hangs exclusive-fullscreen mode switches (log stops at ResizeBuffers).
      prevent_fullscreen_state = false;
      force_borderless = false;

#if DEVELOPMENT
      // Days Gone 4.10 TAA candidates from generic Luma-Unreal logs.
      forced_shader_names.emplace(0xB146BBAE, "DaysGone TAA?");
      forced_shader_names.emplace(0x104E65B1, "DaysGone TAA?");
      forced_shader_names.emplace(0x3B21A41C, "DaysGone TAA?");
      forced_shader_names.emplace(0x4D982586, "DaysGone TAA?");
#endif

      // Create the game sub-class instance (automatically destroyed on exit).
      g_daysGoneGame = new DaysGoneGame();
      game = g_daysGoneGame;

      WriteLoadMarker(hModule, "pre-core");
   }

   CoreMain(hModule, ul_reason_for_call, lpReserved);

   if (ul_reason_for_call == DLL_PROCESS_ATTACH)
      WriteLoadMarker(hModule, "initialized");

   return TRUE;
}

