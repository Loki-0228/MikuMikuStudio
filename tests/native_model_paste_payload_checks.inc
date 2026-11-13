// Include at file scope after the base Fixture/Scale/Check/failAfter definitions in
// native_edit_history_tests.cpp. No other checks.inc fixture is required.
// Call NativeModelPastePayloadChecks(); this file does not register a TU route.
#include "../src/plugins/native_model_paste.hpp"
#include "../src/plugins/storage_store.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include <limits>

namespace {
namespace model_paste_payload_checks {
using Source=plugins::ModelPasteSource;
constexpr unsigned all=plugins::ModelPasteAll;

void Hash(std::uint64_t& hash,const void* bytes,std::size_t size) {
    const auto* data=static_cast<const unsigned char*>(bytes);
    while(size--){hash^=*data++;hash*=1099511628211ull;}
}
template<class T> void HashValue(std::uint64_t& hash,const T& value) {Hash(hash,&value,sizeof(value));}
bool DefaultOptions(const Source& source) {
    return !source.advanceLastRegisteredFrame&&source.resetDisplayCursor&&!source.curveTargetsResolved&&
        source.boneCurveBlock.empty()&&source.morphCurveBlock.empty()&&
        source.boneCurveTargets.empty()&&source.morphCurveTargets.empty();
}
bool Empty(const Source& source) {
    return source.bones.empty()&&source.morphs.empty()&&source.displays.empty()&&
        source.frameOffset==0&&DefaultOptions(source);
}
void SeedOutput(Source& source) {
    source={};source.frameOffset=77;source.advanceLastRegisteredFrame=true;source.resetDisplayCursor=false;
    source.curveTargetsResolved=true;source.boneCurveBlock={1,2};source.morphCurveBlock={3,4};
    source.boneCurveTargets={9};source.morphCurveTargets={8};
    source.bones.resize(1);source.morphs.resize(1);source.displays.resize(1);
    source.bones[0].targetResolved=true;source.bones[0].targetIndex=7;
    source.morphs[0].targetResolved=true;source.morphs[0].targetIndex=6;
    source.displays[0].ik.resize(1);source.displays[0].selectors.resize(1);
    source.displays[0].ikTargetsResolved=true;source.displays[0].ikTargets={5};
}
std::uint64_t PayloadDigest(const Source& source) {
    std::uint64_t hash=14695981039346656037ull;
    HashValue(hash,source.frameOffset);HashValue(hash,source.advanceLastRegisteredFrame);
    HashValue(hash,source.resetDisplayCursor);HashValue(hash,source.curveTargetsResolved);
    for(const auto* bytes:{&source.boneCurveBlock,&source.morphCurveBlock}) {
        HashValue(hash,bytes->size());Hash(hash,bytes->data(),bytes->size());
    }
    for(const auto* targets:{&source.boneCurveTargets,&source.morphCurveTargets}) {
        HashValue(hash,targets->size());Hash(hash,targets->data(),targets->size()*sizeof(int));
    }
    HashValue(hash,source.bones.size());HashValue(hash,source.morphs.size());HashValue(hash,source.displays.size());
    for(const auto& bone:source.bones) {
        HashValue(hash,bone.key);HashValue(hash,bone.scale.v);HashValue(hash,bone.scaleCurve);
        HashValue(hash,bone.targetResolved);HashValue(hash,bone.targetIndex);
    }
    for(const auto& morph:source.morphs) {
        HashValue(hash,morph.key);HashValue(hash,morph.targetResolved);HashValue(hash,morph.targetIndex);
    }
    for(const auto& display:source.displays) {
        HashValue(hash,display.frame);HashValue(hash,display.visible);HashValue(hash,display.ikTargetsResolved);
        HashValue(hash,display.ik.size());HashValue(hash,display.selectors.size());HashValue(hash,display.ikTargets.size());
        Hash(hash,display.ik.data(),display.ik.size()*sizeof(mdl::IkClipboardState));
        Hash(hash,display.selectors.data(),display.selectors.size()*sizeof(mdl::SelectorClipboardState));
        Hash(hash,display.ikTargets.data(),display.ikTargets.size()*sizeof(int));
    }
    return hash;
}

struct ClipboardFixture {
    Fixture model;
    std::unique_ptr<MMDApp> app=std::make_unique<MMDApp>();
    plugins::StorageStore storage;
    plugins::SharedHistory history{storage};
    std::unique_ptr<mdl::BoneClipboardRecord[]> bones=std::make_unique<mdl::BoneClipboardRecord[]>(2);
    std::unique_ptr<mdl::MorphClipboardRecord[]> morphs=std::make_unique<mdl::MorphClipboardRecord[]>(2);
    std::unique_ptr<mdl::DisplayClipboardRecord[]> displays=std::make_unique<mdl::DisplayClipboardRecord[]>(2);
    std::array<std::unique_ptr<mdl::IkClipboardState[]>,2> ik;
    std::array<std::unique_ptr<mdl::SelectorClipboardState[]>,2> selectors;
    std::array<bone_scale::Matrix,2> scales;
    std::array<bone_scale::Curve,2> curves{{{12,24,91,115},{7,31,103,121}}};
    ClipboardFixture() {
        ::new(static_cast<void*>(&app->state)) MMDAppState{};
        app->ModelSlot(0)=model.raw();app->SelectedModelSlot()=0;
        app->SceneModified()=1;app->LastRegisteredFrame()=123;
        model.selected[0]=1;model.dirty[0]=1;model.bones[0].trans[0]=9;
        model.morphs[0].value=.625f;model.m->boneKeys[0].allocated=1;
        model.m->morphKeys[0].allocated=1;model.m->displayKeys[0].allocated=1;
        bone_scale::SetPose(model.raw(),0,Scale(2));bone_scale::SetKey(model.raw(),0,Scale(3));
        bone_scale::SetKeyCurve(model.raw(),0,{15,33,99,117});
        for(unsigned i=0;i<2;++i) {
            auto& bone=bones[i];std::strcpy(bone.name,i?"second bone":"first bone");
            bone.frame=3+i*14;bone.reserved0[0]=static_cast<unsigned char>(41+i);
            bone.reserved1[2]=static_cast<unsigned char>(61+i);bone.physicsDisabled=static_cast<unsigned char>(i);
            for(unsigned axis=0;axis<3;++axis)bone.position[axis]=float(2+i+axis);
            for(unsigned axis=0;axis<4;++axis)bone.rotation[axis]=float(1+i+axis)/8;
            for(unsigned lane=0;lane<16;++lane)bone.interpolation[lane]=static_cast<unsigned char>(11+i+lane*3);
            for(unsigned lane=0;lane<9;++lane)scales[i].v[lane]=float(1+i+lane)/100;
            scales[i].v[0]=2.f+i;scales[i].v[4]=3.f+i;scales[i].v[8]=4.f+i;
            bone_scale::CopyClipboard(app.get(),int(i),scales[i],false,curves[i]);
            auto& morph=morphs[i];std::strcpy(morph.name,i?"second morph":"first morph");
            morph.frame=8+i*15;morph.value=.25f+float(i)/2;morph.reserved[1]=static_cast<unsigned char>(71+i);
            ik[i]=std::make_unique<mdl::IkClipboardState[]>(2);
            selectors[i]=std::make_unique<mdl::SelectorClipboardState[]>(2);
            for(unsigned j=0;j<2;++j) {
                std::strcpy(ik[i][j].boneName,j?"right IK":"left IK");
                ik[i][j].enabled=static_cast<unsigned char>((i+j)%2);
                std::strcpy(selectors[i][j].boneName,j?"child":"root");
                selectors[i][j].modelIndex=int(i+j);selectors[i][j].boneIndex=int(3+i+j);
            }
            displays[i].frame=11+i*18;displays[i].visible=static_cast<unsigned char>(i);
            displays[i].ikCount=2;displays[i].ikStates=ik[i].get();
            displays[i].selectorCount=2;displays[i].selectorStates=selectors[i].get();
        }
        app->BoneClipboard()=bones.get();app->MorphClipboard()=morphs.get();app->DisplayClipboard()=displays.get();
        app->ClipboardCounts().bones=app->ClipboardCounts().morphs=app->ClipboardCounts().displays=2;
    }
    void ReleaseSource() {
        app->BoneClipboard()=nullptr;app->MorphClipboard()=nullptr;app->DisplayClipboard()=nullptr;app->ClipboardCounts()={};
        bones.reset();morphs.reset();displays.reset();
        for(auto& item:ik)item.reset();for(auto& item:selectors)item.reset();
        bone_scale::CopyClipboard(app.get(),0,bone_scale::Matrix{},false);
    }
    ~ClipboardFixture(){ReleaseSource();app->ModelSlot(0)=nullptr;}
    std::uint64_t LiveDigest() {
        auto hash=model.Digest();HashValue(hash,app->state);
        HashValue(hash,model.selected);HashValue(hash,model.morphs);
        if(bones)Hash(hash,bones.get(),2*sizeof(mdl::BoneClipboardRecord));
        if(morphs)Hash(hash,morphs.get(),2*sizeof(mdl::MorphClipboardRecord));
        if(displays)Hash(hash,displays.get(),2*sizeof(mdl::DisplayClipboardRecord));
        // Read only fixture-owned extents, even when an admission test poisons
        // the borrowed pointers/counts. Never follow the malformed input here.
        for(unsigned i=0;i<2;++i) {
            if(ik[i])Hash(hash,ik[i].get(),2*sizeof(mdl::IkClipboardState));
            if(selectors[i])Hash(hash,selectors[i].get(),2*sizeof(mdl::SelectorClipboardState));
            HashValue(hash,bone_scale::Clipboard(app.get(),int(i),false).v);
            HashValue(hash,bone_scale::ClipboardCurve(app.get(),int(i),false));
            HashValue(hash,bone_scale::Pose(model.raw(),int(i)).v);
        }
        HashValue(hash,history.UndoCount());HashValue(hash,history.RedoCount());HashValue(hash,history.NativeEpoch());
        return hash;
    }
};
struct LiveSnapshot {
    std::uint64_t digest;
    bone_scale::AuthoringState scales;
    explicit LiveSnapshot(ClipboardFixture& f):digest(f.LiveDigest()),scales(bone_scale::AuthoringState::Capture(f.model.raw())){}
    bool Matches(ClipboardFixture& f)const {
        return digest==f.LiveDigest()&&bone_scale::AuthoringState::Capture(f.model.raw()).Equals(scales);
    }
};
void MatchesClipboard(const ClipboardFixture& f,const Source& source,unsigned families,unsigned offset) {
    Check(source.frameOffset==offset&&DefaultOptions(source),"payload output keeps only the requested origin and ordinary capture defaults");
    Check(source.bones.size()==((families&1)?2u:0u)&&source.morphs.size()==((families&2)?2u:0u)&&
        source.displays.size()==((families&4)?2u:0u),"payload owns exactly the requested families");
    for(unsigned i=0;i<source.bones.size();++i) {
        const auto& record=source.bones[i];
        Check(!std::memcmp(&record.key,&f.bones[i],sizeof(record.key))&&record.scale==f.scales[i]&&
            record.scaleCurve==f.curves[i]&&!record.targetResolved&&record.targetIndex==-1,
            "bone payload preserves complete raw bytes scale matrix and scale curve without target resolution or mirroring");
    }
    for(unsigned i=0;i<source.morphs.size();++i) {
        const auto& record=source.morphs[i];
        Check(!std::memcmp(&record.key,&f.morphs[i],sizeof(record.key))&&!record.targetResolved&&record.targetIndex==-1,
            "morph payload preserves complete raw bytes and unresolved identity");
    }
    for(unsigned i=0;i<source.displays.size();++i) {
        const auto& record=source.displays[i];
        Check(record.frame==f.displays[i].frame&&record.visible==f.displays[i].visible&&
            record.ik.size()==2&&record.selectors.size()==2&&!record.ikTargetsResolved&&record.ikTargets.empty(),
            "display payload preserves relative frame visibility and nested counts without resolved IK targets");
        Check(record.ik.data()!=f.ik[i].get()&&record.selectors.data()!=f.selectors[i].get()&&
            !std::memcmp(record.ik.data(),f.ik[i].get(),2*sizeof(mdl::IkClipboardState))&&
            !std::memcmp(record.selectors.data(),f.selectors[i].get(),2*sizeof(mdl::SelectorClipboardState)),
            "display owns byte-exact independent IK and selector storage");
    }
}

void PlayheadAndOwnership() {
    Source surviving;std::uint64_t ownedDigest=0;
    {
        ClipboardFixture f;std::uint64_t baseline=0;
        for(int frame:{-10,0,10}) {
            f.app->CurrentFrame()=frame;const LiveSnapshot live(f);Source payload;SeedOutput(payload);
            Check(plugins::CaptureModelPastePayload(*f.app,payload)==MMS_OK,"complete payload capture accepts signed and zero playheads");
            MatchesClipboard(f,payload,all,0);
            if(frame==-10)baseline=PayloadDigest(payload);
            Check(PayloadDigest(payload)==baseline&&live.Matches(f),"the same raw clipboard captures identically without changing the live scene at -10 0 and 10");
            Check(payload.bones[0].key.frame==3&&payload.bones[1].key.frame==17&&
                payload.morphs[0].key.frame==8&&payload.morphs[1].key.frame==23&&
                payload.displays[0].frame==11&&payload.displays[1].frame==29,
                "different family first frames and record spacings are never rebased to zero");
            Source source;SeedOutput(source);const auto status=plugins::CaptureModelPasteSource(*f.app,source);
            if(frame<0)Check(status==MMS_E_ARGUMENT&&Empty(source),"legacy wrapper still rejects negative playheads and clears all prior output");
            else {
                Check(status==MMS_OK,"legacy wrapper accepts the same complete payload at nonnegative playheads");
                MatchesClipboard(f,source,all,unsigned(frame));
                Check(std::int64_t(source.frameOffset)+source.bones[0].key.frame==frame+3&&
                    std::int64_t(source.frameOffset)+source.morphs[0].key.frame==frame+8&&
                    std::int64_t(source.frameOffset)+source.displays[0].frame==frame+11,
                    "legacy wrapper applies one shared origin without changing any relative frame");
            }
            Check(live.Matches(f),"both capture APIs preserve clipboard poses selection metadata dirty flags and history");
        }
        Check(plugins::CaptureModelPastePayload(*f.app,surviving)==MMS_OK,"capture data to retain beyond source lifetime");
        ownedDigest=PayloadDigest(surviving);
        for(unsigned i=0;i<2;++i) {
            f.bones[i]={};f.morphs[i]={};f.displays[i].frame=987;f.displays[i].visible=7;
            f.ik[i][0]={};f.selectors[i][1]={};
            bone_scale::CopyClipboard(f.app.get(),int(i),Scale(9),false,{1,2,3,4});
        }
        Check(PayloadDigest(surviving)==ownedDigest,"rewriting raw records nested buffers and scale clipboard cannot change captured data");
        f.ReleaseSource();
        Check(PayloadDigest(surviving)==ownedDigest,"releasing every raw and nested source allocation preserves captured payload");
    }
    Check(PayloadDigest(surviving)==ownedDigest,"payload remains owned after its source application and model fixture are destroyed");
}

void FamilyIsolation() {
    for(unsigned families=0;families<=all;++families) {
        ClipboardFixture f;
        if(!(families&1)){f.app->BoneClipboard()=nullptr;f.app->ClipboardCounts().bones=UINT32_MAX;}
        if(!(families&2)){f.app->MorphClipboard()=nullptr;f.app->ClipboardCounts().morphs=UINT32_MAX;}
        if(!(families&4)){f.app->DisplayClipboard()=nullptr;f.app->ClipboardCounts().displays=UINT32_MAX;f.displays[0].ikCount=-1;}
        for(int frame:{-10,0,10}) {
            f.app->CurrentFrame()=frame;const LiveSnapshot live(f);Source source;SeedOutput(source);
            Check(plugins::CaptureModelPastePayload(*f.app,source,families)==MMS_OK,"malformed unused families never affect requested payload capture");
            MatchesClipboard(f,source,families,0);
            SeedOutput(source);const auto status=plugins::CaptureModelPasteSource(*f.app,source,families);
            if(frame<0)Check(status==MMS_E_ARGUMENT&&Empty(source),"even a zero-family legacy capture retains its negative-frame refusal");
            else {Check(status==MMS_OK,"legacy wrapper retains family-local admission");MatchesClipboard(f,source,families,unsigned(frame));}
            Check(live.Matches(f),"family isolation does not repair or mutate malformed unused clipboard storage");
        }
    }
    ClipboardFixture f;f.app->ClipboardCounts()={};f.app->BoneClipboard()=nullptr;f.app->MorphClipboard()=nullptr;f.app->DisplayClipboard()=nullptr;
    f.app->CurrentFrame()=10;const LiveSnapshot live(f);Source source;SeedOutput(source);
    Check(plugins::CaptureModelPastePayload(*f.app,source)==MMS_OK&&Empty(source),"zero counts permit null sources and clear previously populated output");
    Check(plugins::CaptureModelPasteSource(*f.app,source)==MMS_OK&&source.frameOffset==10&&source.bones.empty()&&
        source.morphs.empty()&&source.displays.empty()&&DefaultOptions(source)&&live.Matches(f),"empty legacy capture retains its shared origin and changes no live state");
    f.app->DisplayClipboard()=f.displays.get();f.app->ClipboardCounts().displays=2;
    for(unsigned i=0;i<2;++i){f.displays[i].ikCount=f.displays[i].selectorCount=0;f.displays[i].ikStates=nullptr;f.displays[i].selectorStates=nullptr;}
    const LiveSnapshot emptyNested(f);
    Check(plugins::CaptureModelPastePayload(*f.app,source)==MMS_OK&&source.displays.size()==2&&
        source.displays[0].ik.empty()&&source.displays[0].selectors.empty()&&source.displays[1].ik.empty()&&
        source.displays[1].selectors.empty()&&emptyNested.Matches(f),"zero nested counts permit null IK and selector sources");
}

void InvalidSourcesAndOrigins() {
    for(unsigned bad=0;bad<20;++bad) {
        ClipboardFixture f;unsigned families=all;
        switch(bad) {
        case 0:families=8;break;
        case 1:f.app->BoneClipboard()=nullptr;break;
        case 2:f.app->MorphClipboard()=nullptr;break;
        case 3:f.app->DisplayClipboard()=nullptr;break;
        case 4:f.app->ClipboardCounts().bones=mdl::kBoneKeyCapacityLimit+1;break;
        case 5:f.app->ClipboardCounts().morphs=mdl::kMorphKeyCapacityLimit+1;break;
        case 6:f.app->ClipboardCounts().displays=std::uint32_t(mdl::KeyPoolLimit<mdl::DisplayKey>()+1);break;
        case 7:f.displays[1].ikCount=-1;break;
        case 8:f.displays[1].selectorCount=-1;break;
        // These counts are signed. On x64 the pool ceiling is INT_MAX,
        // so ceiling+1 is not a representable positive nested count.
        case 9:f.displays[1].ikCount=(std::numeric_limits<int>::min)();break;
        case 10:f.displays[1].selectorCount=(std::numeric_limits<int>::min)();break;
        case 11:f.displays[1].ikStates=nullptr;break;
        case 12:f.displays[1].selectorStates=nullptr;break;
        case 13:f.bones[1].frame=unsigned(frame_limits::maximum)+1;break;
        case 14:f.morphs[1].frame=unsigned(frame_limits::maximum)+1;break;
        case 15:f.displays[1].frame=unsigned(frame_limits::maximum)+1;break;
        case 16:f.bones[1].frame=UINT32_MAX;break;
        case 17:f.morphs[1].frame=UINT32_MAX;break;
        case 18:f.displays[1].frame=UINT32_MAX;break;
        case 19:families=UINT32_MAX;break;
        }
        const LiveSnapshot live(f);
        for(bool wrapper:{false,true}) {
            Source source;SeedOutput(source);
            const auto status=wrapper?plugins::CaptureModelPasteSource(*f.app,source,families):
                plugins::CaptureModelPastePayload(*f.app,source,families);
            Check(status==MMS_E_ARGUMENT&&Empty(source)&&live.Matches(f),
                "invalid source pointers counts masks and late relative frames reject with empty output and no partial live writes");
        }
    }
    for(unsigned family=0;family<3;++family) {
        ClipboardFixture f;
        if(family==0)f.bones[1].frame=frame_limits::maximum;
        if(family==1)f.morphs[1].frame=frame_limits::maximum;
        if(family==2)f.displays[1].frame=frame_limits::maximum;
        f.app->CurrentFrame()=0;Source source;
        Check(plugins::CaptureModelPasteSource(*f.app,source)==MMS_OK,"maximum relative frame remains legal at origin zero");
        f.app->CurrentFrame()=1;const LiveSnapshot live(f);SeedOutput(source);
        Check(plugins::CaptureModelPastePayload(*f.app,source)==MMS_OK,"payload capture does not apply the playhead to a legal maximum relative frame");
        SeedOutput(source);
        Check(plugins::CaptureModelPasteSource(*f.app,source)==MMS_E_ARGUMENT&&Empty(source)&&live.Matches(f),
            "legacy wrapper rejects each family's late shared-offset overflow without retaining a partial payload");
    }
    for(int frame:{frame_limits::minimum,-10,frame_limits::maximum,(std::numeric_limits<int>::max)()}) {
        ClipboardFixture f;f.app->CurrentFrame()=frame;
        // Origin admission is independent of record arithmetic: all records
        // below are legal even at the maximum accepted nonnegative origin.
        for(unsigned i=0;i<2;++i)f.bones[i].frame=f.morphs[i].frame=f.displays[i].frame=0;
        const LiveSnapshot live(f);Source source;SeedOutput(source);
        Check(plugins::CaptureModelPastePayload(*f.app,source)==MMS_OK,"payload API is independent even of an out-of-range application playhead");
        SeedOutput(source);
        const auto status=plugins::CaptureModelPasteSource(*f.app,source);
        if(frame<0||frame>frame_limits::maximum)Check(status==MMS_E_ARGUMENT&&Empty(source),"legacy origin guard and output clearing remain intact");
        else {Check(status==MMS_OK,"maximum legal origin accepts zero relative frames");MatchesClipboard(f,source,all,unsigned(frame));}
        Check(live.Matches(f),"playhead boundary admission preserves the entire live fixture");
    }
}

void AllocationFailures() {
    for(bool wrapper:{false,true})for(int frame:{0,10}) {
        ClipboardFixture f;f.app->CurrentFrame()=wrapper?frame:-10;const LiveSnapshot live(f);
        unsigned failures=0;bool completed=false;
        for(long allowance=0;allowance<128;++allowance) {
            Source source;SeedOutput(source);
            failAfter=allowance;
            const auto status=wrapper?plugins::CaptureModelPasteSource(*f.app,source):plugins::CaptureModelPastePayload(*f.app,source);
            failAfter=-1;
            Check(live.Matches(f),"allocation sweep preserves all raw source buffers scale authoring live selection and history");
            if(status==MMS_OK) {MatchesClipboard(f,source,all,wrapper?unsigned(frame):0);completed=true;break;}
            ++failures;
            Check(status==MMS_E_MEMORY&&Empty(source),"every failed capture allocation clears previously populated output including VMD metadata");
        }
        Check(completed&&failures>=7,"failure injection reaches both later display payload copies before a complete successful capture");
    }
    ClipboardFixture f;f.app->CurrentFrame()=-10;const LiveSnapshot live(f);Source source;SeedOutput(source);
    failAfter=0;const auto status=plugins::CaptureModelPasteSource(*f.app,source);failAfter=-1;
    Check(status==MMS_E_ARGUMENT&&Empty(source)&&live.Matches(f),"legacy negative-origin refusal precedes any payload allocation");
}
}
void NativeModelPastePayloadChecks() {
    std::puts("  model paste payload: shared origin, owned nested data and capture-only admission");
    model_paste_payload_checks::PlayheadAndOwnership();
    model_paste_payload_checks::FamilyIsolation();
    model_paste_payload_checks::InvalidSourcesAndOrigins();
    model_paste_payload_checks::AllocationFailures();
}
}
