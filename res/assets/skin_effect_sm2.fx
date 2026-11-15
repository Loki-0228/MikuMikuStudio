bool MdsPmxVertexColor = false;
bool MdsPmxUnshadowed = false;
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
int		 SKII1;				// 閾値(D3DFMT_R32F時1500、以外500)

#define	SKII2	2000

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

		VertexShader = compile vs_2_0 ColorRender_VS();
		PixelShader  = compile ps_2_0 ColorRender_PS();
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
	
		VertexShader = compile vs_2_0 ZValuePlot_VS();
		PixelShader  = compile ps_2_0 ZValuePlot_PS();
	}
}

///////////// 分散光なしテクスチャなしバッファシャドウテクニック ////////////////

sampler DefSampler = sampler_state	// サンプラーステート
{
	AddressU  = Clamp;
	AddressV  = Clamp;
	AddressW  = Clamp;
	MINFILTER = LINEAR;
	MAGFILTER = LINEAR;
	MIPFILTER = NONE;
};

struct BufferShadow_OUTPUT
{
    float4 PmxColor : COLOR1;
	float4 Pos      : POSITION;		// 射影変換座標
	float4 ZCalcTex : TEXCOORD0;	// Z値
	float3 N		: TEXCOORD1;	// 法線
	float3 Eye		: TEXCOORD2;	// カメラとの相対位置
};

// 頂点シェーダ
BufferShadow_OUTPUT BufferShadow_VS(float4 Pos : POSITION, float4 Normal : NORMAL, float4 MdsPmxColor : TEXCOORD1)
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
	
	Out.PmxColor = MdsPmxVertexColor ? MdsPmxColor : float4(1,1,1,1);
    Out.ZCalcTex = MdsPmxUnshadowed ? float4(4,4,4,1) : Out.ZCalcTex;
	return Out;
}

// ピクセルシェーダ
float4 BufferShadow_PS(float4 ZCalcTex : TEXCOORD0,float3 N : TEXCOORD1,float3 Eye : TEXCOORD2, float4 MdsPmxColor : COLOR1) : COLOR
{
	// スペキュラ色計算
	float4 Specula = pow(max(0,dot(normalize(N),normalize(-LightDir.xyz + normalize(Eye)))),SpcColor.w);
	Specula *= SpcColor;
	Specula.w = 0.0f;

	// テクスチャ座標に変換
	ZCalcTex /= ZCalcTex.w;
	float2 TransTexCoord;
	TransTexCoord.x = (1.0f + ZCalcTex.x)*0.5f;
	TransTexCoord.y = (1.0f - ZCalcTex.y)*0.5f;
   
	{
		float comp;
		if(parthf) comp=1-saturate(max(ZCalcTex.z-tex2D(DefSampler,TransTexCoord).r , 0.0f)*SKII2*TransTexCoord.y-0.3f);
		else	   comp=1-saturate(max(ZCalcTex.z-tex2D(DefSampler,TransTexCoord).r , 0.0f)*SKII1-0.3f);
        // Coverage affects occlusion only, never the material's lighting.
        float3 coverage = abs(float3(TransTexCoord, ZCalcTex.z) * 2 - 1);
        if (max(coverage.x, max(coverage.y, coverage.z)) > 1) comp = 1;
		comp = min(saturate(dot(normalize(N),-LightDir)*3),comp);	// 分散光
		if(comp==1.0f){
			float4 ans = EgColor + Specula;
			if(transp) ans.a = 0.5f;
			return (ans) * MdsPmxColor;
		}else{
			float4 ans = EgColor*comp + EgColor*ToonColor*(1-comp) + Specula*comp;
			if(transp) ans.a = 0.5f;
			return (ans) * MdsPmxColor;
		}
	}
}

technique BufferShadowTec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;
		VertexShader = compile vs_2_0 BufferShadow_VS();
		PixelShader  = compile ps_2_0 BufferShadow_PS();
	}
}


///////////// 分散光なしテクスチャありバッファシャドウテクニック ////////////////

sampler s1 : register(s1);      //オブジェクトのテクスチャー

struct BShadowTexture_OUTPUT
{
    float4 PmxColor : COLOR1;
	float4 Pos      : POSITION;		// 射影変換座標
	float4 ZCalcTex : TEXCOORD0;	// Z値
	float2 Tex      : TEXCOORD1;	// テクスチャ
	float3 N		: TEXCOORD2;	// 法線
	float3 Eye		: TEXCOORD3;	// カメラとの相対位置
};

