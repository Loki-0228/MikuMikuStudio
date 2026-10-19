#define NOMINMAX
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/undo_storage.hpp"
#include <commctrl.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>
using namespace mikumikustudio;
namespace ac=animation_curve;
namespace { thread_local long boneFailAfter=-1; }
void* operator new(std::size_t bytes){
    if(boneFailAfter==0)throw std::bad_alloc();if(boneFailAfter>0)--boneFailAfter;
    if(auto* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();
}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
void Check(bool ok,const char* text){if(!ok){std::fprintf(stderr,"FAIL: %s\n",text);std::exit(1);}}
struct Model {
    std::unique_ptr<mdl::ModelRecord> record=std::make_unique<mdl::ModelRecord>();
    std::vector<mdl::BoneRecord> bones=std::vector<mdl::BoneRecord>(2);
    std::array<unsigned char,2> selection{},physics{};
    Model(){record->boneCount=2;record->selectedBone=0;record->boneTable=bones.data();record->boneKeys=mdl::BoneKeyPoolCreate(32);
        record->boneSelection=selection.data();record->bonePhysicsState=physics.data();
        std::memcpy(bones[0].name,"test",5);std::memcpy(bones[1].name,"other",6);
        for(int b=0;b<2;++b){record->boneKeys[b].rotation[3]=1;record->boneKeys[b].allocated=1;}
        record->boneKeys[0].next=2;record->boneKeys[2].frame=20;record->boneKeys[2].rotation[3]=1;
        record->boneKeys[2].position[0]=10;record->boneKeys[2].allocated=1;
    }
    unsigned char* data(){return reinterpret_cast<unsigned char*>(record.get());}
    ~Model(){bone_curve::Forget(data());bone_scale::Forget(data());mdl::BoneKeyPoolDestroy(record->boneKeys);
        for(auto& ring:record->undoRings)for(auto& entry:ring.slots){undo_storage::Free(entry.bonePose);undo_storage::Free(entry.auxiliaryPose);}}
};
std::vector<std::uint8_t> Encoded(const bone_curve::Tracks& tracks){
    std::vector<ac::NamedTrack> named;for(int c=0;c<9;++c)named.push_back({std::to_string(c),tracks[c]});
    std::vector<std::uint8_t> bytes;std::string error;Check(ac::Encode(named,bytes,error),"encode comparison tracks");return bytes;
}
void NativeSyncTests(const bone_curve::Tracks& initial){
    Model model;auto* data=model.data();std::string error;bone_curve::Tracks read;
    auto tracks=initial;
    std::size_t at=0;
    Check(ac::InsertPreservingShape(tracks[0],9,at,error)==ac::InsertResult::Inserted,"prepare independent lane");
    tracks[2].keys.back().value=0.1234567890123;
    tracks[4].keys.back().value=450;
    auto reset=[&]{Check(bone_curve::Write(data,0,tracks,error),error.c_str());};
    auto readNow=[&]{Check(bone_curve::Read(data,0,read),"native edit retains authored bone");};
    auto slotAt=[&](unsigned frame){unsigned slot=0;while(model.record->boneKeys[slot].frame!=frame){slot=model.record->boneKeys[slot].next;Check(slot!=0,"native frame exists");}return slot;};
    reset();const auto original=Encoded(tracks);
    model.record->boneKeys[slotAt(9)].allocated=0;
    model.record->boneKeys[slotAt(9)].physicsDisabled=1;
    readNow();Check(Encoded(read)==original,"selection and physics changes preserve exact tracks");
    model.record->boneKeys[0].position[0]=123;
    readNow();auto expected=tracks;expected[0].keys[0].value=123;
    Check(Encoded(read)==Encoded(expected),"native translation changes only its authored key value");
    reset();auto& end=model.record->boneKeys[slotAt(20)];end.rotation[1]=-end.rotation[1];end.rotation[3]=-end.rotation[3];
    readNow();Check(Encoded(read)==original,"quaternion sign preserves Euler revolutions and handles");
    reset();auto& rotate=model.record->boneKeys[slotAt(20)];
    rotate.rotation[1]=float(std::sin(230*3.14159265358979323846/360));
    rotate.rotation[3]=float(std::cos(230*3.14159265358979323846/360));
    readNow();Check(std::abs(read[4].keys.back().value-590)<1e-4,"native rotation stays near authored winding");
    for(int c:{0,1,2,6,7,8}){auto one=tracks;one[c]=read[c];Check(Encoded(one)==original,"rotation preserves other channels");}
    for(double pitch:{90.0,-90.0}){auto gimbal=tracks;gimbal[5].keys.back().value=-30;
        Check(bone_curve::Write(data,0,gimbal,error),"prepare gimbal-lock reference");
        const double half=3.14159265358979323846/360;
        const double sx=std::sin(pitch*half),cx=std::cos(pitch*half),sy=std::sin(500*half),cy=std::cos(500*half),sz=std::sin(-30*half),cz=std::cos(-30*half);
        float target[]={float(cy*sx*cz+sy*cx*sz),float(sy*cx*cz-cy*sx*sz),float(cy*cx*sz-sy*sx*cz),float(cy*cx*cz+sy*sx*sz)};
        std::memcpy(model.record->boneKeys[slotAt(20)].rotation,target,sizeof target);readNow();
        bone_curve::Evaluate(data,20);double dot=0,aa=0,bb=0;
        for(int c=0;c<4;++c){dot+=double(target[c])*model.bones[0].rotQuat[c];aa+=double(target[c])*target[c];bb+=double(model.bones[0].rotQuat[c])*model.bones[0].rotQuat[c];}
        Check(dot*dot/(aa*bb)>1-1e-10,"gimbal-lock native edit preserves quaternion pose");
        Check(std::abs(read[5].keys.back().value+30)<1e-6,"gimbal-lock native edit retains negative authored roll");}
    reset();auto scale=bone_scale::Key(data,slotAt(20));scale.v[0]=2;bone_scale::SetKey(data,int(slotAt(20)),scale);
    readNow();expected=tracks;expected[6].keys.back().value=2;
    Check(Encoded(read)==Encoded(expected),"native scale changes only corresponding axis");
    reset();model.record->boneKeys[slotAt(9)].frame=12;
    readNow();Check(read[0].keys[1].frame==12&&read[1].keys.size()==2,"retiming preserves independent lane schedules");
    for(int c=1;c<9;++c){auto one=tracks;one[c]=read[c];Check(Encoded(one)==original,"retiming leaves unrelated intervals exact");}
    for(int i=0;i<=200;++i){const double f=i*.1;const double mapped=f<=12?f*9/12:9+(f-12)*11/8;
        Check(std::abs(ac::Evaluate(read[0],f).value-ac::Evaluate(tracks[0],mapped).value)<1e-9,"retiming retains normalized Bezier shape");}
    reset();const auto removed=slotAt(9),last=slotAt(20);
    model.record->boneKeys[0].next=last;model.record->boneKeys[last].previous=0;model.record->boneKeys[removed]={};bone_scale::ClearKey(data,int(removed));
    readNow();expected=tracks;expected[0].keys.erase(expected[0].keys.begin()+1);
    Check(Encoded(read)==Encoded(expected),"native deletion removes only authored keys at deleted frame");
    auto wide=tracks;wide[0].keys[0].outWeight=.8;wide[0].keys[1].inWeight=.1;
    wide[0].keys[1].outWeight=.1;wide[0].keys[2].inWeight=.8;
    Check(bone_curve::Write(data,0,wide,error),"prepare long neighboring handles");
    const auto wideMiddle=slotAt(9),wideEnd=slotAt(20);
    model.record->boneKeys[0].next=wideEnd;model.record->boneKeys[wideEnd].previous=0;model.record->boneKeys[wideMiddle]={};bone_scale::ClearKey(data,int(wideMiddle));
    readNow();Check(read[0].keys.size()==2&&ac::Validate(read[0],error),"deletion joins long handles without invalidating the bone");
    Check(read[0].keys.front().outSlope==wide[0].keys.front().outSlope&&read[0].keys.back().inSlope==wide[0].keys.back().inSlope,"joining handles preserves slopes");
    reset();const auto middle=slotAt(9);
    // Native registration at a row contributed by only one lane must create
    // the other lane keys even when the pose and native bytes are unchanged.
    bone_curve::ReconcileRegisteredFrame(data,0,9);readNow();
    for(int c=0;c<9;++c){Check(read[c].keys.size()==3,"registration fills missing lane keys");
        for(int i=0;i<=200;++i)Check(std::abs(ac::Evaluate(read[c],i*.1).value-ac::Evaluate(tracks[c],i*.1).value)<1e-9,"same-pose registration retains the exact curve shape");}
    reset();auto fresh=model.record->boneKeys[middle];fresh.frame=14;fresh.previous=middle;fresh.next=slotAt(20);fresh.position[0]=30;
    model.record->boneKeys[7]=fresh;model.record->boneKeys[middle].next=7;model.record->boneKeys[fresh.next].previous=7;
    readNow();Check(read[0].keys.size()==4&&read[0].keys[2].frame==14&&read[0].keys[2].value==30,"native insertion adds key without losing earlier keys");
    Check(read[0].keys[0].outSlope==tracks[0].keys[0].outSlope&&read[0].keys[1].inSlope==tracks[0].keys[1].inSlope,"insertion preserves the preceding custom interval");
    reset();auto& recycled=model.record->boneKeys[slotAt(9)];recycled.frame=14;recycled.position[0]=30;
    readNow();Check(read[0].keys.size()==3&&read[0].keys[1].frame==14&&read[0].keys[1].value==30,"recycled native slot is treated as deletion and insertion");
    // A malformed transient chain must neither run stale curves nor erase them.
    reset();const auto next=model.record->boneKeys[0].next;model.record->boneKeys[0].next=1000;
    Check(!bone_curve::Has(data,0),"invalid native chain does not run authored overlay");
    model.record->boneKeys[0].next=next;readNow();Check(Encoded(read)==original,"invalid native chain does not destroy authoring");
    reset();auto& controlled=model.record->boneKeys[slotAt(20)];controlled.interpolation[0]=15;controlled.interpolation[4]=40;
    readNow();Check(read[0].keys.back().inSlope!=tracks[0].keys.back().inSlope,"native interpolation updates corresponding interval");
    for(int c=1;c<9;++c){auto one=tracks;one[c]=read[c];Check(Encoded(one)==original,"interpolation preserves other channels");}
    // Exercise actual native registration and its native undo/redo ring.
    reset();model.record->searchCursor=2;model.selection[0]=1;
    model.bones[0].trans[0]=42;model.bones[0].rotQuat[3]=1;
    RegisterSelectedBoneKeys(data,9,2);readNow();Check(read[0].keys[1].value==42,"real native registrar merges edited pose");
    const auto registered=Encoded(read);std::int32_t frame=9;
    UndoModelEdit(data,&frame,2);readNow();Check(Encoded(read)==original,"native undo restores exact independent curves");
    RedoModelEdit(data,&frame,2);readNow();Check(Encoded(read)==registered,"native redo restores reconciled authored curves");
    // Test lazy edits followed immediately by undo, before a read or render.
    reset();model.bones[0].trans[0]=55;RegisterSelectedBoneKeys(data,9,2);
    UndoModelEdit(data,&frame,2);readNow();Check(Encoded(read)==original,"undo merges pending edits before restoring old curves");
    RedoModelEdit(data,&frame,2);readNow();Check(read[0].keys[1].value==55,"redo retains a native edit that was never rendered");
    // Save/load must use the reconciled tracks and establish a fresh baseline.
    auto app=std::make_unique<MMDApp>();std::memset(&app->state,0,sizeof app->state);app->ModelSlot(0)=data;
    app->SelectedModelSlot()=0;
    reset();Check(ShiftBoneKeysForFrameLineInsert(app.get(),9)==2,"actual frame-line insert shifts both native keys");readNow();
    Check(read[0].keys[1].frame==10&&read[0].keys.back().frame==21&&read[1].keys.size()==2,"frame-line insertion retains independent lane schedules");
    const auto inserted=Encoded(read);UndoModelEdit(data,&frame,2);readNow();Check(Encoded(read)==original,"frame-line undo restores exact authoring");
    RedoModelEdit(data,&frame,2);readNow();Check(Encoded(read)==inserted,"frame-line redo retains exact retimed authoring");
    Check(ShiftBoneKeysForFrameLineDelete(app.get(),10,false)==2,"actual frame-line delete removes and shifts keys");readNow();
    Check(read[0].keys.size()==2&&read[0].keys.back().frame==20&&read[1].keys.size()==2,"frame-line deletion keeps remaining authored curves");
    reset();
    model.record->boneKeys[0].position[0]=13;pmm_extension::Document doc;bone_curve::CapturePmm(*app,doc);
    readNow();const auto saved=Encoded(read);bone_curve::Reset();bone_curve::LoadPmm(*app,doc);readNow();
    Check(Encoded(read)==saved,"PMM roundtrip retains reconciled authoring");
    model.record->boneKeys[0].position[1]=3;readNow();Check(read[1].keys.front().value==3,"loaded baseline accepts later native edit");
    const auto exported=Encoded(read);std::vector<std::uint8_t> vmd;
    Check(bone_curve::CaptureVmd(data,0,vmd,error),"export reconciled bone curves");
    Check(vmd.size()>=12&&std::memcmp(vmd.data(),"MMSBCRV1",8)==0&&bone_curve::VmdBlockSize(vmd.data(),vmd.size())==vmd.size(),
          "bone curves write and identify MMSBCRV1");
    auto oldBlock=vmd;oldBlock[1]='D';
    Check(bone_curve::VmdBlockSize(oldBlock.data(),oldBlock.size())==0&&
          !bone_curve::ApplyVmd(data,oldBlock.data(),oldBlock.size(),0,error),"MDSBCRV1 is not decoded");
    readNow();Check(Encoded(read)==exported,"unknown MDS bone block leaves current authored curves intact");
    bone_curve::Reset();Check(bone_curve::ApplyVmd(data,vmd.data(),vmd.size(),0,error),"import reconciled bone curves");readNow();
    Check(Encoded(read)==exported,"VMD roundtrip retains reconciled authoring");
    app->ModelSlot(0)=nullptr;
}
#if !MMD_ORIGINAL_PROFILE
struct PrivateBoneTarget {
    std::vector<mdl::BoneKey> keys;
    std::vector<bone_scale::Matrix> scales;
    std::vector<bone_scale::Curve> curves;
    explicit PrivateBoneTarget(Model& model):keys(model.record->boneKeys,model.record->boneKeys+mdl::BoneKeyCapacity(model.record->boneKeys)),
        scales(keys.size()),curves(keys.size()){
        for(std::size_t i=0;i<keys.size();++i){scales[i]=bone_scale::Key(model.data(),unsigned(i));curves[i]=bone_scale::KeyCurve(model.data(),unsigned(i));}
    }
    unsigned At(unsigned frame)const{for(unsigned slot=0,hops=0;hops<keys.size();++hops){
        if(keys[slot].frame==frame)return slot;if(!keys[slot].next)break;slot=keys[slot].next;}
        Check(false,"private bone target frame exists");return 0;
    }
    bone_curve::NativeKeyView View()const{return {this,keys.size(),
        [](const void* p,std::uint32_t slot)noexcept->const mdl::BoneKey*{
            const auto& value=*static_cast<const PrivateBoneTarget*>(p);return slot<value.keys.size()?&value.keys[slot]:nullptr;},
        [](const void* p,std::uint32_t slot,bone_scale::Matrix& scale,bone_scale::Curve& curve)noexcept{
            const auto& value=*static_cast<const PrivateBoneTarget*>(p);if(slot>=value.keys.size())return false;
            scale=value.scales[slot];curve=value.curves[slot];return true;}};
    }
};
void PublishPrivateBone(Model& model,const bone_curve::AuthoringState& target,const PrivateBoneTarget& native){
    auto prepared=bone_curve::AuthoringRestore::Prepare(model.data(),target);
    Check(prepared->IsCurrent(),"private bone restore is current before native publication");
    boneFailAfter=0;
    std::copy(native.keys.begin(),native.keys.end(),model.record->boneKeys);
    const bool published=prepared->Publish();boneFailAfter=-1;
    Check(published,"native keys and reconciled bone authoring publish without allocating");
}
void PrivateTargetTests(const bone_curve::Tracks& initial){
    auto tracks=initial;std::string error;std::size_t inserted=0;
    {
        Model model;Check(bone_curve::Write(model.data(),0,initial,error),"write bone baseline metadata fixture");
        const auto source=bone_curve::AuthoringState::Capture(model.data());
        PrivateBoneTarget original(model),target=original;
        for(auto& key:target.keys){key.allocated=0;key.reserved[0]=37;key.reserved[1]=91;}
        target.keys[target.At(20)].allocated=1;
        auto metadata=source->Clone();auto pending=bone_curve::AuthoringRestore::Prepare(model.data(),*source);
        Check(!metadata->ReconcileTarget(0,target.View(),{20,20})&&metadata->Equals(*source),
            "selection and reserved baseline bytes do not create an authored change on complete-lane registration");
        PublishPrivateBone(model,*metadata,target);
        Check(pending->IsCurrent(),"metadata-only native baseline refresh does not stale a prepared authoring restore");
        for(unsigned mutation=0;mutation<2;++mutation){
            auto changed=metadata->Clone();PrivateBoneTarget edited=target;
            if(mutation==0)edited.keys[0].position[0]=17;
            else edited.keys[edited.At(20)].interpolation[0]^=1;
            auto stale=bone_curve::AuthoringRestore::Prepare(model.data(),*source);
            Check(changed->ReconcileTarget(0,edited.View(),{20})&&!changed->Equals(*metadata),
                "native position and interpolation edits remain semantic bone authoring changes");
            PublishPrivateBone(model,*changed,edited);
            Check(!stale->IsCurrent(),"real position and interpolation changes still stale prepared authoring restores");
            PublishPrivateBone(model,*metadata,target);
        }
    }
    Check(ac::InsertPreservingShape(tracks[0],9,inserted,error)==ac::InsertResult::Inserted,"create sparse private bone registration source");
    tracks[2].keys.back().value=.1234567890123;tracks[4].keys.back().value=450;
    const auto encoded=Encoded(tracks);
    for(unsigned scenario=0;scenario<4;++scenario){
        Model model;Check(bone_curve::Write(model.data(),0,tracks,error),"write private bone target fixture");
        PrivateBoneTarget original(model),target=original;
        const auto source=bone_curve::AuthoringState::Capture(model.data());auto after=source->Clone();
        const auto charge=source->RetainedBytes();
        std::vector<int> registrations;std::vector<std::pair<int,int>> mapping;const std::vector<std::pair<int,int>>* map=nullptr;
        if(scenario==0)target.keys[0].position[0]=123;
        if(scenario==1)registrations={9,9};
        if(scenario==2){target.keys[target.At(9)].frame=12;target.keys[target.At(20)].frame=25;mapping={{0,0},{9,12},{20,25}};map=&mapping;}
        if(scenario==3){
            const auto winner=original.At(9),end=original.At(20);
            target.keys[0]=original.keys[winner];target.keys[0].frame=0;target.keys[0].previous=0;target.keys[0].next=end;
            target.keys[winner]={};target.keys[end].previous=0;target.scales[0]=original.scales[winner];target.curves[0]=original.curves[winner];
            mapping={{9,0},{20,20}};map=&mapping;
        }
        Check(after->ReconcileTarget(0,target.View(),registrations,map),"private bone reconciliation detects authored or baseline changes");
        Check(source->Equals(*bone_curve::AuthoringState::Capture(model.data()))&&
            !std::memcmp(original.keys.data(),model.record->boneKeys,original.keys.size()*sizeof(mdl::BoneKey)),
            "private target reconstruction never installs a temporary native pool or authoring layer");
        PublishPrivateBone(model,*after,target);bone_curve::Tracks read;
        Check(bone_curve::Read(model.data(),0,read),"read privately rebuilt bone authoring after atomic publication");
        if(scenario==0)Check(read[0].keys.front().value==123&&read[2].keys.back().value==tracks[2].keys.back().value&&read[4].keys.back().value==450,
            "private native value edit preserves unrelated high precision and Euler winding");
        if(scenario==1)for(unsigned c=0;c<9;++c){
            Check(read[c].keys.size()==3,"private same-pose registration fills missing lane keys");
            for(unsigned tick=0;tick<=80;++tick)Check(std::abs(ac::Evaluate(read[c],tick*.25).value-ac::Evaluate(tracks[c],tick*.25).value)<1e-8,
                "private sparse registration preserves each lane's curve shape");
        }
        if(scenario==2)Check(read[0].keys[1].frame==12&&read[0].keys.back().frame==25&&read[1].keys.size()==2&&read[1].keys.back().frame==25,
            "explicit retiming preserves independent authored lane schedules");
        if(scenario==3)for(unsigned c=0;c<9;++c)Check(read[c].keys.front().frame==0&&read[c].keys.size()==2&&
            std::abs(read[c].keys.front().value-ac::Evaluate(tracks[c],9).value)<1e-8,
            "frame-zero collision uses the real winning source frame in every sparse lane");
        PublishPrivateBone(model,*source,original);
        Check(bone_curve::Read(model.data(),0,read)&&Encoded(read)==encoded&&source->RetainedBytes()==charge,
            "private reconciliation undo restores exact curves baseline and retained source");
        PublishPrivateBone(model,*after,target);
        Check(bone_curve::Read(model.data(),0,read),"private reconciliation redo remains usable after the source is restored");
        auto stale=bone_curve::AuthoringRestore::Prepare(model.data(),*source);
        bone_curve::ReconcileRegisteredFrame(model.data(),0,int(target.keys[0].frame));
        Check(!stale->IsCurrent(),"pending registration intent participates in bone authoring stale checks");
    }
    {
        Model model;Check(bone_curve::Write(model.data(),0,initial,error),"write bone authoring budget fixture");
        const auto before=bone_curve::AuthoringState::Capture(model.data());
        const auto end=model.record->boneKeys[0].next;
        auto added=model.record->boneKeys[0];added.frame=9;added.previous=0;added.next=end;
        model.record->boneKeys[7]=added;model.record->boneKeys[0].next=7;model.record->boneKeys[end].previous=7;
        bone_curve::Tracks read;Check(bone_curve::Read(model.data(),0,read),"reconcile one new native row for retained-byte measurement");
        for(const auto& track:read)Check(track.keys.size()==3,"new native row contributes one key to each authored lane");
        const auto after=bone_curve::AuthoringState::Capture(model.data());
        const auto minimumGrowth=9*sizeof(ac::Key)+sizeof(unsigned)+sizeof(mdl::BoneKey)+sizeof(bone_scale::Matrix)+sizeof(bone_scale::Curve);
        Check(after->RetainedBytes()>=before->RetainedBytes()+minimumGrowth,
            "retained charge grows by both new authored keys and their native baseline row");
        auto stale=bone_curve::AuthoringRestore::Prepare(model.data(),*before);
        bone_curve::ReconcileRegisteredFrame(model.data(),0,9);
        const auto pending=bone_curve::AuthoringState::Capture(model.data());
        Check(!stale->IsCurrent()&&!pending->Equals(*after)&&pending->RetainedBytes()>after->RetainedBytes(),
            "pending registration nodes affect equality stale guards and retained charge before a read");
    }
    {
        Model model;Check(bone_curve::Write(model.data(),0,tracks,error),"write bone authoring OOM fixture");
        const auto source=bone_curve::AuthoringState::Capture(model.data());PrivateBoneTarget target(model);target.keys[0].position[0]=17;
        bool completed=false;unsigned failures=0;
        for(long limit=0;limit<512;++limit){auto after=source->Clone();bool success=false;
            boneFailAfter=limit;try{after->ReconcileTarget(0,target.View());success=true;}catch(const std::bad_alloc&){++failures;}boneFailAfter=-1;
            Check(source->Equals(*bone_curve::AuthoringState::Capture(model.data())),"private reconciliation OOM never changes live authoring");
            if(success){completed=true;break;}Check(after->Equals(*source),"private reconciliation OOM preserves its input snapshot");
        }
        Check(completed&&failures>10,"private reconciliation exercises all detached allocation stages");
        auto after=source->Clone();target.keys[0].next=1000000;
        bool rejected=false;try{after->ReconcileTarget(0,target.View());}catch(const std::invalid_argument&){rejected=true;}
        Check(rejected&&after->Equals(*source),"malformed private native view is rejected without erasing authored tracks");
        model.record->boneKeys[0].position[0]=17;
        boneFailAfter=0;const bool visible=bone_curve::Has(model.data(),0);boneFailAfter=-1;
        Check(!visible&&source->Equals(*bone_curve::AuthoringState::Capture(model.data())),"Has allocation failure suppresses drawing and retains the complete authored layer");
        Check(bone_curve::Has(model.data(),0),"native reconciliation resumes after temporary memory refusal");
    }
}
#endif
int main(){
    Model model;bone_curve::Tracks tracks;
    for(int c=0;c<9;++c){ac::Key a,b;a.frame=0;b.frame=20;a.tangent=b.tangent=ac::Tangent::Free;
        a.value=b.value=c>=6?1:0;a.interpolation=b.interpolation=ac::Interpolation::Bezier;
        if(c==0){b.value=10;a.outSlope=2;b.inSlope=-.5;a.outWeight=.2;b.inWeight=.4;}
        if(c==4)b.value=90;
        tracks[c].keys={a,b};}
    NativeSyncTests(tracks);
#if !MMD_ORIGINAL_PROFILE
    PrivateTargetTests(tracks);
#endif
    const auto other=model.record->boneKeys[1];std::string error;
    Check(bone_curve::Write(model.data(),0,tracks,error),error.c_str());
    Check(std::memcmp(&other,&model.record->boneKeys[1],sizeof other)==0,"other bone untouched");
    for(int i=0;i<=200;++i){double f=i*.1;bone_scale::Evaluate(model.data(),float(f));bone_curve::Evaluate(model.data(),f);
        Check(std::abs(model.bones[0].trans[0]-ac::Evaluate(tracks[0],f).value)<2e-6,"fractional playback uses exact free curve");}
    bone_curve::Tracks read;Check(bone_curve::Read(model.data(),0,read),"read authored curve");
    auto split=tracks;std::size_t index=0;
    Check(ac::InsertPreservingShape(split[0],9,index,error)==ac::InsertResult::Inserted,"subdivision accepted");
    Check(bone_curve::Write(model.data(),0,split,error),error.c_str());
    Check(model.record->boneKeys[model.record->boneKeys[0].next].frame==9,"insert reflected in native timeline");
    for(int i=0;i<=400;++i)Check(std::abs(ac::Evaluate(read[0],i*.05).value-ac::Evaluate(split[0],i*.05).value)<1e-10,"insertion retains shape");
    auto app=std::make_unique<MMDApp>();std::memset(&app->state,0,sizeof app->state);app->ModelSlot(0)=model.data();
    pmm_extension::Document doc;bone_curve::CapturePmm(*app,doc);Check(!doc.records.empty(),"PMM stores free curves");
    bone_curve::Reset();bone_curve::LoadPmm(*app,doc);Check(bone_curve::Read(model.data(),0,read),"PMM restores free curves");
    Check(read[0].keys.size()==3,"PMM restores independent key counts");
    Check(std::abs(ac::Evaluate(read[0],7.25).value-ac::Evaluate(tracks[0],7.25).value)<1e-10,"PMM retains fractional value");
    std::vector<std::uint8_t> block;
    Check(bone_curve::CaptureVmd(model.data(),0,block,error),"VMD encodes free curves");
    Check(bone_curve::VmdBlockSize(block.data(),block.size())==block.size(),"VMD block bounds");
    bone_curve::Reset();Check(bone_curve::ApplyVmd(model.data(),block.data(),block.size(),0,error),error.c_str());
    Check(bone_curve::Read(model.data(),0,read)&&read[0].keys.size()==3,"VMD restores independent key counts");
    Check(std::abs(ac::Evaluate(read[0],7.25).value-ac::Evaluate(split[0],7.25).value)<1e-10,"VMD retains exact curve");
    Check(!bone_curve::VmdBlockSize(block.data(),block.size()-1),"truncated VMD block rejected");
    auto bad=split;bad[6].keys[1].inSlope=100;
    const auto before=model.record->boneKeys[0];
    Check(!bone_curve::Write(model.data(),0,bad,error),"invalid scale rejected");
    Check(std::memcmp(&before,&model.record->boneKeys[0],sizeof before)==0,"failed edit leaves native keys intact");
    Check(bone_curve::Read(model.data(),0,read)&&read[0].keys.size()==3,"failed edit retains curves");
    bone_curve::CaptureUndo(model.data(),0);
    model.record->boneKeys[0].position[0]=123;
    Check(bone_curve::Read(model.data(),0,read)&&read[0].keys.front().value==123,"native edits merge into free curves");
    model.record->boneKeys[0]=before;bone_curve::Undo(model.data(),0);
    Check(bone_curve::Has(model.data(),0),"native undo restores authored curve");
    Check(bone_curve::Write(model.data(),0,tracks,error),"editor undo removes inserted frame");
    Check(model.record->boneKeys[model.record->boneKeys[0].next].frame==20,"mirror removes deleted key");
    // Exercise the real host/editor route: display edits remain available in
    // both sources after the selected bone acquired free curves.
    std::vector<mdl::DisplayKey> display(mdl::kDisplayKeyCapacity);
    display[0].visible=1;display[0].allocated=1;model.record->displayKeys=display.data();
    app->EnglishUI()=2;
    HWND parent=CreateWindowExW(0,L"STATIC",L"bone curve test",WS_OVERLAPPEDWINDOW,0,0,1200,800,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(parent!=nullptr,"hidden bone host");app->Hwnd()=parent;
    HWND combo=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|CBS_DROPDOWNLIST,0,0,100,150,parent,reinterpret_cast<HMENU>(433),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"X"));SendMessageW(combo,CB_SETCURSEL,0,0);
    // The main MMS interpolation square follows the interval at the playhead,
    // including the two intervals created by an exact subdivision.
    Check(bone_curve::Write(model.data(),0,split,error),"prepare subdivided preview");
    HDC previewDc=CreateCompatibleDC(nullptr);BITMAPINFO image{};
    image.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);image.bmiHeader.biWidth=128;image.bmiHeader.biHeight=-128;
    image.bmiHeader.biPlanes=1;image.bmiHeader.biBitCount=32;void* pixels=nullptr;
    HBITMAP bitmap=CreateDIBSection(previewDc,&image,DIB_RGB_COLORS,&pixels,nullptr,0);
    Check(previewDc&&bitmap,"preview drawing surface");auto oldBitmap=SelectObject(previewDc,bitmap);app->CurveDC()=previewDc;
    auto preview=[&](int frame){
        std::memset(pixels,255,128*128*4);app->CurrentFrame()=frame;
        Check(bone_curve::DrawPreview(*app),"main square draws free curves");GdiFlush();
        auto* bytes=static_cast<unsigned char*>(pixels);return std::vector<unsigned char>(bytes,bytes+128*128*4);
    };
    const auto firstInterval=preview(5),sameInterval=preview(8),secondInterval=preview(15);
    Check(firstInterval==sameInterval,"same interval keeps interpolation preview");
    Check(firstInterval!=secondInterval,"seeking across inserted key switches main preview interval");
    Check(preview(5)==firstInterval,"seeking back restores the interval preview");
    app->CurveDC()=nullptr;SelectObject(previewDc,oldBitmap);DeleteObject(bitmap);DeleteDC(previewDc);
    app->CurrentFrame()=0;Check(bone_curve::Write(model.data(),0,tracks,error),"restore display-edit fixture");
    curve_host::OpenInterpolation(app.get(),false);
    struct Owned { HWND parent,editor=nullptr; } owned{parent};
    EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM parameter)->BOOL{
        auto& result=*reinterpret_cast<Owned*>(parameter);
        wchar_t cls[80]{};GetClassNameW(window,cls,80);
        if(GetWindow(window,GW_OWNER)==result.parent&&wcscmp(cls,L"MikuMikuStudioCurveEditorWindow")==0)result.editor=window;
        return TRUE;
    },reinterpret_cast<LPARAM>(&owned));
    HWND editor=owned.editor;Check(editor!=nullptr,"bone editor opens");
    Check(SendMessageW(GetDlgItem(editor,1002),CB_GETCOUNT,0,0)==2,"native and free sources available");
    for(int source=0;source<2;++source){
        SendMessageW(GetDlgItem(editor,1002),CB_SETCURSEL,source,0);
        SendMessageW(editor,WM_COMMAND,MAKEWPARAM(1002,CBN_SELCHANGE),reinterpret_cast<LPARAM>(GetDlgItem(editor,1002)));
        HWND tree=GetDlgItem(editor,1015);
        std::function<HTREEITEM(HTREEITEM)> find=[&](HTREEITEM item)->HTREEITEM{
            for(;item;item=TreeView_GetNextSibling(tree,item)){TVITEMW data{};data.mask=TVIF_PARAM;data.hItem=item;SendMessageW(tree,TVM_GETITEMW,0,reinterpret_cast<LPARAM>(&data));
                if(data.lParam==10)return item;if(auto hit=find(TreeView_GetChild(tree,item)))return hit;}return nullptr;};
        auto item=find(TreeView_GetRoot(tree));Check(item!=nullptr,"display channel exists");
        TreeView_SelectItem(tree,item);SendMessageW(tree,WM_KEYDOWN,VK_RETURN,0);
        HWND canvas=GetDlgItem(editor,1020);const auto point=SendMessageW(editor,WM_APP+300,0,0);
        SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,point);SendMessageW(canvas,WM_LBUTTONUP,0,point);
        SetWindowTextW(GetDlgItem(editor,1043),source?L"1":L"0");
        SendMessageW(editor,WM_COMMAND,MAKEWPARAM(1058,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(editor,1058)));
        Check(display[0].visible==source,"display edit works with an authored bone in both sources");
        Check(bone_curve::Read(model.data(),0,read)&&read[0].keys.size()==2,"display edit preserves free curves");
    }
    // Value-curve mode changes use the same native selection as the main row.
    HWND tree=GetDlgItem(editor,1015);
    std::function<HTREEITEM(HTREEITEM)> findX=[&](HTREEITEM item)->HTREEITEM{
        for(;item;item=TreeView_GetNextSibling(tree,item)){TVITEMW data{};data.mask=TVIF_PARAM;data.hItem=item;SendMessageW(tree,TVM_GETITEMW,0,reinterpret_cast<LPARAM>(&data));
            if(data.lParam==1)return item;if(auto hit=findX(TreeView_GetChild(tree,item)))return hit;}return nullptr;};
    auto xItem=findX(TreeView_GetRoot(tree));Check(xItem!=nullptr,"free bone X curve exists");
    for(unsigned slot=0;;slot=model.record->boneKeys[slot].next){model.record->boneKeys[slot].allocated=model.record->boneKeys[slot].frame==20;if(!model.record->boneKeys[slot].next)break;}
    TreeView_SelectItem(tree,xItem);SendMessageW(tree,WM_KEYDOWN,VK_RETURN,0);
    HWND canvas=GetDlgItem(editor,1020);auto point=SendMessageW(editor,WM_APP+300,1,0);
    SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,point);SendMessageW(canvas,WM_LBUTTONUP,0,point);
    app->SceneModified()=0;
    SendMessageW(GetDlgItem(editor,1045),CB_SETCURSEL,1,0);
    SendMessageW(editor,WM_COMMAND,MAKEWPARAM(1045,CBN_SELCHANGE),reinterpret_cast<LPARAM>(GetDlgItem(editor,1045)));
    Check(app->SceneModified()!=0,"bone interpolation edit reaches the real host write");
    unsigned marks=0;
    for(unsigned slot=0;;slot=model.record->boneKeys[slot].next){
        if(model.record->boneKeys[slot].allocated){++marks;Check(model.record->boneKeys[slot].frame==20,"bone interpolation keeps the selected native frame");}
        if(!model.record->boneKeys[slot].next)break;
    }
    Check(marks==1,"bone interpolation does not select the entire native row");
    SendMessageW(editor,WM_COMMAND,MAKEWPARAM(1059,BN_CLICKED),0);
    Check(bone_curve::Read(model.data(),0,read)&&read[0].keys[1].interpolation==tracks[0].keys[1].interpolation,
          "bone curve undo survives native canonicalization and selection synchronization");
    curve_editor::CloseOwned(parent);DestroyWindow(parent);app->Hwnd()=nullptr;model.record->displayKeys=nullptr;
    app->ModelSlot(0)=nullptr;
    std::puts("bone_curve_tests: playback, native value/rotation/scale edits, independent lanes, retiming/deletion/registration, frame lines, undo/redo, PMM/VMD and editor passed");
}
