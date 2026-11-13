#pragma once

// C++17 ownership of the mms.ui.layout.session 1.0/1.1/1.2 service/handles.
// All host calls belong on the querying UI thread at the host's safe point.
// The service context must outlive every session. Destruction NEVER calls the
// host: explicitly Close/Release before completing plugin shutdown.
// Begin claims only; Query obtains the actual revision before Apply or Close.
// Successful mutations invalidate that observation. Query again explicitly;
// stale revisions are never silently refreshed or retried.
#include <mms/layout_session.h>
#include <cstring>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace mms::sdk {
namespace layout_session_detail {
template<class T> constexpr MmsHeader Header(uint16_t minor=0) noexcept { return {uint32_t(sizeof(T)),1,minor}; }
template<class F> MmsResult Call(F&& function) noexcept {
    try { return function(); }
    catch (const std::bad_alloc&) { return MMS_E_MEMORY; }
    catch (const std::length_error&) { return MMS_E_MEMORY; }
    catch (...) { return MMS_E_FAILED; }
}
template<class T,class F,class V> MmsResult Read(T& out,F&& function,V&& valid,uint32_t minor=0) noexcept {
    return Call([&]() -> MmsResult {
        T candidate{}; candidate.h=Header<T>(static_cast<uint16_t>(minor));
        const auto result=function(candidate);
        if(result!=MMS_OK)return result;
        if(candidate.h.size<sizeof(T) || candidate.h.major!=1 || candidate.h.minor!=minor)return MMS_E_VERSION;
        if(!valid(candidate))return MMS_E_FAILED;
        out=candidate;return MMS_OK;
    });
}
inline bool Fields(uint32_t value,uint32_t supported=7) noexcept { return !(value&~supported); }
inline bool Mount(uint32_t value,uint32_t supported) noexcept { return value && !(value&~supported) && !(value&(value-1)); }
inline bool Info(const MmsUiLayoutSessionInfoV1& value) noexcept {
    return value.revision && value.state<=MMS_LAYOUT_SESSION_REVOKED && !(value.reasons&~31u) &&
        value.targetCount && value.activeTargetCount<=value.targetCount &&
        (value.state==MMS_LAYOUT_SESSION_ACTIVE || !value.activeTargetCount);
}
struct Busy {
    bool& value;
    explicit Busy(bool& flag) noexcept : value(flag) { value=true; }
    ~Busy() { value=false; }
};
} // namespace layout_session_detail

// These values can also be combined in an application-owned complete array.
// Inactive fields stay zero, as required by the public C contract.
inline MmsUiLayoutSessionTargetV1 LayoutSessionTarget(MmsHandle element,uint32_t fields) noexcept {
    return {layout_session_detail::Header<MmsUiLayoutSessionTargetV1>(),element,fields,0};
}
inline MmsUiLayoutSessionValueV1 LayoutSessionVisibility(MmsHandle element,bool visible) noexcept {
    return {layout_session_detail::Header<MmsUiLayoutSessionValueV1>(),element,MMS_LAYOUT_SESSION_VISIBLE,uint32_t(visible),0,0,0,0};
}
inline MmsUiLayoutSessionValueV1 LayoutSessionSize(MmsHandle element,float width,float height) noexcept {
    return {layout_session_detail::Header<MmsUiLayoutSessionValueV1>(),element,MMS_LAYOUT_SESSION_SIZE,0,width,height,0,0};
}
inline MmsUiLayoutSessionValueV1 LayoutSessionOrder(MmsHandle element,int32_t order) noexcept {
    return {layout_session_detail::Header<MmsUiLayoutSessionValueV1>(),element,MMS_LAYOUT_SESSION_ORDER,0,0,0,order,0};
}
inline MmsUiLayoutSessionTargetV1_2 LayoutSessionTarget(MmsHandle element,uint32_t fields,MmsHandle parent) noexcept {
    return {layout_session_detail::Header<MmsUiLayoutSessionTargetV1_2>(2),element,fields,0,parent};
}
inline MmsUiLayoutSessionValueV1_2 LayoutSessionParent(MmsHandle element,MmsHandle parent) noexcept {
    return {layout_session_detail::Header<MmsUiLayoutSessionValueV1_2>(2),element,MMS_LAYOUT_SESSION_PARENT,0,0,0,0,0,parent};
}

