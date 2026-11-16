// Members of Native. Reuse the same licensed FXAA/SMAA and temporal rejection
// shaders as the DX9 compositor; only bindings and shader profiles differ.
struct AaFrame {
    ComPtr<ID3D12Resource> source, edges, weights, spatial, resolved, motion, vertices;
    D3DMATRIX camera{},view{};
    unsigned vertexCount=0;
    float jitter[2]{};
};
std::array<AaFrame,3> aaFrames;
ComPtr<ID3D12DescriptorHeap> aaSrvs, aaRtvs;
ComPtr<ID3D12RootSignature> aaRoot;
ComPtr<ID3D12PipelineState> aaCopy, aaFxaa, aaTemporal;
std::array<std::array<ComPtr<ID3D12PipelineState>,4>,4> aaSmaa;
ComPtr<ID3D12Resource> aaArea, aaSearch;
unsigned aaWidth=0,aaHeight=0,aaSamples=0,aaLastSlot=0;
antialias::Algorithm aaAlgorithm=antialias::Algorithm::Count;
bool aaEnabled=false;
int aaFrame=0;
std::vector<unsigned> aaIndices;
bool aaBackground=false,aaWireframe=false;
ComPtr<ID3D12RootSignature> depthResolveRoot;
ComPtr<ID3D12PipelineState> depthResolvePso;
ComPtr<ID3D12DescriptorHeap> depthResolveSrvs;

// Viewport ABI v1 exposes a single-sample depth texture, even with MSAA.
ID3D12Resource* ResolvePluginDepth(Frame& frame,unsigned slot) {
    if(!sampleIndex)return frame.depth.Get();
    if(!depthResolveRoot) {
        D3D12_ROOT_PARAMETER parameter{};D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
        parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameter.DescriptorTable={1,&range};
        Root(&parameter,1,depthResolveRoot);
        D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors=3;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check12(Device()->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&depthResolveSrvs)),"native depth resolve SRVs");
        const char* shader=R"(
float4 vs(uint id:SV_VertexID):SV_Position {return float4(float2((id<<1)&2,id&2)*float2(2,-2)+float2(-1,1),0,1);}
Texture2DMS<float> source:register(t0);
float ps(float4 position:SV_Position):SV_Depth {
    uint w,h,samples;source.GetDimensions(w,h,samples);float depth=1;
    for(uint i=0;i<samples;++i)depth=min(depth,source.Load(int2(position.xy),i));return depth;
})";
        auto vs=Compile(shader,"vs","vs_5_1"),ps=Compile(shader,"ps","ps_5_1");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=depthResolveRoot.Get();
        p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;
        p.SampleMask=UINT_MAX;p.SampleDesc.Count=1;p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.DSVFormat=DXGI_FORMAT_D32_FLOAT;p.DepthStencilState.DepthEnable=TRUE;
        p.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        p.DepthStencilState.FrontFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};p.DepthStencilState.BackFace=p.DepthStencilState.FrontFace;
        auto& blend=p.BlendState.RenderTarget[0];blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE;
        blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_ZERO;blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.LogicOp=D3D12_LOGIC_OP_NOOP;
        Check12(Device()->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&depthResolvePso)),"native depth resolve pipeline");
    }
    auto dsv=dsvs->GetCPUDescriptorHandleForHeapStart();dsv.ptr+=(slot+3)*Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    if(!frame.resolvedDepth) {
        auto desc=frame.depth->GetDesc();desc.SampleDesc.Count=1;desc.SampleDesc.Quality=0;desc.Alignment=0;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_CLEAR_VALUE clear{};clear.Format=DXGI_FORMAT_D32_FLOAT;clear.DepthStencil.Depth=1;
        Check12(Device()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_DEPTH_WRITE,&clear,IID_PPV_ARGS(&frame.resolvedDepth)),"native resolved depth");
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};view.Format=DXGI_FORMAT_D32_FLOAT;view.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
        Device()->CreateDepthStencilView(frame.resolvedDepth.Get(),&view,dsv);
    }
    auto cpu=depthResolveSrvs->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=slot*srvStep;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32_FLOAT;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DMS;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    Device()->CreateShaderResourceView(frame.depth.Get(),&srv,cpu);
    auto barrier=Barrier(frame.depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);frame.list->ResourceBarrier(1,&barrier);
    ID3D12DescriptorHeap* heap=depthResolveSrvs.Get();frame.list->SetDescriptorHeaps(1,&heap);
    frame.list->SetGraphicsRootSignature(depthResolveRoot.Get());frame.list->SetPipelineState(depthResolvePso.Get());
    auto gpu=depthResolveSrvs->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=slot*srvStep;frame.list->SetGraphicsRootDescriptorTable(0,gpu);
    frame.list->OMSetRenderTargets(0,nullptr,FALSE,&dsv);frame.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);frame.list->DrawInstanced(3,1,0,0);
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);frame.list->ResourceBarrier(1,&barrier);
    return frame.resolvedDepth.Get();
}

