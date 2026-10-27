#pragma once

// C++17 ownership over the original mms.scene.evaluate 1.4 table and handles.
// Capture belongs on the host UI thread; the host decides which thread that
// is. Evaluate, reads, retain and release may run on workers. Concurrent const
// reads are allowed, but moving/releasing a wrapper must not race its use.
// Give workers independent Retain() owners when their cleanup is independent.
// Service contexts must outlive every wrapper, including its destructor.
// During stop, join workers and explicitly Release() owners, retaining them
// for retry on failure. Destruction performs one best-effort release only.
#include <mms/evaluation.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace mms::sdk {
namespace evaluation_detail {
template<class T> constexpr MmsHeader Header() noexcept {
    return {static_cast<std::uint32_t>(sizeof(T)),1,0};
}
template<class F> MmsResult Call(F&& function) noexcept {
    try { return function(); }
    catch(const std::bad_alloc&) { return MMS_E_MEMORY; }
    catch(const std::length_error&) { return MMS_E_MEMORY; }
    catch(...) { return MMS_E_FAILED; }
}
inline bool Id(MmsId id) noexcept { return id.lo || id.hi; }
inline bool Object(MmsObjectRef object) noexcept { return Id(object.id) && object.generation; }
template<class T> bool ValueHeader(const T& value) noexcept {
    return value.h.size>=sizeof(T) && value.h.major==1 && value.h.minor==0;
}
template<class T,class F,class Validate> MmsResult Read(T& out,F&& function,Validate&& valid) noexcept {
    return Call([&]() -> MmsResult {
        T candidate{}; candidate.h=Header<T>();
        const auto result=function(candidate);
        if(result!=MMS_OK) return result;
        if(!ValueHeader(candidate)) return MMS_E_VERSION;
        if(!valid(candidate)) return MMS_E_FAILED;
        out=candidate; return MMS_OK;
    });
}
inline bool Complete(const MmsEvaluationApiV1_4& api) noexcept {
    return api.getCapabilities && api.createSource && api.describeSource && api.boneAt && api.sampleBone && api.retain && api.release &&
        api.createSourceChannels && api.describeSourceChannels && api.morphAt && api.sampleMorph &&
        api.describeSourceDisplay && api.ikAt && api.selectorAt && api.sampleVisibility && api.sampleIk && api.sampleParent && api.sampleParentState &&
        api.getSceneCapabilities && api.createSceneSource && api.describeSceneSource && api.sceneModelAt && api.sceneBoneAt &&
        api.evaluateFrame && api.describeFrame && api.frameModelAt && api.readBonePose;
}
inline bool Capabilities(const MmsEvaluationSceneCapsV1& caps) noexcept {
    if(caps.reserved || (caps.flags & ~MMS_EVALUATION_SCENE_SKELETON)) return false;
    if(!caps.flags) return true;
    return caps.maxSourceBytes && caps.maxFrameBytes && caps.maxWorkspaceBytes &&
        caps.maxSourceBytes<=caps.maxClientBytes && caps.maxFrameBytes<=caps.maxClientBytes && caps.maxClientBytes<=caps.maxTotalBytes &&
        caps.maxWorkspaceBytes<=caps.maxClientWorkspaceBytes && caps.maxClientWorkspaceBytes<=caps.maxTotalWorkspaceBytes;
}
inline bool SourceInfo(const MmsEvaluationSceneInfoV1& info) noexcept {
    return info.captureId && info.retainedBytes && info.flags==MMS_EVALUATION_SCENE_SKELETON;
}
inline bool FrameInfo(const MmsEvaluationFrameInfoV1& info) noexcept {
    return info.captureId && info.frameId && info.retainedBytes && info.flags==MMS_EVALUATION_SCENE_SKELETON &&
        info.time.numerator>=0 && info.time.denominator;
}
inline bool Model(const MmsEvaluationSceneModelV1& model,bool frame) noexcept {
    return Object(model.object) && !model.reserved &&
        (frame ? model.flags==MMS_EVALUATION_SCENE_VISIBILITY_VALID && model.visibility<=255 : !model.flags && !model.visibility);
}
inline bool Pose(const MmsEvaluationScenePoseV1& pose) noexcept {
    if(!Id(pose.bone) || pose.reserved) return false;
    for(unsigned i=0;i<16;++i)
        if(!std::isfinite(pose.rigid[i]) || !std::isfinite(pose.scaled[i]) ||
           !std::isfinite(pose.attachmentBefore[i]) || !std::isfinite(pose.attachmentAfter[i])) return false;
    for(float position:pose.jointPosition) if(!std::isfinite(position)) return false;
    return true;
}
struct Lease {
    MmsEvaluationApiV1_4 api{};
    MmsHandle handle=0;
    Lease() noexcept=default;
    Lease(const Lease&)=delete;
    Lease& operator=(const Lease&)=delete;
    Lease(Lease&& other) noexcept : api(other.api),handle(std::exchange(other.handle,0)) { other.api={}; }
    Lease& operator=(Lease&&)=delete;
    ~Lease() noexcept { (void)Release(); }
    MmsResult Release() noexcept {
        if(!handle) return MMS_OK;
        const auto result=Call([&] { return api.release(api.context,handle); });
        if(result!=MMS_OK) return result;
        api={}; handle=0; return MMS_OK;
    }
    MmsHandle Detach() noexcept { api={}; return std::exchange(handle,0); }
    void Adopt(const MmsEvaluationApiV1_4& table,MmsHandle value) noexcept { api=table; handle=value; }
    MmsResult Retain(Lease& out) const noexcept {
        if(!handle) return MMS_E_NOT_READY;
        if(out.handle) return MMS_E_BUSY;
        MmsHandle candidate=0;
        const auto result=Call([&] { return api.retain(api.context,handle,&candidate); });
        // A failed C call MUST NOT publish a handle. A nonzero value written
        // despite failure is not safe to adopt or release: it may be arbitrary.
        if(result!=MMS_OK) return result;
        // Retain must issue a distinct handle. Never release a malformed alias
        // here, since that could destroy the original owner's valid handle.
        if(!candidate || candidate==handle) return MMS_E_FAILED;
        out.Adopt(api,candidate); return MMS_OK;
    }
};
} // namespace evaluation_detail

