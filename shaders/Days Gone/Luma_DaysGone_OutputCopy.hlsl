// Days Gone DLSS copy/unpack: fullscreen point-load. Render-pipeline store
// does float -> UNORM10 conversion for free. SWAP_RB handles BGR-ordered
// data. SRGB_ENCODE replicates the native TAAU tail (sRGB piecewise encode
// + min() quirk + w=0) so DLSS output matches what downstream expects.
Texture2D<float4> DLSSOutputTex : register(t0);

float4 main(float4 pos : SV_Position) : SV_Target0
{
   uint w, h;
   DLSSOutputTex.GetDimensions(w, h);
   uint2 xy = min((uint2)pos.xy, uint2(w, h) - 1);
   float4 c = DLSSOutputTex.Load(int3(xy, 0));
#if SWAP_RB
   c = c.bgra;
#endif
#if SRGB_ENCODE
    float3 enc = 1.055 * pow(max(c.rgb, 0.003131), (1.0 / 2.4)) - 0.055;
    c.rgb = min(enc, c.rgb * 12.92);
#endif
#if SRGB_DECODE
    float3 dec = lerp(c.rgb / 12.92, pow(max((c.rgb + 0.055) / 1.055, 0.0), 2.4), step(0.04045, c.rgb));
    c.rgb = dec;
#endif
// M17: UI tonemap operators for the t0 HUD blend. UI_TM: 0 none, 1 Reinhard,
// 2 ACES (Narkowicz fit), 3 Hable/Hejl filmic. Tonemapped output stays linear
// here — SRGB_ENCODE below moves it to LDR for the blend when defined.
#if UI_TM == 1
    c.rgb = c.rgb / (c.rgb + 0.155) * 1.019;
#elif UI_TM == 2
    c.rgb = (c.rgb * (2.51 * c.rgb + 0.03)) / (c.rgb * (2.43 * c.rgb + 0.59) + 0.14);
#elif UI_TM == 3
    {
       float3 x = c.rgb * 2.0; // exposure
       float3 h = (x * (0.15 * x + 0.10 * 0.50) + 0.20 * 0.02) / (x * (0.15 * x + 0.50) + 0.20 * 0.30) - 0.02 / 0.30;
       float wht = (11.2 * (0.15 * 11.2 + 0.10 * 0.50) + 0.20 * 0.02) / (11.2 * (0.15 * 11.2 + 0.50) + 0.20 * 0.30) - 0.02 / 0.30;
       c.rgb = h / wht;
    }
#endif
   // M10c: alpha 0 (native parity — alpha 1 may break UI blend downstream).
   // M10e: KEEP_ALPHA=1 preserves it (t0 UI composite needs real alpha).
   // M22: ALPHA_ONE=1 forces opaque (native u0.A was opaque; NGX alpha is
   // unknown — translucent zones darkening downstream is the tell).
#if ALPHA_ONE
   c.a = 1.0;
#elif !KEEP_ALPHA
   c.a = 0.0;
#endif
   return c;
}