class LayoutSessionServices;
class LayoutSession {
    MmsUiLayoutSessionApiV1 api_{};
    MmsUiLayoutSessionApiV1_2 extended_{};
    MmsHandle handle_=0;
    uint64_t revision_=0;
    std::thread::id thread_{};
    bool busy_=false, terminal_=false;
    friend class LayoutSessionServices;
    void Clear() noexcept { api_={};extended_={};handle_=0;revision_=0;thread_={};terminal_=false; }
    MmsResult Check() const noexcept {
        if(busy_)return MMS_E_BUSY;
        if(!handle_)return MMS_E_NOT_READY;
        if(thread_!=std::this_thread::get_id())return MMS_E_THREAD;
        return MMS_OK;
    }
    MmsResult Finish(uint64_t expected,bool commit) noexcept {
        if(const auto result=Check())return result;
        if(!expected)return MMS_E_NOT_READY;
        layout_session_detail::Busy guard(busy_);
        const auto result=layout_session_detail::Call([&] {
            return commit?api_.commit(api_.context,handle_,expected):api_.rollback(api_.context,handle_,expected);
        });
        if(result==MMS_OK){revision_=0;terminal_=true;}
        return result;
    }
public:
    LayoutSession() noexcept=default;
    LayoutSession(const LayoutSession&)=delete;
    LayoutSession& operator=(const LayoutSession&)=delete;
    LayoutSession(LayoutSession&& other) noexcept { (void)TransferFrom(other); }
    LayoutSession& operator=(LayoutSession&&)=delete;
    ~LayoutSession()=default;
    explicit operator bool() const noexcept { return handle_!=0; }
    MmsHandle get() const noexcept { return handle_; }
    // Zero means Query is required, not a guessed host revision.
    uint64_t revision() const noexcept { return revision_; }
    bool terminal() const noexcept { return terminal_; }
    const MmsUiLayoutSessionApiV1& api() const noexcept { return api_; }
    // api() is a normalized 1.1 prefix view when negotiated through 1.2;
    // never cast it to the extended table. Save this
    // complete table before Detach when handing a 1.2 session back to C.
    const MmsUiLayoutSessionApiV1_2* extended_api() const noexcept { return extended_.h.size?&extended_:nullptr; }
    MmsResult TransferFrom(LayoutSession& other) noexcept {
        if(busy_ || other.busy_)return MMS_E_BUSY;
        if(this==&other)return MMS_OK;
        if(handle_)return MMS_E_BUSY;
        api_=other.api_;extended_=other.extended_;handle_=other.handle_;revision_=other.revision_;
        thread_=other.thread_;terminal_=other.terminal_;other.Clear();return MMS_OK;
    }
    // Save api() first if handing ownership back to C. Detach never releases.
    MmsHandle Detach() noexcept {
        if(busy_)return 0;
        const auto handle=handle_;Clear();return handle;
    }
    MmsResult Query(MmsUiLayoutSessionInfoV1& out) noexcept {
        if(const auto result=Check())return result;
        layout_session_detail::Busy guard(busy_);
        MmsUiLayoutSessionInfoV1 candidate{};
        const auto result=layout_session_detail::Read(candidate,[&](auto& value){return api_.query(api_.context,handle_,&value);},layout_session_detail::Info);
        if(result!=MMS_OK)return result;
        revision_=candidate.revision;terminal_=candidate.state!=MMS_LAYOUT_SESSION_ACTIVE;out=candidate;return MMS_OK;
    }
    MmsResult QueryClaim(uint64_t expected,uint32_t index,MmsUiLayoutSessionTargetV1& out) noexcept {
        if(const auto result=Check())return result;
        if(!expected)return MMS_E_NOT_READY;
        layout_session_detail::Busy guard(busy_);
        return layout_session_detail::Read(out,[&](auto& value){return api_.queryClaim(api_.context,handle_,expected,index,&value);},
            [](const auto& value){return value.element && layout_session_detail::Fields(value.fields) && !value.reserved;});
    }
    MmsResult QueryClaim(uint32_t index,MmsUiLayoutSessionTargetV1& out) noexcept { return QueryClaim(revision_,index,out); }
    MmsResult QueryClaim(uint64_t expected,uint32_t index,MmsUiLayoutSessionTargetV1_2& out) noexcept {
        if(const auto result=Check())return result;
        if(!extended_.queryClaimEx)return MMS_E_UNSUPPORTED;
        if(!expected)return MMS_E_NOT_READY;
        layout_session_detail::Busy guard(busy_);
        return layout_session_detail::Read(out,[&](auto& value){return extended_.queryClaimEx(extended_.context,handle_,expected,index,&value);},
            [](const auto& value){return value.element && layout_session_detail::Fields(value.fields,15) && !value.reserved &&
                ((value.fields&MMS_LAYOUT_SESSION_PARENT)?value.parent!=0:value.parent==0);},2);
    }
    MmsResult QueryClaim(uint32_t index,MmsUiLayoutSessionTargetV1_2& out) noexcept { return QueryClaim(revision_,index,out); }
    MmsResult Apply(uint64_t expected,MmsSpan values) noexcept {
        if(const auto result=Check())return result;
        if(!expected)return MMS_E_NOT_READY;
        layout_session_detail::Busy guard(busy_);
        const auto result=layout_session_detail::Call([&]{return api_.apply(api_.context,handle_,expected,values);});
        if(result==MMS_OK)revision_=0;
        return result;
    }
    MmsResult Apply(MmsSpan values) noexcept { return Apply(revision_,values); }
    MmsResult Apply(uint64_t expected,const MmsUiLayoutSessionValueV1* values,size_t count) noexcept {
        return Apply(expected,{values,uint64_t(count),sizeof(MmsUiLayoutSessionValueV1),1});
    }
    MmsResult Apply(const MmsUiLayoutSessionValueV1* values,size_t count) noexcept { return Apply(revision_,values,count); }
    MmsResult Apply(uint64_t expected,const std::vector<MmsUiLayoutSessionValueV1>& values) noexcept { return Apply(expected,values.data(),values.size()); }
    MmsResult Apply(const std::vector<MmsUiLayoutSessionValueV1>& values) noexcept { return Apply(revision_,values); }
    MmsResult ApplyEx(uint64_t expected,MmsSpan values) noexcept {
        if(const auto result=Check())return result;
        if(!extended_.applyEx)return MMS_E_UNSUPPORTED;
        if(!expected)return MMS_E_NOT_READY;
        layout_session_detail::Busy guard(busy_);
        const auto result=layout_session_detail::Call([&]{return extended_.applyEx(extended_.context,handle_,expected,values);});
        if(result==MMS_OK)revision_=0;
        return result;
    }
    MmsResult ApplyEx(MmsSpan values) noexcept { return ApplyEx(revision_,values); }
    MmsResult Apply(uint64_t expected,const MmsUiLayoutSessionValueV1_2* values,size_t count) noexcept {
        return ApplyEx(expected,{values,uint64_t(count),sizeof(MmsUiLayoutSessionValueV1_2),2});
    }
    MmsResult Apply(const MmsUiLayoutSessionValueV1_2* values,size_t count) noexcept { return Apply(revision_,values,count); }
    MmsResult Apply(uint64_t expected,const std::vector<MmsUiLayoutSessionValueV1_2>& values) noexcept { return Apply(expected,values.data(),values.size()); }
    MmsResult Apply(const std::vector<MmsUiLayoutSessionValueV1_2>& values) noexcept { return Apply(revision_,values); }
    MmsResult Rollback(uint64_t expected) noexcept { return Finish(expected,false); }
    MmsResult Rollback() noexcept { return Rollback(revision_); }
    MmsResult Commit(uint64_t expected) noexcept { return Finish(expected,true); }
    MmsResult Commit() noexcept { return Commit(revision_); }
    MmsResult Release() noexcept {
        if(busy_)return MMS_E_BUSY;
        if(!handle_)return MMS_OK;
        if(const auto result=Check())return result;
        layout_session_detail::Busy guard(busy_);
        const auto result=layout_session_detail::Call([&]{return api_.release(api_.context,handle_);});
        if(result==MMS_OK)Clear();
        return result;
    }
    // Roll back an active session, then release. No query/retry is hidden here.
    // A successful rollback followed by failed release retains a terminal
    // handle; the next Close only retries release, not the rollback.
    MmsResult Close() noexcept {
        if(busy_)return MMS_E_BUSY;
        if(!handle_)return MMS_OK;
        if(const auto result=Check())return result;
        if(!terminal_){if(const auto result=Rollback())return result;}
        return Release();
    }
};

