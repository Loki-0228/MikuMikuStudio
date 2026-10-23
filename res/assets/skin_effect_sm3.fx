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
float MdsShadowVisibility(float3 projected, float2 uv, float3 normal)
{
    float2 uvDx = ddx(uv), uvDy = ddy(uv);
    // Leaving coverage removes occlusion, not material lighting. The fitted
    // projection keeps scene geometry inside these bounds in both modes.
    if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1 ||
        projected.z < 0 || projected.z > 1)
        return 1;
    // Match 9.32 exactly: linearly interpolate depth first, then apply its
    // continuous mode-specific ramp. Filtering binary comparisons, shifting
    // the lookup by half a texel, or extrapolating a receiver triangle changes
    // grazing-surface gradients and reveals triangle boundaries at low detail.
    // D3D9 raster centres lie at integer pixel coordinates; texture centres
    // lie half a texel later. Correct that mismatch for the stable grid so
    // coarse maps do not invent receiver self-shadow on sloping ground.
    if(MdsStableShadowGrid) uv += 0.5f/MdsShadowMapSize.xy;
    float depth = tex2D(DefSampler, uv).r;
    [branch] if (MdsShadowFilterEnhanced)
        depth = MdsAdaptiveSplineDepth(uv, uvDx, uvDy);
    float visibility = MdsNativeShadowRamp(projected,depth);
    [branch] if (MdsDetailEnabled) {
        float4 detail = mul(float4(projected,1),MdsCoverageToDetail);
        float2 detailXY = detail.xy / detail.w;
        float edgeWeight = saturate((0.98f-max(abs(detailXY.x),abs(detailXY.y)))/0.12f);
        float depthWeight = saturate(dot(float4(projected,1),MdsDetailDepthBlend));
        float weight = min(edgeWeight,depthWeight);
        weight = weight*weight*(3-2*weight);
        [branch] if (weight > 0) {
            float2 detailUV = float2(detailXY.x+1,1-detailXY.y)*0.5f;
            // Both maps deliberately share Z. Blend visibility at the rim,
            // never depths from unrelated texels or surfaces.
            if(MdsStableShadowGrid) detailUV += MdsDetailTexelSize.xy*0.5f;
            float fineDepth = tex2Dlod(MdsDetailSampler,float4(detailUV,0,0)).r;
            visibility = lerp(visibility,MdsNativeShadowRamp(projected,fineDepth),weight);
        }
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
