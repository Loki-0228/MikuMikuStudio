#pragma once
namespace mikumikustudio::antialias {
inline constexpr char kOutlineCoverageShader[] = R"hlsl(
row_major float4x4 transform : register(c0);
float4 viewport : register(c4); // half viewport dimensions
float4 edgeColor : register(c5);
float4 options : register(c6); // use uniform color
struct Output { float4 position : POSITION; float4 color : COLOR0; };
Output outlineVertex(float3 edge : POSITION0, float4 color : COLOR0,
                     float3 body : POSITION1, float3 normal : NORMAL0) {
    Output result;
    result.position = mul(float4(edge, 1), transform);
    result.color = options.x > 0 ? edgeColor : color;
    float4 origin = mul(float4(body, 1), transform);
    // Disabled PMD/PMX edges are inset, not extruded. Do not turn them into
    // visible outlines, or expand vertices across the camera plane.
    if (origin.w > 1e-5 && result.position.w > 1e-5 && dot(edge-body, normal) > 0) {
        float2 delta = result.position.xy / result.position.w - origin.xy / origin.w;
        float width = length(delta * viewport.xy);
        if (width > 1e-5 && width < 1) {
            // Supply spatial AA with continuous subpixel coverage, preserving
            // the authored amount of ink. An opaque minimum-width hull made
            // FXAA/TAA/SMAA visibly thicker than the original MSAA outlines.
            result.position.xy += delta * (1 / width - 1) * result.position.w;
            result.color.a *= width;
        }
    }
    return result;
}
float4 outlinePixel(float4 color : COLOR0) : COLOR0 { return color; }
struct MaskOutput { float4 color : COLOR0; float4 coverage : COLOR1; };
MaskOutput outlineMaskPixel(float4 color : COLOR0) {
    MaskOutput result;
    // COLOR0 is identical to the ordinary outline shader, including coverage
    // alpha. Shared SRCALPHA/INVSRCALPHA blending accumulates coverage in R of
    // the separate MRT; the existing scene depth test rejects occluded hulls.
    result.color = color;
    result.coverage = float4(1, 0, 0, color.a);
    return result;
}
)hlsl";
}