class EvaluationServices;
class EvaluationSceneSource;

struct EvaluationFrameOptions {
    std::uint64_t maxRetainedBytes=0;
    std::uint64_t maxWorkspaceBytes=0;
};

class EvaluationSceneFrame {
    evaluation_detail::Lease lease_;
    friend class EvaluationSceneSource;
public:
    EvaluationSceneFrame() noexcept=default;
    EvaluationSceneFrame(const EvaluationSceneFrame&)=delete;
    EvaluationSceneFrame& operator=(const EvaluationSceneFrame&)=delete;
    EvaluationSceneFrame(EvaluationSceneFrame&&) noexcept=default;
    EvaluationSceneFrame& operator=(EvaluationSceneFrame&&)=delete;
    ~EvaluationSceneFrame() noexcept=default;
    explicit operator bool() const noexcept { return lease_.handle!=0; }
    MmsHandle get() const noexcept { return lease_.handle; }
    const MmsEvaluationApiV1_4& api() const noexcept { return lease_.api; }
    MmsResult Info(MmsEvaluationFrameInfoV1& out) const noexcept {
        if(!*this) return MMS_E_NOT_READY;
        return evaluation_detail::Read(out,[&](auto& value) { return lease_.api.describeFrame(lease_.api.context,lease_.handle,&value); },evaluation_detail::FrameInfo);
    }
    MmsResult ModelAt(std::uint32_t index,MmsEvaluationSceneModelV1& out) const noexcept {
        if(!*this) return MMS_E_NOT_READY;
        return evaluation_detail::Read(out,[&](auto& value) { return lease_.api.frameModelAt(lease_.api.context,lease_.handle,index,&value); },
            [](const auto& value) { return evaluation_detail::Model(value,true); });
    }
    MmsResult ReadBonePose(std::uint32_t model,std::uint32_t bone,MmsEvaluationScenePoseV1& out) const noexcept {
        if(!*this) return MMS_E_NOT_READY;
        return evaluation_detail::Read(out,[&](auto& value) { return lease_.api.readBonePose(lease_.api.context,lease_.handle,model,bone,&value); },evaluation_detail::Pose);
    }
    MmsResult Retain(EvaluationSceneFrame& out) const noexcept { return lease_.Retain(out.lease_); }
    MmsResult Release() noexcept { return lease_.Release(); }
    // Borrowing get()/api() does not transfer ownership. To transfer it, first
    // copy api(), then detach(), and release through that ORIGINAL table.
    MmsHandle detach() noexcept { return lease_.Detach(); }
};

