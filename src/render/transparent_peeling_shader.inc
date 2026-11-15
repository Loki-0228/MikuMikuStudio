R"MDSPEEL(

float4 MdsTarget;
bool MdsFirst;
bool MdsMasked;
sampler MdsPrevious : register(s15) = sampler_state {AddressU=Clamp;AddressV=Clamp;MinFilter=Point;MagFilter=Point;MipFilter=None;};
sampler MdsLayer : register(s14) = sampler_state {AddressU=Clamp;AddressV=Clamp;MinFilter=Point;MagFilter=Point;MipFilter=None;};
sampler MdsAccum : register(s13) = sampler_state {AddressU=Clamp;AddressV=Clamp;MinFilter=Point;MagFilter=Point;MipFilter=None;};
sampler MdsCoverage : register(s12) = sampler_state {AddressU=Clamp;AddressV=Clamp;MinFilter=Point;MagFilter=Point;MipFilter=None;};
struct MdsFragment { float4 color:COLOR0;float4 depth:COLOR1;float depthTest:DEPTH; };
MdsFragment MdsFinish(float4 color,float4 position,float id,float2 pixel) {
    clip(color.a > 0 ? 1 : -1);
    float z=position.z/position.w;
    float key=floor(saturate(z)*16777216.0);
    id=round(id);
    float4 previous=tex2D(MdsPrevious,(pixel+.5)*MdsTarget.zw);
    float previousKey=previous.x*16777216.0;
    float previousId=previous.y*16777216.0-1;
    if(!MdsFirst) {
        clip(previous.w-.5);
        clip((key<previousKey || (key==previousKey && id>previousId)) ? 1 : -1);
    }
    MdsFragment result;result.color=color;result.depth=float4(key/16777216.0,(id+1)/16777216.0,0,1);result.depthTest=key/16777216.0;return result;
}

struct MdsBufferShadowOutput {
	float4 Pos      : POSITION;
	float4 ZCalcTex : TEXCOORD0;
	float3 N		: TEXCOORD1;
	float3 Eye		: TEXCOORD2;

float4 PmxColor:COLOR1;float4 MdsPosition:TEXCOORD6;float MdsId:TEXCOORD7;};
MdsBufferShadowOutput MdsBufferShadowVS(float4 Pos : POSITION, float4 Normal : NORMAL,float MdsId:TEXCOORD5,float4 PmxColor:TEXCOORD1) {
BufferShadow_OUTPUT original=BufferShadow_VS(Pos,Normal,PmxColor);MdsBufferShadowOutput result;
result.Pos=original.Pos;
result.ZCalcTex=original.ZCalcTex;
result.N=original.N;
result.Eye=original.Eye;
result.PmxColor=original.PmxColor;result.MdsPosition=original.Pos;result.MdsId=MdsId;return result;
}
MdsFragment MdsBufferShadowPS(float4 ZCalcTex : TEXCOORD0,float3 N : TEXCOORD1,float3 Eye : TEXCOORD2,float4 MdsPosition:TEXCOORD6,float MdsId:TEXCOORD7,float2 pixel:VPOS,float4 PmxColor:COLOR1){return MdsFinish(BufferShadow_PS(ZCalcTex,N,Eye,PmxColor),MdsPosition,MdsId,pixel);}
technique Mds_BufferShadowTec{pass P0{VertexShader=compile vs_3_0 MdsBufferShadowVS();PixelShader=compile ps_3_0 MdsBufferShadowPS();}}

