///////////// メインプログラム側から引き渡される変数 /////////////////
float4x4 matLightViewProj;	// 光方向ビュー射影変換済み行列
float4x4 matWorldViewProj;	// ワールドビュー射影変換済み行列
float4x4 matWorld;			// ワールド座標のみ行列
float4x4 matRotate;			// 回転のみ行列
float4x4 matWRotate;		//
float4	 EgColor;			// エッジ色
float4	 ToonColor;			// トゥーン色
float4   LightDir;			// 光方向
float4   SpcColor;			// スペキュラ色
float4   Place;				// カメラの位置
float4   DifColor;			// ディフューズ色
float4   TexCAdd;			// テクスチャパレット加算値
float4   TexCMul;			// テクスチャパレット乗算値
float4   SphCAdd;			// スフィアテクスチャパレット値加算値
float4   SphCMul;			// スフィアテクスチャパレット値乗算値
float4   MatDifColor;
float4   MatAmbColor;
float4   MatEmsColor;
float4   MatSpcColor;
bool	 parthf;			// パースペクティブフラグ
bool	 spadd;				// スフィアマップ加算合成フラグ
bool	 transp;			// 半透明フラグ


///////////// 定数 ////////////////
#define SKII1	1500
#define	SKII2	8000
#define	Toon	3

///////////// 単色塗り潰しテクニック ////////////////

// 頂点シェーダ
float4 ColorRender_VS(float4 Pos : POSITION) : POSITION
{
	return mul( Pos, matWorldViewProj );
}

// ピクセルシェーダ
float4 ColorRender_PS() : COLOR
{
	// EgColor色で塗り潰し
	return float4(EgColor.r,EgColor.g,EgColor.b,EgColor.a);
}

technique ColorRenderTec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;

		VertexShader = compile vs_3_0 ColorRender_VS();
		PixelShader  = compile ps_3_0 ColorRender_PS();
	}
}


///////////// Zバッファプロットテクニック ////////////////

struct VS_ZValuePlot_OUTPUT
{
	float4 Pos : POSITION;				// 射影変換座標
	float4 ShadowMapTex : TEXCOORD0;	// Zバッファテクスチャ
};

// 頂点シェーダ
VS_ZValuePlot_OUTPUT ZValuePlot_VS( float4 Pos : POSITION )
{
	VS_ZValuePlot_OUTPUT Out = (VS_ZValuePlot_OUTPUT)0;

	// ライトの目線によるワールドビュー射影変換をする
	Out.Pos = mul( Pos, matLightViewProj );

	// テクスチャ座標を頂点に合わせる
	Out.ShadowMapTex = Out.Pos;

	return Out;
}


// ピクセルシェーダ
float4 ZValuePlot_PS( float4 ShadowMapTex : TEXCOORD0 ) : COLOR
{
	return float4(ShadowMapTex.z/ShadowMapTex.w,0,0,1);
}

technique ZValuePlotTec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;
		AlphaBlendEnable = FALSE;
	
		VertexShader = compile vs_3_0 ZValuePlot_VS();
		PixelShader  = compile ps_3_0 ZValuePlot_PS();
	}
}

///////////// 分散光なしテクスチャなしバッファシャドウテクニック ////////////////

// MME effects may inherit sampler 0 without declaring filter states.
// Preserve the native linear-depth sampling for both built-in and MME materials.
sampler DefSampler = sampler_state	// サンプラーステート
{
	AddressU  = Clamp;
	AddressV  = Clamp;
	AddressW  = Clamp;
	MINFILTER = LINEAR;
	MAGFILTER = LINEAR;
	MIPFILTER = NONE;
};

// Optional cubic depth reconstruction uses the actual shadow-map size.
float4 MdsShadowMapSize = float4(2048,2048,0,0);
bool MdsShadowFilterEnhanced = false;

float MdsSplineShadowDepth(float2 uv)
{
    // Nonnegative cubic B-spline. Four hardware-linear lookups reconstruct
    // a separable 4x4 depth kernel before the original continuous ramp.
    float2 size = MdsShadowMapSize.xy;
    float2 grid = uv * size - 0.5f;
    float2 cell = floor(grid), f = grid - cell;
    float2 f2 = f * f, f3 = f2 * f;
    float2 w0 = (1 - 3*f + 3*f2 - f3) / 6;
    float2 w1 = (4 - 6*f2 + 3*f3) / 6;
    float2 w2 = (1 + 3*f + 3*f2 - 3*f3) / 6;
    float2 w3 = f3 / 6;
    float2 g0 = w0 + w1, g1 = w2 + w3;
    float2 h0 = (cell - 0.5f + w1/g0) / size;
    float2 h1 = (cell + 1.5f + w3/g1) / size;
    return tex2Dlod(DefSampler, float4(h0.x,h0.y,0,0)).r * g0.x*g0.y
         + tex2Dlod(DefSampler, float4(h1.x,h0.y,0,0)).r * g1.x*g0.y
         + tex2Dlod(DefSampler, float4(h0.x,h1.y,0,0)).r * g0.x*g1.y
         + tex2Dlod(DefSampler, float4(h1.x,h1.y,0,0)).r * g1.x*g1.y;
}


float MdsCubicWeight(float coordinate)
{
    float x=abs(coordinate);
    float t=max(2.0f-x,0.0f);
    return x<1.0f ? (4.0f-6.0f*x*x+3.0f*x*x*x)/6.0f : t*t*t/6.0f;
}

float MdsBaseSplineDepth(float2 uv, float2 uvDx, float2 uvDy)
{
    float2 size=MdsShadowMapSize.xy;
    // Define the reconstruction footprint in UV, then integrate the real
    // allocated texels. Increasing map density increases contributing taps,
    // rather than separating four sparse lobes or enlarging the UV footprint.
    float2 footprintSize=MdsShadowMapSize.z>0.0f ? MdsShadowMapSize.zw : size;
    float2 scale=max(1.240967365f*footprintSize/4096.0f,1.0f)*size/footprintSize;
    if(MdsShadowMapSize.z>0.0f) scale*=0.75f;
    float2 dx=uvDx*size,dy=uvDy*size;
    float pixelLimit=2.0f*min(size.x,size.y)/4096.0f;
    dx*=min(1.0f,pixelLimit/max(length(dx),0.00001f));
    dy*=min(1.0f,pixelLimit/max(length(dy),0.00001f));
    // The bounded 2-pixel antialiasing footprint is independent of the map size.
    scale=sqrt(scale*scale+(dx*dx+dy*dy));
    scale=min(scale,8.0f);
    float2 grid=uv*size-0.5f,cell=floor(grid),fraction=grid-cell;
    float2 reach=ceil(2.0f*scale);
    float sum=0.0f,normalization=0.0f;
    [loop] for(int iy=0;iy<17;++iy) {
        if(iy>reach.y) break;
        float ky=-reach.y+2.0f*iy;
        float wy0=MdsCubicWeight((ky-fraction.y)/scale.y);
        float wy1=MdsCubicWeight((ky+1.0f-fraction.y)/scale.y);
        float wy=wy0+wy1;
        if(wy<=0.00000001f) continue;
        float v=(cell.y+ky+0.5f+wy1/wy)/size.y;
        [loop] for(int ix=0;ix<17;++ix) {
            if(ix>reach.x) break;
            float kx=-reach.x+2.0f*ix;
            float wx0=MdsCubicWeight((kx-fraction.x)/scale.x);
            float wx1=MdsCubicWeight((kx+1.0f-fraction.x)/scale.x);
            float wx=wx0+wx1;
            if(wx<=0.00000001f) continue;
            float u=(cell.x+kx+0.5f+wx1/wx)/size.x;
            float weight=wx*wy;
            sum+=weight*tex2Dlod(DefSampler,float4(u,v,0,0)).r;
            normalization+=weight;
        }
    }
    return sum/max(normalization,0.00000001f);
}


float3 MdsContourDirection(float2 uv, float2 footprint)
{
    // Same physical +/- one-coarse-texel window, sampled as a 7x7 grid.
    // Welford's online centered covariance removes a common affine slope
    // without subtracting two large squared-gradient sums.
    float2 mean=float2(0,0);
    float xx=0.0f,yy=0.0f,xy=0.0f,count=0.0f;
    float2 stride=footprint/3.0f;
    [loop] for(int y=0;y<6;++y) {
        [loop] for(int x=0;x<6;++x) {
            float2 origin=uv-footprint+float2(x,y)*stride;
            float a=tex2Dlod(DefSampler,float4(origin,0,0)).r;
            float b=tex2Dlod(DefSampler,float4(origin+float2(stride.x,0),0,0)).r;
            float c=tex2Dlod(DefSampler,float4(origin+float2(0,stride.y),0,0)).r;
            float d=tex2Dlod(DefSampler,float4(origin+stride,0,0)).r;
            float2 g=0.5f*float2(b-a+d-c,c-a+d-b);
            count+=1.0f;
            float2 delta=g-mean;
            mean+=delta/count;
            float2 centered=g-mean;
            xx+=delta.x*centered.x;
            yy+=delta.y*centered.y;
            xy+=delta.x*centered.y;
        }
    }
    xx=max(xx,0.0f);yy=max(yy,0.0f);
    float energy=xx+yy;
    float gap=sqrt((xx-yy)*(xx-yy)+4.0f*xy*xy);
    float coherence=gap/max(energy,1.0e-18f);
    float confidence=saturate((coherence-0.35f)/0.45f)*saturate((energy-1.0e-12f)/1.0e-10f);
    float2 normal=float2(2.0f*xy,yy-xx+gap);
    if(dot(normal,normal)<1.0e-24f) normal=float2(1,0);
    normal*=rsqrt(max(dot(normal,normal),1.0e-24f));
    return float3(normal,confidence);
}

