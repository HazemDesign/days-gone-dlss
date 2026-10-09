// Days Gone viewer: single-channel raw view (no encode) to interrogate
// buffer contents. VIEWCHAN 0=R,1=G,2=B,3=A replicated to RGB,
// 4=depth near-white (1-R: reversed-Z depth reads ~0 far, so plain R
// shows a flat black field; inverted, near geometry pops white).
Texture2D<float4> DLSSOutputTex : register(t0);
cbuffer ViewScale : register(b0)
{
    float2 TargetWH; // viewer target size (viewport px); stretch-to-fill debug view
};

float4 main(float4 pos : SV_Position) : SV_Target0
{
   uint w, h;
   DLSSOutputTex.GetDimensions(w, h);
   float2 src = float2((float)w, (float)h);
   // Display-only stretch-to-fill: 1:1 legacy when TargetWH unset.
   float2 tgt = (TargetWH.x > 0.5 && TargetWH.y > 0.5) ? TargetWH : src;
   uint2 xy = (uint2)min(pos.xy * src / tgt, src - 1.0);
   float4 c = DLSSOutputTex.Load(int3(xy, 0));
#if VIEWCHAN == 0
   return float4(c.r, c.r, c.r, 1.0);
#elif VIEWCHAN == 1
   return float4(c.g, c.g, c.g, 1.0);
#elif VIEWCHAN == 2
   return float4(c.b, c.b, c.b, 1.0);
#elif VIEWCHAN == 4
   return float4(1.0 - c.r, 1.0 - c.r, 1.0 - c.r, 1.0);
#else
   return float4(c.a, c.a, c.a, 1.0);
#endif
}
