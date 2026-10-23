#pragma once
// Private include for transparent_renderer_policy_tests.cpp. Include after
// Host/Model/Uploads/Draws/MakeModel/SourceUnchanged/Install, in their anonymous namespace.
// This is not an SDK header. It needs no changes to the shared capture classes.
namespace maximum_renderer_checks {

// The base fixture shadows IB uploads. Maximum also needs the actual expanded
// VB bytes, which cannot be read back by locking a WRITEONLY buffer at draw time.
// Only our device's WRITEONLY VBs are tracked, with image-backed private tables.
class VertexUploads {
public:
    struct Buffer {
        IDirect3DVertexBuffer9* object=nullptr;
        void** original=nullptr;
        std::array<unsigned char,16384> bytes{};
        UINT size=0,offset=0,length=0;
        DWORD flags=0;
        void* locked=nullptr;
        bool uploaded=false;
    };
    using CreateFn=HRESULT(WINAPI*)(IDirect3DDevice9*,UINT,DWORD,DWORD,D3DPOOL,IDirect3DVertexBuffer9**,HANDLE*);
    using LockFn=HRESULT(WINAPI*)(IDirect3DVertexBuffer9*,UINT,UINT,void**,DWORD);
    using UnlockFn=HRESULT(WINAPI*)(IDirect3DVertexBuffer9*);
    using ReleaseFn=ULONG(WINAPI*)(IDirect3DVertexBuffer9*);
    inline static VertexUploads* active=nullptr;
    inline static std::array<std::array<void*,14>,8> tables{};
    std::array<Buffer,8> buffers{};
    IDirect3DDevice9* device=nullptr;
    void* target=nullptr;
    CreateFn originalCreate=nullptr;
    unsigned created=0,freed=0;
    const char* error=nullptr;