class EvaluationSceneSource {
    evaluation_detail::Lease lease_;
    friend class EvaluationServices;
public:
    EvaluationSceneSource() noexcept=default;
    EvaluationSceneSource(const EvaluationSceneSource&)=delete;
    EvaluationSceneSource& operator=(const EvaluationSceneSource&)=delete;
    EvaluationSceneSource(EvaluationSceneSource&&) noexcept=default;
    EvaluationSceneSource& operator=(EvaluationSceneSource&&)=delete;
    ~EvaluationSceneSource() noexcept=default;
    explicit operator bool() const noexcept { return lease_.handle!=0; }
    MmsHandle get() const noexcept { return lease_.handle; }
    const MmsEvaluationApiV1_4& api() const noexcept { return lease_.api; }
    MmsResult Info(MmsEvaluationSceneInfoV1& out) const noexcept {
        if(!*this) return MMS_E_NOT_READY;
        return evaluation_detail::Read(out,[&](auto& value) { return lease_.api.describeSceneSource(lease_.api.context,lease_.handle,&value); },evaluation_detail::SourceInfo);
    }
    MmsResult ModelAt(std::uint32_t index,MmsEvaluationSceneModelV1& out) const noexcept {
        if(!*this) return MMS_E_NOT_READY;
        return evaluation_detail::Read(out,[&](auto& value) { return lease_.api.sceneModelAt(lease_.api.context,lease_.handle,index,&value); },
            [](const auto& value) { return evaluation_detail::Model(value,false); });
    }
    MmsResult BoneAt(std::uint32_t model,std::uint32_t index,MmsEvaluationSceneBoneV1& out) const noexcept {
        if(!*this) return MMS_E_NOT_READY;
        return evaluation_detail::Read(out,[&](auto& value) { return lease_.api.sceneBoneAt(lease_.api.context,lease_.handle,model,index,&value); },
            [index](const auto& value) { return evaluation_detail::Id(value.bone) && value.index==index && !value.reserved; });
    }
    MmsResult Evaluate(MmsTime time,EvaluationSceneFrame& out,EvaluationFrameOptions options={}) const noexcept {
        if(!*this) return MMS_E_NOT_READY;
        if(out) return MMS_E_BUSY;
        const MmsEvaluationFrameQueryV1 query{evaluation_detail::Header<MmsEvaluationFrameQueryV1>(),lease_.handle,time,
            options.maxRetainedBytes,options.maxWorkspaceBytes,0,0};
        MmsHandle candidate=0;
        const auto result=evaluation_detail::Call([&] { return lease_.api.evaluateFrame(lease_.api.context,&query,&candidate); });
        if(result!=MMS_OK) return result; // Failed calls cannot transfer ownership.
        if(!candidate || candidate==lease_.handle) return MMS_E_FAILED;
        // No description call or allocation after success: the immutable frame
        // is immediately owned and may outlive this source, not its host context.
        out.lease_.Adopt(lease_.api,candidate); return MMS_OK;
    }
    MmsResult Retain(EvaluationSceneSource& out) const noexcept { return lease_.Retain(out.lease_); }
    MmsResult Release() noexcept { return lease_.Release(); }
    MmsHandle detach() noexcept { return lease_.Detach(); }
};

class EvaluationServices {
    MmsEvaluationApiV1_4 api_{};
    bool ready_=false;
public:
    EvaluationServices() noexcept=default;
    // Negotiate the actual 1.4 table. This records no presumed UI-thread ID;
    // host createSceneSource remains the authority for capture thread rules.
    static MmsResult Query(const MmsHostV1& host,EvaluationServices& out) noexcept {
        if(host.h.size<sizeof(host) || host.h.major!=1) return MMS_E_VERSION;
        if(!host.queryService) return MMS_E_ARGUMENT;
        return evaluation_detail::Call([&]() -> MmsResult {
            MmsEvaluationApiV1_4 table{};
            const auto result=host.queryService(host.context,{"mms.scene.evaluate",18},1,4,static_cast<std::uint32_t>(sizeof(table)),&table);
            if(result!=MMS_OK) return result;
            if(table.h.size<sizeof(table) || table.h.major!=1 || table.h.minor<4) return MMS_E_VERSION;
            if(!evaluation_detail::Complete(table)) return MMS_E_UNSUPPORTED;
            EvaluationServices candidate;candidate.api_=table;candidate.ready_=true;out=candidate;return MMS_OK;
        });
    }
    bool ready() const noexcept { return ready_; }
    const MmsEvaluationApiV1_4& api() const noexcept { return api_; }
    MmsResult Capabilities(MmsEvaluationSceneCapsV1& out) const noexcept {
        if(!ready_) return MMS_E_NOT_READY;
        return evaluation_detail::Read(out,[&](auto& value) { return api_.getSceneCapabilities(api_.context,&value); },evaluation_detail::Capabilities);
    }
    MmsResult Capture(EvaluationSceneSource& out,std::uint64_t maxRetainedBytes=0) const noexcept {
        if(!ready_) return MMS_E_NOT_READY;
        if(out) return MMS_E_BUSY;
        const MmsEvaluationSceneQueryV1 query{evaluation_detail::Header<MmsEvaluationSceneQueryV1>(),maxRetainedBytes,0,0};
        MmsHandle candidate=0;
        const auto result=evaluation_detail::Call([&] { return api_.createSceneSource(api_.context,&query,&candidate); });
        // On error an illegally written nonzero value is not a published C
        // lease. The wrapper cannot safely release or repair a violating host.
        if(result!=MMS_OK) return result;
        if(!candidate) return MMS_E_FAILED;
        out.lease_.Adopt(api_,candidate);return MMS_OK;
    }
};

