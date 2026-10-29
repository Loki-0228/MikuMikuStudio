#define NOMINMAX
#include "mikumikustudio/negative_frames.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
using namespace mikumikustudio;
static unsigned checks=0;
void Check(bool value,const char* message){++checks;if(!value){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}}
void CheckSelectionPersistence() {
    constexpr char selectionKey[]="org.mikudancestudio.timeline.negative_selection.v1";
    auto app=std::make_unique<MMDApp>();auto record=std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord,1> bones{};std::array<mdl::MorphRecord,1> morphs{};
    std::array<std::uint32_t,1> boneRows{1},morphRows{2};
    mdl::IkChain ik{};mdl::BoneOrderEntry parent{};mdl::AccessoryRecord accessory{};
    auto* model=reinterpret_cast<unsigned char*>(record.get());
    record->boneCount=record->morphCount=1;record->boneTable=bones.data();record->morphs=morphs.data();
    record->boneKeyIndices=boneRows.data();record->morphKeyIndices=morphRows.data();
    record->ikChainCount=record->boneOrderCount=1;record->ikChains=&ik;mdl::BoneOrder(model)=&parent;
    record->loadComplete=ik.enabled=1;parent.linkedModel=-1;
    std::strcpy(bones[0].name,"bone");std::strcpy(morphs[0].name,"morph");bones[0].rotQuat[3]=1;
    app->ModelSlot(0)=model;app->AccessorySlot(0)=&accessory;
    app->state.jointLineMap[0]=0;app->state.jointLineMap[1]=-1;
    app->CameraFov()=40;app->CameraParentModel()=-1;accessory.parentModel=-1;
    accessory.scale=accessory.opacity=1;
    auto* previous=g_Block;g_Block=app.get();
    for(int frame:{-10,-20}) {
        Check(negative_frames::RegisterBone(model,0,frame),"selection fixture bone registers");
        Check(negative_frames::RegisterMorph(model,0,frame,.5f),"selection fixture morph registers");
        Check(negative_frames::RegisterDisplay(model,frame),"selection fixture visibility, IK and parent register");
        for(auto kind:{negative_frames::Global::Camera,negative_frames::Global::Light,
                      negative_frames::Global::Shadow,negative_frames::Global::Gravity})
            Check(negative_frames::RegisterGlobal(app.get(),kind,frame),"selection fixture global registers");
        Check(negative_frames::RegisterAccessory(app.get(),0,frame),"selection fixture accessory registers");
    }
    for(int mode:{0,1}) {
        app->CameraMode()=mode;
        for(int row=0;row<(mode?5:3);++row)
            Check(negative_frames::SelectCell(app.get(),-10,row,row!=0),"mixed selection covers every negative track type");
    }
    const auto checkSelection=[&](bool selected) {
        for(int mode:{0,1}) {
            app->CameraMode()=mode;
            for(int row=0;row<(mode?5:3);++row) {
                Check(negative_frames::CellSelected(app.get(),-10,row)==selected,"negative track restores selected state");
                Check(!negative_frames::CellSelected(app.get(),-20,row),"negative track preserves unselected state");
            }
        }
    };
    checkSelection(true);
    pmm_extension::Document document;negative_frames::Capture(*app,document);
    const auto originalKeys=pmm_extension::Find(document,negative_frames::kPmmKey)->value;
    Check(originalKeys.size()==8+20*164 && originalKeys[0]==1,"selection persistence keeps legacy negative key layout");
    negative_frames::Reset(app.get());
    Check(negative_frames::Load(*app,document)&&negative_frames::Count(app.get())==20,"all negative track types roundtrip");
    checkSelection(true);
    const auto* selection=pmm_extension::Find(document,selectionKey);
    Check(selection && selection->value.size()==12+20,"negative selection has an independent versioned extension");
    const auto selectionBytes=selection->value;

    const auto checkIgnored=[&](const pmm_extension::Document& input) {
        Check(negative_frames::Load(*app,input)&&negative_frames::Count(app.get())==20,
              "ignored selection metadata never drops negative keyframes");
        checkSelection(false);
    };
    auto legacy=document;pmm_extension::Erase(legacy,selectionKey);checkIgnored(legacy);
    auto malformed=document;
    for(std::size_t offset:{std::size_t(0),std::size_t(4),std::size_t(8),std::size_t(12)}) {
        auto bytes=selectionBytes;bytes[offset]=offset==12?2:std::uint8_t(bytes[offset]^1);
        pmm_extension::Set(malformed,selectionKey,std::move(bytes));checkIgnored(malformed);
    }
    auto truncated=selectionBytes;truncated.pop_back();
    pmm_extension::Set(malformed,selectionKey,std::move(truncated));checkIgnored(malformed);
    auto oversized=selectionBytes;oversized.push_back(0);
    pmm_extension::Set(malformed,selectionKey,std::move(oversized));checkIgnored(malformed);
    auto stale=document;auto changedKeys=originalKeys;
    changedKeys[8+12]=std::uint8_t(changedKeys[8+12]-1); // Same count, first bone moves from -10 to -11.
    pmm_extension::Set(stale,negative_frames::kPmmKey,std::move(changedKeys));checkIgnored(stale);
    app->CameraMode()=0;
    Check(!negative_frames::CellSelected(app.get(),-11,1),"stale metadata cannot select a key moved by an older writer");

    app->ModelSlot(0)=nullptr;
    Check(negative_frames::Load(*app,document)&&negative_frames::Count(app.get())==10,
          "missing model skips only its negative keys");
    app->CameraMode()=1;
    for(int row=0;row<5;++row) {
        Check(negative_frames::CellSelected(app.get(),-10,row),"skipped keys do not shift later selection flags");
        Check(!negative_frames::CellSelected(app.get(),-20,row),"skipped keys do not select later unselected keys");
    }
    app->ModelSlot(0)=model;
    Check(negative_frames::Load(*app,document),"original selection fixture reloads");
    pmm_extension::Document recaptured;negative_frames::Capture(*app,recaptured);
    Check(pmm_extension::Find(recaptured,negative_frames::kPmmKey)->value==originalKeys,
          "selection roundtrip leaves legacy negative key bytes unchanged");
    Check(pmm_extension::Find(recaptured,selectionKey)->value==selectionBytes,
          "all visibility, IK and parent flags survive selection roundtrip");
    negative_frames::Reset(app.get());negative_frames::Capture(*app,recaptured);
    Check(!pmm_extension::Find(recaptured,negative_frames::kPmmKey) && !pmm_extension::Find(recaptured,selectionKey),
          "empty negative tracks remove both payload and selection metadata");
    bone_scale::Forget(model);g_Block=previous;
}
int main() {
    int first=0,last=0,value=0;
    Check(frame_limits::ParseSigned("-1",value)&&value==-1,"signed cursor accepts negative frame");
    Check(!frame_limits::Parse("-1",value),"native key parser remains nonnegative");
    Check(!frame_limits::ParseSigned("-2147483648",value),"reserved signed boundary is rejected");
    Check(frame_limits::ResolveRange(0,-1,600,first,last)&&first==0&&last==600,"zero to minus one selects whole positive range");
    Check(frame_limits::ResolveRange(-10,-1,600,first,last)&&first==-10&&last==-1,"ordered negative bounds remain literal");
    Check(frame_limits::ResolveRange(100,5,600,first,last)&&first==100&&last==596,"inverted positive right bound counts from end");
    Check(frame_limits::ResolveRange(100,-5,600,first,last)&&last==596,"inverted negative right bound counts from end");
    Check(frame_limits::ResolveRange(-10,20,600,first,last,true)&&first==0&&last==20,"playback clips negative start");
    Check(!frame_limits::ResolveRange(-10,-1,600,first,last,true),"negative-only range cannot play");
    Check(frame_limits::ResolveRange(0,0,600,first,last)&&last==0,"normal zero endpoints are not expanded");

    auto app=std::make_unique<MMDApp>();auto record=std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord,2> bones{};std::array<mdl::MorphRecord,2> morphs{};
    std::array<mdl::BoneKey,2> originalBoneKeys{};std::array<mdl::MorphKey,2> originalMorphKeys{};
    std::array<unsigned char,2> selected{},dirty{};
    auto* model=reinterpret_cast<unsigned char*>(record.get());
    record->boneCount=2;record->boneTable=bones.data();record->boneKeys=originalBoneKeys.data();
    record->morphCount=2;record->morphs=morphs.data();record->morphKeys=originalMorphKeys.data();
    record->boneSelection=selected.data();record->bonePhysicsState=dirty.data();
    std::strcpy(bones[0].name,"bone");std::strcpy(morphs[1].name,"morph");
    bones[0].rotQuat[3]=bones[1].rotQuat[3]=1;
    app->ModelSlot(0)=model;app->CurrentFrame()=-10;
    auto* previous=g_Block;g_Block=app.get();
    bones[0].trans[0]=10;
    Check(negative_frames::RegisterBone(model,0,-10),"negative bone key registers outside native pool");
    Check(negative_frames::RegisterMorph(model,1,-10,.8f),"negative morph key registers outside native pool");
    Check(record->maxFrame==0 && originalBoneKeys[0].frame==0 && originalBoneKeys[0].next==0 && originalMorphKeys[1].next==0,
        "negative registration never wraps native frame fields or changes positive duration");
    bones[0].trans[0]=0;morphs[1].value=0;
    negative_frames::EvaluateModel(model,-5);
    Check(std::abs(bones[0].trans[0]-5)<1e-5 && std::abs(morphs[1].value-.4f)<1e-5,
        "negative bone and morph interpolate toward the independent frame-zero baseline");
    app->CameraPositionX()=12;app->CameraFov()=40;
    Check(negative_frames::RegisterGlobal(app.get(),negative_frames::Global::Camera,-10),"negative camera register");
    app->LightDirection()[0]=.7f;
    Check(negative_frames::RegisterGlobal(app.get(),negative_frames::Global::Light,-10),"negative light register");
    app->CameraPositionX()=0;negative_frames::EvaluateGlobals(app.get(),-10);
    Check(app->CameraPositionX()==12,"negative global seek restores exact key");
    app->CameraMode()=0;negative_frames::SelectRange(app.get(),-10,-1);
    Check(negative_frames::Command(app.get(),420),"negative keyframes copy");
    app->CurrentFrame()=-30;
    Check(negative_frames::Command(app.get(),421),"negative keyframes paste at signed destination");
    Check(negative_frames::Count(app.get())==6,"copy inserts bone and morph at negative frame without changing global keys");
    pmm_extension::Document document;negative_frames::Capture(*app,document);
    Check(pmm_extension::Find(document,negative_frames::kPmmKey)!=nullptr,"negative keys are encoded in MMSXPMM2 namespaced record");
    Check(pmm_extension::Representable(document),"negative extension is representable by existing v2 codec");
    negative_frames::Reset(app.get());app->CurrentFrame()=0;
    Check(negative_frames::Load(*app,document)&&negative_frames::Count(app.get())==6&&app->CurrentFrame()==-30,
        "negative keys and signed cursor survive extension roundtrip");
    auto corrupt=document;pmm_extension::Set(corrupt,negative_frames::kPmmKey,{1,2,3});
    app->CurrentFrame()=37;
    Check(!negative_frames::Load(*app,corrupt)&&negative_frames::Count(app.get())==6,"malformed negative payload cannot partially replace tracks");
    Check(app->CurrentFrame()==37,"malformed extension cannot partially restore signed cursor");
    bones[0].trans[0]=0;morphs[1].value=0;negative_frames::EvaluateModel(model,-30);
    Check(bones[0].trans[0]==10 && morphs[1].value==.8f,"roundtrip preserves pasted negative values");
    std::array<std::uint32_t,2> boneRows{1,3},morphRows{4,2};
    record->boneKeyIndices=boneRows.data();record->morphKeyIndices=morphRows.data();
    negative_frames::SelectBox(app.get(),-31,-29,1,2,true,false);
    negative_frames::SelectColumns(app.get());
    Check(negative_frames::Command(app.get(),420),"negative marquee and vertical selection produce clipboard");
    app->CurrentFrame()=-50;negative_frames::Command(app.get(),421);
    Check(negative_frames::Count(app.get())==8,"vertical selection includes both bone and morph at same negative frame");
    negative_frames::SelectBox(app.get(),-11,-9,2,3,true,false);
    negative_frames::Command(app.get(),420);app->CurrentFrame()=-70;negative_frames::Command(app.get(),421);
    Check(negative_frames::Count(app.get())==9,"replacement marquee selects only matching frame and row");
    negative_frames::SelectBox(app.get(),-51,1,1,4,true,false);
    negative_frames::SelectBox(app.get(),-51,-49,1,4,false,true);
    negative_frames::Command(app.get(),420);app->CurrentFrame()=-100;negative_frames::Command(app.get(),421);
    Check(negative_frames::Count(app.get())==13,"cross-zero marquee and subtractive marquee retain remaining negative keys");
    negative_frames::RegisterBone(model,0,-4);
    negative_frames::SelectCell(app.get(),-10,1,false);
    negative_frames::BeginDrag(app.get());negative_frames::Drag(app.get(),100);
    Check(negative_frames::CellSelected(app.get(),-5,1),"negative drag stops before an unselected key without overwriting it");
    negative_frames::Drag(app.get(),0);negative_frames::EndDrag(app.get());
    Check(negative_frames::CellSelected(app.get(),-10,1),"returning to drag anchor restores exact original frame");
    app->CameraMode()=1;app->CurrentFrame()=0;
    negative_frames::SelectCell(app.get(),-10,0,false);
    negative_frames::BeginDrag(app.get());negative_frames::Drag(app.get(),100);negative_frames::EndDrag(app.get());
    Check(negative_frames::CellSelected(app.get(),-1,0),"signed drag preserves the frame zero boundary");
    negative_frames::Reset(app.get());bone_scale::Forget(model);g_Block=previous;
    CheckSelectionPersistence();
    std::printf("PASS negative frames checks=%u\n",checks);
}
