// Days Gone own camera MVs (M47 rotation / M51 full 6DOF): motion vectors
// from the stashed gameplay view data (VS CB1 2048B: R at +128 row-major
// proven orthonormal, VP at +0 proves reversed-Z infinite proj near=10cm,
// translation row at +640 with negated twin at +896).
// Rotation path needs no depth/translation (exact look-around).
// Full path reprojects via depth (R24, reversed-infinite: z = Near/d)
// with T in two candidate forms (A: V-row3, B: camera pos) -- live A/B
// settles it. Sky/invalid depth falls back to rotation for that pixel.
// Output matches the MVConvert convention (zero-centered PIXELS, NGX
// mvs scale 1.0 pass-through, top-left-positive). OWN_NEG mirrors sign
// (rotation sign triage; shared NDC math covers full modes too).
// FULLMODE: 0 rotation, 1 full-A (V-row T), 2 full-B (campos T).
cbuffer OwnMV : register(b0)
{
    row_major float4x4 RCurr; // view rotation, current frame (world->view)
    row_major float4x4 RPrev; // view rotation, previous frame (world->view)
    float4 TCur;              // +640 row3, current frame
    float4 TPrev;             // +640 row3, previous frame
    float2 ProjP;             // (p00, p11) CURRENT frame -- unproject ONLY
    float2 ResWH;             // render target dims (pixels)
    float Near;               // 10.0 (UE cm, TRUE near -- not the NGX slider)
    float2 ProjPrev;          // (p00, p11) PREVIOUS frame -- reproject ONLY (zoom fix)
    float Pad2;
};
Texture2D<float> DepthTex : register(t0); // R24 depth (full modes only)

float3 RV_Mul(float3 v, row_major float4x4 M) { return v.x * M[0].xyz + v.y * M[1].xyz + v.z * M[2].xyz; }
float3 RVT_Mul(float3 v, row_major float4x4 M) { return float3(dot(v, M[0].xyz), dot(v, M[1].xyz), dot(v, M[2].xyz)); }

float2 main(float4 pos : SV_Position) : SV_Target0
{
    float w = ResWH.x, h = ResWH.y;
    float2 uv = pos.xy / ResWH;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float3 dView = float3(ndc.x / ProjP.x, ndc.y / ProjP.y, 1.0);
    float3 dWorld;
    float3 dPrev;
#if FULLMODE != 0
    float d = DepthTex.Load(int3((int2)min(pos.xy, ResWH - 1.0), 0));
    if (d > 1e-5)
    {
        float3 vpos = dView * (Near / d);
#if FULLMODE == 1
        float3 Cc = -(RVT_Mul(TCur.xyz, RCurr));
        float3 Cp = -(RVT_Mul(TPrev.xyz, RPrev));
        dWorld = RVT_Mul(vpos, RCurr) + Cc;
        dPrev = RV_Mul(dWorld, RPrev) + TPrev.xyz;
#else
        dWorld = RVT_Mul(vpos, RCurr) + TCur.xyz;
        dPrev = RV_Mul(dWorld - TPrev.xyz, RPrev);
#endif
    }
    else
    {
        dWorld = RVT_Mul(dView, RCurr);
        dPrev = RV_Mul(dWorld, RPrev);
    }
#else
    dWorld = RVT_Mul(dView, RCurr);
    dPrev = RV_Mul(dWorld, RPrev);
#endif
    float2 mv = float2(0.0, 0.0);
    if (dPrev.z > 1e-4)
    {
        float2 ndcPrev = float2(dPrev.x * ProjPrev.x / dPrev.z, dPrev.y * ProjPrev.y / dPrev.z);
        float2 dd = float2(ndc.x - ndcPrev.x, ndc.y - ndcPrev.y);
#if OWN_NEG
        mv = float2(dd.x * 0.5 * w, -dd.y * 0.5 * h);
#else
        mv = float2(-dd.x * 0.5 * w, dd.y * 0.5 * h);
#endif
    }
    return mv;
}
