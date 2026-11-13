// Include at file scope in the existing native history test harness, after
// native_global_registration_checks.inc, native_accessory_registration_checks.inc
// (and its pose fixture dependencies), then native_global_paste_checks.inc.
// Requires Check/failAfter from the harness. Call NativeGlobalPastePayloadChecks();
// this file does not add a TU route or change the shared build.
#include "../src/plugins/native_global_paste.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/scene_ownership.hpp"
#include <limits>

namespace {
namespace global_paste_payload_checks {
using Source=plugins::GlobalPasteSource;

template<class T> bool SameRecords(const std::vector<T>& a,const std::vector<T>& b) {
    return a.size()==b.size()&&(a.empty()||!std::memcmp(a.data(),b.data(),a.size()*sizeof(T)));
}
bool Same(const Source& a,const Source& b) {
    return a.frameOffset==b.frameOffset&&SameRecords(a.cameras,b.cameras)&&SameRecords(a.lights,b.lights)&&
        SameRecords(a.shadows,b.shadows)&&SameRecords(a.gravity,b.gravity)&&SameRecords(a.accessories,b.accessories);
}
bool Empty(const Source& source) {
    return !source.frameOffset&&source.cameras.empty()&&source.lights.empty()&&source.shadows.empty()&&
        source.gravity.empty()&&source.accessories.empty();
}
void SeedOutput(Source& source) {
    source={};source.frameOffset=77;
    source.cameras.resize(1);source.lights.resize(1);source.shadows.resize(1);
    source.gravity.resize(1);source.accessories.resize(1);
}
Source Expected(const GlobalPasteFixture& f) {
    Source source;source.cameras=f.cameras;source.lights=f.lights;source.shadows=f.shadows;
    source.gravity=f.gravity;source.accessories=f.accessories;return source;
}
void Hash(std::uint64_t& hash,const void* bytes,std::size_t size) {
    const auto* data=static_cast<const unsigned char*>(bytes);
    while(size--){hash^=*data++;hash*=1099511628211ull;}
}
template<class T> void HashValue(std::uint64_t& hash,const T& value) {Hash(hash,&value,sizeof(value));}
template<class T> void HashRecords(std::uint64_t& hash,const std::vector<T>& records) {
    HashValue(hash,records.size());Hash(hash,records.data(),records.size()*sizeof(T));
}
std::uint64_t LiveDigest(GlobalPasteFixture& f) {
    auto hash=f.Digest();HashValue(hash,f.accessory);
    // Read only fixture-owned extents, never a deliberately invalid app count.
    HashRecords(hash,f.cameras);HashRecords(hash,f.lights);HashRecords(hash,f.shadows);
    HashRecords(hash,f.gravity);HashRecords(hash,f.accessories);
    HashValue(hash,f.history.UndoCount());HashValue(hash,f.history.RedoCount());HashValue(hash,f.history.NativeEpoch());
    HashValue(hash,f.app->SceneRevision());HashValue(hash,f.app->SceneGeneration());
    HashValue(hash,f.app->SceneMutationInProgress());HashValue(hash,f.app->sceneRecoveryPending);return hash;
}
struct ClipboardFixture : GlobalPasteFixture {
    ClipboardFixture() {
        for(unsigned i=0;i<3;++i) {
            // Unsorted duplicates remain in source order. Each family starts
            // at a different nonzero relative frame, sharing one origin.
            const unsigned delta=i==1?11u:0u;
            auto& c=cameras[i];c.frame=3+delta;c.parentBone=0;
            for(unsigned lane=0;lane<24;++lane)c.interpolation[lane]=std::uint8_t(11+lane+i);
            for(unsigned lane=0;lane<3;++lane)c.reserved[lane]=std::uint8_t(41+lane+i);
            auto& l=lights[i];l.frame=8+delta;
            for(unsigned axis=0;axis<3;++axis)l.color[axis]=.1f*float(1+i+axis);
            auto& s=shadows[i];s.frame=13+delta;
            for(unsigned lane=0;lane<3;++lane)s.reserved[lane]=std::uint8_t(51+lane+i);
            auto& g=gravity[i];g.frame=18+delta;g.direction[0]=.1f*float(i);g.direction[2]=.2f*float(i);
            for(unsigned lane=0;lane<3;++lane)g.reserved[lane]=std::uint8_t(61+lane+i);
            auto& a=accessories[i];a.frameOffset=23+delta;a.parentBone=0;
            for(unsigned axis=0;axis<3;++axis){a.position[axis]=float(5+i+axis);a.rotation[axis]=.1f*float(1+i+axis);}
            for(unsigned lane=0;lane<3;++lane)a.reserved0[lane]=std::uint8_t(71+lane+i);
            for(unsigned lane=0;lane<2;++lane)a.reserved1[lane]=std::uint8_t(81+lane+i);
        }
    }
};
void SetCount(GlobalPasteFixture& f,unsigned family,std::uint32_t count) {
    auto& n=f.app->ClipboardCounts();
    switch(family){case 0:n.cameras=count;break;case 1:n.lights=count;break;case 2:n.shadows=count;break;
        case 3:n.gravity=count;break;default:n.accessories=count;break;}
}
void ClearPointer(GlobalPasteFixture& f,unsigned family) {
    switch(family){case 0:f.app->CameraClipboard()=nullptr;break;case 1:f.app->LightClipboard()=nullptr;break;
        case 2:f.app->ShadowClipboard()=nullptr;break;case 3:f.app->GravityClipboard()=nullptr;break;
        default:f.app->AccessoryClipboard()=nullptr;break;}
}
void ClearFamily(Source& source,unsigned family) {
    switch(family){case 0:source.cameras.clear();break;case 1:source.lights.clear();break;case 2:source.shadows.clear();break;
        case 3:source.gravity.clear();break;default:source.accessories.clear();break;}
}

void PlayheadAndOwnership() {
    ClipboardFixture f;const auto expected=Expected(f);Source source;
    for(int frame:{frame_limits::minimum,-10,0,10,frame_limits::maximum,(std::numeric_limits<int>::max)()}) {
        f.app->CurrentFrame()=frame;const auto before=LiveDigest(f);SeedOutput(source);
        Check(plugins::CaptureGlobalPastePayload(*f.app,source)==MMS_OK&&Same(source,expected)&&LiveDigest(f)==before,
            "global payload is identical at negative zero positive and out-of-range playheads with every raw field intact");
        Check(source.cameras.front().frame==3&&source.lights.front().frame==8&&source.shadows.front().frame==13&&
            source.gravity.front().frame==18&&source.accessories.front().frameOffset==23&&!source.frameOffset,
            "five different first relative frames keep one zero origin without family normalization");
        Check(source.cameras.data()!=f.cameras.data()&&source.lights.data()!=f.lights.data()&&source.shadows.data()!=f.shadows.data()&&
            source.gravity.data()!=f.gravity.data()&&source.accessories.data()!=f.accessories.data(),"all five captured arrays own their storage");
    }
    for(auto& k:f.cameras)k={};for(auto& k:f.lights)k={};for(auto& k:f.shadows)k={};
    for(auto& k:f.gravity)k={};for(auto& k:f.accessories)k={};
    Check(Same(source,expected),"rewriting every clipboard source record does not change captured bytes");
    f.ClearClipboard();
    // clear() alone would retain the allocation and would not test ownership.
    decltype(f.cameras){}.swap(f.cameras);decltype(f.lights){}.swap(f.lights);decltype(f.shadows){}.swap(f.shadows);
    decltype(f.gravity){}.swap(f.gravity);decltype(f.accessories){}.swap(f.accessories);
    Check(Same(source,expected),"freeing all five clipboard allocations leaves the complete owned payload intact");
    const auto before=LiveDigest(f);
    Check(plugins::CaptureGlobalPastePayload(*f.app,source)==MMS_OK&&Empty(source)&&LiveDigest(f)==before,
        "empty clipboard clears old payload without consulting the playhead");
}

void FamilyIsolation() {
    for(unsigned mask=0;mask<32;++mask)for(bool nullEmpty:{false,true}) {
        ClipboardFixture f;auto expected=Expected(f);f.app->CurrentFrame()=-10;
        for(unsigned family=0;family<5;++family)if(!(mask&(1u<<family))) {
            SetCount(f,family,0);if(nullEmpty)ClearPointer(f,family);ClearFamily(expected,family);
        }
        const auto before=LiveDigest(f);Source source;SeedOutput(source);
        Check(plugins::CaptureGlobalPastePayload(*f.app,source)==MMS_OK&&Same(source,expected)&&LiveDigest(f)==before,
            "every family combination accepts zero counts with null or nonnull storage and owns exactly the counted records");
    }
}

void DeferredSemantics() {
    for(unsigned mutation=0;mutation<8;++mutation) {
        ClipboardFixture f;f.app->CurrentFrame()=-10;
        if(mutation==0){f.cameras.back().parentModel=999;f.cameras.back().parentBone=123;}
        if(mutation==1)f.app->AccessorySlot(7)=nullptr;
        if(mutation==2)f.accessories.back().slot=255;
        if(mutation==3){f.accessories.back().parentModel=999;f.accessories.back().parentBone=321;}
        if(mutation==4)f.lights.back().color[0]=std::numeric_limits<float>::quiet_NaN();
        if(mutation==5)f.shadows.back().mode=255;
        if(mutation==6)f.gravity.back().acceleration=std::numeric_limits<float>::infinity();
        if(mutation==7){f.cameras.back().frame=f.lights.back().frame=f.shadows.back().frame=f.gravity.back().frame=UINT32_MAX;
            f.accessories.back().frameOffset=UINT32_MAX;}
        const auto expected=Expected(f);const auto before=LiveDigest(f);Source source;SeedOutput(source);
        Check(plugins::CaptureGlobalPastePayload(*f.app,source)==MMS_OK&&Same(source,expected)&&LiveDigest(f)==before,
            "capture preserves raw attachment slot finite-value mode and frame data without target resolution or semantic rejection");
    }
    ClipboardFixture f;f.app->CurrentFrame()=-10;
    f.app->PlaybackActive()=1;f.app->FrameStepPlayback()=1;f.app->state.recordPlaybackStartPending=1;
    f.app->sceneRecoveryPending=true;ScopedSceneMutation mutation(*f.app);
    const auto expected=Expected(f);const auto before=LiveDigest(f);Source source;
    Check(plugins::CaptureGlobalPastePayload(*f.app,source)==MMS_OK&&Same(source,expected)&&LiveDigest(f)==before,
        "raw capture does not admit a scene edit or consult playback mutation and recovery state");
}

void InvalidSources() {
    const std::array<std::size_t,5> limits{{mdl::KeyPoolLimit<mdl::CameraKey>(),mdl::KeyPoolLimit<mdl::LightKey>(),
        mdl::KeyPoolLimit<mdl::SelfShadowKey>(),mdl::KeyPoolLimit<mdl::GravityKey>(),mdl::KeyPoolLimit<mdl::AccessoryKey>()}};
    for(unsigned family=0;family<5;++family)for(unsigned failure=0;failure<3;++failure) {
        ClipboardFixture f;f.app->CurrentFrame()=-10;
        // Exercise each late boundary after earlier families have copied.
        if(failure!=1)ClearPointer(f,family);
        if(failure!=0)SetCount(f,family,std::uint32_t(limits[family]+1));
        const auto before=LiveDigest(f);Source source;SeedOutput(source);
        Check(plugins::CaptureGlobalPastePayload(*f.app,source)==MMS_E_ARGUMENT&&Empty(source)&&LiveDigest(f)==before,
            "each family's null pointer and oversized count reject atomically with empty output and unchanged live data");
        SeedOutput(source);failAfter=0;
        const auto result=plugins::CaptureGlobalPastePayload(*f.app,source);failAfter=-1;
        Check(result==(family?MMS_E_MEMORY:MMS_E_ARGUMENT)&&Empty(source)&&LiveDigest(f)==before,
            "sequential copy admission still reports an earlier allocation failure before a later invalid pointer or count");
    }
}

void AllocationFailures() {
    for(int frame:{-10,0,10}) {
        ClipboardFixture f;f.app->CurrentFrame()=frame;const auto expected=Expected(f);const auto before=LiveDigest(f);
        unsigned failures=0;bool completed=false;
        for(long allowance=0;allowance<128;++allowance) {
            Source source;SeedOutput(source);failAfter=allowance;
            const auto result=plugins::CaptureGlobalPastePayload(*f.app,source);failAfter=-1;
            Check(LiveDigest(f)==before,"every capture OOM leaves raw source pools selection poses metadata and history unchanged");
            if(result==MMS_OK){Check(Same(source,expected),"capture OOM sweep eventually owns every raw record");completed=true;break;}
            ++failures;Check(result==MMS_E_MEMORY&&Empty(source),"capture OOM discards all earlier family copies and previously populated output");
        }
        Check(completed&&failures>=5,"capture allocation sweep reaches the last accessory copy at every playhead sign");
    }
}

void LegacyAdmission() {
    ClipboardFixture f;const auto retained=GlobalPastePlan(f);std::shared_ptr<plugins::HistoryChange> change;
    for(unsigned gate=0;gate<4;++gate) {
        f.app->CurrentFrame()=-10;f.app->PlaybackActive()=gate==0;f.app->FrameStepPlayback()=gate==1;
        f.app->state.recordPlaybackStartPending=gate==2;
        const auto check=[&] {
            const auto before=LiveDigest(f);change=retained;failAfter=0;
            const auto result=plugins::PlanGlobalKeyPaste(*f.app,change);failAfter=-1;
            Check(result==MMS_E_NOT_READY&&!change&&LiveDigest(f)==before,
                "legacy playing pending-playback and mutation gates precede frame checks and capture allocation");
        };
        if(gate==3){ScopedSceneMutation mutation(*f.app);check();}else check();
    }
    f.app->PlaybackActive()=f.app->FrameStepPlayback()=0;f.app->state.recordPlaybackStartPending=0;
    const auto source=Expected(f);
    for(int frame:{-10,-1,(std::numeric_limits<int>::max)()}) {
        f.app->CurrentFrame()=frame;const auto before=LiveDigest(f);
        for(bool explicitSource:{false,true}) {
            change=retained;failAfter=0;
            const auto result=explicitSource?plugins::PlanGlobalKeyPaste(*f.app,source,change):plugins::PlanGlobalKeyPaste(*f.app,change);
            failAfter=-1;
            Check(result==MMS_E_ARGUMENT&&!change&&LiveDigest(f)==before,
                "both legacy planner signatures retain negative cross-zero and out-of-range refusal before allocation");
        }
    }
    for(unsigned family=0;family<5;++family) {
        ClipboardFixture overflow;overflow.app->CurrentFrame()=1;
        // Earlier families remain legal; only this family's final key overflows.
        const auto maximum=std::uint32_t(frame_limits::maximum);
        switch(family){case 0:overflow.cameras.back().frame=maximum;break;case 1:overflow.lights.back().frame=maximum;break;
            case 2:overflow.shadows.back().frame=maximum;break;case 3:overflow.gravity.back().frame=maximum;break;
            default:overflow.accessories.back().frameOffset=maximum;break;}
        const auto before=LiveDigest(overflow);Source payload;
        Check(plugins::CaptureGlobalPastePayload(*overflow.app,payload)==MMS_OK&&Same(payload,Expected(overflow)),
            "capture accepts a legal maximum relative frame independently of a positive playhead");
        change=retained;
        Check(plugins::PlanGlobalKeyPaste(*overflow.app,change)==MMS_E_ARGUMENT&&!change&&LiveDigest(overflow)==before,
            "legacy wrapper still rejects offset overflow in every late family without publication");
    }
}

void LegacyPositivePlanning() {
    for(int frame:{0,7}) {
        ClipboardFixture f,oracle;f.app->CurrentFrame()=oracle.app->CurrentFrame()=frame;
        auto expected=Expected(oracle);expected.frameOffset=unsigned(frame);
        const auto before=LiveDigest(f),oracleBefore=LiveDigest(oracle);const GlobalPasteKeys original(*f.app);
        auto change=GlobalPastePlan(f);std::shared_ptr<plugins::HistoryChange> explicitChange;
        Check(plugins::PlanGlobalKeyPaste(*oracle.app,expected,explicitChange)==MMS_OK&&change&&explicitChange&&
            LiveDigest(f)==before&&LiveDigest(oracle)==oracleBefore,"legacy wrapper and existing source planner remain private at zero and positive origins");
        GlobalPasteCommit(f,change);GlobalPasteCommit(oracle,explicitChange);const GlobalPasteKeys pasted(*oracle.app);
        Check(pasted.Same(*f.app)&&f.app->CurrentFrame()==frame&&f.app->LastRegisteredFrame()==unsigned(frame)+34&&
            f.app->LastRegisteredFrame()==oracle.app->LastRegisteredFrame()&&f.history.UndoCount()==1,
            "legacy mixed positive planning keeps source planner output shared offset and one history step");
        Check(GlobalPasteAt(f.app->CameraKeys(),unsigned(frame)+3).fov==f.cameras.back().fov&&
            GlobalPasteAt(f.app->LightKeys(),unsigned(frame)+8).color[0]==f.lights.back().color[0]&&
            GlobalPasteAt(f.app->ShadowKeys(),unsigned(frame)+13).mode==f.shadows.back().mode&&
            GlobalPasteAt(f.app->GravityKeys(),unsigned(frame)+18).noise==f.gravity.back().noise&&
            GlobalPasteAt(f.app->AccessoryKeys(7),unsigned(frame)+23).opacity==f.accessories.back().opacity,
            "all five families retain their different relative origins and last duplicate wins");
        const auto unchanged=LiveDigest(f);auto noop=GlobalPastePlan(f);
        Check(!noop&&LiveDigest(f)==unchanged,"repeating the same positive wrapper plan remains a clean no-op");
        f.ClearClipboard();f.app->CurrentFrame()=23;
        Check(f.history.History(false)==MMS_OK&&original.Same(*f.app)&&f.app->LastRegisteredFrame()==9&&f.app->CurrentFrame()==23,
            "legacy undo retains the original keys and current real playhead after clipboard removal");
        Check(f.history.History(true)==MMS_OK&&pasted.Same(*f.app)&&f.app->LastRegisteredFrame()==unsigned(frame)+34&&f.app->CurrentFrame()==23,
            "legacy redo owns all positive paste data after clipboard removal");
        Check(f.history.History(false)==MMS_OK&&f.history.RedoCount()==1,"seed a redo branch for empty wrapper planning");
        const auto emptyBefore=LiveDigest(f);noop=GlobalPastePlan(f);
        Check(!noop&&LiveDigest(f)==emptyBefore&&f.history.RedoCount()==1,"empty legacy wrapper planning preserves redo and all live data");
    }
}
}
void NativeGlobalPastePayloadChecks() {
    std::puts("  global paste payload: owned raw records, independent origin and legacy admission");std::fflush(stdout);
    global_paste_payload_checks::PlayheadAndOwnership();
    global_paste_payload_checks::FamilyIsolation();
    global_paste_payload_checks::DeferredSemantics();
    global_paste_payload_checks::InvalidSources();
    global_paste_payload_checks::AllocationFailures();
    global_paste_payload_checks::LegacyAdmission();
    global_paste_payload_checks::LegacyPositivePlanning();
}
}
