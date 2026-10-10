// Days Gone MV hybrid (M11/compute only): per-pixel game truth + own-camera
// fill for DLSS. t0 = raw game velocity (fmt35 shared buffer -- static /
// alpha / skinned pixels are never drawn and stay cleared zero); t1 =
// own-camera MVs (FP16). Decodes t0 with the same DECODE variant as
// MVConvert (output: zero-centered PIXELS, same units as both feeds), then
// outputs game where the RAW game texel was written (g.x > 0.0), else own.
// Cleared pixels read raw exactly 0.0; written zero-velocity sits at ~0.5
// bias, so selecting on decoded==0 was wrong both ways -- the select MUST be
// on raw g.x, never on decoded gd. Where the game has truth the feed is
// identical to today; cleared pixels get own camera motion instead of zero
// MVs. Downstream mvs scales (incl. the shared x2.0 mode) apply unchanged.
//   DECODE variants mirror MVConvert (1 upstream UE .. 8 X-neg + snap);
//   snap branches decode plain here (the raw select handles cleared texels).
Texture2D<float2> MVGameTex : register(t0);
Texture2D<float2> MVOwnTex : register(t1);

float2 main(float4 pos : SV_Position) : SV_Target0
{
    uint w, h;
    MVOwnTex.GetDimensions(w, h);
    uint2 xy = min((uint2)pos.xy, uint2(w, h) - 1);
    uint gw, gh;
    MVGameTex.GetDimensions(gw, gh);
    float2 g = MVGameTex.Load(int3(min((uint2)pos.xy, uint2(gw, gh) - 1), 0));
    float2 own = MVOwnTex.Load(int3(xy, 0));
#if DECODE == 1
    float2 gd = (g - 0.499992) * 4.008016 * 0.5;
#elif DECODE == 2
    float2 gd = (g * 2.0 - 0.5) * 0.5;
#elif DECODE == 3
    float2 gd = (g - 0.499992) * 4.008016 * 0.5;
#elif DECODE == 4
    float2 gd = -((g - 0.499992) * 4.008016 * 0.5);
#elif DECODE == 5
    float2 gd = (g * 2.0 - 0.5) * 0.5;
#elif DECODE == 6
    float2 gd = -((g - 0.499992) * 4.008016 * 0.5);
#elif DECODE == 7
    float2 b7 = (g - 0.499992) * 4.008016 * 0.5;
    float2 gd = float2(b7.x, -b7.y);
#elif DECODE == 8
    float2 b8 = (g - 0.499992) * 4.008016 * 0.5;
    float2 gd = float2(-b8.x, b8.y);
#else
    float2 gd = g;
#endif
    gd.x *= (float)w;
    gd.y *= (float)h;
    return ((g.x > 0.0) ? gd : own);
}
