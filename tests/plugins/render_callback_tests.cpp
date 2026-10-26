// Regression checks for the production render service, including real allocation failures.
#include "render_service.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>

static thread_local bool failAllocation = false;
static thread_local unsigned allocationHits = 0;
void* operator new(std::size_t n) {
    if (failAllocation) { ++allocationHits; throw std::bad_alloc(); }
    if (auto p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
using namespace mikumikustudio::plugins;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return false; } } while(false)
struct Seen { unsigned frames=0, begins=0, ends=0, releases=0; MmsRenderInfoV1 info{}; };
static void MMS_CALL Frame(void* p,const MmsRenderInfoV1* info) { auto& s=*static_cast<Seen*>(p); ++s.frames; s.info=*info; }
static void MMS_CALL Phase(void* p,const MmsRenderInfoV1* info,uint32_t phase) {
    auto& s=*static_cast<Seen*>(p); if(phase==MMS_RENDER_PHASE_BEGIN)++s.begins; else if(phase==MMS_RENDER_PHASE_END)++s.ends; s.info=*info;
}
static void MMS_CALL Released(void* p) { ++static_cast<Seen*>(p)->releases; }
static void MMS_CALL ThrowFrame(void*,const MmsRenderInfoV1*) { throw 7; }
static void MMS_CALL ThrowPhase(void*,const MmsRenderInfoV1*,uint32_t) { throw 9; }
static bool IndependentCallbacks() {
    RenderService service; RenderService::Client c; c.id="callback.test"; Seen seen;
    auto api=service.Api12(c);
    CHECK(api.setFrameCallback(api.context,Frame,&seen)==MMS_OK);
    CHECK(api.setPhaseCallback(api.context,Phase,&seen)==MMS_OK);
    service.FrameBegin(); service.FramePresented(640,480,21); service.FrameEnd();
    CHECK(seen.frames==1 && seen.begins==1 && seen.ends==1);
    api.clearFrameCallback(api.context);
    service.FrameBegin(); service.FramePresented(640,480,21); service.FrameEnd();
    CHECK(seen.frames==1 && seen.begins==2 && seen.ends==2);
    CHECK(api.setFrameCallback(api.context,Frame,&seen)==MMS_OK);
    api.clearPhaseCallback(api.context);
    service.FrameBegin(); service.FramePresented(800,600,22); service.FrameEnd();
    CHECK(seen.frames==2 && seen.begins==2 && seen.ends==2);
    CHECK(seen.info.frameIndex==3 && seen.info.width==800 && seen.info.height==600);
    api.clearFrameCallback(api.context); api.clearPhaseCallback(api.context);
    service.FramePresented(800,600,22); service.FrameBegin(); service.FrameEnd();
    CHECK(seen.frames==2 && seen.begins==2 && seen.ends==2);
    CHECK(api.setPhaseCallback(api.context,Phase,&seen)==MMS_OK);
    service.FrameBegin(); CHECK(seen.begins==3);
    return true;
}
static bool AllocationAndExceptions() {
    RenderService service; RenderService::Client a,b; a.id="first"; b.id="second"; Seen first,second;
    auto aa=service.Api12(a),bb=service.Api12(b);
    CHECK(aa.setFrameCallback(aa.context,Frame,&first)==MMS_OK);
    CHECK(bb.setFrameCallback(bb.context,Frame,&second)==MMS_OK);
    CHECK(aa.setPhaseCallback(aa.context,Phase,&first)==MMS_OK);
    CHECK(bb.setPhaseCallback(bb.context,Phase,&second)==MMS_OK);
    allocationHits=0; failAllocation=true;
    service.FramePresented(321,123,20); service.FrameBegin(); service.FrameEnd();
    failAllocation=false;
    CHECK(allocationHits==3);
    CHECK(first.frames==0 && second.frames==0 && first.begins==0 && second.begins==0 && first.ends==0 && second.ends==0);
    MmsRenderInfoV1 info{}; CHECK(aa.query(aa.context,&info)==MMS_OK);
    CHECK(info.frameIndex==1 && info.width==321 && info.height==123);
    service.FramePresented(321,123,20); service.FrameBegin(); service.FrameEnd();
    CHECK(first.frames==1 && second.frames==1 && first.begins==1 && second.begins==1 && first.ends==1 && second.ends==1);
    CHECK(aa.setFrameCallback(aa.context,ThrowFrame,nullptr)==MMS_OK);
    CHECK(aa.setPhaseCallback(aa.context,ThrowPhase,nullptr)==MMS_OK);
    service.FramePresented(1,1,20); service.FrameBegin(); service.FrameEnd();
    CHECK(second.frames==2 && second.begins==2 && second.ends==2);
    return true;
}
static bool FailedRegistration(bool phase) {
    RenderService service; RenderService::Client target,fillers[256]; target.id="target";
    auto api=service.Api12(target); api.clearFrameCallback(api.context); api.clearPhaseCallback(api.context);
    // Find an actual capacity boundary rather than assuming a library's vector
    // growth factor. Successful attempts are cleared before adding a filler.
    Seen seen; MmsResult r=MMS_OK;
    for(auto& c:fillers) {
        service.Api(c); allocationHits=0; failAllocation=true;
        r=phase?api.setPhaseCallback(api.context,Phase,&seen):api.setFrameCallback(api.context,Frame,&seen);
        failAllocation=false;
        if(r==MMS_E_MEMORY) break;
        CHECK(r==MMS_OK && allocationHits==0);
        api.clearFrameCallback(api.context); api.clearPhaseCallback(api.context);
    }
    CHECK(allocationHits==1 && r==MMS_E_MEMORY);
    CHECK(target.frame==nullptr && target.phase==nullptr && target.frameContext==nullptr && target.phaseContext==nullptr);
    r=phase?api.setPhaseCallback(api.context,Phase,&seen):api.setFrameCallback(api.context,Frame,&seen);
    CHECK(r==MMS_OK);
    service.FramePresented(1,1,0); service.FrameBegin();
    CHECK(phase ? seen.begins==1 : seen.frames==1);
    return true;
}
static bool Retirement() {
    RenderService service; RenderService::Client c; c.id="retired"; Seen seen;
    auto api=service.Api12(c);
    CHECK(api.setFrameCallback(api.context,Frame,&seen)==MMS_OK);
    CHECK(api.setPhaseCallback(api.context,Phase,&seen)==MMS_OK);
    MmsRenderLeaseDescV1 desc{Header<MmsRenderLeaseDescV1>(),Text(c.id),Released,&seen,MMS_RENDER_LEASE_DEVICE,0};
    MmsHandle handle=0; CHECK(api.acquireLease(api.context,&desc,&handle)==MMS_OK);
    service.DropClient(c.id);
    CHECK(api.setFrameCallback(api.context,Frame,&seen)==MMS_E_NOT_READY);
    CHECK(api.setPhaseCallback(api.context,Phase,&seen)==MMS_E_NOT_READY);
    MmsHandle sentinel=123; CHECK(api.acquireLease(api.context,&desc,&sentinel)==MMS_E_NOT_READY && sentinel==123);
    CHECK(api.releaseLease(api.context,handle)==MMS_E_NOT_FOUND);
    service.FramePresented(1,1,0); service.FrameBegin(); service.FrameEnd(); service.DeviceLost(2);
    CHECK(seen.frames==0 && seen.begins==0 && seen.ends==0 && seen.releases==0);
    api.clearFrameCallback(api.context); api.clearPhaseCallback(api.context);
    return true;
}
int main() {
    const bool ok=IndependentCallbacks() && AllocationAndExceptions() && FailedRegistration(false) && FailedRegistration(true) && Retirement();
    std::printf("render callbacks %s checks=%u\n",ok?"PASS":"FAIL",checks); return ok?0:1;
}
