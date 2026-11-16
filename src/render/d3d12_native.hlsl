struct Vertex {
    float3 position,normal;float2 uv;float4 additionalUv[4];float3 edgePosition;uint edgeColor;
};
StructuredBuffer<Vertex> vertices : register(t0);
StructuredBuffer<Vertex> previousVertices : register(t4);
Texture2D diffuseMap : register(t1);
Texture2D sphereMap : register(t2);
Texture2D toonMap : register(t3);
SamplerState linearWrap : register(s0);
SamplerState linearClamp : register(s1);
cbuffer Draw : register(b0) {
    row_major float4x4 worldViewProjection,worldView;
    float4 lightDirection,lightColor,diffuse,ambient,specular,camera,flags;
    row_major float4x4 previousWorldViewProjection,motionWorldViewProjection,previousWorldView;
};
struct Interpolated {
    float4 position:SV_Position;
    float3 normal:NORMAL;float3 eye:TEXCOORD2;
    float2 uv:TEXCOORD0;float2 additionalUv:TEXCOORD1;
    float4 edgeColor:COLOR0;
    float4 currentClip:TEXCOORD3,previousClip:TEXCOORD4;
    float2 linearDepth:TEXCOORD5;
};
Interpolated vs(uint index:SV_VertexID) {
    Vertex v=vertices[index];Interpolated o;
    float3 p=flags.x!=0 ? v.edgePosition : v.position;
    o.position=mul(float4(p,1),worldViewProjection);
    o.currentClip=mul(float4(p,1),motionWorldViewProjection);
    Vertex old=previousVertices[index];
    float3 oldPosition=flags.x!=0?old.edgePosition:old.position;
    o.previousClip=mul(float4(oldPosition,1),previousWorldViewProjection);
    o.linearDepth=float2(mul(float4(p,1),worldView).z,mul(float4(oldPosition,1),previousWorldView).z);
    o.normal=mul(float4(v.normal,0),worldView).xyz;
    o.eye=-mul(float4(p,1),worldView).xyz;
    o.uv=v.uv;o.additionalUv=v.additionalUv[0].xy;
    o.edgeColor=float4((v.edgeColor>>16)&255,(v.edgeColor>>8)&255,v.edgeColor&255,v.edgeColor>>24)/255.;
    return o;
}
struct Pixel {float4 color:SV_Target0;float4 motion:SV_Target1;};
Pixel result(Interpolated i,float4 color) {
    Pixel o;o.color=color;o.motion=float4(0,0,max(i.linearDepth.x,1e-6),0);
    if(flags.z!=0&&i.currentClip.w>0&&i.previousClip.w>0) {
        float3 current=i.currentClip.xyz/i.currentClip.w,previous=i.previousClip.xyz/i.previousClip.w;
        o.motion=float4((previous.xy-current.xy)*float2(.5,-.5),max(i.linearDepth.x,1e-6),max(i.linearDepth.y,1e-6));
    }
    return o;
}
Pixel ps(Interpolated i) {
    if(flags.x!=0) {clip(i.edgeColor.a-1./255.);return result(i,i.edgeColor);}
    float3 normal=normalize(i.normal);
    float3 direction=normalize(mul(float4(lightDirection.xyz,0),worldView).xyz);
    float amount=dot(normal,-direction)*.5+.5;
    float3 toon=toonMap.Sample(linearClamp,float2(.5,1-saturate(amount))).rgb;
    float4 base=diffuseMap.Sample(linearWrap,i.uv)*diffuse;
    float3 color=saturate((diffuse.rgb+ambient.rgb)*lightColor.rgb)*toon;
    color*=diffuseMap.Sample(linearWrap,i.uv).rgb;
    if(flags.y!=0) {
        float2 uv=flags.y==3 ? i.additionalUv : float2(normal.x*.5+.5,.5-normal.y*.5);
        float3 sphere=sphereMap.Sample(linearWrap,uv).rgb;
        color=flags.y==2 ? color+sphere : color*sphere;
    }
    color+=specular.rgb*lightColor.rgb*pow(saturate(dot(normal,normalize(-direction+normalize(i.eye)))),max(specular.w,.0001));
    clip(base.a-1./255.); return result(i,float4(color,base.a));
}