// Mesh wrappers negotiate 1.5 explicitly. They own the SAME C source and
// frame handles as the 1.4 functions; there is no parallel object registry.
namespace evaluation_detail {
inline MmsEvaluationApiV1_4 Prefix(const MmsEvaluationApiV1_5& api) noexcept {
    MmsEvaluationApiV1_4 result{};
    static_assert(offsetof(MmsEvaluationApiV1_5,getMeshCapabilities)==sizeof(result),"evaluation prefix");
    std::memcpy(&result,&api,sizeof(result));
    return result;
}
inline bool CompleteMesh(const MmsEvaluationApiV1_5& api) noexcept {
    return Complete(Prefix(api)) && api.getMeshCapabilities && api.createMeshSceneSource && api.describeMesh &&
        api.readMeshIndices && api.readMeshMaterials && api.readMeshBindVertices && api.readMeshVertices;
}
inline bool MeshCaps(const MmsEvaluationMeshCapsV1& caps) noexcept {
    constexpr std::uint32_t known=MMS_EVALUATION_MESH_TOPOLOGY|MMS_EVALUATION_MESH_CPU_VERTICES;
    return !(caps.flags&~known) && caps.maxItemsPerRead && !caps.reserved[0] && !caps.reserved[1];
}
inline bool MeshInfo(const MmsEvaluationMeshInfoV1& info) noexcept {
    constexpr std::uint32_t known=MMS_EVALUATION_MESH_TOPOLOGY|MMS_EVALUATION_MESH_CPU_VERTICES;
    return Object(info.object) && !(info.flags&~known) && !info.reserved &&
        info.additionalUvCount<=4 && info.indexCount%3==0;
}
inline bool MeshMaterial(const MmsEvaluationMeshMaterialV1& value) noexcept {
    return !value.reserved[0] && !value.reserved[1] && value.firstIndex%3==0 &&
        value.indexCount%3==0 && value.indexCount<=UINT32_MAX-value.firstIndex;
}
inline bool MeshVertex(const MmsEvaluationMeshVertexV1& value) noexcept {
    if(value.reserved[0] || value.reserved[1]) return false;
    for(float n:value.position) if(!std::isfinite(n)) return false;
    for(float n:value.normal) if(!std::isfinite(n)) return false;
    for(float n:value.uv) if(!std::isfinite(n)) return false;
    for(float n:value.additionalUv) if(!std::isfinite(n)) return false;
    return true;
}
inline MmsResult MeshCapabilities(const MmsEvaluationApiV1_5& api,MmsEvaluationMeshCapsV1& out) noexcept {
    return Read(out,[&](auto& value){return api.getMeshCapabilities(api.context,&value);},MeshCaps);
}
template<class T,class ReadFunction,class Validate>
MmsResult MeshBatch(const MmsEvaluationApiV1_5& api,std::uint32_t count,
    std::vector<T>& out,ReadFunction&& read,Validate&& valid) noexcept {
    return Call([&]() -> MmsResult {
        MmsEvaluationMeshCapsV1 caps{};
        const auto capResult=MeshCapabilities(api,caps);
        if(capResult!=MMS_OK) return capResult;
        if(count>caps.maxItemsPerRead) return MMS_E_ARGUMENT;
        std::vector<T> candidate(count);
        const auto result=read(count ? candidate.data() : nullptr);
        if(result!=MMS_OK) return result;
        for(const auto& value:candidate) {
            if constexpr(!std::is_same_v<T,std::uint32_t>)
                if(!ValueHeader(value)) return MMS_E_VERSION;
            if(!valid(value)) return MMS_E_FAILED;
        }
        out.swap(candidate);return MMS_OK;
    });
}
} // namespace evaluation_detail

