// HLSL 2021, DXC ps_6_0 only. P03.04 PhysicalDiffuseV1 opaque pixel stage.
// Shares the legacy 176-byte DrawConstants ABI and vertex interface, so the
// separately admitted legacy viewport.vs.dxil feeds it unchanged; the legacy
// pixel shader stays byte-identical in its own file. Output is unexposed
// scene-linear radiance in cd/m^2 (linear Rec.709/D65, (1,1,1) == 1 cd/m^2)
// into the FP16 target. No tone or transfer function is applied here; the
// OutputTransform pass applies both exactly once.
//   Lo = rho * (Lenv + (E / pi) * max(dot(n, l), 0))
// illumination.xyz: world direction toward the light; illumination.w: E / pi.
// parameters.x: environment luminance Lenv; parameters.y: unused, zero;
// parameters.z: expansion (vertex stage only); parameters.w: texture mode as legacy
// (no texture=0, color mip0=1, color mips=2, scalar R mips=3).
// Textures and tints scale reflectance only; excess reflectance is rejected
// before submission, never clamped or treated as emission.
// No shadows, GI, temporal history, HDR display or ray tracing.
Texture2D<float4> colorTexture : register(t0);
SamplerState colorSampler : register(s0);
cbuffer DrawConstants : register(b0) {
    row_major float4x4 modelViewProjection;
    row_major float4x4 normalTransform;
    float4 baseColor;
    float4 illumination;
    float4 parameters;
};
struct VertexOutput { float4 position : SV_Position; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
float3 filteredColor(float2 uv, float2 uvDx, float2 uvDy) {
    uint width, height, levels;
    colorTexture.GetDimensions(0, width, height, levels);
    float2 dx = uvDx * float2(width, height);
    float2 dy = uvDy * float2(width, height);
    float footprintSquared = max(dot(dx, dx), dot(dy, dy));
    float lod = min(0.5 * log2(max(1.0, footprintSquared)), float(levels - 1));
    float lower = floor(lod);
    float fraction = lod - lower;
    float3 a = colorTexture.SampleLevel(colorSampler, uv, lower).rgb;
    // Same explicit complete-chain profile as the legacy stage: exact levels
    // need one lookup, and the mip weight stays in float linear color.
    [branch] if (fraction == 0.0) return a;
    float3 b = colorTexture.SampleLevel(colorSampler, uv, lower + 1.0).rgb;
    return lerp(a, b, fraction);
}
float4 PSPhysical(VertexOutput input) : SV_Target {
    float2 uvDx = ddx(input.uv);
    float2 uvDy = ddy(input.uv);
    float cosine = max(0, dot(normalize(input.normal), illumination.xyz));
    float3 reflectance = baseColor.rgb;
    if (parameters.w > 1.5) {
        float3 sampled = filteredColor(input.uv, uvDx, uvDy);
        reflectance *= parameters.w > 2.5 ? sampled.rrr : sampled;
    }
    else if (parameters.w > 0.5)
        reflectance *= colorTexture.SampleLevel(colorSampler, input.uv, 0).rgb;
    float3 outgoing = reflectance * (parameters.x + illumination.w * cosine);
    return float4(max(0, outgoing), 1);
}