float MdsAdaptiveSplineDepth(float2 uv, float2 uvDx, float2 uvDy)
{
    if(MdsShadowMapSize.z<=0.0f) return MdsBaseSplineDepth(uv,uvDx,uvDy);
    float2 size=MdsShadowMapSize.xy,coarse=MdsShadowMapSize.zw;
    float3 direction=MdsContourDirection(uv,1.0f/coarse);
    if(direction.z<0.05f) return MdsBaseSplineDepth(uv,uvDx,uvDy);
    float2 tangent=float2(-direction.y,direction.x);
    float base=max(1.240967365f*coarse.x/4096.0f,1.0f)*size.x/coarse.x;
    // Cubic B-spline variance is scale^2/3. The five-point binomial
    // [1,4,6,4,1]/16 has variance step^2. Add only the missing tangent
    // variance between the 0.75-scale v6 kernel and the full-scale v5 kernel.
    float step=base*sqrt((1.0f-0.75f*0.75f)*direction.z/3.0f);
    float sum=0.0f;
    [loop] for(int k=-2;k<=2;++k) {
        float weight=abs(k)==2?1.0f:(k==0?6.0f:4.0f);
        sum+=weight*MdsBaseSplineDepth(uv+tangent*(step*k)/size,uvDx,uvDy);
    }
    return sum/16.0f;
}


// Restore the pre-fit shadow coordinates for the native material ramp.
float MdsShadowDerivativeOffset = 0;
float4x4 MdsShadowToNative = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
float MdsNativeShadowRamp(float3 projected, float depth) {
    float4 receiver=mul(float4(projected,1),MdsShadowToNative);
    float4 caster=mul(float4(projected.xy,depth,1),MdsShadowToNative);
    float difference=max(receiver.z/receiver.w-caster.z/caster.w,0);
    float nativeY=(1-receiver.y/receiver.w)*0.5f;
    return 1-saturate(difference*(parthf ? SKII2*nativeY : SKII1)-0.3f);
}
bool MdsStableShadowGrid = false;
float4 MdsDetailTexelSize = {0,0,0,0};
bool MdsDetailEnabled = false;
float4x4 MdsCoverageToDetail;
float4 MdsDetailDepthBlend = {0,0,0,1};
sampler MdsDetailSampler : register(s3) = sampler_state {
    MinFilter = LINEAR;
    MagFilter = LINEAR;
    MipFilter = NONE;
    AddressU = CLAMP;
    AddressV = CLAMP;
};
float4 MdsNearTexelSize = {0,0,0,0};
bool MdsNearEnabled = false;
bool MdsPreferMiddle = false;
float4x4 MdsCoverageToNear;
float4 MdsNearDepthBlend = {0,0,0,1};
sampler MdsNearSampler : register(s4) = sampler_state {
    MinFilter = LINEAR;
    MagFilter = LINEAR;
    MipFilter = NONE;
    AddressU = CLAMP;
    AddressV = CLAMP;
};
float4 MdsBridgeTexelSize = {0,0,0,0};
bool MdsBridgeEnabled = false;
float4x4 MdsCoverageToBridge = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
float4 MdsBridgeDepthBlend = {0,0,0,1};
// x=middle, y=near, z=bridge; larger rank wins where regions overlap.
float3 MdsDetailRank = {1,2,0};
// x=middle, y=near, z=bridge. Zero preserves the legacy linear test path.
float3 MdsDetailLogBlend = {0,0,0};
float MdsShadowDepthWeight(float linearWeight, float ratio)
{
    // Equal camera zoom ratios advance equally through the transition. The
    // input is already saturated, so endpoints and the near precision survive.
    if(ratio>0.0001f)
        return saturate(log2(max(1-ratio*linearWeight,0.0001f))/log2(1-ratio));
    return linearWeight;
}
sampler MdsBridgeSampler : register(s5) = sampler_state {
    MinFilter = LINEAR;
    MagFilter = LINEAR;
    MipFilter = NONE;
    AddressU = CLAMP;
    AddressV = CLAMP;
};
// Private raw coverage sampler; stage 0 remains the shared MME depth texture.
sampler MdsNativeCoverageSampler : register(s6) = sampler_state {
    MinFilter = LINEAR;
    MagFilter = LINEAR;
    MipFilter = NONE;
    AddressU = CLAMP;
    AddressV = CLAMP;
};
// Three-point visibility convolution followed by bilinear reconstruction.
// Native enhanced receivers reconstruct visibility from raw texels. External
// effects keep the shared depth prefilter and its original three-tap weights.
bool MdsLayerFilterEnhanced = false;
float MdsShadowFilterSideWeight = 0.141951218f;
// Internal regression switch; the UI still uses the enhanced-filter toggle.
bool MdsDirectionalShadowFilter = true;
// Compact support reaches zero at the outer crossing, so moving the 8x8
// window across a texel boundary cannot add or remove a finite contribution.
void MdsAccumulateShadowCrossing(float first, float second, float2 edgePoint,
    float2 axis, float threshold, float inverseRange,
    inout float4 moments, inout float2 moments2, inout float4 signs)
{
    float delta=second-first;
    if((first<threshold && second>=threshold) || (second<threshold && first>=threshold)) {
        float radial=max(0,1-dot(edgePoint,edgePoint)/12.25f);
        float weight=abs(delta)*inverseRange*radial*radial;
        moments+=weight*float4(1,edgePoint.x,edgePoint.y,edgePoint.x*edgePoint.x);
        moments2+=weight*float2(edgePoint.x*edgePoint.y,edgePoint.y*edgePoint.y);
        // A faint opposite edge at the footprint rim must still veto a
        // single-line fit. Broaden only its confidence weight; keep the
        // fitted contour's quartic moments and continuous support unchanged.
        weight=abs(delta)*inverseRange*sqrt(radial);
        signs.xy+=weight*(delta>=0?axis:-axis);
        signs.zw+=weight*axis;
    }
}
// Reconstruct with float weights: hardware linear weights can quantize
// a magnified shadow texel into visible bands before the depth comparison.
float MdsLinearShadowDepth(sampler source,float2 uv,float2 texel) {
    float2 grid=uv/texel-0.5f;
    float2 cell=floor(grid),f=grid-cell;
    float2 a=(cell+0.5f)*texel;
    float x=tex2Dlod(source,float4(a,0,0)).r;
    float y=tex2Dlod(source,float4(a+float2(texel.x,0),0,0)).r;
    float z=tex2Dlod(source,float4(a+float2(0,texel.y),0,0)).r;
    float w=tex2Dlod(source,float4(a+texel,0,0)).r;
    return lerp(lerp(x,y,f.x),lerp(z,w,f.x),f.y);
}
float MdsPlanarFilteredVisibility(sampler source, float3 projected, float2 uv,
    float2 texel, float2 uvDx, float2 uvDy, float2 depthDxDy)
{
    // Compare raw texel centres before reconstruction. Averaging a nearby
    // caster with a distant surface creates depths that do not exist.
    float determinant=uvDx.x*uvDy.y-uvDx.y*uvDy.x;
    float conditioning=max(length(uvDx)*length(uvDy),1.0e-20f);
    float2 gradient=float2(0,0);
    if(abs(determinant)>conditioning*0.0001f && conditioning>1.0e-18f) {
        gradient=float2(depthDxDy.x*uvDy.y-depthDxDy.y*uvDx.y,
            uvDx.x*depthDxDy.y-uvDy.x*depthDxDy.x)/determinant;
    } else {
        return MdsNativeShadowRamp(projected,MdsLinearShadowDepth(source,uv,texel));
    }
    // Never extrapolate a plane across a large fraction of the shared Z range.
    if(!(dot(abs(gradient),2*texel)<0.25f))
        return MdsNativeShadowRamp(projected,MdsLinearShadowDepth(source,uv,texel));
    float2 receiverUV=uv;
    uv=clamp(uv,texel*0.5f,1-texel*0.5f);
    float2 grid=uv/texel-0.5f;
    float2 cell=floor(grid), f=grid-cell;
    float a=MdsShadowFilterSideWeight, b=1-2*a;
    // Three depth taps followed by bilinear reconstruction have four weights
    // per axis. Preserve that footprint, but average native visibility.
    float4 wx=float4(a*(1-f.x),b*(1-f.x)+a*f.x,a*(1-f.x)+b*f.x,a*f.x);
    float4 wy=float4(a*(1-f.y),b*(1-f.y)+a*f.y,a*(1-f.y)+b*f.y,a*f.y);
    float4 receiver=mul(float4(projected,1),MdsShadowToNative);
    float compareScale=(MdsShadowToNative[2][2]-(receiver.z/receiver.w)*MdsShadowToNative[2][3])/receiver.w;
    float compareWarp=MdsShadowToNative[2][3]/receiver.w;
    float strength=parthf ? SKII2*(1-receiver.y/receiver.w)*0.5f : SKII1;
    float visibility=0, squareVisibility=0;
    float minimumVisibility=1, maximumVisibility=0;
    [loop] for(int y=0;y<4;++y) {
        float weightY=y==0?wy.x:(y==1?wy.y:(y==2?wy.z:wy.w));
        [loop] for(int x=0;x<4;++x) {
            float weightX=x==0?wx.x:(x==1?wx.y:(x==2?wx.z:wx.w));
            float2 sampleUV=clamp((cell+float2(x,y)-0.5f)*texel,texel*0.5f,1-texel*0.5f);
            float depth=tex2Dlod(source,float4(sampleUV,0,0)).r;
            // Express the receiver plane at this texel back at the centre,
            // retaining the original mode strength and comparison bias.
            depth-=dot(gradient,sampleUV-receiverUV);
            float gap=projected.z-depth;
            float compared=1-saturate(max(gap*compareScale/(1-gap*compareWarp),0)*strength-0.3f);
            float weight=weightX*weightY;
            visibility+=weight*compared;
            squareVisibility+=weight*compared*compared;
            minimumVisibility=min(minimumVisibility,compared);
            maximumVisibility=max(maximumVisibility,compared);
        }
    }
    [branch] if(!MdsDirectionalShadowFilter) return saturate(visibility);
    float contrast=sqrt(max(squareVisibility-visibility*visibility,0));
    float contrastWeight=smoothstep(0.03f,0.15f,contrast);
    float visibilityRange=maximumVisibility-minimumVisibility;
    // Uniform/weak regions keep the 16-tap result. Only visible silhouettes
    // need the wider neighbourhood; retain the same receiver-plane safeguard.
    [branch] if(!(contrastWeight>0) || !(visibilityRange>1.0e-6f) ||
        !(dot(abs(gradient),4*texel)<0.25f)) return saturate(visibility);
    {
        float2 screenDx=uvDx/texel, screenDy=uvDy/texel;
        float gramX=screenDx.x*screenDx.x+screenDy.x*screenDy.x;
        float gramY=screenDx.y*screenDx.y+screenDy.y*screenDy.y;
        float gramXY=screenDx.x*screenDx.y+screenDy.x*screenDy.y;
        float smallestFootprint2=0.5f*(gramX+gramY-
            sqrt((gramX-gramY)*(gramX-gramY)+4*gramXY*gramXY));
        [branch] if(smallestFootprint2>=2.25f) return saturate(visibility);
    }

    // Join horizontal/vertical threshold crossings into one local contour.
    // Re-read the texel above for vertical crossings. This preserves the 8x8
    // footprint while freeing the row registers for precise depth gradients
    // on ps_3_0. The wider neighbourhood is visited only at visible edges.
    float threshold=0.5f*(minimumVisibility+maximumVisibility);
    float inverseRange=1/visibilityRange;
    float4 moments=0; // weight, weighted x, weighted y, weighted xx
    float2 moments2=0; // weighted xy, weighted yy
    float4 signs=0; // signed horizontal/vertical, absolute horizontal/vertical

    [loop] for(int y=0;y<8;++y) {
        float previousValue=0;
        [loop] for(int x=0;x<8;++x) {
            float2 sampleUV=clamp((cell+float2(x,y)-2.5f)*texel,texel*0.5f,1-texel*0.5f);
            float depth=tex2Dlod(source,float4(sampleUV,0,0)).r;
            depth-=dot(gradient,sampleUV-receiverUV);
            float gap=projected.z-depth;
            float compared=1-saturate(max(gap*compareScale/(1-gap*compareWarp),0)*strength-0.3f);
            if(x>0) MdsAccumulateShadowCrossing(previousValue,compared,
                float2(x-3.5f,y-3)-f,float2(1,0),threshold,inverseRange,
                moments,moments2,signs);
            if(y>0) {
                float2 aboveUV=clamp((cell+float2(x,y-1)-2.5f)*texel,texel*0.5f,1-texel*0.5f);
                float aboveDepth=tex2Dlod(source,float4(aboveUV,0,0)).r-dot(gradient,aboveUV-receiverUV);
                float aboveGap=projected.z-aboveDepth;
                float above=1-saturate(max(aboveGap*compareScale/(1-aboveGap*compareWarp),0)*strength-0.3f);
                MdsAccumulateShadowCrossing(above,compared,
                    float2(x-3,y-3.5f)-f,float2(0,1),threshold,inverseRange,
                    moments,moments2,signs);
            }
            previousValue=compared;

        }
    }
    [branch] if(!(moments.x>1.0e-6f)) return saturate(visibility);
    float2 center=moments.yz/moments.x;
    float xx=max(moments.w/moments.x-center.x*center.x,0);
    float yy=max(moments2.y/moments.x-center.y*center.y,0);
    float xy=moments2.x/moments.x-center.x*center.y;
    float spread=sqrt((xx-yy)*(xx-yy)+4*xy*xy);
    float large=0.5f*(xx+yy+spread), small=max(0,0.5f*(xx+yy-spread));
    [branch] if(!(large>1.0e-6f)) return saturate(visibility);
    // Stable minor eigenvector: select the component using the larger
    // diagonal to avoid cancellation on exactly horizontal/vertical contours.
    float2 edgeNormal=xx>yy?float2(xy,small-xx):float2(small-yy,xy);
    float normalLength=length(edgeNormal);
    [branch] if(!(normalLength>1.0e-6f)) return saturate(visibility);
    edgeNormal/=normalLength;
    if(dot(edgeNormal,signs.xy)<0) edgeNormal=-edgeNormal;
    float coherence=abs(dot(edgeNormal,signs.xy))/max(dot(abs(edgeNormal),signs.zw),1.0e-6f);
    float normalTexelsPerPixel=length(float2(dot(uvDx/texel,edgeNormal),dot(uvDy/texel,edgeNormal)));
    float confidence=smoothstep(0.9f,0.995f,coherence)*
        (1-smoothstep(0.04f,0.14f,small/large))*
        smoothstep(0.1f,0.3f,moments.x)*smoothstep(0.05f,0.2f,large)*
        contrastWeight*(1-smoothstep(0.75f,1.5f,normalTexelsPerPixel));
    float signedDistance=-dot(center,edgeNormal);
    float reconstructed=lerp(minimumVisibility,maximumVisibility,
        smoothstep(-0.65f,0.65f,signedDistance));
    return saturate(lerp(visibility,reconstructed,confidence));
}