class EvaluationMeshSource;
class EvaluationMeshServices;
class EvaluationMeshFrame {
    evaluation_detail::Lease lease_;
    MmsEvaluationApiV1_5 api_{};
    friend class EvaluationMeshSource;
public:
    EvaluationMeshFrame() noexcept=default;
    EvaluationMeshFrame(const EvaluationMeshFrame&)=delete;
    EvaluationMeshFrame& operator=(const EvaluationMeshFrame&)=delete;
    EvaluationMeshFrame(EvaluationMeshFrame&&) noexcept=default;
    EvaluationMeshFrame& operator=(EvaluationMeshFrame&&)=delete;
    explicit operator bool() const noexcept {return lease_.handle!=0;}
    MmsHandle get() const noexcept {return lease_.handle;}
    const MmsEvaluationApiV1_5& api() const noexcept {return api_;}
    MmsResult Info(MmsEvaluationFrameInfoV1& out) const noexcept {
        if(!*this)return MMS_E_NOT_READY;
        return evaluation_detail::Read(out,[&](auto& value){return api_.describeFrame(api_.context,get(),&value);},evaluation_detail::FrameInfo);
    }
    MmsResult ModelAt(std::uint32_t index,MmsEvaluationSceneModelV1& out) const noexcept {
        if(!*this)return MMS_E_NOT_READY;
        return evaluation_detail::Read(out,[&](auto& value){return api_.frameModelAt(api_.context,get(),index,&value);},
            [](const auto& value){return evaluation_detail::Model(value,true);});
    }
    MmsResult ReadBonePose(std::uint32_t model,std::uint32_t bone,MmsEvaluationScenePoseV1& out) const noexcept {
        if(!*this)return MMS_E_NOT_READY;
        return evaluation_detail::Read(out,[&](auto& value){return api_.readBonePose(api_.context,get(),model,bone,&value);},evaluation_detail::Pose);
    }
    MmsResult ReadVertices(std::uint32_t model,std::uint32_t first,std::uint32_t count,
        std::vector<MmsEvaluationMeshVertexV1>& out) const noexcept {
        if(!*this)return MMS_E_NOT_READY;
        return evaluation_detail::MeshBatch(api_,count,out,[&](auto* data){return api_.readMeshVertices(api_.context,get(),model,first,count,data);},evaluation_detail::MeshVertex);
    }
    MmsResult Retain(EvaluationMeshFrame& out) const noexcept {
        const auto result=lease_.Retain(out.lease_);if(result==MMS_OK)out.api_=api_;return result;
    }
    MmsResult Release() noexcept {const auto result=lease_.Release();if(result==MMS_OK)api_={};return result;}
    MmsHandle detach() noexcept {api_={};return lease_.Detach();}
};

class EvaluationMeshSource {
    evaluation_detail::Lease lease_;
    MmsEvaluationApiV1_5 api_{};
    friend class EvaluationMeshServices;
    friend class EvaluationMaterialServices;
public:
    EvaluationMeshSource() noexcept=default;
    EvaluationMeshSource(const EvaluationMeshSource&)=delete;
    EvaluationMeshSource& operator=(const EvaluationMeshSource&)=delete;
    EvaluationMeshSource(EvaluationMeshSource&&) noexcept=default;
    EvaluationMeshSource& operator=(EvaluationMeshSource&&)=delete;
    explicit operator bool() const noexcept {return lease_.handle!=0;}
    MmsHandle get() const noexcept {return lease_.handle;}
    const MmsEvaluationApiV1_5& api() const noexcept {return api_;}
    MmsResult Info(MmsEvaluationSceneInfoV1& out) const noexcept {
        if(!*this)return MMS_E_NOT_READY;
        return evaluation_detail::Read(out,[&](auto& value){return api_.describeSceneSource(api_.context,get(),&value);},evaluation_detail::SourceInfo);
    }
    MmsResult ModelAt(std::uint32_t index,MmsEvaluationSceneModelV1& out) const noexcept {
        if(!*this)return MMS_E_NOT_READY;
        return evaluation_detail::Read(out,[&](auto& value){return api_.sceneModelAt(api_.context,get(),index,&value);},
            [](const auto& value){return evaluation_detail::Model(value,false);});
    }
    MmsResult BoneAt(std::uint32_t model,std::uint32_t index,MmsEvaluationSceneBoneV1& out) const noexcept {
        if(!*this)return MMS_E_NOT_READY;
        return evaluation_detail::Read(out,[&](auto& value){return api_.sceneBoneAt(api_.context,get(),model,index,&value);},
            [index](const auto& value){return evaluation_detail::Id(value.bone)&&value.index==index&&!value.reserved;});
    }
    MmsResult MeshInfo(std::uint32_t model,MmsEvaluationMeshInfoV1& out) const noexcept {
        if(!*this)return MMS_E_NOT_READY;
        return evaluation_detail::Read(out,[&](auto& value){return api_.describeMesh(api_.context,get(),model,&value);},evaluation_detail::MeshInfo);
    }
    MmsResult ReadIndices(std::uint32_t model,std::uint32_t first,std::uint32_t count,std::vector<std::uint32_t>& out) const noexcept {
        if(!*this)return MMS_E_NOT_READY;
        return evaluation_detail::MeshBatch(api_,count,out,[&](auto* data){return api_.readMeshIndices(api_.context,get(),model,first,count,data);},[](auto){return true;});
    }
    MmsResult ReadMaterials(std::uint32_t model,std::uint32_t first,std::uint32_t count,std::vector<MmsEvaluationMeshMaterialV1>& out) const noexcept {
        if(!*this)return MMS_E_NOT_READY;
        return evaluation_detail::MeshBatch(api_,count,out,[&](auto* data){return api_.readMeshMaterials(api_.context,get(),model,first,count,data);},evaluation_detail::MeshMaterial);
    }
    MmsResult ReadBindVertices(std::uint32_t model,std::uint32_t first,std::uint32_t count,std::vector<MmsEvaluationMeshVertexV1>& out) const noexcept {
        if(!*this)return MMS_E_NOT_READY;
        return evaluation_detail::MeshBatch(api_,count,out,[&](auto* data){return api_.readMeshBindVertices(api_.context,get(),model,first,count,data);},evaluation_detail::MeshVertex);
    }
    MmsResult Evaluate(MmsTime time,EvaluationMeshFrame& out,EvaluationFrameOptions options={}) const noexcept {
        if(!*this)return MMS_E_NOT_READY;
        if(out)return MMS_E_BUSY;
        const MmsEvaluationFrameQueryV1 query{evaluation_detail::Header<MmsEvaluationFrameQueryV1>(),get(),time,options.maxRetainedBytes,options.maxWorkspaceBytes,0,0};
        MmsHandle candidate=0;
        const auto result=evaluation_detail::Call([&]{return api_.evaluateFrame(api_.context,&query,&candidate);});
        if(result!=MMS_OK)return result;
        if(!candidate || candidate==get())return MMS_E_FAILED;
        out.lease_.Adopt(evaluation_detail::Prefix(api_),candidate);out.api_=api_;return MMS_OK;
    }
    MmsResult Retain(EvaluationMeshSource& out) const noexcept {
        const auto result=lease_.Retain(out.lease_);if(result==MMS_OK)out.api_=api_;return result;
    }
    MmsResult Release() noexcept {const auto result=lease_.Release();if(result==MMS_OK)api_={};return result;}
    MmsHandle detach() noexcept {api_={};return lease_.Detach();}
};

