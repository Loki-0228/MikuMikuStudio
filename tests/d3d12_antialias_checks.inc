// Record a real viewport pass against the real device and read its depth back.
// Only callback registration is intercepted; all render/copy commands use D3D12.
struct DepthProbe {
    inline static DepthProbe* active=nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT width=0,height=0,calls=0;
    std::string error;
    DepthProbe() {
        Check(MH_Initialize()==MH_OK,"initialize owned viewport callback probe");active=this;
        Check(MH_CreateHook(reinterpret_cast<void*>(&plugins::HasDx12ViewportPass),reinterpret_cast<void*>(&Has),nullptr)==MH_OK,"register viewport availability probe");
        Check(MH_CreateHook(reinterpret_cast<void*>(&plugins::RecordDx12ViewportPass),reinterpret_cast<void*>(&Record),nullptr)==MH_OK,"register depth readback callback");
        Check(MH_EnableHook(reinterpret_cast<void*>(&plugins::HasDx12ViewportPass))==MH_OK,"enable viewport availability probe");
        Check(MH_EnableHook(reinterpret_cast<void*>(&plugins::RecordDx12ViewportPass))==MH_OK,"enable depth readback callback");
    }
    ~DepthProbe() {
        WaitD3D12Native();
        for(auto function:{reinterpret_cast<void*>(&plugins::HasDx12ViewportPass),reinterpret_cast<void*>(&plugins::RecordDx12ViewportPass)}){MH_DisableHook(function);MH_RemoveHook(function);}
        MH_Uninitialize();active=nullptr;
    }
    static bool Has() noexcept {return true;}
    static std::int32_t Record(const MmsDx12ViewportFrameV1& frame) noexcept {
        auto& self=*active;
        try {
            const auto desc=frame.depth->GetDesc();
            Check(desc.SampleDesc.Count==1&&desc.Format==DXGI_FORMAT_R32_TYPELESS,"viewport plugins always receive single-sample typeless depth");
            UINT64 bytes;frame.device->GetCopyableFootprints(&desc,0,1,0,&self.footprint,nullptr,nullptr,&bytes);
            D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;
            buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;self.readback.Reset();
            Hr(frame.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&self.readback)),"allocate plugin depth readback");
            Check(frame.retain(frame.lifetime,self.readback.Get())==MMS_OK,"retain plugin depth readback through submission");
            D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition={frame.depth,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_COPY_SOURCE};
            frame.commands->ResourceBarrier(1,&barrier);
            D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=frame.depth;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=self.readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=self.footprint;
            frame.commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);frame.commands->ResourceBarrier(1,&barrier);
            self.width=frame.width;self.height=frame.height;++self.calls;return MMS_OK;
        }catch(const std::exception& e){self.error=e.what();return MMS_E_NOT_READY;}
    }
    void Verify() {
        Check(error.empty()&&calls&&readback,"viewport callback recorded successfully");
        void* data;Hr(readback->Map(0,nullptr,&data),"read actual plugin depth");
        const auto* row=reinterpret_cast<const float*>(static_cast<const char*>(data)+footprint.Offset+(height/2)*footprint.Footprint.RowPitch);
        const float center=row[width/2];D3D12_RANGE empty{0,0};readback->Unmap(0,&empty);
        Check(std::abs(center-.2f)<.0001f,"plugin depth resolve preserves nearest model depth");
    }
};

void MipChecks(Host& host,Geometry& geometry,const fs::path& output) {
    ShutdownD3D12Native();host.app->antialiasing.enabled=false;
    for(auto& vertex:geometry.vertices){vertex.uv[0]=(vertex.position[0]+1)*32;vertex.uv[1]=(vertex.position[1]+1)*32;}
    model_vertex_cache::Forget(reinterpret_cast<unsigned char*>(geometry.record.get()));
    Microsoft::WRL::ComPtr<IDirect3DTexture9> texture;
    Hr(host.device->CreateTexture(64,64,0,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,nullptr),"create complete source mip chain");
    Check(texture->GetLevelCount()==7,"source texture exposes seven mips");
    for(unsigned level=0;level<texture->GetLevelCount();++level) {
        D3DLOCKED_RECT rect{};D3DSURFACE_DESC desc{};Hr(texture->GetLevelDesc(level,&desc),"read source mip size");
        Hr(texture->LockRect(level,&rect,nullptr,0),"fill source mip");
        for(unsigned y=0;y<desc.Height;++y)std::fill_n(reinterpret_cast<DWORD*>(static_cast<char*>(rect.pBits)+y*rect.Pitch),desc.Width,level?0xff00ff00u:0xffff0000u);
        texture->UnlockRect(level);
    }
    wcscpy_s(geometry.materials[0].texturePath,L"owned-mip-fixture");
    auto& entry=host.renderer->resourcePool[0];entry.heapBuffer=_wcsdup(L"owned-mip-fixture");entry.comObject=texture.Detach();
    const DWORD center=Capture(host,output/L"mip-chain.bmp");
    Check(((center>>8)&255)>240&&((center>>16)&255)<8,"minification samples uploaded green mips instead of red mip zero");
    ShutdownD3D12Native();entry.comObject->Release();entry.comObject=nullptr;
    Hr(host.device->CreateTexture(63,31,0,D3DUSAGE_AUTOGENMIPMAP,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,nullptr),"create source with hidden automatic mips");
    Check(texture->GetLevelCount()==1,"D3D9 exposes only mip zero for automatic chain");
    D3DLOCKED_RECT rect{};Hr(texture->LockRect(0,&rect,nullptr,0),"fill automatic mip source");
    for(unsigned y=0;y<31;++y)for(unsigned x=0;x<63;++x)
        reinterpret_cast<DWORD*>(static_cast<char*>(rect.pBits)+y*rect.Pitch)[x]=(x+y)%2?0xffffffffu:0xff000000u;
    texture->UnlockRect(0);entry.comObject=texture.Detach();
    const DWORD reduced=Capture(host,output/L"autogen-mip-chain.bmp");
    for(unsigned shift:{0u,8u,16u})Check(std::abs(int((reduced>>shift)&255)-128)<5,"automatic odd-sized checker mips converge to grey without aliasing");
    ShutdownD3D12Native();entry.comObject->Release();free(entry.heapBuffer);entry={};geometry.materials[0].texturePath[0]=0;
}