// 頂点シェーダ
BShadowTexture_OUTPUT BShadowTexture_VS(float4 Pos : POSITION, float4 Normal : NORMAL, float2 Tex : TEXCOORD0 , float4 MdsPmxColor : TEXCOORD1)
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
	
	Out.PmxColor = MdsPmxVertexColor ? MdsPmxColor : float4(1,1,1,1);
    Out.ZCalcTex = MdsPmxUnshadowed ? float4(4,4,4,1) : Out.ZCalcTex;
	return Out;
}

// ピクセルシェーダ
float4 BShadowTexture_PS(float4 ZCalcTex : TEXCOORD0,float2 Tex : TEXCOORD1,float3 N : TEXCOORD2,float3 Eye : TEXCOORD3, float4 MdsPmxColor : COLOR1) : COLOR
{
	// スペキュラ色計算
	float4 Specula = pow(max(0,dot(normalize(N),normalize(-LightDir.xyz + normalize(Eye)))),SpcColor.w);
	Specula *= SpcColor;
	Specula.w = 0.0f;

   // テクスチャ座標に変換
   ZCalcTex /= ZCalcTex.w;
   float2 TransTexCoord;
   TransTexCoord.x = (1.0f + ZCalcTex.x)*0.5f;
   TransTexCoord.y = (1.0f - ZCalcTex.y)*0.5f;
   float alp = TexCMul.w + TexCAdd.w;

	{
		float comp;
		if(parthf) comp=1-saturate(max(ZCalcTex.z - tex2D(DefSampler,TransTexCoord).r,0.0f)*SKII2*TransTexCoord.y-0.3f);
		else	   comp=1-saturate(max(ZCalcTex.z - tex2D(DefSampler,TransTexCoord).r,0.0f)*SKII1-0.3f);
        // Coverage affects occlusion only, never the material's lighting.
        float3 coverage = abs(float3(TransTexCoord, ZCalcTex.z) * 2 - 1);
        if (max(coverage.x, max(coverage.y, coverage.z)) > 1) comp = 1;
		comp = min(saturate(dot(normalize(N),-LightDir)*3),comp);	// 分散光
		if(comp==1.0f){
			float4 ans = EgColor*((tex2D(s1,Tex) * TexCMul + TexCAdd)*alp +(1-alp)) + Specula*comp;
			if(transp) ans.a = 0.5f;
			else	   ans.w = EgColor.w * tex2D(s1,Tex).w;
			return (ans) * MdsPmxColor;
		}else{
			float4 ans = (EgColor*(tex2D(s1,Tex) * TexCMul + TexCAdd)*alp+(1-alp))*(comp + ToonColor*(1-comp)) + Specula*comp;
			if(transp) ans.a = 0.5f;
			else	   ans.w = EgColor.w * tex2D(s1,Tex).w;
			return (ans) * MdsPmxColor;
		}
	}
}

technique BShadowTextureTec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;
		VertexShader = compile vs_2_0 BShadowTexture_VS();
		PixelShader  = compile ps_2_0 BShadowTexture_PS();
	}
}


///////////// 分散光なしテクスチャなしスフィアありバッファシャドウテクニック ////////////////

// 頂点シェーダ
BShadowTexture_OUTPUT BShadowSphia_VS(float4 Pos : POSITION, float4 Normal : NORMAL, float4 MdsPmxColor : TEXCOORD1)
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
	
	Out.PmxColor = MdsPmxVertexColor ? MdsPmxColor : float4(1,1,1,1);
    Out.ZCalcTex = MdsPmxUnshadowed ? float4(4,4,4,1) : Out.ZCalcTex;
	return Out;
}