// Only extrapolate a tangent plane across a locally flat receiver. Measure
// normal change per shadow texel, so coarse grids cannot extend individual
// face triangles across an entire character. Planar walls keep their correction.
float MdsLayerFilteredVisibility(sampler source, float3 projected, float2 uv,
    float2 texel, float2 uvDx, float2 uvDy, float2 depthDxDy, float normalVariation)
{
    float determinant=abs(uvDx.x*uvDy.y-uvDx.y*uvDy.x);
    float footprint=max(length(uvDx),length(uvDy))*max(texel.x,texel.y)/max(determinant,1.0e-20f);
    float curved=smoothstep(0.05f,0.2f,normalVariation*footprint);
    float nativeVisibility=0;
    [branch] if(curved>0)
        nativeVisibility=MdsNativeShadowRamp(projected,MdsLinearShadowDepth(source,uv,texel));
    [branch] if(curved>=1) return nativeVisibility;
    float filtered=MdsPlanarFilteredVisibility(source,projected,uv,texel,uvDx,uvDy,depthDxDy);
    return lerp(filtered,nativeVisibility,curved);
}

// Reconstruct intermediate grids from the existing bridge map. Grid nodes
// are subdivisions of the real coverage grid, not camera-relative offsets.
bool MdsProgressiveBridge = false;
float MdsBridgeGridDepth(float3 projected, float subdivisions)
{
    float2 gridSize=MdsShadowMapSize.xy*subdivisions;
    float2 position=float2(projected.x+1,1-projected.y)*0.5f*gridSize;
    float2 cell=floor(position), f=position-cell;
    float4 depths=0;
    [unroll] for(int i=0;i<4;++i) {
        float2 corner=cell+float2(i%2,i/2);
        float2 covUV=corner/gridSize;
        float4 b=mul(float4(covUV.x*2-1,1-covUV.y*2,projected.z,1),MdsCoverageToBridge);
        float2 sampleUV=float2(b.x/b.w+1,1-b.y/b.w)*0.5f+MdsBridgeTexelSize.xy*0.5f;
        float d=tex2Dlod(MdsBridgeSampler,float4(sampleUV,0,0)).r;
        if(i==0) depths.x=d;
        else if(i==1) depths.y=d;
        else if(i==2) depths.z=d;
        else depths.w=d;
    }
    float depth=lerp(lerp(depths.x,depths.y,f.x),lerp(depths.z,depths.w,f.x),f.y);
    return depth;
}
float MdsProgressiveBridgeVisibility(float3 projected,float fineDepth,float coarseDepth,float weight)
{
    float ratio=max(abs(MdsCoverageToBridge[0][0])/(MdsShadowMapSize.x*MdsBridgeTexelSize.x),1);
    float octaves=log2(ratio);
    // Keep the interval continuous when the slider crosses a power of two.
    // Each middle step changes grid spacing by at most 2x; extra endpoint
    // intervals connect the native fine sample and the actual coverage map.
    float phase=(1-weight)*(octaves+2);
    float lod=clamp(octaves+1-phase,0,octaves);
    float lower=floor(lod), mixGrid=lod-lower;
    // Match the continuous footprint, including across slider octave boundaries.
    mixGrid=2*(1-exp2(-mixGrid));
    float visibility=MdsBridgeGridDepth(projected,exp2(lower));
    [branch] if(mixGrid>0)
        visibility=lerp(visibility,MdsBridgeGridDepth(projected,exp2(lower+1)),mixGrid);
    float fineMix=saturate(phase), coarseMix=saturate(phase-octaves-1);
    fineMix=fineMix*fineMix*(3-2*fineMix);
    coarseMix=coarseMix*coarseMix*(3-2*coarseMix);
    return MdsNativeShadowRamp(projected,lerp(lerp(fineDepth,visibility,fineMix),coarseDepth,coarseMix));
}
// Reconstruct one virtual level on the actual adjacent coarse grid. The
// relative 2x2 transform supports translations and small affine off-diagonals;
// raw depth remains in the common light Z coordinates throughout.
float MdsPairGridDepth(sampler fineMap,float2 fineUV,float2 coarseXY,
    float2 fineTexel,float2 coarseTexel,float4 relative,float subdivisions)
{
    float2 gridSize=subdivisions/coarseTexel;
    float2 position=float2(coarseXY.x+1,1-coarseXY.y)*0.5f*gridSize;
    float2 cell=floor(position),f=position-cell;
    float2 delta=float2(2*cell.x/gridSize.x-1,1-2*cell.y/gridSize.y)-coarseXY;
    float2 origin=fineUV+0.5f*float2(dot(delta,relative.xz),-dot(delta,relative.yw))+0.5f*fineTexel;
    float2 stepX=float2(relative.x,-relative.y)/gridSize.x;
    float2 stepY=float2(-relative.z,relative.w)/gridSize.y;
    float a=tex2Dlod(fineMap,float4(origin,0,0)).r;
    float b=tex2Dlod(fineMap,float4(origin+stepX,0,0)).r;
    float c=tex2Dlod(fineMap,float4(origin+stepY,0,0)).r;
    float d=tex2Dlod(fineMap,float4(origin+stepX+stepY,0,0)).r;
    return lerp(lerp(a,b,f.x),lerp(c,d,f.x),f.y);
}
float2 MdsProgressivePairVisibility(sampler fineMap,float4x4 fineMapping,float2 fineTexel,
    float4x4 coarseMapping,float2 coarseTexel,float3 projected,float fineDepth,float coarseDepth,float weight)
{
    float determinant=coarseMapping[0][0]*coarseMapping[1][1]-coarseMapping[0][1]*coarseMapping[1][0];
    float4 fine=mul(float4(projected,1),fineMapping),coarse=mul(float4(projected,1),coarseMapping);
    [branch] if(!(abs(determinant)>1.0e-12f && abs(fine.w)>1.0e-8f && abs(coarse.w)>1.0e-8f))
        return float2(0,0);
    float4 relative=float4(
        coarseMapping[1][1]*fineMapping[0][0]-coarseMapping[0][1]*fineMapping[1][0],
        coarseMapping[1][1]*fineMapping[0][1]-coarseMapping[0][1]*fineMapping[1][1],
        coarseMapping[0][0]*fineMapping[1][0]-coarseMapping[1][0]*fineMapping[0][0],
        coarseMapping[0][0]*fineMapping[1][1]-coarseMapping[1][0]*fineMapping[0][1]);
    relative*=coarse.w/(determinant*fine.w);
    float ratio=max(length(relative.xz)*coarseTexel.x/fineTexel.x,1);
    [branch] if(!(ratio>=1 && ratio<65536))return float2(0,0);
    float octaves=log2(ratio),phase=(1-weight)*(octaves+2);
    float lod=clamp(octaves+1-phase,0,octaves),lower=floor(lod);
    float gridMix=2*(1-exp2(lower-lod));
    float2 fineUV=float2(fine.x/fine.w+1,1-fine.y/fine.w)*0.5f;
    float2 coarseXY=coarse.xy/coarse.w;
    float virtualDepth=MdsPairGridDepth(fineMap,fineUV,coarseXY,fineTexel,coarseTexel,relative,exp2(lower));
    [branch] if(gridMix>0)
        virtualDepth=lerp(virtualDepth,MdsPairGridDepth(fineMap,fineUV,coarseXY,fineTexel,coarseTexel,relative,exp2(lower+1)),gridMix);
    float enter=saturate(phase),leave=saturate(phase-octaves-1);
    enter=enter*enter*(3-2*enter);leave=leave*leave*(3-2*leave);
    return float2(MdsNativeShadowRamp(projected,lerp(lerp(fineDepth,virtualDepth,enter),coarseDepth,leave)),1);
}
float MdsShadowVisibility(float3 projected, float2 uv, float3 normal)
{
    float2 uvDx = ddx(projected.xy)*float2(.5f,-.5f), uvDy = ddy(projected.xy)*float2(.5f,-.5f);
    float2 depthDxDy=float2(ddx(projected.z),ddy(projected.z));
    float normalVariation=max(length(ddx(normal)),length(ddy(normal)));
    projected.z += MdsShadowDerivativeOffset; // Restore depth after differentiating the centred interpolant.
    // Leaving coverage removes occlusion, not material lighting. The fitted
    // projection keeps scene geometry inside these bounds in both modes.
    if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1 ||
        projected.z < 0 || projected.z > 1)
        return 1;
    // Filter-off retains the original interpolated-depth comparison. Enhanced
    // receivers instead compare texel centres with the same continuous native
    // ramp, compensating their receiver plane before visibility reconstruction.
    // D3D9 raster centres lie at integer pixel coordinates; texture centres
    // lie half a texel later. Correct that mismatch for the stable grid so
    // coarse maps do not invent receiver self-shadow on sloping ground.
    if(MdsStableShadowGrid) uv += 0.5f/MdsShadowMapSize.xy;
    float depth = MdsLinearShadowDepth(DefSampler,uv,1/MdsShadowMapSize.xy);
    float visibility;
    [branch] if (MdsShadowFilterEnhanced && MdsLayerFilterEnhanced)
        visibility=MdsLayerFilteredVisibility(MdsNativeCoverageSampler,projected,uv,1/MdsShadowMapSize.xy,uvDx,uvDy,depthDxDy,normalVariation);
    else {
        [branch] if (MdsShadowFilterEnhanced) depth=MdsAdaptiveSplineDepth(uv,uvDx,uvDy);
        visibility=MdsNativeShadowRamp(projected,depth);
    }
    float detailVisibility = visibility, detailWeight = 0, detailDepth = depth;
    float nearVisibility = visibility, nearWeight = 0, nearDepth = depth;
    [branch] if (MdsDetailEnabled) {
        float4 detail = mul(float4(projected,1),MdsCoverageToDetail);
        float2 detailXY = detail.xy / detail.w;
        float edgeWeight = saturate((0.98f-max(abs(detailXY.x),abs(detailXY.y)))/0.12f);
        float depthWeight = saturate(dot(float4(projected,1),MdsDetailDepthBlend));
        depthWeight = MdsShadowDepthWeight(depthWeight,MdsDetailLogBlend.x);
        float weight = min(edgeWeight,depthWeight);
        weight = weight*weight*(3-2*weight);
        detailWeight = weight;
        float2 detailUV = float2(detailXY.x+1,1-detailXY.y)*0.5f;
        float2 detailDx=ddx(detailUV),detailDy=ddy(detailUV);
        [branch] if (weight > 0) {
            // Both maps deliberately share Z. Blend visibility at the rim,
            // never depths from unrelated texels or surfaces.
            if(MdsStableShadowGrid) detailUV += MdsDetailTexelSize.xy*0.5f;
            float fineDepth = tex2Dlod(MdsDetailSampler,float4(detailUV,0,0)).r;
            detailDepth=fineDepth;
            [branch] if (MdsLayerFilterEnhanced)
                detailVisibility=MdsLayerFilteredVisibility(MdsDetailSampler,projected,detailUV,MdsDetailTexelSize.xy,detailDx,detailDy,depthDxDy,normalVariation);
            else detailVisibility=MdsNativeShadowRamp(projected,fineDepth);
        }
    }
    [branch] if (MdsNearEnabled) {
        float4 detail = mul(float4(projected,1),MdsCoverageToNear);
        float2 detailXY = detail.xy / detail.w;
        float edgeWeight = saturate((0.98f-max(abs(detailXY.x),abs(detailXY.y)))/0.12f);
        float depthWeight = saturate(dot(float4(projected,1),MdsNearDepthBlend));
        depthWeight = MdsShadowDepthWeight(depthWeight,MdsDetailLogBlend.y);
        float weight = min(edgeWeight,depthWeight);
        weight = weight*weight*(3-2*weight);
        nearWeight = weight;
        float2 detailUV = float2(detailXY.x+1,1-detailXY.y)*0.5f;
        float2 detailDx=ddx(detailUV),detailDy=ddy(detailUV);
        [branch] if (weight > 0) {
            // Both maps deliberately share Z. Blend visibility at the rim,
            // never depths from unrelated texels or surfaces.
            if(MdsStableShadowGrid) detailUV += MdsNearTexelSize.xy*0.5f;
            float fineDepth = tex2Dlod(MdsNearSampler,float4(detailUV,0,0)).r;
            nearDepth=fineDepth;
            [branch] if (MdsLayerFilterEnhanced)
                nearVisibility=MdsLayerFilteredVisibility(MdsNearSampler,projected,detailUV,MdsNearTexelSize.xy,detailDx,detailDy,depthDxDy,normalVariation);
            else nearVisibility=MdsNativeShadowRamp(projected,fineDepth);
        }
    }
    float bridgeVisibility = visibility, bridgeWeight = 0, bridgeDepth = depth, bridgeNativeWeight = 0;
    [branch] if (MdsBridgeEnabled) {
        float4 detail = mul(float4(projected,1),MdsCoverageToBridge);
        float2 detailXY = detail.xy / detail.w;
        float edgeWeight = saturate((0.98f-max(abs(detailXY.x),abs(detailXY.y)))/0.12f);
        float depthWeight = saturate(dot(float4(projected,1),MdsBridgeDepthBlend));
        depthWeight = MdsShadowDepthWeight(depthWeight,MdsDetailLogBlend.z);
        float weight = min(edgeWeight,depthWeight);
        weight = weight*weight*(3-2*weight);
        bridgeWeight = weight;
        bridgeNativeWeight = weight;
        float2 detailUV = float2(detailXY.x+1,1-detailXY.y)*0.5f;
        float2 detailDx=ddx(detailUV),detailDy=ddy(detailUV);
        [branch] if (weight > 0) {
            if(MdsStableShadowGrid) detailUV += MdsBridgeTexelSize.xy*0.5f;
            float fineDepth = tex2Dlod(MdsBridgeSampler,float4(detailUV,0,0)).r;
            bridgeDepth=fineDepth;
            [branch] if (MdsLayerFilterEnhanced)
                bridgeVisibility=MdsLayerFilteredVisibility(MdsBridgeSampler,projected,detailUV,MdsBridgeTexelSize.xy,detailDx,detailDy,depthDxDy,normalVariation);
            else bridgeVisibility=MdsNativeShadowRamp(projected,fineDepth);
        }
        // Blend the correction in before the spatial rim. A hard inner-rim
        // gate would replace the whole algorithm at one visible line.
        float progressionArea=smoothstep(0.75f,1.f,edgeWeight);
        [branch] if(MdsProgressiveBridge && MdsStableShadowGrid && !MdsLayerFilterEnhanced &&
            !MdsShadowFilterEnhanced && progressionArea>0 && depthWeight>0 && depthWeight<1 &&
            detailWeight<1 && nearWeight<1) {
            float progressWeight=depthWeight*depthWeight*(3-2*depthWeight);
            float progressed=MdsProgressiveBridgeVisibility(projected,bridgeDepth,depth,progressWeight);
            float oldLayer=lerp(visibility,bridgeVisibility,weight);
            float newLayer=lerp(visibility,progressed,edgeWeight*edgeWeight*(3-2*edgeWeight));
            bridgeVisibility=lerp(oldLayer,newLayer,progressionArea);
            bridgeWeight=1;
        }
    }
    // Compose from outside to inside. A rewritten layer contains the actual
    // lower-layer result, so its final unit weight never discards a fallback.
    [branch] if(MdsProgressiveBridge && MdsStableShadowGrid && !MdsLayerFilterEnhanced &&
        !MdsShadowFilterEnhanced && MdsDetailRank.x==1 && MdsDetailRank.y==2 &&
        MdsDetailRank.z==0 && !MdsPreferMiddle) {
        float lowerVisibility=lerp(visibility,bridgeVisibility,bridgeWeight);
        float detailNativeWeight=detailWeight;
        [branch] if(MdsDetailEnabled && MdsBridgeEnabled && bridgeNativeWeight>0 &&
            detailWeight>0 && detailWeight<1 && nearWeight<1) {
            float4 fine=mul(float4(projected,1),MdsCoverageToDetail);
            float edge=saturate((0.98f-max(abs(fine.x/fine.w),abs(fine.y/fine.w)))/0.12f);
            float area=smoothstep(0.75f,1.f,edge);
            float depthFade=saturate(dot(float4(projected,1),MdsDetailDepthBlend));
            depthFade=MdsShadowDepthWeight(depthFade,MdsDetailLogBlend.x);
            [branch] if(area>0 && depthFade>0 && depthFade<1) {
                float w=depthFade*depthFade*(3-2*depthFade);
                float2 progressed=MdsProgressivePairVisibility(MdsDetailSampler,MdsCoverageToDetail,MdsDetailTexelSize.xy,
                    MdsCoverageToBridge,MdsBridgeTexelSize.xy,projected,detailDepth,bridgeDepth,w);
                // The raw coarse map may only partly contribute to the lower
                // result. Confidence then vanishes at this layer's far endpoint,
                // instead of snapping raw coarse depth back to that composite.
                float confidence=saturate(bridgeNativeWeight*w/max(w+(1-bridgeNativeWeight),1.0e-7f));
                float oldLayer=lerp(lowerVisibility,detailVisibility,detailWeight);
                float newLayer=lerp(lowerVisibility,progressed.x,edge*edge*(3-2*edge));
                detailVisibility=lerp(oldLayer,newLayer,area*confidence*progressed.y);
                detailWeight=1;
            }
        }
        lowerVisibility=lerp(lowerVisibility,detailVisibility,detailWeight);
        [branch] if(MdsNearEnabled && MdsDetailEnabled && detailNativeWeight>0 && nearWeight>0 && nearWeight<1) {
            float4 fine=mul(float4(projected,1),MdsCoverageToNear);
            float edge=saturate((0.98f-max(abs(fine.x/fine.w),abs(fine.y/fine.w)))/0.12f);
            float area=smoothstep(0.75f,1.f,edge);
            float depthFade=saturate(dot(float4(projected,1),MdsNearDepthBlend));
            depthFade=MdsShadowDepthWeight(depthFade,MdsDetailLogBlend.y);
            [branch] if(area>0 && depthFade>0 && depthFade<1) {
                float w=depthFade*depthFade*(3-2*depthFade);
                float2 progressed=MdsProgressivePairVisibility(MdsNearSampler,MdsCoverageToNear,MdsNearTexelSize.xy,
                    MdsCoverageToDetail,MdsDetailTexelSize.xy,projected,nearDepth,detailDepth,w);
                float confidence=saturate(detailNativeWeight*w/max(w+(1-detailNativeWeight),1.0e-7f));
                float oldLayer=lerp(lowerVisibility,nearVisibility,nearWeight);
                float newLayer=lerp(lowerVisibility,progressed.x,edge*edge*(3-2*edge));
                nearVisibility=lerp(oldLayer,newLayer,area*confidence*progressed.y);
                nearWeight=1;
            }
        }
    }
    [branch] if(MdsBridgeEnabled) {
        // Each layer retains only the weight not claimed by a denser layer.
        // Together with coverage these weights sum to one, including all rims.
        float3 contribution = float3(detailWeight,nearWeight,bridgeWeight);
        contribution.x *= MdsDetailRank.y>MdsDetailRank.x ? 1-nearWeight : 1;
        contribution.x *= MdsDetailRank.z>MdsDetailRank.x ? 1-bridgeWeight : 1;
        contribution.y *= MdsDetailRank.x>MdsDetailRank.y ? 1-detailWeight : 1;
        contribution.y *= MdsDetailRank.z>MdsDetailRank.y ? 1-bridgeWeight : 1;
        contribution.z *= MdsDetailRank.x>MdsDetailRank.z ? 1-detailWeight : 1;
        contribution.z *= MdsDetailRank.y>MdsDetailRank.z ? 1-nearWeight : 1;
        float coverageWeight = (1-detailWeight)*(1-nearWeight)*(1-bridgeWeight);
        return visibility*coverageWeight + dot(contribution,
            float3(detailVisibility,nearVisibility,bridgeVisibility));
    }
    // Texture tiers can reverse the density order. The CPU compares actual
    // texels per world unit, so the finer available layer is blended last.
    [branch] if (MdsPreferMiddle) {
        visibility = lerp(visibility,nearVisibility,nearWeight);
        visibility = lerp(visibility,detailVisibility,detailWeight);
    } else {
        visibility = lerp(visibility,detailVisibility,detailWeight);
        visibility = lerp(visibility,nearVisibility,nearWeight);
    }
    return visibility;
}

