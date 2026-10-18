// Ordinary, unbound editor entry points. Shared-history planners are not used.
#define NOMINMAX
#include "mikudancestudio/animation_curve.hpp"
#include "mikudancestudio/bone_curve.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/bone_scale.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/undo_storage.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>
namespace mikudancestudio { void ApplyFrameRangeScale(HWND); }
namespace {
thread_local long allocationLimit=-1;
thread_local mikudancestudio::mdl::ModelRecord* completeProbe=nullptr;
thread_local float completeProbePosition=0;
thread_local bool failedAfterNative=false;
bool CompletedNativeRange()noexcept{
    if(!completeProbe||!completeProbe->boneKeys)return false;
    const auto* keys=completeProbe->boneKeys;unsigned at=0;
    for(unsigned hop=0;hop<32;++hop){if(at>=32)return false;const auto& key=keys[at];
        if(key.frame==70&&key.position[0]==completeProbePosition&&!key.next)return true;
        if(!key.next)return false;at=key.next;}
    return false;
}
}
void* operator new(std::size_t size){if(allocationLimit==0){failedAfterNative=failedAfterNative||CompletedNativeRange();throw std::bad_alloc();}if(allocationLimit>0)--allocationLimit;
    if(auto* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
using namespace mikudancestudio;
namespace ac=animation_curve;
namespace {
unsigned checks=0;
void Check(bool result,const char* message){++checks;if(!result){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
struct Fixture {
    std::unique_ptr<MMDApp> app=std::make_unique<MMDApp>();
    std::unique_ptr<mdl::ModelRecord> model=std::make_unique<mdl::ModelRecord>();
    std::unique_ptr<D3DRenderer> renderer=std::make_unique<D3DRenderer>();
    std::array<mdl::BoneRecord,2> bones{};std::array<unsigned char,2> selected{{1,0}},dirty{};
    std::array<std::uint32_t,2> indices{{0,1}},cursors{};std::array<unsigned char,2> active{};
    MMDApp* previous=nullptr;HWND window=nullptr;
    Fixture(){
        app->state=MMDAppState{};std::memset(renderer.get(),0,sizeof(*renderer));
        window=CreateWindowExW(0,L"STATIC",L"ordinary bone history checks",WS_OVERLAPPED,0,0,640,480,nullptr,nullptr,nullptr,nullptr);
        Check(window!=nullptr,"create hidden ordinary model/range owner");
        model->hwnd=window;model->boneCount=2;model->boneTable=bones.data();model->boneSelection=selected.data();model->bonePhysicsState=dirty.data();
        model->boneKeyIndices=indices.data();model->boneKeyCursors=cursors.data();model->boneTrackActive=active.data();
        model->boneKeys=mdl::BoneKeyPoolCreate(32);model->morphKeys=mdl::MorphKeyPoolCreate(8);model->displayKeys=mdl::KeyPoolCreate<mdl::DisplayKey>(8);
        model->displayKeys[0].visible=1;model->selectedBone=0;model->searchCursor=5;model->maxFrame=30;
        for(unsigned bone=0;bone<2;++bone){bones[bone].type=mdl::BoneType::RotateMove;bones[bone].slotIndex=-1;bones[bone].rotQuat[3]=1;
            model->boneKeys[bone].rotation[3]=1;sprintf_s(bones[bone].name,"bone%u",bone);}
        model->boneKeys[0].next=2;
        for(unsigned index=2;index<=4;++index){auto& key=model->boneKeys[index];key.frame=(index-1)*10;key.previous=index==2?0:index-1;key.next=index==4?0:index+1;
            key.rotation[3]=1;for(unsigned lane=0;lane<3;++lane)key.position[lane]=float(index*int(lane+1));}
        for(unsigned index:{0u,1u,2u,3u,4u})for(unsigned lane=0;lane<4;++lane){auto& key=model->boneKeys[index];
            key.interpolation[lane]=static_cast<unsigned char>(12+index);key.interpolation[4+lane]=static_cast<unsigned char>(25+index);
            key.interpolation[8+lane]=static_cast<unsigned char>(90+index);key.interpolation[12+lane]=static_cast<unsigned char>(113+index);}
        app->Hwnd()=window;app->Renderer()=renderer.get();app->ModelSlot(0)=Raw();app->SelectedModelSlot()=0;
        app->CurrentFrame()=17;app->PlaybackPhysicsMode()=0;app->EnglishUI()=1;app->SidebarWidth()=100;
        previous=g_Block;g_Block=app.get();
        Control(L"BUTTON",panel::kPhysicsFrameCheckbox,BS_AUTOCHECKBOX);
        for(int id:{panel::kScaleFromEdit,panel::kScaleToEdit,panel::kScaleRateEdit})Control(L"EDIT",id,0);
        for(int id:{panel::kScaleBoneCheckbox,panel::kScaleMorphCheckbox,panel::kScaleDispIkCheckbox})Control(L"BUTTON",id,BS_AUTOCHECKBOX);
    }
    ~Fixture(){g_Block=previous;bone_scale::Forget(Raw());
        for(auto& ring:model->undoRings)for(auto& entry:ring.slots){undo_storage::Free(entry.bonePose);undo_storage::Free(entry.auxiliaryPose);}
        mdl::BoneKeyPoolDestroy(model->boneKeys);mdl::MorphKeyPoolDestroy(model->morphKeys);mdl::KeyPoolDestroy(model->displayKeys);
        app->ModelSlot(0)=nullptr;DestroyWindow(window);}
    unsigned char* Raw(){return reinterpret_cast<unsigned char*>(model.get());}
    void Control(const wchar_t* type,int id,DWORD extra){Check(CreateWindowExW(0,type,L"",WS_CHILD|extra,0,0,80,20,window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),nullptr,nullptr)!=nullptr,"create ordinary editor controls");}
    unsigned At(unsigned frame){for(unsigned index=0,hops=0;hops<mdl::BoneKeyCapacity(*model);++hops){
        if(model->boneKeys[index].frame==frame)return index;if(!model->boneKeys[index].next)break;index=model->boneKeys[index].next;}
        Check(false,"ordinary bone frame exists");return 0;}
    std::vector<mdl::BoneKey> Keys(){return {model->boneKeys,model->boneKeys+mdl::BoneKeyCapacity(*model)};}
    void Range(const wchar_t* first,const wchar_t* last,const wchar_t* scale){
        SetDlgItemTextW(window,panel::kScaleFromEdit,first);SetDlgItemTextW(window,panel::kScaleToEdit,last);SetDlgItemTextW(window,panel::kScaleRateEdit,scale);
        CheckDlgButton(window,panel::kScaleBoneCheckbox,BST_CHECKED);CheckDlgButton(window,panel::kScaleMorphCheckbox,BST_UNCHECKED);
        CheckDlgButton(window,panel::kScaleDispIkCheckbox,BST_UNCHECKED);ApplyFrameRangeScale(window);
    }
};
bool SameKeys(const std::vector<mdl::BoneKey>& a,const std::vector<mdl::BoneKey>& b){return a.size()==b.size()&&!std::memcmp(a.data(),b.data(),a.size()*sizeof(a[0]));}
void AutomaticOverwrite(){
    Fixture f;CheckDlgButton(f.window,panel::kPhysicsFrameCheckbox,BST_CHECKED);
    const auto first=f.At(10),middle=f.At(20),last=f.At(30);const auto before=f.Keys();
    f.bones[0].trans[0]=7;f.bones[0].trans[1]=-3;f.bones[0].trans[2]=8;
    RegisterSelectedBoneKeys(f.Raw(),20,0);const auto after=f.Keys();
    const auto& entry=f.model->undoRings[0].slots[f.model->undoState[0]];
    Check(entry.operation==2&&entry.dirty==3,"ordinary automatic overwrite captures the existing key and both nonzero neighbours");
    Check(std::memcmp(after[first].interpolation,before[first].interpolation,16)&&std::memcmp(after[last].interpolation,before[last].interpolation,16),
        "real automatic interpolation actually changes both neighbouring key handles");
    const auto* saved=static_cast<const mdl::BoneKeySnapshot*>(entry.auxiliaryPose);std::array<bool,3> found{};
    for(int i=0;i<entry.dirty;++i){if(saved[i].keyIndex==int(first))found[0]=true;if(saved[i].keyIndex==int(middle))found[1]=true;if(saved[i].keyIndex==int(last))found[2]=true;}
    Check(found[0]&&found[1]&&found[2],"ordinary snapshot indexes identify frames 10, 20 and 30 rather than only the root-adjacent case");
    std::int32_t frame=20;
    for(unsigned cycle=0;cycle<2;++cycle){UndoModelEdit(f.Raw(),&frame,0);Check(SameKeys(f.Keys(),before),"ordinary Undo restores every native record and neighbour handle byte");
        RedoModelEdit(f.Raw(),&frame,0);Check(SameKeys(f.Keys(),after),"ordinary Redo restores the complete automatically rebuilt intervals");}
}
#if !MMD_ORIGINAL_PROFILE
std::vector<std::uint8_t> Encoded(const bone_curve::Tracks& tracks){std::vector<ac::NamedTrack> rows;
    for(unsigned lane=0;lane<9;++lane)rows.push_back({std::to_string(lane),tracks[lane]});std::vector<std::uint8_t> result;std::string error;
    Check(ac::Encode(rows,result,error),"encode exact sparse Free tracks");return result;}
bone_curve::Tracks Read(Fixture& f,unsigned bone=0){bone_curve::Tracks tracks;Check(bone_curve::Read(f.Raw(),int(bone),tracks),"ordinary native operation preserves readable Free authoring");return tracks;}
bone_curve::Tracks FreeTracks(){bone_curve::Tracks tracks;
    for(unsigned lane=0;lane<9;++lane)for(int frame:{0,10,20,30,50}){
        if(lane==1&&(frame==10||frame==20))continue;if(lane==4&&(frame==10||frame==30))continue;
        ac::Key key;key.frame=frame;key.value=lane>=6?1+double(frame)*.01:lane==4?frame*18+.000001234:double(frame)*.17*(lane+1)+.0000001234;
        key.tangent=ac::Tangent::Free;key.interpolation=lane==1?ac::Interpolation::Step:ac::Interpolation::Bezier;
        key.inWeight=.21;key.outWeight=.17;key.inSlope=key.outSlope=lane>=6?.003:lane==4?7.1:.03*(lane+1);tracks[lane].keys.push_back(key);}
    return tracks;
}
void SeedFree(Fixture& f){std::string error;Check(bone_curve::Write(f.Raw(),0,FreeTracks(),error),"seed nine sparse authored position, Euler and scale lanes");
    bone_scale::SetKeyCurve(f.Raw(),int(f.At(50)),{7,29,101,119});
    Check(bone_scale::KeyCurve(f.Raw(),int(f.At(50)))==bone_scale::Curve({7,29,101,119}),"seed native scale timing before the ordinary range tool");Read(f);}
bone_curve::Tracks ExpectedStretch(const bone_curve::Tracks& before){auto result=before;
    const auto mapped=[](int frame){return frame<10?frame:frame<=30?10+(frame-10)*2:frame+20;};
    for(auto& track:result){const auto original=track.keys;
        for(unsigned i=0;i<track.keys.size();++i){auto& key=track.keys[i];key.frame=mapped(key.frame);
            if(i)key.inSlope*=double(original[i].frame-original[i-1].frame)/(key.frame-mapped(original[i-1].frame));
            if(i+1<track.keys.size())key.outSlope*=double(original[i+1].frame-original[i].frame)/(mapped(original[i+1].frame)-key.frame);}}
    return result;
}
void Samples(const bone_curve::Tracks& before,const bone_curve::Tracks& after){
    for(unsigned lane=0;lane<9;++lane){Check(before[lane].keys.size()==after[lane].keys.size(),"range tool preserves every sparse lane schedule");
        for(unsigned end=1;end<before[lane].keys.size();++end)for(unsigned part=0;part<=32;++part){const double t=double(part)/32;
            const double a=before[lane].keys[end-1].frame+t*(before[lane].keys[end].frame-before[lane].keys[end-1].frame);
            const double b=after[lane].keys[end-1].frame+t*(after[lane].keys[end].frame-after[lane].keys[end-1].frame);
            const double expected=ac::Evaluate(before[lane],a).value,actual=ac::Evaluate(after[lane],b).value;
            Check(std::abs(expected-actual)<=1e-9*std::max(1.0,std::abs(expected)),"every ordinary retimed interval retains its normalized Free/Step value shape");}}
}
void FreeRangeStretch(){Fixture f;SeedFree(f);const auto before=Read(f);const auto beforeBytes=Encoded(before);
    const auto scale=bone_scale::Capture(f.Raw());const auto oldScaleCurve=bone_scale::KeyCurve(f.Raw(),int(f.At(50)));const auto ring=f.model->undoState[0];
    f.Range(L"10",L"30",L"2");const auto after=Read(f);const auto afterBytes=Encoded(after);
    Check(Encoded(ExpectedStretch(before))==afterBytes,"real unbound range command preserves all nine exact sparse tracks, values, handles and modes");Samples(before,after);
    Check(f.app->CurrentFrame()==17&&f.model->undoState[0]==(ring+2)%30,"ordinary range edit keeps the playhead and opens the existing paired undo steps");
    Check(bone_scale::KeyCurve(f.Raw(),int(f.At(70)))==oldScaleCurve,"ordinary retime carries native scale control bytes with the moved key");
    std::int32_t frame=17;for(unsigned cycle=0;cycle<2;++cycle){UndoModelEdit(f.Raw(),&frame,0);Check(Encoded(Read(f))==beforeBytes,
        "one ordinary Undo restores every original sparse Free lane exactly");
        const auto restored=bone_scale::Capture(f.Raw());Check(restored.keys==scale.keys&&restored.curves==scale.curves,"ordinary range Undo restores native scale matrices and curves");
        RedoModelEdit(f.Raw(),&frame,0);Check(Encoded(Read(f))==afterBytes,"ordinary range Redo restores all retimed lanes exactly");}
}
void FreeRangeCollision(){Fixture f;SeedFree(f);
    // Reorder physical slots so source frame 20, not the latest frame 50,
    // is written last by the ordinary pool-order registrar sweep.
    const auto low=f.At(20),high=f.At(50);auto* keys=f.model->boneKeys;
    const auto scaleA=bone_scale::Key(f.Raw(),int(low)),scaleB=bone_scale::Key(f.Raw(),int(high));
    const auto curveA=bone_scale::KeyCurve(f.Raw(),int(low)),curveB=bone_scale::KeyCurve(f.Raw(),int(high));
    std::swap(keys[low],keys[high]);for(std::size_t i=0;i<mdl::BoneKeyCapacity(*f.model);++i){
        if(keys[i].next==low)keys[i].next=high;else if(keys[i].next==high)keys[i].next=low;
        if(keys[i].previous==low)keys[i].previous=high;else if(keys[i].previous==high)keys[i].previous=low;}
    bone_scale::SetKey(f.Raw(),int(low),scaleB);bone_scale::SetKey(f.Raw(),int(high),scaleA);
    bone_scale::SetKeyCurve(f.Raw(),int(low),curveB);bone_scale::SetKeyCurve(f.Raw(),int(high),curveA);
    Check(bone_scale::Key(f.Raw(),int(low))==scaleB&&bone_scale::Key(f.Raw(),int(high))==scaleA&&
        bone_scale::KeyCurve(f.Raw(),int(low))==curveB&&bone_scale::KeyCurve(f.Raw(),int(high))==curveA,"reorder scale records with their native slots");
    const auto before=Read(f);const auto beforeBytes=Encoded(before);Check(f.At(20)>f.At(50),"collision fixture makes physical slot order differ from frame order");
    f.Range(L"0",L"50",L"0.01");const auto after=Read(f);const auto afterBytes=Encoded(after);
    for(unsigned lane=0;lane<9;++lane)Check(after[lane].keys.size()==1&&after[lane].keys.front().frame==0&&
        after[lane].keys.front().value==ac::Evaluate(before[lane],20).value,"collapsed root keeps the actual final source slot's precise authored value in every lane");
    std::int32_t frame=17;UndoModelEdit(f.Raw(),&frame,0);Check(Encoded(Read(f))==beforeBytes,"ordinary collision Undo restores sparse source authoring");
    RedoModelEdit(f.Raw(),&frame,0);Check(Encoded(Read(f))==afterBytes,"ordinary collision Redo restores the actual winning authored root");
}
void EmptyAndPreparationFailure(){Fixture f;SeedFree(f);const auto before=Encoded(Read(f));const auto ring=f.model->undoState[0];
    f.Range(L"60",L"70",L"2");Check(Encoded(Read(f))==before&&f.model->undoState[0]==ring,"range beyond the last key releases deferral without consuming history or authoring");
    const auto keys=f.Keys();allocationLimit=0;{bone_curve::RangeRetime pending(f.Raw(),10,30,2);Check(!pending.Ready(),"Free retime reports preparation allocation failure");}allocationLimit=-1;
    Check(SameKeys(keys,f.Keys())&&Encoded(Read(f))==before&&f.model->undoState[0]==ring,"failed preparation leaves native keys and Free authoring available for retry");
    f.Range(L"10",L"30",L"2");Check(Encoded(Read(f))!=before,"a later ordinary range edit remains usable after empty and failed preparations");
}
void UntouchedAutomaticLanes(){Fixture f;auto affected=FreeTracks(),other=FreeTracks();
    const auto early=[](ac::Track& track){track.keys.resize(2);track.keys[0].frame=0;track.keys[1].frame=5;
        for(auto& key:track.keys){key.tangent=ac::Tangent::Automatic;key.outWeight=.7;key.inWeight=.9999-.7;}ac::UpdateAutomatic(track);};
    early(affected[2]);for(auto& track:other)early(track);
    for(auto& key:affected[0].keys)key.tangent=ac::Tangent::Automatic;ac::UpdateAutomatic(affected[0]);
    std::string error;Check(bone_curve::Write(f.Raw(),0,affected,error)&&bone_curve::Write(f.Raw(),1,other,error),
        "seed an affected bone plus an automatic bone and sparse lane entirely before the scaled range");
    const auto before=Read(f),peer=Read(f,1);const auto beforeBytes=Encoded(before),peerBytes=Encoded(peer);
    f.Range(L"10",L"30",L"2");const auto after=Read(f);auto untouched=before;untouched[2]=after[2];
    Check(Encoded(untouched)==beforeBytes&&Encoded(Read(f,1))==peerBytes,
        "ordinary retime preserves all Automatic metadata in an untouched sparse lane and another untouched bone");
    Check(after[0].keys.front().tangent==ac::Tangent::Automatic&&after[0].keys.back().tangent==ac::Tangent::Automatic&&
        after[0].keys[1].tangent==ac::Tangent::Free&&after[0].keys[2].tangent==ac::Tangent::Free,
        "only Automatic keys adjoining a changed interval freeze their shape-preserving slopes");
    const auto afterBytes=Encoded(after);std::int32_t frame=17;UndoModelEdit(f.Raw(),&frame,0);
    Check(Encoded(Read(f))==beforeBytes&&Encoded(Read(f,1))==peerBytes,"ordinary retime Undo keeps unaffected Automatic authoring exact across bones");
    RedoModelEdit(f.Raw(),&frame,0);Check(Encoded(Read(f))==afterBytes&&Encoded(Read(f,1))==peerBytes,
        "ordinary retime Redo restores only the changed intervals' tangent modes");
}
void CompletionAllocationFailure(){bool observed=false;
    for(long limit=0;limit<2048&&!observed;++limit){Fixture f;SeedFree(f);const auto before=Encoded(Read(f));f.model->selectedBone=1;
        completeProbe=f.model.get();completeProbePosition=f.model->boneKeys[f.At(50)].position[0];failedAfterNative=false;
        allocationLimit=limit;try{f.Range(L"10",L"30",L"2");}catch(const std::bad_alloc&){}
        allocationLimit=-1;completeProbe=nullptr;
        Check(f.model->selectedBone==1,"ordinary range keeps the originally active bone across registration allocation failures");
        if(!failedAfterNative)continue;
        observed=true;Check(f.app->SceneModified()!=0&&f.app->LastRegisteredFrame()>=f.model->maxFrame,
            "post-native range completion allocation failure retains dirty and safe maximum metadata");
        // The ordinary path promises a recoverable paired undo, not the
        // all-or-nothing publication of the bound shared-history planner.
        std::int32_t frame=17;UndoModelEdit(f.Raw(),&frame,0);
        Check(Encoded(Read(f))==before,"after post-native completion OOM the existing ordinary Undo restores exact original Free authoring");
        f.Range(L"10",L"30",L"2");Check(Encoded(Read(f))!=before,
            "ordinary authoring remains usable after undoing a completion allocation failure");
    }
    Check(observed,"allocation budget scan reaches a real failure after final native range key publication");
}
#endif
}
int main(){std::setvbuf(stdout,nullptr,_IONBF,0);AutomaticOverwrite();
#if !MMD_ORIGINAL_PROFILE
    FreeRangeStretch();FreeRangeCollision();EmptyAndPreparationFailure();UntouchedAutomaticLanes();CompletionAllocationFailure();
#endif
    std::printf("PASS ordinary bone curve/ring history: %u checks\n",checks);return 0;}