// ピクセルシェーダ
float4 BShadowSphia_PS(float4 ZCalcTex : TEXCOORD0,float2 Tex : TEXCOORD1,float3 N : TEXCOORD2,float3 Eye : TEXCOORD3, float4 MdsPmxColor : COLOR1) : COLOR
{
	// スペキュラ色計算
	float4 Specula = pow(max(0,dot(normalize(N),normalize(-LightDir.xyz + normalize(Eye)))),SpcColor.w);
	Specula *= SpcColor;
	Specula.w = 0.0f;

	// テクスチャ座標に変換
	ZCalcTex /= ZCalcTex.w;
	float2 TransTexCoord;
	TransTexCoord.x = (1.0f + ZCalcTex.x)*0.5f;
	TransTexCoord.y = (1.0f - ZCalcTex.y)*0.5f;
	float alp = SphCMul.w + SphCAdd.w;
	
	// スフィアマップ座標計算
	Tex.x = Tex.x * 0.5f + 0.5f;
	Tex.y = Tex.y * -0.5f + 0.5f;

	float4 ans;
	if(spadd) ans = EgColor + (tex2D(s1,Tex) * SphCMul + SphCAdd)*alp;
	else	  ans = EgColor * ((tex2D(s1,Tex) * SphCMul + SphCAdd)*alp+(1-alp));
	ans.w = EgColor.w * tex2D(s1,Tex).w;
	{
		float comp;
		if(parthf) comp=1-saturate(max(ZCalcTex.z - tex2D(DefSampler,TransTexCoord).r,0.0f)*SKII2*TransTexCoord.y-0.3f);
		else	   comp=1-saturate(max(ZCalcTex.z - tex2D(DefSampler,TransTexCoord).r,0.0f)*SKII1-0.3f);
        // Coverage affects occlusion only, never the material's lighting.
        float3 coverage = abs(float3(TransTexCoord, ZCalcTex.z) * 2 - 1);
        if (max(coverage.x, max(coverage.y, coverage.z)) > 1) comp = 1;
		comp = min(saturate(dot(normalize(N),-LightDir)*3),comp);	// 分散光
		if(comp==1.0f){
			ans = ans + Specula*comp;
			if(transp) ans.a = 0.5f;
			else	   ans.w = EgColor.w * tex2D(s1,Tex).w;
			return (ans) * MdsPmxColor;
		}else{
			ans = ans*lerp(ToonColor, 1, comp) + Specula*comp;
			if(transp) ans.a = 0.5f;
			else	   ans.w = EgColor.w * tex2D(s1,Tex).w;
			return (ans) * MdsPmxColor;
		}
	}
}

technique BShadowSphiaTec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;
		VertexShader = compile vs_2_0 BShadowSphia_VS();
		PixelShader  = compile ps_2_0 BShadowSphia_PS();
	}
}


///////////// 分散光なしテクスチャありスフィアマップありバッファシャドウテクニック ////////////////

sampler s2 : register(s2);      //オブジェクトのテクスチャー

struct BShadowSphiaTexture_OUTPUT
{
    float4 PmxColor : COLOR1;
	float4 Pos      : POSITION;		// 射影変換座標
	float4 ZCalcTex : TEXCOORD0;	// Z値
	float2 Tex      : TEXCOORD1;	// テクスチャ座標
	float2 SpTex	: TEXCOORD2;	// スフィアマップテクスチャ座標
	float3 N		: TEXCOORD3;	// 法線
	float3 Eye		: TEXCOORD4;	// カメラとの相対位置
};

// 頂点シェーダ
BShadowSphiaTexture_OUTPUT BShadowSphiaTexture_VS(float4 Pos : POSITION, float4 Normal : NORMAL, float2 Tex : TEXCOORD0 , float4 MdsPmxColor : TEXCOORD1)
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
	
	Out.PmxColor = MdsPmxVertexColor ? MdsPmxColor : float4(1,1,1,1);
    Out.ZCalcTex = MdsPmxUnshadowed ? float4(4,4,4,1) : Out.ZCalcTex;
	return Out;
}

