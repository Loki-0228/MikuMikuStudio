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
#include <new>
#include <stdexcept>
#include <utility>

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
} // namespace mms::sdk