class LayoutSessionServices {
    MmsUiLayoutSessionApiV1 api_{};
    MmsUiLayoutSessionApiV1_2 extended_{};
    std::thread::id thread_{};
    bool ready_=false;
    MmsResult Check() const noexcept {
        if(!ready_)return MMS_E_NOT_READY;
        return thread_==std::this_thread::get_id()?MMS_OK:MMS_E_THREAD;
    }
public:
    // Default preserves the 1.0 contract. Request minor=1 explicitly for
    // workspace panels, or minor=2 for Parent; callers choose any fallback.
    static MmsResult Query(const MmsHostV1& host,LayoutSessionServices& out,uint32_t minor=0) noexcept {
        if(minor>2)return MMS_E_VERSION;
        if(host.h.size<sizeof(host) || host.h.major!=1)return MMS_E_VERSION;
        if(!host.queryService)return MMS_E_ARGUMENT;
        return layout_session_detail::Call([&]() -> MmsResult {
            MmsUiLayoutSessionApiV1 candidate{};
            MmsUiLayoutSessionApiV1_2 extended{};
            const auto result=host.queryService(host.context,{"mms.ui.layout.session",21},1,minor,
                minor==2?sizeof(extended):sizeof(candidate),minor==2?static_cast<void*>(&extended):static_cast<void*>(&candidate));
            if(result!=MMS_OK)return result;
            if(minor==2){
                if(extended.h.size<sizeof(extended) || extended.h.major!=1 || extended.h.minor!=2)return MMS_E_VERSION;
                if(!extended.queryCapabilitiesEx || !extended.queryTargetEx || !extended.beginEx || !extended.applyEx || !extended.queryClaimEx)return MMS_E_UNSUPPORTED;
                std::memcpy(&candidate,&extended,sizeof(candidate));
            }
            if(candidate.h.size<sizeof(candidate) || candidate.h.major!=1 || candidate.h.minor!=minor)return MMS_E_VERSION;
            if(!candidate.queryCapabilities || !candidate.queryTarget || !candidate.begin || !candidate.apply || !candidate.query ||
                !candidate.queryClaim || !candidate.rollback || !candidate.commit || !candidate.release)return MMS_E_UNSUPPORTED;
            // The separately stored old view has only 88 bytes. Its callbacks
            // expose exactly 1.1, so never advertise the extended object's size
            // or minor through api(). extended_api() retains negotiated 1.2.
            if(minor==2)candidate.h={uint32_t(sizeof(candidate)),1,1};
            LayoutSessionServices value;value.api_=candidate;value.extended_=extended;value.thread_=std::this_thread::get_id();value.ready_=true;
            out=value;return MMS_OK;
        });
    }
    bool ready() const noexcept { return ready_; }
    const MmsUiLayoutSessionApiV1& api() const noexcept { return api_; }
    // api() exposes a normalized 1.1 view after 1.2 negotiation. This complete
    // table reports the negotiated version and provides all five Ex calls.
    const MmsUiLayoutSessionApiV1_2* extended_api() const noexcept { return extended_.h.size?&extended_:nullptr; }
    MmsResult Capabilities(MmsUiLayoutSessionCapsV1& out) const noexcept {
        if(const auto result=Check())return result;
        const auto api=api_;
        return layout_session_detail::Read(out,[&](auto& value){return api.queryCapabilities(api.context,&value);},[&](const auto& value){
            return value.fields && layout_session_detail::Fields(value.fields) && value.mounts && !(value.mounts&~(api.h.minor?31u:15u)) &&
                value.maxTargets && value.maxSessionsPerPlugin && value.maxSessionsPerPlugin<=value.maxSessionsGlobal && !value.reserved;
        });
    }
    MmsResult Target(MmsHandle element,MmsUiLayoutSessionTargetInfoV1& out) const noexcept {
        if(const auto result=Check())return result;
        const auto api=api_;
        return layout_session_detail::Read(out,[&](auto& value){return api.queryTarget(api.context,element,&value);},[&](const auto& value){
            return element && value.element==element && value.fields && layout_session_detail::Fields(value.fields) && layout_session_detail::Mount(value.mount,api.h.minor?31u:15u);
        });
    }
    MmsResult CapabilitiesEx(MmsUiLayoutSessionCapsV1& out) const noexcept {
        if(const auto result=Check())return result;
        if(!extended_.queryCapabilitiesEx)return MMS_E_UNSUPPORTED;
        const auto api=extended_;
        return layout_session_detail::Read(out,[&](auto& value){return api.queryCapabilitiesEx(api.context,&value);},[](const auto& value){
            return value.fields && layout_session_detail::Fields(value.fields,15) && value.mounts && !(value.mounts&~31u) &&
                value.maxTargets && value.maxSessionsPerPlugin && value.maxSessionsPerPlugin<=value.maxSessionsGlobal && !value.reserved;
        });
    }
    MmsResult TargetEx(MmsHandle element,MmsUiLayoutSessionTargetInfoV1& out) const noexcept {
        if(const auto result=Check())return result;
        if(!extended_.queryTargetEx)return MMS_E_UNSUPPORTED;
        const auto api=extended_;
        return layout_session_detail::Read(out,[&](auto& value){return api.queryTargetEx(api.context,element,&value);},[&](const auto& value){
            return element && value.element==element && value.fields && layout_session_detail::Fields(value.fields,15) &&
                layout_session_detail::Mount(value.mount,31) && !(value.mount==MMS_LAYOUT_SESSION_WORKSPACE && (value.fields&MMS_LAYOUT_SESSION_PARENT));
        });
    }
    MmsResult Begin(MmsSpan targets,LayoutSession& out) const noexcept {
        if(const auto result=Check())return result;
        if(out.handle_ || out.busy_)return MMS_E_BUSY;
        const auto api=api_;const auto extended=extended_;const auto thread=thread_;
        layout_session_detail::Busy guard(out.busy_);
        MmsHandle handle=0;
        const auto result=layout_session_detail::Call([&]{return api.begin(api.context,targets,&handle);});
        // Failure outputs are untrusted, including a handle written by a
        // misbehaving callback. Never adopt or release those arbitrary values.
        if(result!=MMS_OK)return result;
        if(!handle)return MMS_E_FAILED;
        out.api_=api;out.extended_=extended;out.handle_=handle;out.thread_=thread;out.revision_=0;out.terminal_=false;return MMS_OK;
    }
    MmsResult Begin(const MmsUiLayoutSessionTargetV1* targets,size_t count,LayoutSession& out) const noexcept {
        return Begin({targets,uint64_t(count),sizeof(MmsUiLayoutSessionTargetV1),1},out);
    }
    MmsResult Begin(const std::vector<MmsUiLayoutSessionTargetV1>& targets,LayoutSession& out) const noexcept { return Begin(targets.data(),targets.size(),out); }
    MmsResult BeginEx(MmsSpan targets,LayoutSession& out) const noexcept {
        if(const auto result=Check())return result;
        if(!extended_.beginEx)return MMS_E_UNSUPPORTED;
        if(out.handle_ || out.busy_)return MMS_E_BUSY;
        const auto api=api_;const auto extended=extended_;const auto thread=thread_;
        layout_session_detail::Busy guard(out.busy_);
        MmsHandle handle=0;
        const auto result=layout_session_detail::Call([&]{return extended.beginEx(extended.context,targets,&handle);});
        if(result!=MMS_OK)return result;
        if(!handle)return MMS_E_FAILED;
        out.api_=api;out.extended_=extended;out.handle_=handle;out.thread_=thread;out.revision_=0;out.terminal_=false;return MMS_OK;
    }
    MmsResult Begin(const MmsUiLayoutSessionTargetV1_2* targets,size_t count,LayoutSession& out) const noexcept {
        return BeginEx({targets,uint64_t(count),sizeof(MmsUiLayoutSessionTargetV1_2),2},out);
    }
    MmsResult Begin(const std::vector<MmsUiLayoutSessionTargetV1_2>& targets,LayoutSession& out) const noexcept { return Begin(targets.data(),targets.size(),out); }
    MmsResult BeginParent(MmsHandle element,MmsHandle parent,LayoutSession& out) const noexcept {
        const auto target=LayoutSessionTarget(element,MMS_LAYOUT_SESSION_PARENT,parent);return Begin(&target,1,out);
    }
    MmsResult BeginVisibility(MmsHandle element,LayoutSession& out) const noexcept {
        const auto target=LayoutSessionTarget(element,MMS_LAYOUT_SESSION_VISIBLE);return Begin(&target,1,out);
    }
    MmsResult BeginSize(MmsHandle element,LayoutSession& out) const noexcept {
        const auto target=LayoutSessionTarget(element,MMS_LAYOUT_SESSION_SIZE);return Begin(&target,1,out);
    }
    MmsResult BeginOrder(MmsHandle element,LayoutSession& out) const noexcept {
        const auto target=LayoutSessionTarget(element,MMS_LAYOUT_SESSION_ORDER);return Begin(&target,1,out);
    }
};
} // namespace mms::sdk