struct BufferShadow_OUTPUT
{
	float4 Pos      : POSITION;		// 射影変換座標
	float4 ZCalcTex : TEXCOORD0;	// Z値
	float3 N		: TEXCOORD1;	// 法線
	float3 Eye		: TEXCOORD2;	// カメラとの相対位置
};

// 頂点シェーダ
BufferShadow_OUTPUT BufferShadow_VS(float4 Pos : POSITION, float4 Normal : NORMAL)
{
	BufferShadow_OUTPUT Out = (BufferShadow_OUTPUT)0;

	// 普通にカメラの目線によるワールドビュー射影変換をする
	Out.Pos = mul( Pos, matWorldViewProj );
   
	// ライトの目線によるワールドビュー射影変換をする
	Out.ZCalcTex = mul( Pos, matLightViewProj );
    Out.ZCalcTex.z -= MdsShadowDerivativeOffset*Out.ZCalcTex.w;
	
	// 法線
	Out.N = Normal.xyz;

	// カメラ相対位置
	Out.Eye = Place - Pos.xyz;
	
	return Out;
}

// ピクセルシェーダ
float4 BufferShadow_PS(float4 ZCalcTex : TEXCOORD0,float3 N : TEXCOORD1,float3 Eye : TEXCOORD2) : COLOR
{
	// スペキュラ色計算
	float4 Specula = pow(max(0,dot(normalize(N),normalize(-LightDir.xyz + normalize(Eye)))),SpcColor.w);
	Specula *= SpcColor;
	Specula.w = 0.0f;
	Specula = saturate(Specula);

	// テクスチャ座標に変換
	ZCalcTex /= ZCalcTex.w;
	float2 TransTexCoord;
	TransTexCoord.x = (1.0f + ZCalcTex.x)*0.5f;
	TransTexCoord.y = (1.0f - ZCalcTex.y)*0.5f;
    float shadowVisibility = MdsShadowVisibility(ZCalcTex.xyz, TransTexCoord, N);
   
	{
		float comp = shadowVisibility;
		comp = min(saturate(dot(normalize(N),-LightDir)*Toon),comp);	// 分散光
		if(comp==1.0f){
			float4 ans = EgColor + Specula;
			if(transp) ans.a = 0.5f;
			return ans;
		}else{
			float4 ans = EgColor*comp + EgColor*ToonColor*(1-comp) + Specula*comp;
			if(transp) ans.a = 0.5f;
			else	   ans.a = EgColor.a;
			return ans;			
		}
	}
}