class EvaluationMeshServices {
    MmsEvaluationApiV1_5 api_{};
    bool ready_=false;
public:
    EvaluationMeshServices() noexcept=default;
    static MmsResult Query(const MmsHostV1& host,EvaluationMeshServices& out) noexcept {
        if(host.h.size<sizeof(host)||host.h.major!=1)return MMS_E_VERSION;
        if(!host.queryService)return MMS_E_ARGUMENT;
        return evaluation_detail::Call([&]() -> MmsResult {
            MmsEvaluationApiV1_5 table{};
            const auto result=host.queryService(host.context,{"mms.scene.evaluate",18},1,5,static_cast<std::uint32_t>(sizeof(table)),&table);
            if(result!=MMS_OK)return result;
            if(table.h.size<sizeof(table)||table.h.major!=1||table.h.minor<5)return MMS_E_VERSION;
            if(!evaluation_detail::CompleteMesh(table))return MMS_E_UNSUPPORTED;
            EvaluationMeshServices candidate;candidate.api_=table;candidate.ready_=true;out=candidate;return MMS_OK;
        });
    }
    bool ready() const noexcept {return ready_;}
    const MmsEvaluationApiV1_5& api() const noexcept {return api_;}
    MmsResult Capabilities(MmsEvaluationMeshCapsV1& out) const noexcept {
        if(!ready_)return MMS_E_NOT_READY;return evaluation_detail::MeshCapabilities(api_,out);
    }
    MmsResult Capture(EvaluationMeshSource& out,std::uint64_t maxRetainedBytes=0) const noexcept {
        if(!ready_)return MMS_E_NOT_READY;if(out)return MMS_E_BUSY;
        const MmsEvaluationSceneQueryV1 query{evaluation_detail::Header<MmsEvaluationSceneQueryV1>(),maxRetainedBytes,0,0};
        MmsHandle candidate=0;
        const auto result=evaluation_detail::Call([&]{return api_.createMeshSceneSource(api_.context,&query,&candidate);});
        if(result!=MMS_OK)return result;if(!candidate)return MMS_E_FAILED;
        out.lease_.Adopt(evaluation_detail::Prefix(api_),candidate);out.api_=api_;return MMS_OK;
    }
};

