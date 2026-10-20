#pragma once
namespace mikumikustudio::antialias {
inline constexpr char kShader[] = R"hlsl(
sampler2D scene : register(s0);
sampler2D history : register(s1);
sampler2D motion : register(s2);
sampler2D previousDepth : register(s3);
float4 pixel : register(c0); // inverse dimensions, history weight, unused
float4 bounds : register(c1); // viewport pixel-center UV bounds
float4 reproject : register(c2); // current jitter UV, absolute/relative depth tolerance
float4 previousSampling : register(c3); // previous depth texture's jitter UV
float4 readScene(float2 uv) { return tex2D(scene,clamp(uv,bounds.xy,bounds.zw)); }
// Actual previous/current geometry positions provide velocity without jitter.
// The history lookup follows that velocity. Linear-depth rejection prevents a
// newly exposed surface from inheriting the occluder's color.
float4 temporal(float2 uv : TEXCOORD0) : COLOR0 {
    // Resolved history lives on a fixed display grid. Raw color and motion
    // instead live on this frame's jittered grid, including rejection paths.
    float2 currentUV=clamp(uv+reproject.xy,bounds.xy,bounds.zw);
    float4 current=readScene(currentUV), low=current, high=current;
    if(pixel.z<=0) return current;
    float4 velocity=tex2D(motion,currentUV);
    float depthSlope=abs(ddx(velocity.w))+abs(ddy(velocity.w));
    if(velocity.z>0 && velocity.w<=0) return current;
    bool uncovered=velocity.z<=0;
    float fixedPriorDepth=tex2D(previousDepth,clamp(uv+previousSampling.xy,bounds.xy,bounds.zw)).z;
    float coverageTolerance=max(reproject.z,abs(fixedPriorDepth)*reproject.w)+
        min(depthSlope*.75,abs(fixedPriorDepth)*.01);
    bool stationaryNeighbor=false;
    [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x) {
        float2 at=clamp(currentUV+float2(x,y)*pixel.xy,bounds.xy,bounds.zw);
        float4 c=readScene(at);low=min(low,c);high=max(high,c);
        // Fill uncovered silhouette pixels without replacing a visible
        // surface's own motion with a nearer, unrelated neighboring surface.
        float4 candidate=tex2D(motion,at);
        stationaryNeighbor=stationaryNeighbor || (candidate.w>0 && all(candidate.xy==0) &&
            fixedPriorDepth>0 && abs(candidate.w-fixedPriorDepth)<=coverageTolerance);
        if(uncovered && candidate.z>0 && (velocity.z<=0 || candidate.z<velocity.z)) velocity=candidate;
    }
    float2 previous=uv+velocity.xy;
    if(velocity.z<=0 || velocity.w<=0 || any(previous<bounds.xy) || any(previous>bounds.zw)) return current;
    float tolerance=max(reproject.z,abs(velocity.w)*reproject.w);
    // A sloping surface changes depth across the subpixel jitter footprint.
    // Bound the allowance so a depth discontinuity cannot admit far occluders.
    tolerance+=min(depthSlope*.75,abs(velocity.w)*.01);
    float2 previousUV=clamp(previous+previousSampling.xy,bounds.xy,bounds.zw);
    float priorDepth=tex2D(previousDepth,previousUV).z;
    float mismatch=abs(priorDepth-velocity.w);
    // A one-pixel depth neighborhood covers changed subpixel coverage without
    // accepting unrelated geometry farther from the predicted previous depth.
    [unroll] for(int dy=-1;dy<=1;++dy) [unroll] for(int dx=-1;dx<=1;++dx) {
        float2 at=clamp(previousUV+float2(dx,dy)*pixel.xy,bounds.xy,bounds.zw);
        float depth=tex2D(previousDepth,at).z;
        if(depth>0) mismatch=min(mismatch,abs(depth-velocity.w));
    }
    // A stationary thin edge can cover background in only one jitter phase.
    // Include background as coverage. A nearer surface is allowed only when a
    // tracked stationary neighbor still owns that depth; moving or vanished
    // occluders reject history. Moving geometry always uses depth validation.
    bool stationaryCoverage=all(velocity.xy==0) &&
        (priorDepth<=0 || priorDepth>=velocity.w-tolerance || stationaryNeighbor);
    if(mismatch>tolerance && !stationaryCoverage) return current;
    float4 old=clamp(tex2D(history,previous),low,high);
    // A still surface can converge longer without trailing moving geometry.
    float weight=length(velocity.xy/pixel.xy)<.05?pixel.z:min(pixel.z,.875);
    return lerp(current,old,weight);
}
)hlsl";
}
