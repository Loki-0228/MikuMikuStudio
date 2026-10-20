#pragma once
namespace mikumikustudio::antialias {
// Official MEDIUM/HIGH presets and T2x, with color edges for MMD's colored lines.
// The algorithm and lookup data retain their license in third_party.
inline constexpr char kSmaaShader[] = R"hlsl(
float4 smaaMetrics : register(c0);
float4 smaaBounds : register(c1);
float4 smaaSubsamples : register(c2);
float4 smaaTemporalState : register(c3); // valid history, absolute/relative depth tolerance
float4 smaaOutputOffset : register(c4); // undo jitter only after motion capture falls back
float4 smaaSampleOffsets : register(c5); // current and previous raw texture jitter UV
#define SMAA_RT_METRICS smaaMetrics
#define SMAA_HLSL_3 1
#ifndef MMS_SMAA_ENHANCED
#define MMS_SMAA_ENHANCED 1
#endif
#ifndef MMS_SMAA_TEMPORAL
#define MMS_SMAA_TEMPORAL 0
#endif
#if MMS_SMAA_ENHANCED
#define SMAA_PRESET_HIGH 1
#else
#define SMAA_PRESET_MEDIUM 1
#endif
#define SMAA_REPROJECTION MMS_SMAA_TEMPORAL
// The motion buffer stores previous-current, opposite to the upstream convention.
#define SMAA_DECODE_VELOCITY(sample) (-(sample).rg)
#define SMAA_AREATEX_SELECT(sample) sample.rg
)hlsl"
#include "smaa_reference.hpp"
R"hlsl(
sampler2D smaaInput : register(s0);
sampler2D smaaSecondary : register(s1);
sampler2D smaaSearch : register(s2);
sampler2D smaaPreviousDepth : register(s3);
sampler2D smaaOriginal : register(s4);
sampler2D smaaBlendWeights : register(s5);
float4 smaaEdges(float2 uv : TEXCOORD0) : COLOR0 {
    float4 offsets[3];
    SMAAEdgeDetectionVS(uv,offsets);
    // Only the scene viewport is initialized in the full-size render target.
    [unroll] for(int i=0;i<3;++i) {
        offsets[i].xy=clamp(offsets[i].xy,smaaBounds.xy,smaaBounds.zw);
        offsets[i].zw=clamp(offsets[i].zw,smaaBounds.xy,smaaBounds.zw);
    }
    return float4(SMAAColorEdgeDetectionPS(uv,offsets,smaaInput),0,0);
}
float4 smaaWeights(float2 uv : TEXCOORD0) : COLOR0 {
    float2 pixel;float4 offsets[3];
    SMAABlendingWeightCalculationVS(uv,pixel,offsets);
    return SMAABlendingWeightCalculationPS(uv,pixel,offsets,smaaInput,smaaSecondary,smaaSearch,smaaSubsamples);
}
float4 smaaResolve(float2 uv : TEXCOORD0) : COLOR0 {
    uv=clamp(uv+smaaOutputOffset.xy,smaaBounds.xy,smaaBounds.zw);
    // Preserve the outermost scene pixels; never blend undefined editor pixels
    // into an inset viewport or into a cropped/export render target.
    if(any(uv<=smaaBounds.xy+smaaMetrics.xy*.01) || any(uv>=smaaBounds.zw-smaaMetrics.xy*.01)) {
        float4 color=tex2D(smaaInput,clamp(uv,smaaBounds.xy,smaaBounds.zw));
        #if MMS_SMAA_TEMPORAL
        color.a=sqrt(5.0*length(tex2D(smaaSearch,uv).rg));
        #endif
        return color;
    }
    float4 offset;SMAANeighborhoodBlendingVS(uv,offset);
    return SMAANeighborhoodBlendingPS(uv,offset,smaaInput,smaaSecondary
        #if MMS_SMAA_TEMPORAL
        ,smaaSearch
        #endif
    );
}
#if MMS_SMAA_TEMPORAL
// Upstream packs velocity into neighborhood alpha for its temporal rejection.
// Recover the current spatially filtered alpha rather than exposing that packing
// to transparent image/video outputs.
float smaaOutputAlpha(float2 uv) {
    float4 offset;SMAANeighborhoodBlendingVS(uv,offset);
    float4 a;
    a.x=tex2D(smaaBlendWeights,offset.xy).a;
    a.y=tex2D(smaaBlendWeights,offset.zw).g;
    a.wz=tex2D(smaaBlendWeights,uv).xz;
    if(dot(a,1)<1e-5 || any(uv<=smaaBounds.xy+smaaMetrics.xy*.01) || any(uv>=smaaBounds.zw-smaaMetrics.xy*.01))
        return tex2D(smaaOriginal,uv).a;
    bool horizontal=max(a.x,a.z)>max(a.y,a.w);
    float2 weight=horizontal?a.xz:a.yw;
    weight/=dot(weight,1);
    float4 blend=horizontal?float4(a.x,0,a.z,0):float4(0,a.y,0,a.w);
    float4 at=mad(blend,float4(smaaMetrics.xy,-smaaMetrics.xy),uv.xyxy);
    return weight.x*tex2D(smaaOriginal,clamp(at.xy,smaaBounds.xy,smaaBounds.zw)).a+
        weight.y*tex2D(smaaOriginal,clamp(at.zw,smaaBounds.xy,smaaBounds.zw)).a;
}
float4 smaaTemporal(float2 uv : TEXCOORD0) : COLOR0 {
    float2 currentUV=clamp(uv+smaaSampleOffsets.xy,smaaBounds.xy,smaaBounds.zw);
    float4 packed=tex2D(smaaInput,currentUV);
    float4 current=packed;
    float alpha=smaaOutputAlpha(currentUV);
    current.a=alpha;
    if(smaaTemporalState.x<=0)return current;
    float4 velocity=tex2D(smaaSearch,currentUV);
    float depthSlope=abs(ddx(velocity.w))+abs(ddy(velocity.w));
    if(velocity.z>0 && velocity.w<=0)return current;
    bool uncovered=velocity.z<=0;
    float fixedPriorDepth=tex2D(smaaPreviousDepth,clamp(uv+smaaSampleOffsets.zw,smaaBounds.xy,smaaBounds.zw)).z;
    float coverageTolerance=max(smaaTemporalState.y,abs(fixedPriorDepth)*smaaTemporalState.z)+
        min(depthSlope*.75,abs(fixedPriorDepth)*.01);
    bool stationaryNeighbor=false;
    // A phase may uncover an edge pixel. Borrow only tracked neighboring
    // geometry, never another object's history through a transparent fragment.
    [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x) {
        float2 at=clamp(currentUV+float2(x,y)*smaaMetrics.xy,smaaBounds.xy,smaaBounds.zw);
        float4 candidate=tex2D(smaaSearch,at);
        stationaryNeighbor=stationaryNeighbor || (candidate.w>0 && all(candidate.xy==0) &&
            fixedPriorDepth>0 && abs(candidate.w-fixedPriorDepth)<=coverageTolerance);
        if(uncovered && candidate.z>0 && (velocity.z<=0 || candidate.z<velocity.z))velocity=candidate;
    }
    // The native-line prefilter plus reconstruction can reach two pixels
    // beyond the tracked hull. Match this fringe across the two phases too,
    // while retaining the ordinary neighborhood for visible geometry.
    if(uncovered && velocity.z<=0) {
        [unroll] for(int y=-2;y<=2;++y) [unroll] for(int x=-2;x<=2;++x) {
            if(abs(x)==2 || abs(y)==2) {
                float2 at=clamp(currentUV+float2(x,y)*smaaMetrics.xy,smaaBounds.xy,smaaBounds.zw);
                float4 candidate=tex2D(smaaSearch,at);
                if(candidate.z>0 && (velocity.z<=0 || candidate.z<velocity.z))velocity=candidate;
            }
        }
    }
    float2 previous=uv+velocity.xy+smaaSampleOffsets.zw;
    if(velocity.z<=0 || velocity.w<=0 || any(previous<smaaBounds.xy) || any(previous>smaaBounds.zw))return current;
    float tolerance=max(smaaTemporalState.y,abs(velocity.w)*smaaTemporalState.z);
    tolerance+=min(depthSlope*.75,abs(velocity.w)*.01);
    float priorDepth=tex2D(smaaPreviousDepth,previous).z;
    float mismatch=abs(priorDepth-velocity.w);
    [unroll] for(int dy=-1;dy<=1;++dy) [unroll] for(int dx=-1;dx<=1;++dx) {
        float2 at=clamp(previous+float2(dx,dy)*smaaMetrics.xy,smaaBounds.xy,smaaBounds.zw);
        float depth=tex2D(smaaPreviousDepth,at).z;
        if(depth>0)mismatch=min(mismatch,abs(depth-velocity.w));
    }
    // The other phase may see background behind a stationary subpixel edge.
    // A nearer sample needs a still-visible stationary neighbor at its depth.
    // Moving or vanished occluders still reject history.
    bool stationaryCoverage=all(velocity.xy==0) &&
        (priorDepth<=0 || priorDepth>=velocity.w-tolerance || stationaryNeighbor);
    if(mismatch>tolerance && !stationaryCoverage)return current;
    // Keep upstream's two-frame blend and velocity-change weight, with both
    // raw neighborhood images aligned to the fixed display pixel. History is
    // the previous neighborhood result, never the recursively resolved result.
    float4 old=tex2D(smaaSecondary,previous);
    float delta=abs(packed.a*packed.a-old.a*old.a)/5.0;
    float weight=0.5*saturate(1.0-sqrt(delta)*SMAA_REPROJECTION_WEIGHT_SCALE);
    float4 result=lerp(packed,old,weight);
    result.a=alpha;
    return result;
}
#endif
)hlsl";
}