// Material wrappers negotiate 1.6 explicitly. Query failure preserves the
// output service; callers may separately query EvaluationMeshServices for a
// 1.5 fallback. All objects operate on the same underlying C handles.
namespace evaluation_detail {
inline MmsEvaluationApiV1_5 MeshPrefix(const MmsEvaluationApiV1_6& api) noexcept {
    MmsEvaluationApiV1_5 result{};
    static_assert(offsetof(MmsEvaluationApiV1_6,getMaterialCapabilities)==sizeof(result),"evaluation material prefix");
    std::memcpy(&result,&api,sizeof(result));return result;
}
inline bool CompleteMaterial(const MmsEvaluationApiV1_6& api) noexcept {
    return CompleteMesh(MeshPrefix(api)) && api.getMaterialCapabilities && api.createMaterialSceneSource && api.materialAt && api.readFrameMaterials;
}
inline bool MaterialCaps(const MmsEvaluationMaterialCapsV1& value) noexcept {
    constexpr std::uint32_t known=MMS_EVALUATION_MATERIAL_CAP_CPU_VALUES|MMS_EVALUATION_MATERIAL_CAP_PMD|MMS_EVALUATION_MATERIAL_CAP_PMX;
    return !(value.flags&~known) && value.maxItemsPerRead && !value.reserved[0] && !value.reserved[1];
}
inline bool MaterialResult(const MmsEvaluationMaterialV1& value,std::uint32_t index) noexcept {
    if(!Id(value.material) || value.index!=index || value.reserved[0] || value.reserved[1] ||
       (value.flags!=MMS_EVALUATION_MATERIAL_FORMAT_PMD && value.flags!=MMS_EVALUATION_MATERIAL_FORMAT_PMX)) return false;
    for(float v:value.nativeDiffuse)if(!std::isfinite(v))return false;
    for(float v:value.diffuseMirrorAndAlpha)if(!std::isfinite(v))return false;
    for(float v:value.specular)if(!std::isfinite(v))return false;
    for(float v:value.ambient)if(!std::isfinite(v))return false;
    if(!std::isfinite(value.specularPower)||!std::isfinite(value.edgeSize))return false;
    for(float v:value.edgeColor)if(!std::isfinite(v))return false;
    for(float v:value.textureAdd)if(!std::isfinite(v))return false;
    for(float v:value.textureMultiply)if(!std::isfinite(v))return false;
    for(float v:value.sphereAdd)if(!std::isfinite(v))return false;
    for(float v:value.sphereMultiply)if(!std::isfinite(v))return false;
    for(float v:value.toonAdd)if(!std::isfinite(v))return false;
    for(float v:value.toonMultiply)if(!std::isfinite(v))return false;
    return true;
}
inline MmsResult MaterialCapabilities(const MmsEvaluationApiV1_6& api,MmsEvaluationMaterialCapsV1& out) noexcept {
    return Read(out,[&](auto& value){return api.getMaterialCapabilities(api.context,&value);},MaterialCaps);
}
}

class EvaluationMaterialSource;
class EvaluationMaterialFrame {
    EvaluationMeshFrame mesh_;
    MmsEvaluationApiV1_6 api_{};
    friend class EvaluationMaterialSource;
public:
    EvaluationMaterialFrame() noexcept=default;
    EvaluationMaterialFrame(const EvaluationMaterialFrame&)=delete;
    EvaluationMaterialFrame& operator=(const EvaluationMaterialFrame&)=delete;
    EvaluationMaterialFrame(EvaluationMaterialFrame&&) noexcept=default;
    EvaluationMaterialFrame& operator=(EvaluationMaterialFrame&&)=delete;
    explicit operator bool() const noexcept {return bool(mesh_);}
    MmsHandle get() const noexcept {return mesh_.get();}
    // The borrowed view is the very same handle, valid only as long as this owner.
    const EvaluationMeshFrame& Mesh() const noexcept {return mesh_;}
    const MmsEvaluationApiV1_6& api() const noexcept {return api_;}
    MmsResult ReadMaterials(std::uint32_t model,std::uint32_t first,std::uint32_t count,
        std::vector<MmsEvaluationMaterialV1>& out) const noexcept {
        if(!*this)return MMS_E_NOT_READY;
        return evaluation_detail::Call([&]() -> MmsResult {
            MmsEvaluationMaterialCapsV1 caps{};const auto status=evaluation_detail::MaterialCapabilities(api_,caps);
            if(status!=MMS_OK)return status;
            if(count>caps.maxItemsPerRead || count>UINT32_MAX-first)return MMS_E_ARGUMENT;
            std::vector<MmsEvaluationMaterialV1> values(count);
            const auto result=api_.readFrameMaterials(api_.context,get(),model,first,count,count?values.data():nullptr);
            if(result!=MMS_OK)return result;
            for(std::uint32_t i=0;i<count;++i){
                if(!evaluation_detail::ValueHeader(values[i]))return MMS_E_VERSION;
                if(!evaluation_detail::MaterialResult(values[i],first+i))return MMS_E_FAILED;
            }
            out.swap(values);return MMS_OK;
        });
    }
    MmsResult Retain(EvaluationMaterialFrame& out) const noexcept {
        const auto result=mesh_.Retain(out.mesh_);if(result==MMS_OK)out.api_=api_;return result;
    }
    MmsResult Release() noexcept {const auto result=mesh_.Release();if(result==MMS_OK)api_={};return result;}
    MmsHandle detach() noexcept {api_={};return mesh_.detach();}
};