// ピクセルシェーダ
float4 BShadowSphiaTexture_PS(float4 ZCalcTex : TEXCOORD0,float2 Tex : TEXCOORD1,float2 SpTex : TEXCOORD2,float3 N : TEXCOORD3,float3 Eye : TEXCOORD4, float4 MdsPmxColor : COLOR1) : COLOR
{
	// スペキュラ色計算
	float4 Specula = pow(max(0,dot(normalize(N),normalize(-LightDir.xyz + normalize(Eye)))),SpcColor.w);
	Specula *= SpcColor;
	Specula.w = 0.0f;

	// テクスチャ座標に変換
	ZCalcTex /= ZCalcTex.w;
	float2 TransTexCoord;
	TransTexCoord.x = (1.0f + ZCalcTex.x)*0.5f;
	TransTexCoord.y = (1.0f - ZCalcTex.y)*0.5f;
   
  	// スフィアマップ座標計算
	SpTex.x = SpTex.x * 0.5f + 0.5f;
	SpTex.y = SpTex.y * -0.5f + 0.5f;

	float4 ans;
	if(spadd) ans = EgColor * (tex2D(s1,Tex) * TexCMul + TexCAdd) + (tex2D(s2,SpTex) * SphCMul + SphCAdd);
	else	  ans = EgColor * (tex2D(s1,Tex) * TexCMul + TexCAdd) * (tex2D(s2,SpTex) * SphCMul + SphCAdd);
	ans.w = EgColor.w * tex2D(s1,Tex).w * tex2D(s2,SpTex).w;
	
	{
		float comp;
		if(parthf) comp=1-saturate(max(ZCalcTex.z - tex2D(DefSampler,TransTexCoord).r,0.0f)*SKII2*TransTexCoord.y-0.3f);
		else	   comp=1-saturate(max(ZCalcTex.z - tex2D(DefSampler,TransTexCoord).r,0.0f)*SKII1-0.3f);
        // Coverage affects occlusion only, never the material's lighting.
        float3 coverage = abs(float3(TransTexCoord, ZCalcTex.z) * 2 - 1);
        if (max(coverage.x, max(coverage.y, coverage.z)) > 1) comp = 1;
		comp = min(saturate(dot(normalize(N),-LightDir)*3),comp);	// 分散光

			ans = ans*lerp(ToonColor, 1, comp) + Specula*comp;
			if(transp) ans.a = 0.5f;
			return (ans) * MdsPmxColor;

	}
}

technique BShadowSphiaTextureTec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;
		VertexShader = compile vs_2_0 BShadowSphiaTexture_VS();
		PixelShader  = compile ps_2_0 BShadowSphiaTexture_PS();
	}
}

///////////// 分散光なしテクスチャなしテクスチャコード２ありバッファシャドウテクニック ////////////////

// 頂点シェーダ
BShadowTexture_OUTPUT BShadowTexCd2_VS(float4 Pos : POSITION, float4 Normal : NORMAL, float4 Tex : TEXCOORD1)
{
	BShadowTexture_OUTPUT Out = (BShadowTexture_OUTPUT)0;
	
	// テクスチャ座標
	Out.Tex.xy = Tex.xy;

	// 普通にカメラの目線によるワールドビュー射影変換をする
	Out.Pos = mul( Pos, matWorldViewProj );
   
	// ライトの目線によるワールドビュー射影変換をする
	Out.ZCalcTex = mul( Pos, matLightViewProj );

	// 法線
	Out.N = Normal.xyz;

	// カメラ相対位置
	Out.Eye = Place - Pos.xyz;
	
	Out.PmxColor = MdsPmxVertexColor ? Tex : float4(1,1,1,1);
    Out.ZCalcTex = MdsPmxUnshadowed ? float4(4,4,4,1) : Out.ZCalcTex;
	return Out;
}

// ピクセルシェーダ
float4 BShadowTexCd2_PS(float4 ZCalcTex : TEXCOORD0,float2 Tex : TEXCOORD1,float3 N : TEXCOORD2,float3 Eye : TEXCOORD3, float4 MdsPmxColor : COLOR1) : COLOR
{
	// スペキュラ色計算
	float4 Specula = pow(max(0,dot(normalize(N),normalize(-LightDir.xyz + normalize(Eye)))),SpcColor.w);
	Specula *= SpcColor;
	Specula.w = 0.0f;

   // テクスチャ座標に変換
   ZCalcTex /= ZCalcTex.w;
   float2 TransTexCoord;
   TransTexCoord.x = (1.0f + ZCalcTex.x)*0.5f;
   TransTexCoord.y = (1.0f - ZCalcTex.y)*0.5f;

	{
		float comp;
		if(parthf) comp=1-saturate(max(ZCalcTex.z - tex2D(DefSampler,TransTexCoord).r,0.0f)*SKII2*TransTexCoord.y-0.3f);
		else	   comp=1-saturate(max(ZCalcTex.z - tex2D(DefSampler,TransTexCoord).r,0.0f)*SKII1-0.3f);
        // Coverage affects occlusion only, never the material's lighting.
        float3 coverage = abs(float3(TransTexCoord, ZCalcTex.z) * 2 - 1);
        if (max(coverage.x, max(coverage.y, coverage.z)) > 1) comp = 1;
		comp = min(saturate(dot(normalize(N),-LightDir)*3),comp);	// 分散光
		if(comp==1.0f){
			float4 ans = EgColor*(tex2D(s1,Tex) * SphCMul + SphCAdd) + Specula*comp;
			if(transp) ans.a = 0.5f;
			return (ans) * MdsPmxColor;
		}else{
			float4 ans = EgColor*(tex2D(s1,Tex) * SphCMul + SphCAdd)*comp + EgColor*tex2D(s1,Tex)*ToonColor*(1-comp) + Specula*comp;
			if(transp) ans.a = 0.5f;
			return (ans) * MdsPmxColor;
		}
	}
}