static bool AaPost(antialias::Algorithm a) { return a>=antialias::Algorithm::Fxaa && a<antialias::Algorithm::Count; }
static bool AaTemporalMode(antialias::Algorithm a) {
    return a==antialias::Algorithm::Taa || a==antialias::Algorithm::SmaaT2xStandard || a==antialias::Algorithm::SmaaT2xEnhanced;
}
static void ReplaceShader(std::string& s,const std::string& from,const std::string& to) {
    size_t at=0;while((at=s.find(from,at))!=std::string::npos){s.replace(at,from.size(),to);at+=to.size();}
}
static std::string ModernShader(const char* original) {
    std::string s=original;
    const std::regex constant(R"(float4\s+(\w+)\s*:\s*register\(c(\d+)\)\s*;)");
    s=std::regex_replace(s,constant,"\n#define $1 aaConstants[$2]\n");
    s=std::regex_replace(s,std::regex(R"(register\(s(\d+)\))"),"register(t$1)");
    ReplaceShader(s,": COLOR0",": SV_Target0");
    ReplaceShader(s,"tex2D(motion,","aaPoint(motion,");
    ReplaceShader(s,"tex2D(previousDepth,","aaPoint(previousDepth,");
    ReplaceShader(s,"tex2D(smaaPreviousDepth,","aaPoint(smaaPreviousDepth,");
    ReplaceShader(s,"tex2D(smaaSearch,","aaPoint(smaaSearch,");
    return std::string(R"(
cbuffer NativeAA : register(b0) { float4 aaConstants[6]; }
SamplerState aaLinear : register(s0);
SamplerState aaNearest : register(s1);
#define sampler2D Texture2D
#define tex2D(t,uv) t.SampleLevel(aaLinear,uv,0)
#define tex2Dlod(t,uv) t.SampleLevel(aaLinear,(uv).xy,(uv).w)
#define tex2Dgrad(t,uv,dx,dy) t.SampleGrad(aaLinear,uv,dx,dy)
float4 aaPoint(Texture2D t,float2 uv) {return t.SampleLevel(aaNearest,uv,0);}
)")+s;
}
inline static constexpr char aaQuad[]=R"(
struct Quad {float2 uv:TEXCOORD0;float4 position:SV_Position;};
Quad vs(uint id:SV_VertexID) {
    Quad o;o.uv=float2((id<<1)&2,id&2);o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;
}
Texture2D image:register(t0);SamplerState linearClamp:register(s0);
float4 copy(float2 uv:TEXCOORD0):SV_Target0{return image.SampleLevel(linearClamp,uv,0);}
)";
void AaPipeline(ComPtr<ID3D12PipelineState>& pipeline,const char* shader,const char* entry) {
    if(pipeline)return;
    const auto vs=Compile(aaQuad,"vs","vs_5_1"),ps=Compile(shader,entry,"ps_5_1");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=aaRoot.Get();
    p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable=TRUE;p.SampleMask=UINT_MAX;p.SampleDesc.Count=1;
    p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.NumRenderTargets=1;
    p.RTVFormats[0]=DXGI_FORMAT_B8G8R8A8_UNORM;p.BlendState.RenderTarget[0].RenderTargetWriteMask=15;
    auto& blend=p.BlendState.RenderTarget[0];blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE;
    blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_ZERO;blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;
    blend.LogicOp=D3D12_LOGIC_OP_NOOP;
    p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    p.DepthStencilState.FrontFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
    p.DepthStencilState.BackFace=p.DepthStencilState.FrontFace;
    const HRESULT result=Device()->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&pipeline));
    if(FAILED(result)) {
        ComPtr<ID3D12InfoQueue> diagnostics;
        if(SUCCEEDED(Device()->QueryInterface(IID_PPV_ARGS(&diagnostics)))&&diagnostics->GetNumStoredMessages()) {
            const auto index=diagnostics->GetNumStoredMessages()-1;SIZE_T size=0;diagnostics->GetMessage(index,nullptr,&size);
            std::vector<unsigned char> bytes(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            if(SUCCEEDED(diagnostics->GetMessage(index,message,&size)))throw std::runtime_error(std::string("Native AA ")+entry+": "+message->pDescription);
        }
    }
    Check12(result,"native antialias pipeline");
}
void AaTexture(ComPtr<ID3D12Resource>& texture,DXGI_FORMAT format,unsigned w,unsigned h) {
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
    d.DepthOrArraySize=1;d.MipLevels=1;d.Format=format;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    Check12(Device()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&texture)),"native antialias texture");
}
D3D12_CPU_DESCRIPTOR_HANDLE AaRtv(ID3D12Resource* texture,unsigned index) {
    auto handle=aaRtvs->GetCPUDescriptorHandleForHeapStart();handle.ptr+=index*rtvStep;
    Device()->CreateRenderTargetView(texture,nullptr,handle);return handle;
}
void AaLookup(ComPtr<ID3D12Resource>& texture,Frame& frame,unsigned w,unsigned h,unsigned channels,const unsigned char* bytes) {
    if(texture)return;
    AaTexture(texture,DXGI_FORMAT_B8G8R8A8_UNORM,w,h);
    const auto desc=texture->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 total;
    Device()->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&total);
    ComPtr<ID3D12Resource> upload;Buffer(upload,total,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void* data;D3D12_RANGE empty{0,0};Check12(upload->Map(0,&empty,&data),"native SMAA lookup upload");
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
        const auto* value=bytes+(y*w+x)*channels;
        reinterpret_cast<DWORD*>(static_cast<char*>(data)+footprint.Offset+y*footprint.Footprint.RowPitch)[x]=
            0xff000000u|(unsigned(value[0])<<16)|(unsigned(channels==2?value[1]:value[0])<<8);
    }
    upload->Unmap(0,nullptr);
    auto barrier=Barrier(texture.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);frame.list->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=texture.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprint;
    frame.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);frame.list->ResourceBarrier(1,&barrier);frame.uploads.push_back(upload);
}
void AaPrepare(MMDApp* app,Frame& frame) {
    const bool post=app->antialiasing.enabled && AaPost(app->antialiasing.algorithm);
    if(aaAlgorithm!=app->antialiasing.algorithm || aaEnabled!=post){aaSamples=0;aaAlgorithm=app->antialiasing.algorithm;aaEnabled=post;}
    if(!post)return;
    if(!aaRoot) {
        D3D12_ROOT_PARAMETER parameters[2]{};D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,6,0,0,0};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[0].DescriptorTable={1,&range};
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[1].Constants.Num32BitValues=24;
        D3D12_STATIC_SAMPLER_DESC samplers[2]{};
        for(unsigned i=0;i<2;++i){samplers[i].Filter=i?D3D12_FILTER_MIN_MAG_MIP_POINT:D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samplers[i].AddressU=samplers[i].AddressV=samplers[i].AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[i].MaxLOD=D3D12_FLOAT32_MAX;samplers[i].ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;samplers[i].ShaderRegister=i;}
        Root(parameters,2,aaRoot,samplers,2);
        D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=3*64;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check12(Device()->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&aaSrvs)),"native antialias SRVs");
        heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=3*16;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        Check12(Device()->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&aaRtvs)),"native antialias RTVs");
        AaPipeline(aaCopy,aaQuad,"copy");
    }
    if(aaWidth!=width || aaHeight!=height) {
        WaitAll();aaSamples=0;aaWidth=width;aaHeight=height;
        for(auto& f:aaFrames)for(auto* texture:{std::addressof(f.source),std::addressof(f.edges),std::addressof(f.weights),std::addressof(f.spatial),std::addressof(f.resolved),std::addressof(f.motion)})
            AaTexture(*texture,texture==std::addressof(f.motion)?DXGI_FORMAT_R32G32B32A32_FLOAT:DXGI_FORMAT_B8G8R8A8_UNORM,width,height);
    }
    if(aaAlgorithm==antialias::Algorithm::Fxaa || aaAlgorithm==antialias::Algorithm::Taa) {
        AaPipeline(aaFxaa,ModernShader(antialias::kFxaaShader).c_str(),"fxaa");
        if(aaAlgorithm==antialias::Algorithm::Taa)AaPipeline(aaTemporal,ModernShader(antialias::kShader).c_str(),"temporal");
    } else {
        const bool temporal=AaTemporalMode(aaAlgorithm);
        const bool enhanced=aaAlgorithm==antialias::Algorithm::Smaa || aaAlgorithm==antialias::Algorithm::SmaaT2xEnhanced;
        const unsigned preset=unsigned(enhanced)+2*unsigned(temporal);
        const auto shader=std::string(enhanced?"#define MMS_SMAA_ENHANCED 1\n":"#define MMS_SMAA_ENHANCED 0\n")+
            (temporal?"#define MMS_SMAA_TEMPORAL 1\n":"#define MMS_SMAA_TEMPORAL 0\n")+ModernShader(antialias::kSmaaShader);
        const char* entries[]={"smaaEdges","smaaWeights","smaaResolve","smaaTemporal"};
        for(unsigned i=0;i<(temporal?4u:3u);++i)AaPipeline(aaSmaa[preset][i],shader.c_str(),entries[i]);
        AaLookup(aaArea,frame,AREATEX_WIDTH,AREATEX_HEIGHT,2,areaTexBytes);
        AaLookup(aaSearch,frame,SEARCHTEX_WIDTH,SEARCHTEX_HEIGHT,1,searchTexBytes);
    }
}
void AaDraw(ID3D12GraphicsCommandList* list,unsigned slot,unsigned pass,ID3D12Resource* output,
    ID3D12PipelineState* pipeline,const std::array<ID3D12Resource*,6>& input,const float* constants) {
    const D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};const D3D12_RECT rect{0,0,LONG(width),LONG(height)};
    list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&rect);
    auto cpu=aaSrvs->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=(slot*64+pass*6)*srvStep;
    for(auto* resource:input) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=resource->GetDesc().Format;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;
        Device()->CreateShaderResourceView(resource,&d,cpu);cpu.ptr+=srvStep;
    }
    auto transition=Barrier(output,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);list->ResourceBarrier(1,&transition);
    const auto rtv=AaRtv(output,slot*16+pass);list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    const float zero[4]{};list->ClearRenderTargetView(rtv,zero,0,nullptr);
    ID3D12DescriptorHeap* heap=aaSrvs.Get();list->SetDescriptorHeaps(1,&heap);list->SetGraphicsRootSignature(aaRoot.Get());list->SetPipelineState(pipeline);
    auto gpu=aaSrvs->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=(slot*64+pass*6)*srvStep;
    list->SetGraphicsRootDescriptorTable(0,gpu);list->SetGraphicsRoot32BitConstants(1,24,constants,0);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(3,1,0,0);
    std::swap(transition.Transition.StateBefore,transition.Transition.StateAfter);list->ResourceBarrier(1,&transition);
}
void AaCopyResource(ID3D12GraphicsCommandList* list,ID3D12Resource* destination,ID3D12Resource* source,
    D3D12_RESOURCE_STATES dstState,D3D12_RESOURCE_STATES srcState) {
    D3D12_RESOURCE_BARRIER barriers[]={Barrier(source,srcState,D3D12_RESOURCE_STATE_COPY_SOURCE),Barrier(destination,dstState,D3D12_RESOURCE_STATE_COPY_DEST)};
    list->ResourceBarrier(2,barriers);list->CopyResource(destination,source);
    for(auto& b:barriers)std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(2,barriers);
}
void AaFinish(ID3D12GraphicsCommandList* list,unsigned slot) {
    if(!aaEnabled)return;
    auto& f=aaFrames[slot];auto& old=aaFrames[aaSamples?aaLastSlot:slot];auto* source=f.source.Get();
    AaCopyResource(list,source,frames[slot].color.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    float values[24]{};values[0]=1.f/width;values[1]=1.f/height;
    values[4]=.5f/width;values[5]=.5f/height;values[6]=1-values[4];values[7]=1-values[5];
    const auto inputs=[&](ID3D12Resource* a,ID3D12Resource* b=nullptr,ID3D12Resource* c=nullptr,ID3D12Resource* d=nullptr,ID3D12Resource* e=nullptr,ID3D12Resource* g=nullptr){
        return std::array<ID3D12Resource*,6>{a,b?b:source,c?c:source,d?d:source,e?e:source,g?g:source};};
    if(aaAlgorithm==antialias::Algorithm::Fxaa || aaAlgorithm==antialias::Algorithm::Taa) {
        AaDraw(list,slot,0,f.spatial.Get(),aaFxaa.Get(),inputs(source),values);
        if(aaAlgorithm==antialias::Algorithm::Taa) {
            values[2]=aaSamples?std::min(.95f,float(aaSamples)/(aaSamples+1)):0;
            values[8]=f.jitter[0];values[9]=f.jitter[1];values[10]=values[11]=.002f;
            values[12]=old.jitter[0];values[13]=old.jitter[1];
            AaDraw(list,slot,1,f.resolved.Get(),aaTemporal.Get(),inputs(f.spatial.Get(),aaSamples?old.resolved.Get():source,f.motion.Get(),aaSamples?old.motion.Get():f.motion.Get()),values);
        } else AaCopyResource(list,f.resolved.Get(),f.spatial.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    } else {
        const bool temporal=AaTemporalMode(aaAlgorithm);
        const bool enhanced=aaAlgorithm==antialias::Algorithm::Smaa || aaAlgorithm==antialias::Algorithm::SmaaT2xEnhanced;
        auto& pipeline=aaSmaa[unsigned(enhanced)+2*unsigned(temporal)];
        values[2]=float(width);values[3]=float(height);
        if(temporal)values[8]=values[9]=values[10]=aaSamples%2?2.f:1.f;
        values[12]=aaSamples?1.f:0.f;values[13]=values[14]=.002f;
        values[20]=f.jitter[0];values[21]=f.jitter[1];values[22]=old.jitter[0];values[23]=old.jitter[1];
        AaDraw(list,slot,0,f.edges.Get(),pipeline[0].Get(),inputs(source),values);
        AaDraw(list,slot,1,f.weights.Get(),pipeline[1].Get(),inputs(f.edges.Get(),aaArea.Get(),aaSearch.Get()),values);
        AaDraw(list,slot,2,f.spatial.Get(),pipeline[2].Get(),inputs(source,f.weights.Get(),f.motion.Get()),values);
        if(temporal)AaDraw(list,slot,3,f.resolved.Get(),pipeline[3].Get(),inputs(f.spatial.Get(),old.spatial.Get(),f.motion.Get(),old.motion.Get(),source,f.weights.Get()),values);
        else AaCopyResource(list,f.resolved.Get(),f.spatial.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    AaCopyResource(list,frames[slot].color.Get(),f.resolved.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    aaLastSlot=slot;aaSamples=std::min(aaSamples+1,64u);
}