struct MdsBShadowTextureOutput {
	float4 Pos      : POSITION;
	float4 ZCalcTex : TEXCOORD0;
	float2 Tex      : TEXCOORD1;
	float3 N		: TEXCOORD2;
	float3 Eye		: TEXCOORD3;

float4 PmxColor:COLOR1;float4 MdsPosition:TEXCOORD6;float MdsId:TEXCOORD7;};
MdsBShadowTextureOutput MdsBShadowTextureVS(float4 Pos : POSITION, float4 Normal : NORMAL, float2 Tex : TEXCOORD0 ,float MdsId:TEXCOORD5,float4 PmxColor:TEXCOORD1) {
BShadowTexture_OUTPUT original=BShadowTexture_VS(Pos,Normal,Tex,PmxColor);MdsBShadowTextureOutput result;
result.Pos=original.Pos;
result.ZCalcTex=original.ZCalcTex;
result.Tex=original.Tex;
result.N=original.N;
result.Eye=original.Eye;
result.PmxColor=original.PmxColor;result.MdsPosition=original.Pos;result.MdsId=MdsId;return result;
}
MdsFragment MdsBShadowTexturePS(float4 ZCalcTex : TEXCOORD0,float2 Tex : TEXCOORD1,float3 N : TEXCOORD2,float3 Eye : TEXCOORD3,float4 MdsPosition:TEXCOORD6,float MdsId:TEXCOORD7,float2 pixel:VPOS,float4 PmxColor:COLOR1){return MdsFinish(BShadowTexture_PS(ZCalcTex,Tex,N,Eye,PmxColor),MdsPosition,MdsId,pixel);}
technique Mds_BShadowTextureTec{pass P0{VertexShader=compile vs_3_0 MdsBShadowTextureVS();PixelShader=compile ps_3_0 MdsBShadowTexturePS();}}

struct MdsBShadowSphiaOutput {
	float4 Pos      : POSITION;
	float4 ZCalcTex : TEXCOORD0;
	float2 Tex      : TEXCOORD1;
	float3 N		: TEXCOORD2;
	float3 Eye		: TEXCOORD3;

float4 PmxColor:COLOR1;float4 MdsPosition:TEXCOORD6;float MdsId:TEXCOORD7;};
MdsBShadowSphiaOutput MdsBShadowSphiaVS(float4 Pos : POSITION, float4 Normal : NORMAL,float MdsId:TEXCOORD5,float4 PmxColor:TEXCOORD1) {
BShadowTexture_OUTPUT original=BShadowSphia_VS(Pos,Normal,PmxColor);MdsBShadowSphiaOutput result;
result.Pos=original.Pos;
result.ZCalcTex=original.ZCalcTex;
result.Tex=original.Tex;
result.N=original.N;
result.Eye=original.Eye;
result.PmxColor=original.PmxColor;result.MdsPosition=original.Pos;result.MdsId=MdsId;return result;
}
MdsFragment MdsBShadowSphiaPS(float4 ZCalcTex : TEXCOORD0,float2 Tex : TEXCOORD1,float3 N : TEXCOORD2,float3 Eye : TEXCOORD3,float4 MdsPosition:TEXCOORD6,float MdsId:TEXCOORD7,float2 pixel:VPOS,float4 PmxColor:COLOR1){return MdsFinish(BShadowSphia_PS(ZCalcTex,Tex,N,Eye,PmxColor),MdsPosition,MdsId,pixel);}
technique Mds_BShadowSphiaTec{pass P0{VertexShader=compile vs_3_0 MdsBShadowSphiaVS();PixelShader=compile ps_3_0 MdsBShadowSphiaPS();}}

struct MdsBShadowSphiaTextureOutput {
	float4 Pos      : POSITION;
	float4 ZCalcTex : TEXCOORD0;
	float2 Tex      : TEXCOORD1;
	float2 SpTex	: TEXCOORD2;
	float3 N		: TEXCOORD3;
	float3 Eye		: TEXCOORD4;

float4 PmxColor:COLOR1;float4 MdsPosition:TEXCOORD6;float MdsId:TEXCOORD7;};
MdsBShadowSphiaTextureOutput MdsBShadowSphiaTextureVS(float4 Pos : POSITION, float4 Normal : NORMAL, float2 Tex : TEXCOORD0 ,float MdsId:TEXCOORD5,float4 PmxColor:TEXCOORD1) {
BShadowSphiaTexture_OUTPUT original=BShadowSphiaTexture_VS(Pos,Normal,Tex,PmxColor);MdsBShadowSphiaTextureOutput result;
result.Pos=original.Pos;
result.ZCalcTex=original.ZCalcTex;
result.Tex=original.Tex;
result.SpTex=original.SpTex;
result.N=original.N;
result.Eye=original.Eye;
result.PmxColor=original.PmxColor;result.MdsPosition=original.Pos;result.MdsId=MdsId;return result;
}
MdsFragment MdsBShadowSphiaTexturePS(float4 ZCalcTex : TEXCOORD0,float2 Tex : TEXCOORD1,float2 SpTex : TEXCOORD2,float3 N : TEXCOORD3,float3 Eye : TEXCOORD4,float4 MdsPosition:TEXCOORD6,float MdsId:TEXCOORD7,float2 pixel:VPOS,float4 PmxColor:COLOR1){return MdsFinish(BShadowSphiaTexture_PS(ZCalcTex,Tex,SpTex,N,Eye,PmxColor),MdsPosition,MdsId,pixel);}
technique Mds_BShadowSphiaTextureTec{pass P0{VertexShader=compile vs_3_0 MdsBShadowSphiaTextureVS();PixelShader=compile ps_3_0 MdsBShadowSphiaTexturePS();}}