technique BShadowTexCd2Tec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;
		VertexShader = compile vs_2_0 BShadowTexCd2_VS();
		PixelShader  = compile ps_2_0 BShadowTexCd2_PS();
	}
}


///////////// 分散光なしテクスチャありテクスチャコード２ありバッファシャドウテクニック ////////////////

// 頂点シェーダ
BShadowSphiaTexture_OUTPUT BShadowTextureTexCd2Tec_VS(float4 Pos : POSITION, float4 Normal : NORMAL, float2 Tex : TEXCOORD0, float4 Tex2 : TEXCOORD1 )
{
	BShadowSphiaTexture_OUTPUT Out = (BShadowSphiaTexture_OUTPUT)0;
	
	// テクスチャ座標
	Out.Tex = Tex;

	// スフィアマップテクスチャ座標
	Out.SpTex = Tex2.xy;

	// 普通にカメラの目線によるワールドビュー射影変換をする
	Out.Pos = mul( Pos, matWorldViewProj );
   
	// ライトの目線によるワールドビュー射影変換をする
	Out.ZCalcTex = mul( Pos, matLightViewProj );

	// 法線
	Out.N = Normal.xyz;

	// カメラ相対位置
	Out.Eye = Place - Pos.xyz;
	
	Out.PmxColor = MdsPmxVertexColor ? Tex2 : float4(1,1,1,1);
    Out.ZCalcTex = MdsPmxUnshadowed ? float4(4,4,4,1) : Out.ZCalcTex;
	return Out;
}

// ピクセルシェーダ
float4 BShadowTextureTexCd2Tec_PS(float4 ZCalcTex : TEXCOORD0,float2 Tex : TEXCOORD1,float2 SpTex : TEXCOORD2,float3 N : TEXCOORD3,float3 Eye : TEXCOORD4, float4 MdsPmxColor : COLOR1) : COLOR
{
	// スペキュラ色計算
	float4 Specula = pow(max(0,dot(normalize(N),normalize(-LightDir.xyz + normalize(Eye)))),SpcColor.w);
	Specula *= SpcColor;
	Specula.w = 0.0f;

	// テクスチャ座標に変換
	ZCalcTex /= ZCalcTex.w;
	float2 TransTexCoord;
	TransTexCoord.x = (1.0f + ZCalcTex.x)*0.5f;
	TransTexCoord.y = (1.0f - ZCalcTex.y)*0.5f;
   
	float4 ans;
	if(spadd) ans = EgColor * (tex2D(s1,Tex) * TexCMul + TexCAdd) + (tex2D(s2,SpTex) * SphCMul + SphCAdd);
	else	  ans = EgColor * (tex2D(s1,Tex) * TexCMul + TexCAdd) * (tex2D(s2,SpTex) * SphCMul + SphCAdd);
	ans.w = (tex2D(s1,Tex) * TexCMul + TexCAdd).w;
	
	{
		float comp;
		if(parthf) comp=1-saturate(max(ZCalcTex.z - tex2D(DefSampler,TransTexCoord).r,0.0f)*SKII2*TransTexCoord.y-0.3f);
		else	   comp=1-saturate(max(ZCalcTex.z - tex2D(DefSampler,TransTexCoord).r,0.0f)*SKII1-0.3f);
        // Coverage affects occlusion only, never the material's lighting.
        float3 coverage = abs(float3(TransTexCoord, ZCalcTex.z) * 2 - 1);
        if (max(coverage.x, max(coverage.y, coverage.z)) > 1) comp = 1;
		comp = min(saturate(dot(normalize(N),-LightDir)*3),comp);	// 分散光

			ans = ans*lerp(ToonColor, 1, comp) + Specula*comp;
			if(transp) ans.a = 0.5f;
			return (ans) * MdsPmxColor;

	}
}

technique BShadowTextureTexCd2Tec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;
		VertexShader = compile vs_2_0 BShadowTextureTexCd2Tec_VS();
		PixelShader  = compile ps_2_0 BShadowTextureTexCd2Tec_PS();
	}
}

///////////// 分散光ありテクスチャなしバッファシャドウテクニック ////////////////