technique BufferShadowTec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;
		VertexShader = compile vs_3_0 BufferShadow_VS();
		PixelShader  = compile ps_3_0 BufferShadow_PS();
	}
}


///////////// 分散光なしテクスチャありバッファシャドウテクニック ////////////////

sampler s1 : register(s1);      //オブジェクトのテクスチャー

struct BShadowTexture_OUTPUT
{
	float4 Pos      : POSITION;		// 射影変換座標
	float4 ZCalcTex : TEXCOORD0;	// Z値
	float2 Tex      : TEXCOORD1;	// テクスチャ
	float3 N		: TEXCOORD2;	// 法線
	float3 Eye		: TEXCOORD3;	// カメラとの相対位置
};

// 頂点シェーダ
BShadowTexture_OUTPUT BShadowTexture_VS(float4 Pos : POSITION, float4 Normal : NORMAL, float2 Tex : TEXCOORD0 )
{
	BShadowTexture_OUTPUT Out = (BShadowTexture_OUTPUT)0;
	
	// テクスチャ座標
	Out.Tex = Tex;

	// 普通にカメラの目線によるワールドビュー射影変換をする
	Out.Pos = mul( Pos, matWorldViewProj );
   
	// ライトの目線によるワールドビュー射影変換をする
	Out.ZCalcTex = mul( Pos, matLightViewProj );
    Out.ZCalcTex.z -= MdsShadowDerivativeOffset*Out.ZCalcTex.w;

	// 法線
	Out.N = Normal.xyz;

	// カメラ相対位置
	Out.Eye = Place - Pos.xyz;
	
	return Out;
}