    explicit VertexUploads(IDirect3DDevice9* owned):device(owned) {
        Check(!active,"one Maximum VB tracker at a time");
        MEMORY_BASIC_INFORMATION region{};
        Check(VirtualQuery(tables.data(),&region,sizeof(region))!=0 && region.Type==MEM_IMAGE && region.State==MEM_COMMIT,
              "Maximum VB private vtables belong to the test image");
        // IDirect3DVertexBuffer9 inherits the same 11 Resource slots as IB:
        // Release=2, Lock=11, Unlock=12, GetDesc=13. Device CreateVB=26.
        target=(*reinterpret_cast<void***>(device))[26];
        Check(MH_CreateHook(target,reinterpret_cast<void*>(&Create),reinterpret_cast<void**>(&originalCreate))==MH_OK,
              "install owned-device Maximum VB upload hook");
        active=this;
        if(MH_EnableHook(target)!=MH_OK) {
            active=nullptr; MH_RemoveHook(target); target=nullptr;
            Check(false,"enable owned-device Maximum VB upload hook");
        }
    }
    ~VertexUploads() {
        for(auto& b:buffers) if(b.object) *reinterpret_cast<void***>(b.object)=b.original;
        if(target) { MH_DisableHook(target); MH_RemoveHook(target); }
        active=nullptr;
    }
    Buffer* Find(IDirect3DVertexBuffer9* object) noexcept {
        if(!object) return nullptr;
        for(auto& b:buffers) if(b.object==object) return &b;
        return nullptr;
    }
    static HRESULT WINAPI Create(IDirect3DDevice9* d,UINT size,DWORD usage,DWORD fvf,D3DPOOL pool,
                                 IDirect3DVertexBuffer9** out,HANDLE* shared) {
        auto& self=*active;
        const HRESULT hr=self.originalCreate(d,size,usage,fvf,pool,out,shared);
        if(d!=self.device || !(usage&D3DUSAGE_WRITEONLY) || FAILED(hr) || !out || !*out) return hr;
        Buffer* b=nullptr;
        for(auto& candidate:self.buffers) if(!candidate.object) { b=&candidate; break; }
        if(!b || size>b->bytes.size()) {
            self.error="Maximum VB shadow capacity exceeded"; (*out)->Release(); *out=nullptr; return E_OUTOFMEMORY;
        }
        *b=Buffer{}; b->object=*out; b->size=size; b->original=*reinterpret_cast<void***>(*out);
        auto& table=tables[static_cast<std::size_t>(b-self.buffers.data())];
        std::copy_n(b->original,table.size(),table.begin());
        table[2]=reinterpret_cast<void*>(&Drop); table[11]=reinterpret_cast<void*>(&Lock); table[12]=reinterpret_cast<void*>(&Unlock);
        *reinterpret_cast<void***>(*out)=table.data();
        if(!PlausibleD3dObject(*out)) {
            self.error="Maximum VB capture failed production resource validation";
            *reinterpret_cast<void***>(*out)=b->original; b->object=nullptr;
            (*out)->Release(); *out=nullptr; return E_FAIL;
        }
        ++self.created;
        return hr;
    }
    static ULONG WINAPI Drop(IDirect3DVertexBuffer9* object) {
        auto& self=*active; auto* b=self.Find(object); if(!b) std::terminate();
        const auto call=reinterpret_cast<ReleaseFn>(b->original[2]);
        const ULONG remaining=call(object);
        if(!remaining) { b->object=nullptr; b->locked=nullptr; ++self.freed; }
        return remaining;
    }
    static HRESULT WINAPI Lock(IDirect3DVertexBuffer9* object,UINT offset,UINT length,void** data,DWORD flags) {
        auto& self=*active; auto* b=self.Find(object); if(!b) std::terminate();
        const HRESULT hr=reinterpret_cast<LockFn>(b->original[11])(object,offset,length,data,flags);
        if(SUCCEEDED(hr) && data) {
            b->locked=*data; b->offset=offset; b->flags=flags;
            b->length=length?length:(offset<=b->size?b->size-offset:0);
            if(offset>b->size || b->length>b->size-offset) self.error="Maximum VB native lock outside allocation";
        }
        return hr;
    }
    static HRESULT WINAPI Unlock(IDirect3DVertexBuffer9* object) {
        auto& self=*active; auto* b=self.Find(object); if(!b) std::terminate();
        if(b->locked && !(b->flags&D3DLOCK_READONLY) && b->offset<=b->size && b->length<=b->size-b->offset) {
            std::memcpy(b->bytes.data()+b->offset,b->locked,b->length); b->uploaded=true;
        }
        const HRESULT hr=reinterpret_cast<UnlockFn>(b->original[12])(object); b->locked=nullptr; return hr;
    }
    void CheckReleased() {
        Check(!error,error?error:"Maximum upload tracker healthy");
        for(const auto& b:buffers) Check(!b.object && !b.locked,"Maximum frame releases expanded VB and its lock");
        Check(created==freed,"Maximum expanded VB reference counts return to zero");
    }
};

class Fragments : public deformed_mesh_capture::DrawObserver {
public:
    struct Face { const void* object=nullptr; unsigned material=0; std::array<std::array<float,24>,3> vertex{}; };
    VertexUploads& vertices; Uploads& indices;
    std::array<Face,64> faces{};
    unsigned count=0;
    const char* error=nullptr;
    bool observing=false;
    Fragments(MMDApp& app,VertexUploads& v,Uploads& i):vertices(v),indices(i) {
        std::string reason;
        Check(deformed_mesh_capture::Observe(app,this,reason),reason.empty()?"register Maximum uploaded-vertex observer":reason.c_str());
        observing=true;
    }
    ~Fragments() { if(observing) deformed_mesh_capture::StopObserving(this); }
    void Reset() { count=0; error=nullptr; }
    void Indexed(IDirect3DDevice9* d,D3DPRIMITIVETYPE type,INT base,UINT,UINT,UINT start,UINT primitives,
                 const void* object,unsigned material) noexcept override {
        if(d!=vertices.device || !object) return;
        IDirect3DVertexBuffer9* vb=nullptr; IDirect3DIndexBuffer9* ib=nullptr; UINT offset=0,stride=0;
        const HRESULT vh=d->GetStreamSource(0,&vb,&offset,&stride),ih=d->GetIndices(&ib);
        auto* v=vertices.Find(vb); auto* i=indices.Find(ib);
        if(type!=D3DPT_TRIANGLELIST || base!=0 || offset!=0 || stride!=96 || FAILED(vh) || FAILED(ih) ||
           !v || !i || !v->uploaded || !i->uploaded || i->format!=D3DFMT_INDEX32) error="Maximum did not draw captured 96-byte generated geometry";
        else if(primitives>faces.size()-count || start>i->size/4 || primitives*3>i->size/4-start) error="Maximum captured fragment/index capacity exceeded";
        else for(UINT f=0;f<primitives;++f) {
            Face face; face.object=object; face.material=material;
            for(UINT c=0;c<3;++c) {
                UINT index=0; std::memcpy(&index,i->bytes.data()+4*(start+3*f+c),4);
                if(index>=v->size/96) { error="Maximum generated index exceeds actual VB upload"; break; }
                std::memcpy(face.vertex[c].data(),v->bytes.data()+96*index,96);
            }
            if(error) break;
            faces[count++]=face;
        }
        Release(vb); Release(ib);
    }
    void Unsupported(IDirect3DDevice9* d,D3DPRIMITIVETYPE,UINT) noexcept override {
        if(d==vertices.device) error="Maximum emitted unsupported material geometry";
    }
};

struct Source { Model* model; unsigned material; };
double Cross(double ax,double ay,double bx,double by,double cx,double cy) {
    return (bx-ax)*(cy-ay)-(by-ay)*(cx-ax);
}
std::array<double,3> Weights(double x,double y,const Vertex& a,const Vertex& b,const Vertex& c) {
    const double denominator=Cross(a.x,a.y,b.x,b.y,c.x,c.y);
    return {Cross(x,y,b.x,b.y,c.x,c.y)/denominator,
            Cross(a.x,a.y,x,y,c.x,c.y)/denominator,
            Cross(a.x,a.y,b.x,b.y,x,y)/denominator};
}
std::array<double,3> Weights(double x,double y,const Fragments::Face& f) {
    const Vertex a{f.vertex[0][0],f.vertex[0][1]},b{f.vertex[1][0],f.vertex[1][1]},c{f.vertex[2][0],f.vertex[2][1]};
    return Weights(x,y,a,b,c);
}
void VerifyFragments(const Fragments& captured,const Draws& draws,const std::vector<Source>& sources) {
    Check(!captured.error,captured.error?captured.error:"Maximum fragment capture healthy");
    Check(captured.count==draws.count && captured.count>sources.size(),"Maximum really split source faces into multiple submitted fragments");
    Check(captured.count<captured.faces.size(),"Maximum fragment fixture fits observer capacity");
    std::array<double,8> area{}; std::array<unsigned,8> fragments{};
    Check(sources.size()<=area.size(),"Maximum source fixture fits independent area oracle");
    for(unsigned n=0;n<captured.count;++n) {
        const auto& face=captured.faces[n]; const auto& actual=draws.faces[n];
        unsigned source=0;
        while(source<sources.size() && (sources[source].model->record.get()!=face.object || sources[source].material!=face.material)) ++source;
        Check(source<sources.size(),"every generated fragment keeps a real source object/material");
        Check(actual.object==face.object && actual.material==face.material,"independent observers agree on actual material identity");
        Check(actual.stride==96 && actual.scratch && actual.ib!=sources[source].model->ib,"Maximum binds generated stride and private INDEX32");
        for(const auto& s:sources) Check(actual.vb!=s.model->vb,"Maximum expanded VB is not any source VB");
        Check(actual.vb==draws.faces[0].vb && actual.ib==draws.faces[0].ib,"all generated runs use the same fully prepared upload");
        Check(actual.indices==std::array<UINT,3>{3*n,3*n+1,3*n+2},"Maximum actual uploaded indices address sequential generated vertices");
        const auto& model=*sources[source].model; const auto& ids=model.faces[sources[source].material];
        const auto& a=model.vertices[ids[0]]; const auto& b=model.vertices[ids[1]]; const auto& c=model.vertices[ids[2]];
        const double original=Cross(a.x,a.y,b.x,b.y,c.x,c.y);
        const double fragment=Cross(face.vertex[0][0],face.vertex[0][1],face.vertex[1][0],face.vertex[1][1],face.vertex[2][0],face.vertex[2][1]);
        Check(fragment*original>0 && std::abs(fragment)>1e-9,"generated fragment has nonzero area and original winding");
        area[source]+=std::abs(fragment); ++fragments[source];
        for(const auto& vertex:face.vertex) {
            Check(std::isfinite(vertex[0]) && std::isfinite(vertex[1]) && std::isfinite(vertex[2]),"generated position is finite");
            const auto weights=Weights(vertex[0],vertex[1],a,b,c);
            for(double w:weights) Check(w>=-2e-5 && w<=1+2e-5,"generated vertex lies inside its original triangle");
            const double z=weights[0]*a.z+weights[1]*b.z+weights[2]*c.z;
            Check(std::abs(z-vertex[2])<2e-5,"generated XYZ satisfies independent source-plane interpolation");
        }
    }
    bool sawSplit=false;
    for(unsigned source=0;source<sources.size();++source) {
        const auto& model=*sources[source].model; const auto& ids=model.faces[sources[source].material];
        const auto& a=model.vertices[ids[0]]; const auto& b=model.vertices[ids[1]]; const auto& c=model.vertices[ids[2]];
        const double original=std::abs(Cross(a.x,a.y,b.x,b.y,c.x,c.y));
        Check(fragments[source]>0 && std::abs(area[source]-original)<2e-5,"fragment areas cover exactly the original face area");
        sawSplit|=fragments[source]>1;
        unsigned samples=0;
        // Offset interior lattice avoids x=0 and most generated boundaries.
        // Skip exact fragment edges, then require each source sample once.
        for(int iy=1;iy<8;++iy) for(int ix=1;ix<8;++ix) {
            const double u=ix/10.0,v=iy/10.0;
            if(u+v>=.9) continue;
            const double x=(1-u-v)*a.x+u*b.x+v*c.x,y=(1-u-v)*a.y+u*b.y+v*c.y;
            unsigned coverage=0; bool edge=false;
            for(unsigned f=0;f<captured.count;++f) {
                const auto& face=captured.faces[f];
                if(face.object!=model.record.get() || face.material!=sources[source].material) continue;
                const auto weights=Weights(x,y,face);
                const double minimum=std::min(weights[0],std::min(weights[1],weights[2]));
                if(minimum>=-1e-6 && minimum<=1e-6) edge=true;
                if(minimum>1e-6) ++coverage;
            }
            if(!edge) { ++samples; Check(coverage==1,"interior source point is covered by exactly one generated fragment"); }
        }
        Check(samples>=5,"independent coverage oracle sampled enough non-boundary points");
        std::printf("maximum source=%u fragments=%u area=%.8g expected=%.8g interior-samples=%u\n",source,fragments[source],area[source],original,samples);
    }
    Check(sawSplit,"crossing geometry produced an actual split, not two unchanged faces");
}

void UploadSource(Model& model) {
    void* memory=nullptr; Hr(model.vb->Lock(0,0,&memory,0),"lock Maximum source fixture update");
    std::memcpy(memory,model.vertices.data(),model.vertices.size()*sizeof(Vertex));
    Hr(model.vb->Unlock(),"upload Maximum source fixture update");
}
void VerifySource(Model& model,bool pmx,const std::vector<mdl::ModelMaterialRecord>& materials) {
    SourceUnchanged(model,pmx);
    Check(model.record->vertexBuffer==model.vb && model.record->indexBuffer==model.ib,
          "Maximum leaves author VB/IB bindings unchanged");
    Check(model.materials.size()==materials.size() &&
          std::memcmp(model.materials.data(),materials.data(),materials.size()*sizeof(materials[0]))==0,
          "Maximum leaves author material records unchanged");
    // Source IBs are MANAGED without WRITEONLY. Read their real storage, not
    // the upload shadow, so an accidental write cannot hide behind CPU indices.
    void* memory=nullptr; Hr(model.ib->Lock(0,0,&memory,D3DLOCK_READONLY),"read real source IB after Maximum");
    bool same=true; UINT position=0;
    for(const auto& face:model.faces) for(UINT expected:face) {
        UINT actual=0; const UINT width=pmx?4:2;
        std::memcpy(&actual,static_cast<const unsigned char*>(memory)+width*position++,width);
        same=same && actual==expected;
    }
    Hr(model.ib->Unlock(),"unlock real source IB after Maximum");
    Check(same,"Maximum leaves real source IB bytes unchanged");
}
void CrossingFace(Model& model,unsigned material,bool red) {
    for(UINT id:model.faces[material]) {
        auto& v=model.vertices[id]; v.z=.5f+(red?.35f:-.35f)*v.x;
    }
    auto& m=model.materials[material];
    for(unsigned c=0;c<3;++c) {
        const float value=(red?c==0:c==1)?1.f:0.f;
        m.diffuse[c]=m.diffuseMirror[c]=m.ambient[c]=value;
    }
}
void VerifyPixels(Host& host) {
    D3DLOCKED_RECT lock{}; Hr(host.readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"lock Maximum color samples");
    // At NDC x<0 the red plane is nearer, at x>0 the green plane is nearer.
    // Source-over of two alpha=.5 primaries over black gives 128/64 channels.
    std::array<DWORD,2> pixels{};
    for(unsigned i=0;i<2;++i) pixels[i]=*reinterpret_cast<DWORD*>(static_cast<unsigned char*>(lock.pBits)+60*lock.Pitch+(i?60:36)*4);
    Hr(host.readback->UnlockRect(),"unlock Maximum color samples");
    for(unsigned i=0;i<2;++i) {
        const int red=i?64:128,green=i?128:64;
        std::printf("maximum sample=%u,60 pixel=%08lx expected-rgb=%d,%d,0\n",i?60:36,pixels[i],red,green);
        Check(std::abs(int(pixels[i]>>16&255)-red)<=3 && std::abs(int(pixels[i]>>8&255)-green)<=3 && int(pixels[i]&255)<=3,
              "Maximum pixels follow independent crossing-plane source-over order");
    }
}

void Scene(Host& host,Uploads& uploads,Draws& draws,bool pmx,int resource,unsigned kind) {
    // kind0: one model, two materials; kind1: two models, global scope;
    // kind2: one crossing model plus an isolated second model, local scope.
    // kind2 deliberately makes Effect skip peeling through its real gate.
    auto a=MakeModel(host,pmx,0,kind==1?std::vector<float>{.5f}:std::vector<float>{.5f,.5f});
    std::unique_ptr<Model> b;
    CrossingFace(*a,0,true);
    if(kind==1) { b=MakeModel(host,pmx,32,{.5f}); CrossingFace(*b,0,false); }
    else CrossingFace(*a,1,false);
    if(kind==2) {
        b=MakeModel(host,pmx,64,{.4f});
        const auto& ids=b->faces[0];
        b->vertices[ids[0]].x=.65f; b->vertices[ids[0]].y=-.8f;
        b->vertices[ids[1]].x=.8f; b->vertices[ids[1]].y=-.55f;
        b->vertices[ids[2]].x=.95f; b->vertices[ids[2]].y=-.8f;
    }
    UploadSource(*a); if(b) UploadSource(*b);
    const auto materialsA=a->materials;
    const auto materialsB=b?b->materials:std::vector<mdl::ModelMaterialRecord>{};
    std::vector<Source> sources={{a.get(),0}};
    if(kind!=1) sources.push_back({a.get(),1});
    if(b) { sources.push_back({b.get(),0}); Install(host,*a,*b); }
    else { a->record->comboSelIndex=0; host.app->ModelSlot(0)=reinterpret_cast<unsigned char*>(a->record.get()); host.app->ModelSlot(1)=nullptr; }
    host.app->TransparentMaterialSortPrecision()=TransparentSortPrecision::Maximum;
    host.app->TransparentSortCrossObjects()=kind==1;
    auto vertices=std::make_unique<VertexUploads>(host.device);
    {
        Fragments fragments(*host.app,*vertices,uploads);
        for(unsigned repeat=0;repeat<2;++repeat) {
            uploads.Reset(); fragments.Reset();
            std::printf("maximum-bsp pmx=%d resource=%d kind=%u cross=%d repeat=%u\n",pmx,resource,kind,kind==1,repeat);
            host.Frame(draws,uploads,resource);
            VerifyFragments(fragments,draws,sources); VerifyPixels(host);
            VerifySource(*a,pmx,materialsA); if(b) VerifySource(*b,pmx,materialsB);
            vertices->CheckReleased();
        }
    }
    host.Unbind(); vertices->CheckReleased();
}
} // namespace maximum_renderer_checks

// Parent integration: call after MakeModel/SourceUnchanged are declared, with
// the base Uploads and Draws active. No extra production seam or public ABI.
// Full profile, 32-byte PMD/PMX sources, orthographic XYZ only. This deliberately
// does not certify extra UV interpolation, perspective attributes, clipping,
// peeling, external MME, device-loss or DrawIndexedPrimitive HRESULT handling.
void MaximumRendererChecks(Host& host,Uploads& uploads,Draws& draws,bool pmx,int resource) {
    Check(resource==0 || resource==117 || resource==118,"Maximum fixture accepts only known native renderers");
    host.Effect(resource);
    if(!resource) {
        maximum_renderer_checks::Scene(host,uploads,draws,pmx,resource,0);
        maximum_renderer_checks::Scene(host,uploads,draws,pmx,resource,1);
    }
    maximum_renderer_checks::Scene(host,uploads,draws,pmx,resource,2);
}