struct MdsBShadowTexCd2Output {
	float4 Pos      : POSITION;
	float4 ZCalcTex : TEXCOORD0;
	float2 Tex      : TEXCOORD1;
	float3 N		: TEXCOORD2;
	float3 Eye		: TEXCOORD3;

float4 PmxColor:COLOR1;float4 MdsPosition:TEXCOORD6;float MdsId:TEXCOORD7;};
MdsBShadowTexCd2Output MdsBShadowTexCd2VS(float4 Pos : POSITION, float4 Normal : NORMAL, float4 Tex : TEXCOORD1,float MdsId:TEXCOORD5) {
BShadowTexture_OUTPUT original=BShadowTexCd2_VS(Pos,Normal,Tex);MdsBShadowTexCd2Output result;
result.Pos=original.Pos;
result.ZCalcTex=original.ZCalcTex;
result.Tex=original.Tex;
result.N=original.N;
result.Eye=original.Eye;
result.PmxColor=original.PmxColor;result.MdsPosition=original.Pos;result.MdsId=MdsId;return result;
}
MdsFragment MdsBShadowTexCd2PS(float4 ZCalcTex : TEXCOORD0,float2 Tex : TEXCOORD1,float3 N : TEXCOORD2,float3 Eye : TEXCOORD3,float4 MdsPosition:TEXCOORD6,float MdsId:TEXCOORD7,float2 pixel:VPOS,float4 PmxColor:COLOR1){return MdsFinish(BShadowTexCd2_PS(ZCalcTex,Tex,N,Eye,PmxColor),MdsPosition,MdsId,pixel);}
technique Mds_BShadowTexCd2Tec{pass P0{VertexShader=compile vs_3_0 MdsBShadowTexCd2VS();PixelShader=compile ps_3_0 MdsBShadowTexCd2PS();}}

struct MdsBShadowTextureTexCd2TecOutput {
	float4 Pos      : POSITION;
	float4 ZCalcTex : TEXCOORD0;
	float2 Tex      : TEXCOORD1;
	float2 SpTex	: TEXCOORD2;
	float3 N		: TEXCOORD3;
	float3 Eye		: TEXCOORD4;

float4 PmxColor:COLOR1;float4 MdsPosition:TEXCOORD6;float MdsId:TEXCOORD7;};
MdsBShadowTextureTexCd2TecOutput MdsBShadowTextureTexCd2TecVS(float4 Pos : POSITION, float4 Normal : NORMAL, float2 Tex : TEXCOORD0, float4 Tex2 : TEXCOORD1 ,float MdsId:TEXCOORD5) {
BShadowSphiaTexture_OUTPUT original=BShadowTextureTexCd2Tec_VS(Pos,Normal,Tex,Tex2);MdsBShadowTextureTexCd2TecOutput result;
result.Pos=original.Pos;
result.ZCalcTex=original.ZCalcTex;
result.Tex=original.Tex;
result.SpTex=original.SpTex;
result.N=original.N;
result.Eye=original.Eye;
result.PmxColor=original.PmxColor;result.MdsPosition=original.Pos;result.MdsId=MdsId;return result;
}
MdsFragment MdsBShadowTextureTexCd2TecPS(float4 ZCalcTex : TEXCOORD0,float2 Tex : TEXCOORD1,float2 SpTex : TEXCOORD2,float3 N : TEXCOORD3,float3 Eye : TEXCOORD4,float4 MdsPosition:TEXCOORD6,float MdsId:TEXCOORD7,float2 pixel:VPOS,float4 PmxColor:COLOR1){return MdsFinish(BShadowTextureTexCd2Tec_PS(ZCalcTex,Tex,SpTex,N,Eye,PmxColor),MdsPosition,MdsId,pixel);}
technique Mds_BShadowTextureTexCd2Tec{pass P0{VertexShader=compile vs_3_0 MdsBShadowTextureTexCd2TecVS();PixelShader=compile ps_3_0 MdsBShadowTextureTexCd2TecPS();}}