// ピクセルシェーダ
float4 BShadowTexture_PS(float4 ZCalcTex : TEXCOORD0,float2 Tex : TEXCOORD1,float3 N : TEXCOORD2,float3 Eye : TEXCOORD3) : COLOR
{
	// スペキュラ色計算
	float4 Specula = pow(max(0,dot(normalize(N),normalize(-LightDir.xyz + normalize(Eye)))),SpcColor.w);
	Specula *= SpcColor;
	Specula.w = 0.0f;
	Specula = saturate(Specula);

   // テクスチャ座標に変換
   ZCalcTex /= ZCalcTex.w;
   float2 TransTexCoord;
   TransTexCoord.x = (1.0f + ZCalcTex.x)*0.5f;
   TransTexCoord.y = (1.0f - ZCalcTex.y)*0.5f;
    float shadowVisibility = MdsShadowVisibility(ZCalcTex.xyz, TransTexCoord, N);
   float alpha = TexCMul.w + TexCAdd.w;

	{
		float comp = shadowVisibility;
		comp = min(saturate(dot(normalize(N),-LightDir)*Toon),comp);	// 分散光
		if(comp==1.0f){
			float4 ans = EgColor*((tex2D(s1,Tex) * TexCMul + TexCAdd) * alpha + (1-alpha)) + Specula*comp;
			if(transp) ans.w = 0.5f;
			else ans.w = EgColor.w * tex2D(s1,Tex).w;
			return ans;
		}else{
			float4 tmpColor=EgColor*((tex2D(s1,Tex) * TexCMul + TexCAdd) * alpha + (1-alpha));
			float4 ans = tmpColor*(comp+ToonColor*(1-comp)) + Specula*comp;
			if(transp) ans.w = 0.5f;
			else	   ans.w = EgColor.w * tex2D(s1,Tex).w;
			return ans;
		}
	}
}

technique BShadowTextureTec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;
		VertexShader = compile vs_3_0 BShadowTexture_VS();
		PixelShader  = compile ps_3_0 BShadowTexture_PS();
	}
}


///////////// 分散光なしテクスチャなしスフィアありバッファシャドウテクニック ////////////////

// 頂点シェーダ
BShadowTexture_OUTPUT BShadowSphia_VS(float4 Pos : POSITION, float4 Normal : NORMAL)
{
	BShadowTexture_OUTPUT Out = (BShadowTexture_OUTPUT)0;
	
	// スフィアマップテクスチャ座標
	Out.Tex = mul(Normal, matRotate);

	// 普通にカメラの目線によるワールドビュー射影変換をする
	Out.Pos = mul( Pos, matWorldViewProj );
   
	// ライトの目線によるワールドビュー射影変換をする
	Out.ZCalcTex = mul( Pos, matLightViewProj );
    Out.ZCalcTex.z -= MdsShadowDerivativeOffset*Out.ZCalcTex.w;

	// 法線
	Out.N = Normal.xyz;

	// カメラ相対位置
	Out.Eye = Place - Pos.xyz;
	
	return Out;
}

// ピクセルシェーダ
float4 BShadowSphia_PS(float4 ZCalcTex : TEXCOORD0,float2 Tex : TEXCOORD1,float3 N : TEXCOORD2,float3 Eye : TEXCOORD3) : COLOR
{
	// スペキュラ色計算
	float4 Specula = pow(max(0,dot(normalize(N),normalize(-LightDir.xyz + normalize(Eye)))),SpcColor.w);
	Specula *= SpcColor;
	Specula.w = 0.0f;
	Specula = saturate(Specula);

	// テクスチャ座標に変換
	ZCalcTex /= ZCalcTex.w;
	float2 TransTexCoord;
	TransTexCoord.x = (1.0f + ZCalcTex.x)*0.5f;
	TransTexCoord.y = (1.0f - ZCalcTex.y)*0.5f;
    float shadowVisibility = MdsShadowVisibility(ZCalcTex.xyz, TransTexCoord, N);
	
	// スフィアマップ座標計算
	Tex.x = Tex.x * 0.5f + 0.5f;
	Tex.y = Tex.y * -0.5f + 0.5f;
	float alpha = SphCMul.w + SphCAdd.w;

	float4 ans;
	if(spadd) ans = EgColor + (tex2D(s1,Tex) * SphCMul + SphCAdd) * alpha;
	else	  ans = EgColor * ((tex2D(s1,Tex) * SphCMul + SphCAdd)*alpha + (1-alpha));
	ans.w = EgColor.w * tex2D(s1,Tex).w;

	{
		float comp = shadowVisibility;
		comp = min(saturate(dot(normalize(N),-LightDir)*Toon),comp);	// 分散光
		if(comp==1.0f){
			ans = ans + Specula*comp;
			if(transp) ans.w = 0.5f;
			else	   ans.w = EgColor.w * tex2D(s1,Tex).w;
			return ans;
		}else{
			ans = ans*comp + ans*ToonColor*(1-comp) + Specula*comp;
			if(transp) ans.w = 0.5f;
			else	   ans.w = EgColor.w * tex2D(s1,Tex).w;
			return ans;
		}
	}
}

technique BShadowSphiaTec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;
		VertexShader = compile vs_3_0 BShadowSphia_VS();
		PixelShader  = compile ps_3_0 BShadowSphia_PS();
	}
}


///////////// 分散光なしテクスチャありスフィアマップありバッファシャドウテクニック ////////////////

sampler s2 : register(s2);      //オブジェクトのテクスチャー

struct BShadowSphiaTexture_OUTPUT
{
	float4 Pos      : POSITION;		// 射影変換座標
	float4 ZCalcTex : TEXCOORD0;	// Z値
	float2 Tex      : TEXCOORD1;	// テクスチャ座標
	float2 SpTex	: TEXCOORD2;	// スフィアマップテクスチャ座標
	float3 N		: TEXCOORD3;	// 法線
	float3 Eye		: TEXCOORD4;	// カメラとの相対位置
};

// 頂点シェーダ
BShadowSphiaTexture_OUTPUT BShadowSphiaTexture_VS(float4 Pos : POSITION, float4 Normal : NORMAL, float2 Tex : TEXCOORD0 )
{
	BShadowSphiaTexture_OUTPUT Out = (BShadowSphiaTexture_OUTPUT)0;
	
	// テクスチャ座標
	Out.Tex = Tex;

	// スフィアマップテクスチャ座標
	Out.SpTex = mul(Normal, matRotate);

	// 普通にカメラの目線によるワールドビュー射影変換をする
	Out.Pos = mul( Pos, matWorldViewProj );
   
	// ライトの目線によるワールドビュー射影変換をする
	Out.ZCalcTex = mul( Pos, matLightViewProj );
    Out.ZCalcTex.z -= MdsShadowDerivativeOffset*Out.ZCalcTex.w;

	// 法線
	Out.N = Normal.xyz;

	// カメラ相対位置
	Out.Eye = Place - Pos.xyz;
	
	return Out;
}

// ピクセルシェーダ
float4 BShadowSphiaTexture_PS(float4 ZCalcTex : TEXCOORD0,float2 Tex : TEXCOORD1,float2 SpTex : TEXCOORD2,float3 N : TEXCOORD3,float3 Eye : TEXCOORD4) : COLOR
{
	// スペキュラ色計算
	float4 Specula = pow(max(0,dot(normalize(N),normalize(-LightDir.xyz + normalize(Eye)))),SpcColor.w);
	Specula *= SpcColor;
	Specula.w = 0.0f;
	Specula = saturate(Specula);

	// テクスチャ座標に変換
	ZCalcTex /= ZCalcTex.w;
	float2 TransTexCoord;
	TransTexCoord.x = (1.0f + ZCalcTex.x)*0.5f;
	TransTexCoord.y = (1.0f - ZCalcTex.y)*0.5f;
    float shadowVisibility = MdsShadowVisibility(ZCalcTex.xyz, TransTexCoord, N);
	float texalp = TexCMul.w + TexCAdd.w;
   
  	// スフィアマップ座標計算
	SpTex.x = SpTex.x * 0.5f + 0.5f;
	SpTex.y = SpTex.y * -0.5f + 0.5f;
	float sphalp = SphCMul.w + SphCAdd.w;

	float4 ans;
	float4 texColor = (tex2D(s1,Tex) * TexCMul + TexCAdd)* texalp + (1-texalp);
	if(spadd) ans = EgColor * texColor + (tex2D(s2,SpTex) * SphCMul + SphCAdd) * sphalp;
	else	  ans = EgColor * texColor * ((tex2D(s2,SpTex) * SphCMul + SphCAdd) * sphalp + (1-sphalp));
	ans.w = EgColor.w * tex2D(s1,Tex).w * tex2D(s2,SpTex).w;
	
	{
		float comp = shadowVisibility;
		comp = min(saturate(dot(normalize(N),-LightDir)*Toon),comp);	// 分散光
		if(comp==1.0f){
			ans = ans + Specula*comp;
			if(transp) ans.w = 0.5f;
			else	   ans.w = EgColor.w * tex2D(s1,Tex).w * tex2D(s2,SpTex).w;
			return ans;
		}else{
			ans = ans*(comp+ToonColor*(1-comp)) + Specula*comp;
			if(transp) ans.w = 0.5f;
			else	   ans.w = EgColor.w * tex2D(s1,Tex).w * tex2D(s2,SpTex).w;
			return ans;
		}
	}
}

technique BShadowSphiaTextureTec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;
		VertexShader = compile vs_3_0 BShadowSphiaTexture_VS();
		PixelShader  = compile ps_3_0 BShadowSphiaTexture_PS();
	}
}

///////////// 分散光なしテクスチャなしテクスチャコード２ありバッファシャドウテクニック ////////////////