class EvaluationMaterialServices;
class EvaluationMaterialSource {
    EvaluationMeshSource mesh_;
    MmsEvaluationApiV1_6 api_{};
    friend class EvaluationMaterialServices;
public:
    EvaluationMaterialSource() noexcept=default;
    EvaluationMaterialSource(const EvaluationMaterialSource&)=delete;
    EvaluationMaterialSource& operator=(const EvaluationMaterialSource&)=delete;
    EvaluationMaterialSource(EvaluationMaterialSource&&) noexcept=default;
    EvaluationMaterialSource& operator=(EvaluationMaterialSource&&)=delete;
    explicit operator bool() const noexcept {return bool(mesh_);}
    MmsHandle get() const noexcept {return mesh_.get();}
    const EvaluationMeshSource& Mesh() const noexcept {return mesh_;}
    const MmsEvaluationApiV1_6& api() const noexcept {return api_;}
    MmsResult MaterialAt(std::uint32_t model,std::uint32_t index,MmsEvaluationMaterialIdV1& out) const noexcept {
        if(!*this)return MMS_E_NOT_READY;
        return evaluation_detail::Read(out,[&](auto& value){return api_.materialAt(api_.context,get(),model,index,&value);},
            [index](const auto& value){return evaluation_detail::Id(value.material)&&value.index==index&&!value.reserved;});
    }
    MmsResult Evaluate(MmsTime time,EvaluationMaterialFrame& out,EvaluationFrameOptions options={}) const noexcept {
        if(!*this)return MMS_E_NOT_READY;if(out)return MMS_E_BUSY;
        const auto result=mesh_.Evaluate(time,out.mesh_,options);if(result==MMS_OK)out.api_=api_;return result;
    }
    MmsResult Retain(EvaluationMaterialSource& out) const noexcept {
        const auto result=mesh_.Retain(out.mesh_);if(result==MMS_OK)out.api_=api_;return result;
    }
    MmsResult Release() noexcept {const auto result=mesh_.Release();if(result==MMS_OK)api_={};return result;}
    MmsHandle detach() noexcept {api_={};return mesh_.detach();}
};

class EvaluationMaterialServices {
    MmsEvaluationApiV1_6 api_{};
    bool ready_=false;
public:
    EvaluationMaterialServices() noexcept=default;
    static MmsResult Query(const MmsHostV1& host,EvaluationMaterialServices& out) noexcept {
        if(host.h.size<sizeof(host)||host.h.major!=1)return MMS_E_VERSION;
        if(!host.queryService)return MMS_E_ARGUMENT;
        return evaluation_detail::Call([&]() -> MmsResult {
            MmsEvaluationApiV1_6 table{};
            const auto result=host.queryService(host.context,{"mms.scene.evaluate",18},1,6,static_cast<std::uint32_t>(sizeof(table)),&table);
            if(result!=MMS_OK)return result;
            if(table.h.size<sizeof(table)||table.h.major!=1||table.h.minor<6)return MMS_E_VERSION;
            if(!evaluation_detail::CompleteMaterial(table))return MMS_E_UNSUPPORTED;
            EvaluationMaterialServices candidate;candidate.api_=table;candidate.ready_=true;out=candidate;return MMS_OK;
        });
    }
    bool ready() const noexcept {return ready_;}
    const MmsEvaluationApiV1_6& api() const noexcept {return api_;}
    MmsResult Capabilities(MmsEvaluationMaterialCapsV1& out) const noexcept {
        if(!ready_)return MMS_E_NOT_READY;return evaluation_detail::MaterialCapabilities(api_,out);
    }
    MmsResult Capture(EvaluationMaterialSource& out,std::uint64_t maxRetainedBytes=0) const noexcept {
        if(!ready_)return MMS_E_NOT_READY;if(out)return MMS_E_BUSY;
        const MmsEvaluationSceneQueryV1 query{evaluation_detail::Header<MmsEvaluationSceneQueryV1>(),maxRetainedBytes,0,0};
        MmsHandle candidate=0;const auto result=evaluation_detail::Call([&]{return api_.createMaterialSceneSource(api_.context,&query,&candidate);});
        if(result!=MMS_OK)return result;if(!candidate)return MMS_E_FAILED;
        const auto prefix=evaluation_detail::MeshPrefix(api_);
        out.mesh_.lease_.Adopt(evaluation_detail::Prefix(prefix),candidate);out.mesh_.api_=prefix;out.api_=api_;return MMS_OK;
    }
};
} // namespace mms::sdk