struct DiffuseBufferShadow_OUTPUT
{
    float4 PmxColor : COLOR1;
	float4 Pos      : POSITION;		// 射影変換座標
	float4 ZCalcTex : TEXCOORD0;	// Z値
	float3 N		: TEXCOORD1;	// 法線
	float3 Eye		: TEXCOORD2;	// カメラとの相対位置
	float4 Color	: COLOR0;		// Diffuse色
};

// 頂点シェーダ
DiffuseBufferShadow_OUTPUT DiffuseBufferShadow_VS(float4 Pos : POSITION, float4 Normal : NORMAL, float4 MdsPmxColor : TEXCOORD1)
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
	Out.Color = EgColor + max(0,DifColor * dot(normalize(Out.N),L ));
	Out.Color.a = EgColor.a;

	Out.PmxColor = MdsPmxVertexColor ? MdsPmxColor : float4(1,1,1,1);
    Out.ZCalcTex = MdsPmxUnshadowed ? float4(4,4,4,1) : Out.ZCalcTex;
	return Out;
}

// ピクセルシェーダ
float4 DiffuseBufferShadow_PS(float4 ZCalcTex : TEXCOORD0,float3 N : TEXCOORD1,float3 Eye : TEXCOORD2,float4 Color : COLOR0, float4 MdsPmxColor : COLOR1) : COLOR
{
	// スペキュラ色計算
	float4 Specula = pow(max(0,dot(normalize(N),normalize(-LightDir.xyz + normalize(Eye)))),SpcColor.w);
	Specula *= SpcColor;
	Specula.w = 0.0f;
	
   // テクスチャ座標に変換
   ZCalcTex /= ZCalcTex.w;
   float2 TransTexCoord;
   TransTexCoord.x = (1.0f + ZCalcTex.x)*0.5f;
   TransTexCoord.y = (1.0f - ZCalcTex.y)*0.5f;

	{
		float comp;
		if(parthf) comp=1-saturate(max(ZCalcTex.z - tex2D(DefSampler,TransTexCoord).r,0.0f)*SKII2*TransTexCoord.y-0.3f);
		else	   comp=1-saturate(max(ZCalcTex.z - tex2D(DefSampler,TransTexCoord).r,0.0f)*SKII1-0.3f);
        // Coverage affects occlusion only, never the material's lighting.
        float3 coverage = abs(float3(TransTexCoord, ZCalcTex.z) * 2 - 1);
        if (max(coverage.x, max(coverage.y, coverage.z)) > 1) comp = 1;
		if(comp==1.0f){
			return (Color + Specula) * MdsPmxColor;
		}else{
			return ((Color + Specula)*comp + EgColor*(1-comp)) * MdsPmxColor;
		}
	}
}

technique DiffuseBufferShadowTec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;
		VertexShader = compile vs_2_0 DiffuseBufferShadow_VS();
		PixelShader  = compile ps_2_0 DiffuseBufferShadow_PS();
	}
}


///////////// 分散光ありテクスチャありバッファシャドウテクニック ////////////////

struct DiffuseBSTexture_OUTPUT
{
    float4 PmxColor : COLOR1;
	float4 Pos      : POSITION;		// 射影変換座標
	float4 ZCalcTex : TEXCOORD0;	// Z値
	float2 Tex      : TEXCOORD1;	// テクスチャ
	float3 N		: TEXCOORD2;	// 法線
	float3 Eye		: TEXCOORD3;	// カメラとの相対位置
	float4 Color	: COLOR0;		// Diffuse色
};

// 頂点シェーダ
DiffuseBSTexture_OUTPUT DiffuseBSTexture_VS(float4 Pos : POSITION, float4 Normal : NORMAL, float2 Tex : TEXCOORD0 , float4 MdsPmxColor : TEXCOORD1)
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
	Out.Color = EgColor + max(0,DifColor * dot(normalize(Out.N),L ));

	Out.PmxColor = MdsPmxVertexColor ? MdsPmxColor : float4(1,1,1,1);
    Out.ZCalcTex = MdsPmxUnshadowed ? float4(4,4,4,1) : Out.ZCalcTex;
	return Out;
}

