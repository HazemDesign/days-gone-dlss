// Days Gone MV convert: decode the game's fmt35 velocity buffer for DLSS.
// Bytecode proof (BE0130E5 immediates 0.2495 / 0.499992 / 4.008016 /
// -2.003978): this is STOCK UE encoding e = v*0.2495 + 0.499992
// (0.5-centered; 4.008016 = 1/0.2495 is the exact inverse), identical to
// upstream Luma_MotionVec_UE4_Decode. Luma convention (core DLSS.cpp
// passes InMVScale/Jitter straight to NGX; NVIDIA: scale 1.0 = pass
// through): the MV texture must hold zero-centered PIXELS, mvs scale 1.0.
//   0 = raw (diagnostic only: 0.5-center reads as constant motion)
//   1 = upstream UE: (c-0.499992)/0.2495*0.5*res (DEFAULT)
//   2 = 0.25-centered fallback: (c*2-0.5)*0.5*res (only if a live cSTATS
//       ever proves a 0.25-centered producer; the m28 0.25 sample had
//       identical X/Y channels = not trustworthy velocity)
//   3 = upstream + raw-written snap: RAW c.x > 0 (written pixel) decodes,
//       cleared pixels (raw exactly 0.0) map to 0 MVs. Written zero-velocity
//       sits at ~0.5 bias (never raw 0), so the old decoded==0 test was wrong
//       both ways (cleared-0 decoded to ~-1 bias = false written; written-zero
//       decoded to 0 = false unwritten). M32: viewer shows BLACK bg = cleared.
 //   4 = negated upstream (sign triage: core wants top-left-positive;
//       upstream emits -delta — if motion ghosts backwards, try this)
 //   5 = raw-written snap + 0.25-centered (M33: mode 3 proved the bg (stable
//       stationary) but motion still shimmers — motion content may use the
//       (v+1)*0.25 encoding while clear is 0)
 //   6 = negated upstream + raw-written snap (M34: mode 4's verdict was
//       INVALID — no snap, so its bg bias dominated. Sign is only testable
//       WITH snap; mode 5 failing kills the 0.25-motion idea, leaving sign
//       as the prime motion-only suspect: backwards reprojection is stable
//       stationary, chaos on motion)
 //   7 = Y-negated upstream + raw-written snap (M37: modes 3/6 prove
//       0.5-centered bulk but BOTH full-sign options shimmer identically —
//       single-axis flips were never tested; upstream bakes -delta*(0.5,-0.5))
 //   8 = X-negated upstream + raw-written snap (M37: mirrors the upstream
//       convention — theory favorite; if motion locks here, sign was it)
Texture2D<float2> MVInputTex : register(t0);

float2 main(float4 pos : SV_Position) : SV_Target0
{
    uint w, h;
    MVInputTex.GetDimensions(w, h);
    uint2 xy = min((uint2)pos.xy, uint2(w, h) - 1);
    float2 c = MVInputTex.Load(int3(xy, 0));
#if DECODE == 1
    float2 mv = (c - 0.499992) * 4.008016 * 0.5;
#elif DECODE == 2
    float2 mv = (c * 2.0 - 0.5) * 0.5;
#elif DECODE == 3
    float2 mv = (c.x > 0.0) ? (c - 0.499992) * 4.008016 * 0.5 : float2(0.0, 0.0);
#elif DECODE == 4
    float2 mv = -((c - 0.499992) * 4.008016 * 0.5);
#elif DECODE == 5
    float2 mv = (c.x > 0.0) ? (c * 2.0 - 0.5) * 0.5 : float2(0.0, 0.0);
#elif DECODE == 6
    float2 mv = (c.x > 0.0) ? -((c - 0.499992) * 4.008016 * 0.5) : float2(0.0, 0.0);
#elif DECODE == 7
    float2 b7 = (c - 0.499992) * 4.008016 * 0.5;
    float2 mv = (c.x > 0.0) ? float2(b7.x, -b7.y) : float2(0.0, 0.0);
#elif DECODE == 8
    float2 b8 = (c - 0.499992) * 4.008016 * 0.5;
    float2 mv = (c.x > 0.0) ? float2(-b8.x, b8.y) : float2(0.0, 0.0);
#else
    float2 mv = c;
#endif
    mv.x *= (float)w;
    mv.y *= (float)h;
    return mv;
}
