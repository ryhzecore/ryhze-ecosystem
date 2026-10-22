// HLSL 2021, DXC vs_6_0/ps_6_0. P03.04 OutputTransform: one fullscreen pass
// that reads the FP16 scene-linear target by exact pixel Load (no sampler) and
// writes the RGBA8 display image. x = Lo * exposure.x where exposure.x is
// 2^stops / reference luminance; y = x / (1 + x) (ReinhardRGB-v1); then the
// piecewise IEC 61966-2-1 sRGB transfer exactly once. Stored HDR pixels are
// never changed here; grid, selection and overlays draw afterwards into the
// display image through the legacy stage. Not an HDR display path.
Texture2D<float4> sceneLinear : register(t0);
cbuffer OutputConstants : register(b0) {
    float4 exposure; // x: 2^stops / reference luminance (cd/m^2); yzw: zero
    float4 reserved; // Declared padding to the 32-byte ABI; zero
};
struct Fullscreen { float4 position : SV_Position; };
Fullscreen VSOutput(uint id : SV_VertexID) {
    // One clip-space triangle covering the viewport: (-1,-1), (3,-1), (-1,3).
    Fullscreen output;
    float2 corner = float2(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0);
    output.position = float4(corner, 0, 1);
    return output;
}
float4 PSOutput(Fullscreen input) : SV_Target {
    float3 radiance = sceneLinear.Load(int3(int2(input.position.xy), 0)).rgb;
    float3 exposed = max(0, radiance * exposure.x);
    float3 displayLinear = exposed / (1 + exposed);
    float3 srgbLow = displayLinear * 12.92;
    float3 srgbHigh = 1.055 * pow(displayLinear, 1.0 / 2.4) - 0.055;
    return float4(lerp(srgbLow, srgbHigh, step(0.0031308, displayLinear)), 1);
}