// 頂点シェーダ
BShadowTexture_OUTPUT BShadowTexCd2_VS(float4 Pos : POSITION, float4 Normal : NORMAL, float2 Tex : TEXCOORD1)
{
	BShadowTexture_OUTPUT Out = (BShadowTexture_OUTPUT)0;
	
	// テクスチャ座標
	Out.Tex = Tex;

	// 普通にカメラの目線によるワールドビュー射影変換をする
	Out.Pos = mul( Pos, matWorldViewProj );
   
	// ライトの目線によるワールドビュー射影変換をする
	Out.ZCalcTex = mul( Pos, matLightViewProj );
    Out.ZCalcTex.z -= MdsShadowDerivativeOffset*Out.ZCalcTex.w;

	// 法線
	Out.N = Normal.xyz;

	// カメラ相対位置
	Out.Eye = Place - Pos.xyz;
	
	return Out;
}

// ピクセルシェーダ
float4 BShadowTexCd2_PS(float4 ZCalcTex : TEXCOORD0,float2 Tex : TEXCOORD1,float3 N : TEXCOORD2,float3 Eye : TEXCOORD3) : COLOR
{
	// スペキュラ色計算
	float4 Specula = pow(max(0,dot(normalize(N),normalize(-LightDir.xyz + normalize(Eye)))),SpcColor.w);
	Specula *= SpcColor;
	Specula.w = 0.0f;
	Specula = saturate(Specula);

   // テクスチャ座標に変換
   ZCalcTex /= ZCalcTex.w;
   float2 TransTexCoord;
   TransTexCoord.x = (1.0f + ZCalcTex.x)*0.5f;
   TransTexCoord.y = (1.0f - ZCalcTex.y)*0.5f;
    float shadowVisibility = MdsShadowVisibility(ZCalcTex.xyz, TransTexCoord, N);
   float alpha = SphCMul.w + SphCAdd.w;

	{
		float comp = shadowVisibility;
		comp = min(saturate(dot(normalize(N),-LightDir)*Toon),comp);	// 分散光
		if(comp==1.0f){
			float4 ans = EgColor*((tex2D(s1,Tex) * SphCMul + SphCAdd)*alpha + (1-alpha)) + Specula*comp;
			if(transp) ans.a = 0.5f;
			else	   ans.w = EgColor.w * tex2D(s1,Tex).w;
			return ans;
		}else{
			float4 tmpColor=EgColor*((tex2D(s1,Tex) * SphCMul + SphCAdd)*alpha + (1-alpha));
			float4 ans = tmpColor*(comp+ToonColor*(1-comp)) + Specula*comp;
			if(transp) ans.a = 0.5f;
			else	   ans.w = EgColor.w * tex2D(s1,Tex).w;
			return ans;
		}
	}
}

technique BShadowTexCd2Tec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;
		VertexShader = compile vs_3_0 BShadowTexCd2_VS();
		PixelShader  = compile ps_3_0 BShadowTexCd2_PS();
	}
}

///////////// 分散光なしテクスチャありテクスチャコード２ありバッファシャドウテクニック ////////////////

// 頂点シェーダ
BShadowSphiaTexture_OUTPUT BShadowTextureTexCd2Tec_VS(float4 Pos : POSITION, float4 Normal : NORMAL, float2 Tex : TEXCOORD0, float2 Tex2 : TEXCOORD1 )
{
	BShadowSphiaTexture_OUTPUT Out = (BShadowSphiaTexture_OUTPUT)0;
	
	// テクスチャ座標
	Out.Tex = Tex;

	// スフィアマップテクスチャ座標
	Out.SpTex = Tex2;

	// 普通にカメラの目線によるワールドビュー射影変換をする
	Out.Pos = mul( Pos, matWorldViewProj );
   
	// ライトの目線によるワールドビュー射影変換をする
	Out.ZCalcTex = mul( Pos, matLightViewProj );
    Out.ZCalcTex.z -= MdsShadowDerivativeOffset*Out.ZCalcTex.w;

	// 法線
	Out.N = Normal.xyz;

	// カメラ相対位置
	Out.Eye = Place - Pos.xyz;
	
	return Out;
}

// ピクセルシェーダ
float4 BShadowTextureTexCd2Tec_PS(float4 ZCalcTex : TEXCOORD0,float2 Tex : TEXCOORD1,float2 SpTex : TEXCOORD2,float3 N : TEXCOORD3,float3 Eye : TEXCOORD4) : COLOR
{
	// スペキュラ色計算
	float4 Specula = pow(max(0,dot(normalize(N),normalize(-LightDir.xyz + normalize(Eye)))),SpcColor.w);
	Specula *= SpcColor;
	Specula.w = 0.0f;
	Specula = saturate(Specula);

	// テクスチャ座標に変換
	ZCalcTex /= ZCalcTex.w;
	float2 TransTexCoord;
	TransTexCoord.x = (1.0f + ZCalcTex.x)*0.5f;
	TransTexCoord.y = (1.0f - ZCalcTex.y)*0.5f;
    float shadowVisibility = MdsShadowVisibility(ZCalcTex.xyz, TransTexCoord, N);
	float texalp = TexCMul.w + TexCAdd.w;
	float sphalp = SphCMul.w + SphCAdd.w;
   
	float4 ans;
	float4 texColor = (tex2D(s1,Tex) * TexCMul + TexCAdd)*texalp + ( 1-texalp);
	if(spadd) ans = EgColor * texColor + (tex2D(s2,SpTex) * SphCMul + SphCAdd) * sphalp;
	else	  ans = EgColor * texColor * ((tex2D(s2,SpTex) * SphCMul + SphCAdd)*sphalp + (1-sphalp));
	ans.w = EgColor.w * tex2D(s1,Tex).w * tex2D(s2,SpTex).w;
	
	{
		float comp = shadowVisibility;
		comp = min(saturate(dot(normalize(N),-LightDir)*Toon),comp);	// 分散光
		if(comp==1.0f){
			ans = ans + Specula*comp;
			if(transp) ans.a = 0.5f;
			else	   EgColor.w * tex2D(s1,Tex).w * tex2D(s2,SpTex).w;
			return ans;
		}else{
			ans = ans*(comp+ToonColor*(1-comp)) + Specula*comp;
			if(transp) ans.a = 0.5f;
			else	   EgColor.w * tex2D(s1,Tex).w * tex2D(s2,SpTex).w;
			return ans;
		}
	}
}

technique BShadowTextureTexCd2Tec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;
		VertexShader = compile vs_3_0 BShadowTextureTexCd2Tec_VS();
		PixelShader  = compile ps_3_0 BShadowTextureTexCd2Tec_PS();
	}
}


///////////// 分散光ありテクスチャなしバッファシャドウテクニック ////////////////

struct DiffuseBufferShadow_OUTPUT
{
	float4 Pos      : POSITION;		// 射影変換座標
	float4 ZCalcTex : TEXCOORD0;	// Z値
	float3 N		: TEXCOORD1;	// 法線
	float3 Eye		: TEXCOORD2;	// カメラとの相対位置
	float4 Color	: COLOR0;		// Diffuse色
};

// 頂点シェーダ
DiffuseBufferShadow_OUTPUT DiffuseBufferShadow_VS(float4 Pos : POSITION, float4 Normal : NORMAL)
{
	DiffuseBufferShadow_OUTPUT Out = (DiffuseBufferShadow_OUTPUT)0;

	// 普通にカメラの目線によるワールドビュー射影変換をする
	Out.Pos = mul( Pos, matWorldViewProj );
   
	// ライトの目線によるワールドビュー射影変換をする
	Out.ZCalcTex = mul( Pos, matLightViewProj );
    Out.ZCalcTex.z -= MdsShadowDerivativeOffset*Out.ZCalcTex.w;

	// 法線
	Out.N = mul(Normal,matWorld).xyz;

	// カメラ相対位置
	Out.Eye = Place - mul(Pos,matWorld).xyz;
	
	// Diffuse色
	float3 L = -LightDir.xyz;
	Out.Color = saturate(EgColor + max(0,DifColor * dot(normalize(Out.N),L )));
	Out.Color.a = EgColor.a;

	return Out;
}

// ピクセルシェーダ
float4 DiffuseBufferShadow_PS(float4 ZCalcTex : TEXCOORD0,float3 N : TEXCOORD1,float3 Eye : TEXCOORD2,float4 Color : COLOR0) : COLOR
{
	// スペキュラ色計算
	float4 Specula = pow(max(0,dot(normalize(N),normalize(-LightDir.xyz + normalize(Eye)))),SpcColor.w);
	Specula *= SpcColor;
	Specula.w = 0.0f;
	Specula = saturate(Specula);
	
   // テクスチャ座標に変換
   ZCalcTex /= ZCalcTex.w;
   float2 TransTexCoord;
   TransTexCoord.x = (1.0f + ZCalcTex.x)*0.5f;
   TransTexCoord.y = (1.0f - ZCalcTex.y)*0.5f;
    float shadowVisibility = MdsShadowVisibility(ZCalcTex.xyz, TransTexCoord, N);

	{
		float comp = shadowVisibility;
		if(comp==1.0f){
			return Color + Specula;
		}else{
			float4 ans = (Color + Specula)*comp + EgColor*(1-comp);
			ans.a = Color.a;
			return ans;
		}
	}
}

technique DiffuseBufferShadowTec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;
	
		VertexShader = compile vs_3_0 DiffuseBufferShadow_VS();
		PixelShader  = compile ps_3_0 DiffuseBufferShadow_PS();
	}
}


///////////// 分散光ありテクスチャありバッファシャドウテクニック ////////////////

struct DiffuseBSTexture_OUTPUT
{
	float4 Pos      : POSITION;		// 射影変換座標
	float4 ZCalcTex : TEXCOORD0;	// Z値
	float2 Tex      : TEXCOORD1;	// テクスチャ
	float3 N		: TEXCOORD2;	// 法線
	float3 Eye		: TEXCOORD3;	// カメラとの相対位置
	float4 Color	: COLOR0;		// Diffuse色
};