// Ping-pong accumulation without hardware blending: the previous group is
// copied first, so pixels this layer drops keep their accumulated colour and
// nearest depth, and the source-over blend is then computed by hand with
// blending disabled.  COLOR0 carries premultiplied RGB plus accumulated alpha,
// COLOR1 carries the nearest valid fragment depth with alpha=1, and oDepth
// carries the layer depth so the pass can depth-test against the read-only
// opaque depth buffer.
struct MdsAccumCopyOutput {float4 color:COLOR0;float4 depth:COLOR1;};
MdsAccumCopyOutput MdsAccumCopyPS(float2 uv:TEXCOORD0){
 MdsAccumCopyOutput result;result.color=tex2D(MdsLayer,uv);result.depth=tex2D(MdsPrevious,uv);return result;
}
technique MdsAccumCopy {pass P0 {VertexShader=NULL;PixelShader=compile ps_3_0 MdsAccumCopyPS();}}

struct MdsAccumulateOutput {float4 color:COLOR0;float4 depth:COLOR1;float depthTest:DEPTH;};
MdsAccumulateOutput MdsAccumulatePS(float2 uv:TEXCOORD0){
 float4 depth=tex2D(MdsPrevious,uv);clip(depth.w-.5);
 float coverage=MdsMasked ? tex2D(MdsCoverage,uv).r : 1.0;
 clip(coverage-0.01);
 float4 layer=tex2D(MdsLayer,uv);
 float4 previous=tex2D(MdsAccum,uv);
 float alpha=layer.a*coverage;
 MdsAccumulateOutput result;
 result.color=float4(layer.rgb*alpha+previous.rgb*(1-alpha),alpha+previous.a*(1-alpha));
 result.depth=float4(depth.x,0,0,1);result.depthTest=depth.x;
 return result;
}
technique MdsAccumulate {pass P0 {VertexShader=NULL;PixelShader=compile ps_3_0 MdsAccumulatePS();}}

// Occlusion coverage for a multisampled opaque depth buffer.  The mask target
// uses the depth buffer's sample type, so the per-sample LESSEQUAL test leaves
// one sample write per surviving sample and the resolve turns that into the
// occluded fraction of the pixel.  Only that fraction is quantised; the layer
// colour and the whole accumulation stay FP32.
struct MdsMaskOutput {float4 mask:COLOR0;float depthTest:DEPTH;};
MdsMaskOutput MdsMaskPS(float2 uv:TEXCOORD0){
 float4 depth=tex2D(MdsPrevious,uv);clip(depth.w-.5);
 MdsMaskOutput result;result.mask=1;result.depthTest=depth.x;return result;
}
technique MdsMask {pass P0 {VertexShader=NULL;PixelShader=compile ps_3_0 MdsMaskPS();}}

// Single commit of the accumulated premultiplied colour into the original
// render target; oDepth restores the nearest valid fragment depth.
struct MdsCommitOutput {float4 color:COLOR0;float depth:DEPTH;};
MdsCommitOutput MdsCommitPS(float2 uv:TEXCOORD0){
 float4 depth=tex2D(MdsPrevious,uv);clip(depth.w-.5);
 MdsCommitOutput result;result.color=tex2D(MdsLayer,uv);result.depth=depth.x;return result;
}
technique MdsCommit {pass P0 {VertexShader=NULL;PixelShader=compile ps_3_0 MdsCommitPS();}}
)MDSPEEL"