// ピクセルシェーダ
float4 DiffuseBSTexture_PS(float4 ZCalcTex : TEXCOORD0,float2 Tex : TEXCOORD1,float3 N : TEXCOORD2,float3 Eye : TEXCOORD3,float4 Color : COLOR0, float4 MdsPmxColor : COLOR1) : COLOR
{
	// スペキュラ色計算
	float4 Specula = pow(max(0,dot(normalize(N),normalize(-LightDir.xyz + normalize(Eye)))),SpcColor.w);
	Specula *= SpcColor;
	Specula.w = 0.0f;
	
   // テクスチャ座標に変換
   ZCalcTex /= ZCalcTex.w;
   float2 TransTexCoord;
   TransTexCoord.x = (1.0f + ZCalcTex.x)*0.5f;
   TransTexCoord.y = (1.0f - ZCalcTex.y)*0.5f;

	{
		float comp;
		if(parthf) comp=1-saturate(max(ZCalcTex.z - tex2D(DefSampler,TransTexCoord).r,0.0f)*SKII2*TransTexCoord.y-0.3f);
		else	   comp=1-saturate(max(ZCalcTex.z - tex2D(DefSampler,TransTexCoord).r,0.0f)*SKII1-0.3f);
        // Coverage affects occlusion only, never the material's lighting.
        float3 coverage = abs(float3(TransTexCoord, ZCalcTex.z) * 2 - 1);
        if (max(coverage.x, max(coverage.y, coverage.z)) > 1) comp = 1;
		if(comp==1.0f){
			return (Color * tex2D(s1,Tex) + Specula) * MdsPmxColor;
		}else{
			return ((Color * tex2D(s1,Tex) + Specula)*comp + EgColor*tex2D(s1,Tex)*(1-comp)) * MdsPmxColor;
		}
	}
}

technique DiffuseBSTextureTec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;
		VertexShader = compile vs_2_0 DiffuseBSTexture_VS();
		PixelShader  = compile ps_2_0 DiffuseBSTexture_PS();
	}
}


///////////// 分散光ありテクスチャなしスフィアマップありバッファシャドウテクニック ////////////////

// 頂点シェーダ
DiffuseBSTexture_OUTPUT DiffuseBSSphia_VS(float4 Pos : POSITION, float4 Normal : NORMAL, float4 MdsPmxColor : TEXCOORD1)
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
	Out.Color = EgColor + max(0,DifColor * dot(normalize(Out.N),L ));

	Out.PmxColor = MdsPmxVertexColor ? MdsPmxColor : float4(1,1,1,1);
    Out.ZCalcTex = MdsPmxUnshadowed ? float4(4,4,4,1) : Out.ZCalcTex;
	return Out;
}

// ピクセルシェーダ
float4 DiffuseBSSphia_PS(float4 ZCalcTex : TEXCOORD0,float2 Tex : TEXCOORD1,float3 N : TEXCOORD2,float3 Eye : TEXCOORD3,float4 Color : COLOR0, float4 MdsPmxColor : COLOR1) : COLOR
{
	// スペキュラ色計算
	float4 Specula = pow(max(0,dot(normalize(N),normalize(-LightDir.xyz + normalize(Eye)))),SpcColor.w);
	Specula *= SpcColor;
	Specula.w = 0.0f;
	
	// テクスチャ座標に変換
	ZCalcTex /= ZCalcTex.w;
	float2 TransTexCoord;
	TransTexCoord.x = (1.0f + ZCalcTex.x)*0.5f;
	TransTexCoord.y = (1.0f - ZCalcTex.y)*0.5f;

	// スフィアマップ座標計算
	float2 SpTex;
	SpTex.x = Tex.x * 0.5f + 0.5f;
	SpTex.y = Tex.y * -0.5f + 0.5f;

	float4 ans;
	float4 texUV=tex2D(s1,SpTex);
	if(spadd) ans = Color + texUV;
	else	  ans = Color * texUV;

	{
		float comp;
		if(parthf) comp=1-saturate(max(ZCalcTex.z - tex2D(DefSampler,TransTexCoord).r,0.0f)*SKII2*TransTexCoord.y-0.3f);
		else	   comp=1-saturate(max(ZCalcTex.z - tex2D(DefSampler,TransTexCoord).r,0.0f)*SKII1-0.3f);
        // Coverage affects occlusion only, never the material's lighting.
        float3 coverage = abs(float3(TransTexCoord, ZCalcTex.z) * 2 - 1);
        if (max(coverage.x, max(coverage.y, coverage.z)) > 1) comp = 1;
		if(comp==1.0f){
			return (ans + Specula) * MdsPmxColor;
		}else{
			if(spadd) return ((ans + Specula)*comp + (EgColor+texUV)*(1-comp)) * MdsPmxColor;
			else	  return ((ans + Specula)*comp + (EgColor*texUV)*(1-comp)) * MdsPmxColor;
		}
	}
}