// 頂点シェーダ
DiffuseBSTexture_OUTPUT DiffuseBSTexture_VS(float4 Pos : POSITION, float4 Normal : NORMAL, float2 Tex : TEXCOORD0 )
{
	DiffuseBSTexture_OUTPUT Out = (DiffuseBSTexture_OUTPUT)0;

	// 普通にカメラの目線によるワールドビュー射影変換をする
	Out.Pos = mul( Pos, matWorldViewProj );
   
	// ライトの目線によるワールドビュー射影変換をする
	Out.ZCalcTex = mul( Pos, matLightViewProj );
    Out.ZCalcTex.z -= MdsShadowDerivativeOffset*Out.ZCalcTex.w;

	// テクスチャ座標
	Out.Tex = Tex;

	// 法線
	Out.N = mul(Normal,matWorld).xyz;

	// カメラ相対位置
	Out.Eye = Place - mul(Pos,matWorld).xyz;
	
	// Diffuse色
	float3 L = -LightDir.xyz;
	Out.Color = saturate(EgColor + max(0,DifColor * dot(normalize(Out.N),L )));
	Out.Color.a = EgColor.a;

	return Out;
}

// ピクセルシェーダ
float4 DiffuseBSTexture_PS(float4 ZCalcTex : TEXCOORD0,float2 Tex : TEXCOORD1,float3 N : TEXCOORD2,float3 Eye : TEXCOORD3,float4 Color : COLOR0) : COLOR
{
	// スペキュラ色計算
	float4 Specula = pow(max(0,dot(normalize(N),normalize(-LightDir.xyz + normalize(Eye)))),SpcColor.w);
	Specula *= SpcColor;
	Specula.w = 0.0f;
	Specula = saturate(Specula);

   // テクスチャ座標に変換
   ZCalcTex /= ZCalcTex.w;
   float2 TransTexCoord;
   TransTexCoord.x = (1.0f + ZCalcTex.x)*0.5f;
   TransTexCoord.y = (1.0f - ZCalcTex.y)*0.5f;
    float shadowVisibility = MdsShadowVisibility(ZCalcTex.xyz, TransTexCoord, N);

	{
		float comp = shadowVisibility;
		if(comp==1.0f){
			return Color * tex2D(s1,Tex) + Specula;
		}else{
			return (Color * tex2D(s1,Tex) + Specula)*comp + EgColor*tex2D(s1,Tex)*(1-comp);
		}
	}
}

technique DiffuseBSTextureTec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;
	
		VertexShader = compile vs_3_0 DiffuseBSTexture_VS();
		PixelShader  = compile ps_3_0 DiffuseBSTexture_PS();
	}
}


///////////// 分散光ありテクスチャなしスフィアマップありバッファシャドウテクニック ////////////////

// 頂点シェーダ
DiffuseBSTexture_OUTPUT DiffuseBSSphia_VS(float4 Pos : POSITION, float4 Normal : NORMAL)
{
	DiffuseBSTexture_OUTPUT Out = (DiffuseBSTexture_OUTPUT)0;

	// スフィアマップテクスチャ座標
	Out.Tex = mul(Normal,matWRotate);

	// 普通にカメラの目線によるワールドビュー射影変換をする
	Out.Pos = mul( Pos, matWorldViewProj );
   
	// ライトの目線によるワールドビュー射影変換をする
	Out.ZCalcTex = mul( Pos, matLightViewProj );
    Out.ZCalcTex.z -= MdsShadowDerivativeOffset*Out.ZCalcTex.w;

	// 法線
	Out.N = mul(Normal,matWorld).xyz;

	// カメラ相対位置
	Out.Eye = Place - mul(Pos,matWorld).xyz;
	
	// Diffuse色
	float3 L = -LightDir.xyz;
	Out.Color = saturate(EgColor + max(0,DifColor * dot(normalize(Out.N),L )));
	Out.Color.a = EgColor.a;

	return Out;
}

// ピクセルシェーダ
float4 DiffuseBSSphia_PS(float4 ZCalcTex : TEXCOORD0,float2 Tex : TEXCOORD1,float3 N : TEXCOORD2,float3 Eye : TEXCOORD3,float4 Color : COLOR0) : COLOR
{
	// スペキュラ色計算
	float4 Specula = pow(max(0,dot(normalize(N),normalize(-LightDir.xyz + normalize(Eye)))),SpcColor.w);
	Specula *= SpcColor;
	Specula.w = 0.0f;
	Specula = saturate(Specula);

	// テクスチャ座標に変換
	ZCalcTex /= ZCalcTex.w;
	float2 TransTexCoord;
	TransTexCoord.x = (1.0f + ZCalcTex.x)*0.5f;
	TransTexCoord.y = (1.0f - ZCalcTex.y)*0.5f;
    float shadowVisibility = MdsShadowVisibility(ZCalcTex.xyz, TransTexCoord, N);

	// スフィアマップ座標計算
	float2 SpTex;
	SpTex.x = Tex.x * 0.5f + 0.5f;
	SpTex.y = Tex.y * -0.5f + 0.5f;

	float4 ans;
	float4 texUV=tex2D(s1,SpTex);
	if(spadd) ans = Color + texUV;
	else	  ans = Color * texUV;

	{
		float comp = shadowVisibility;
		if(comp==1.0f){
			return ans + Specula;
		}else{
			if(spadd){
				ans = (ans + Specula)*comp + (EgColor+texUV)*(1-comp);
				ans.a = EgColor.a;
				return ans;
			}else{
				ans = (ans + Specula)*comp + (EgColor*texUV)*(1-comp);
				ans.a = EgColor.a;
				return ans;
			}
		}
	}
}

technique DiffuseBSSphiaTec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;
	
		VertexShader = compile vs_3_0 DiffuseBSSphia_VS();
		PixelShader  = compile ps_3_0 DiffuseBSSphia_PS();
	}
}

///////////// 分散光ありテクスチャありスフィアマップありバッファシャドウテクニック ////////////////

struct DiffuseBSSphiaTex_OUTPUT
{
	float4 Pos      : POSITION;		// 射影変換座標
	float4 ZCalcTex : TEXCOORD0;	// Z値
	float2 Tex      : TEXCOORD1;	// テクスチャ座標
	float2 SpTex    : TEXCOORD2;	// スフィアマップ座標
	float3 N		: TEXCOORD3;	// 法線
	float3 Eye		: TEXCOORD4;	// カメラとの相対位置
	float4 Color	: COLOR0;		// Diffuse色
};

// 頂点シェーダ
DiffuseBSSphiaTex_OUTPUT DiffuseBSSphiaTex_VS(float4 Pos : POSITION, float4 Normal : NORMAL, float2 Tex : TEXCOORD0 )
{
	DiffuseBSSphiaTex_OUTPUT Out = (DiffuseBSSphiaTex_OUTPUT)0;

	// 普通にカメラの目線によるワールドビュー射影変換をする
	Out.Pos = mul( Pos, matWorldViewProj );
   
	// ライトの目線によるワールドビュー射影変換をする
	Out.ZCalcTex = mul( Pos, matLightViewProj );
    Out.ZCalcTex.z -= MdsShadowDerivativeOffset*Out.ZCalcTex.w;

	// テクスチャ座標
	Out.Tex = Tex;

	// スフィアマップテクスチャ座標
	Out.SpTex = mul(Normal,matWRotate);

	// 法線
	Out.N = mul(Normal,matWorld).xyz;

	// カメラ相対位置
	Out.Eye = Place - mul(Pos,matWorld).xyz;
	
	// Diffuse色
	float3 L = -LightDir.xyz;
	Out.Color = saturate(EgColor + max(0,DifColor * dot(normalize(Out.N),L )));
	Out.Color.a = EgColor.a;

	return Out;
}

// ピクセルシェーダ
float4 DiffuseBSSphiaTex_PS(float4 ZCalcTex : TEXCOORD0,float2 Tex : TEXCOORD1,float2 SpTex : TEXCOORD2,float3 N : TEXCOORD3,float3 Eye : TEXCOORD4,float4 Color : COLOR0) : COLOR
{
	// スペキュラ色計算
	float4 Specula = pow(max(0,dot(normalize(N),normalize(-LightDir.xyz + normalize(Eye)))),SpcColor.w);
	Specula *= SpcColor;
	Specula.w = 0.0f;
	Specula = saturate(Specula);

   // テクスチャ座標に変換
   ZCalcTex /= ZCalcTex.w;
   float2 TransTexCoord;
   TransTexCoord.x = (1.0f + ZCalcTex.x)*0.5f;
   TransTexCoord.y = (1.0f - ZCalcTex.y)*0.5f;
    float shadowVisibility = MdsShadowVisibility(ZCalcTex.xyz, TransTexCoord, N);

	// スフィアマップ座標計算
	SpTex.x = SpTex.x * 0.5f + 0.5f;
	SpTex.y = SpTex.y * -0.5f + 0.5f;

	float4 ans;
	float4 tex1UV=tex2D(s1,Tex);
	float4 tex2UV=tex2D(s2,SpTex);
	if(spadd) ans = Color * tex1UV + tex2UV;
	else	  ans = Color * tex1UV * tex2UV;
	float answ = Color.a * tex1UV.a;

	{
		float comp = shadowVisibility;
		if(comp==1.0f){
			return ans + Specula;
		}else{
			if(spadd){
				ans = (ans + Specula)*comp + (EgColor*tex1UV+tex2UV)*(1-comp);
				ans.a = answ;
				return ans;
			}else{
				ans = (ans + Specula)*comp + (EgColor*tex1UV*tex2UV)*(1-comp);
				ans.a = answ;
				return ans;
			}
		}
	}
}

technique DiffuseBSSphiaTexTec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;
	
		VertexShader = compile vs_3_0 DiffuseBSSphiaTex_VS();
		PixelShader  = compile ps_3_0 DiffuseBSSphiaTex_PS();
	}
}
