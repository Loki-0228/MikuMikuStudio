#pragma once
namespace mikudancestudio::antialias::outline_multisample {
inline constexpr char kShader[] = R"hlsl(
row_major float4x4 transform : register(c0);
float4 lineColor : register(c4);
float4 options : register(c5);
struct LineOut { float4 position : POSITION; float4 color : COLOR0; };
LineOut outlineVertex(float3 position : POSITION0,float4 color : COLOR0) {
    LineOut result;
    result.position=mul(float4(position,1),transform);
    result.color=options.x>0?lineColor:color;
    return result;
}
float4 outlinePixel(float4 color : COLOR0) : COLOR0 { return color; }
sampler2D lineLayer : register(s0);
float4 inverseSize : register(c0);
float4 bounds : register(c1);
float4 softening : register(c2); // x enable width-aware line-layer filtering
float4 readLine(float2 uv) {
    return tex2Dlod(lineLayer,float4(clamp(uv,bounds.xy,bounds.zw),0,0));
}
float lineWidth(float2 uv) {
    float covered=0;
    [unroll] for(int y=-1;y<=1;++y) {
        [unroll] for(int x=-1;x<=1;++x)
            covered+=readLine(uv+float2(x,y)*inverseSize.xy).a;
    }
    // The covered area in a three-pixel-long neighborhood estimates the
    // visible width, including fractional MSAA and authored transparency.
    return covered/3.0;
}
float4 composite(float2 uv : TEXCOORD0) : COLOR0 {
    float4 center=readLine(uv);
    [branch] if(softening.x<0.5) return center;
    float width=lineWidth(uv);
    // Empty line neighborhoods are exactly zero in premultiplied RGBA and
    // every flux term is zero. Avoid the wider searches across the image.
    [branch] if(width==0.0) return center;
    float4 result=center;
    [unroll] for(int y=-1;y<=1;++y) {
        [unroll] for(int x=-1;x<=1;++x) {
            if(x!=0 || y!=0) {
                float2 neighbor=uv+float2(x,y)*inverseSize.xy;
                float4 sample=readLine(neighbor);
                // Pair-symmetric coefficients move equal RGBA amounts in
                // each direction. Thin lines retain weak filtering; thicker
                // lines gain a wider transition without changing base hulls.
                float pairWidth=max(width,lineWidth(neighbor));
                float strength=lerp(0.025,0.085,smoothstep(0.2,1.5,pairWidth));
                if(x!=0 && y!=0)strength*=0.5;
                result+=(sample-center)*strength;
            }
        }
    }
    return result;
}
)hlsl";
}