technique DiffuseBSSphiaTec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;
		VertexShader = compile vs_2_0 DiffuseBSSphia_VS();
		PixelShader  = compile ps_2_0 DiffuseBSSphia_PS();
	}
}

///////////// 分散光ありテクスチャありスフィアマップありバッファシャドウテクニック ////////////////

struct DiffuseBSSphiaTex_OUTPUT
{
    float4 PmxColor : COLOR1;
	float4 Pos      : POSITION;		// 射影変換座標
	float4 ZCalcTex : TEXCOORD0;	// Z値
	float2 Tex      : TEXCOORD1;	// テクスチャ座標
	float2 SpTex    : TEXCOORD2;	// スフィアマップ座標
	float3 N		: TEXCOORD3;	// 法線
	float3 Eye		: TEXCOORD4;	// カメラとの相対位置
	float4 Color	: COLOR0;		// Diffuse色
};

// 頂点シェーダ
DiffuseBSSphiaTex_OUTPUT DiffuseBSSphiaTex_VS(float4 Pos : POSITION, float4 Normal : NORMAL, float2 Tex : TEXCOORD0 , float4 MdsPmxColor : TEXCOORD1)
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
	Out.Color = EgColor + max(0,DifColor * dot(normalize(Out.N),L ));

	Out.PmxColor = MdsPmxVertexColor ? MdsPmxColor : float4(1,1,1,1);
    Out.ZCalcTex = MdsPmxUnshadowed ? float4(4,4,4,1) : Out.ZCalcTex;
	return Out;
}

// ピクセルシェーダ
float4 DiffuseBSSphiaTex_PS(float4 ZCalcTex : TEXCOORD0,float2 Tex : TEXCOORD1,float2 SpTex : TEXCOORD2,float3 N : TEXCOORD3,float3 Eye : TEXCOORD4,float4 Color : COLOR0, float4 MdsPmxColor : COLOR1) : COLOR
{
	// スペキュラ色計算
	float4 Specula = pow(max(0,dot(normalize(N),normalize(-LightDir.xyz + normalize(Eye)))),SpcColor.w);
	Specula *= SpcColor;
	Specula.w = 0.0f;
	
   // テクスチャ座標に変換
   ZCalcTex /= ZCalcTex.w;
   float2 TransTexCoord;
   TransTexCoord.x = (1.0f + ZCalcTex.x)*0.5f;
   TransTexCoord.y = (1.0f - ZCalcTex.y)*0.5f;

	// スフィアマップ座標計算
	SpTex.x = SpTex.x * 0.5f + 0.5f;
	SpTex.y = SpTex.y * -0.5f + 0.5f;

	float4 ans;
	float4 tex1UV=tex2D(s1,Tex);
	float4 tex2UV=tex2D(s2,SpTex);
	if(spadd) ans = Color * tex1UV + tex2UV;
	else	  ans = Color * tex1UV * tex2UV;

	{
		float comp;
		if(parthf) comp=1-saturate(max(ZCalcTex.z - tex2D(DefSampler,TransTexCoord).r,0.0f)*SKII2*TransTexCoord.y-0.3f);
		else	   comp=1-saturate(max(ZCalcTex.z - tex2D(DefSampler,TransTexCoord).r,0.0f)*SKII1-0.3f);
        // Coverage affects occlusion only, never the material's lighting.
        float3 coverage = abs(float3(TransTexCoord, ZCalcTex.z) * 2 - 1);
        if (max(coverage.x, max(coverage.y, coverage.z)) > 1) comp = 1;
		if(comp==1.0f){
			return (ans + Specula) * MdsPmxColor;
		}else{
			if(spadd) return ((ans + Specula)*comp + (EgColor*tex1UV+tex2UV)*(1-comp)) * MdsPmxColor;
			else	  return ((ans + Specula)*comp + (EgColor*tex1UV*tex2UV)*(1-comp)) * MdsPmxColor;
		}
	}
}

technique DiffuseBSSphiaTexTec
{
	pass P0
	{
		ColorOp[0]   = Disable;
		AlphaOp[0]   = Disable;
		VertexShader = compile vs_2_0 DiffuseBSSphiaTex_VS();
		PixelShader  = compile ps_2_0 DiffuseBSSphiaTex_PS();
	}
}
