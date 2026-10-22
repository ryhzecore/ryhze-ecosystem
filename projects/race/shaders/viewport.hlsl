// HLSL 2021, DXC vs_6_0/ps_6_0. Partial opaque viewport profile:
// linear RGB Lambert + constant ambient, Reinhard then SDR sRGB exactly once.
// Optional opaque sRGB texture: LINEAR CLAMP, top-left UV origin. Legacy mip 0
// and explicit complete-chain isotropic mip filtering remain distinct profiles.
// Scalar reflectance uses an opaque linear UNORM chain: R multiplies diffuse, G/B ignored.
// No shadows, GI, temporal history, HDR display or ray tracing.
Texture2D<float4> colorTexture : register(t0);
SamplerState colorSampler : register(s0);
cbuffer DrawConstants : register(b0) {
    row_major float4x4 modelViewProjection;
    row_major float4x4 normalTransform;
    float4 baseColor;
    float4 illumination; // xyz: direction toward the light, w: intensity
    float4 parameters;   // x: ambient, y: unlit, z: expansion, w: no texture=0, color mip0=1, color mips=2, scalar R mips=3
};
struct VertexInput { float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct VertexOutput { float4 position : SV_Position; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    output.position = mul(modelViewProjection, float4(input.position * parameters.z, 1));
    output.normal = mul((float3x3)normalTransform, input.normal);
    output.uv = input.uv;
    return output;
}
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
    // Keep the mip weight in float linear color. A tiny derivative change near
    // an integer LOD must not become a visible step in the sampler's mip blend.
    // Exact levels (including magnification and the last mip) need one lookup.
    [branch] if (fraction == 0.0) return a;
    float3 b = colorTexture.SampleLevel(colorSampler, uv, lower + 1.0).rgb;
    return lerp(a, b, fraction);
}
float4 PSMain(VertexOutput input) : SV_Target {
    float2 uvDx = ddx(input.uv);
    float2 uvDy = ddy(input.uv);
    float diffuse = max(0, dot(normalize(input.normal), illumination.xyz));
    float lighting = parameters.y > 0.5 ? 1 : parameters.x + illumination.w * diffuse;
    float3 radiance = max(0, baseColor.rgb * lighting);
    if (parameters.w > 1.5) {
        float3 sampled = filteredColor(input.uv, uvDx, uvDy);
        radiance *= parameters.w > 2.5 ? sampled.rrr : sampled;
    }
    else if (parameters.w > 0.5)
        radiance *= colorTexture.SampleLevel(colorSampler, input.uv, 0).rgb;
    float3 displayLinear = radiance / (1 + radiance);
    float3 srgbLow = displayLinear * 12.92;
    float3 srgbHigh = 1.055 * pow(displayLinear, 1.0 / 2.4) - 0.055;
    return float4(lerp(srgbLow, srgbHigh, step(0.0031308, displayLinear)), 1);
}
