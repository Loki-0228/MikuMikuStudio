// Include after native_unused_key_checks.inc. These cases deliberately make
// native endpoints equal while the effective author curve is not constant.
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include <limits>
namespace {
namespace unused_curve_checks {
using namespace plugins;
struct Scene : ModelKeyFixture {
    Scene(){unused_key_checks::Constant(*this);}
    ~Scene(){history.Reset();history.Collect();bone_curve::Forget(first.raw());bone_curve::Forget(second.raw());}
};
bone_curve::Tracks BoneTracks(bool turn) {
    bone_curve::Tracks tracks;
    for(unsigned lane=0;lane<tracks.size();++lane)for(int frame:{0,10,30}){
        animation_curve::Key key;key.frame=frame;key.value=lane>=6?1:0;
        if(turn&&lane==4&&frame==10)key.value=360;
        key.tangent=animation_curve::Tangent::Free;key.interpolation=animation_curve::Interpolation::Bezier;
        key.inWeight=key.outWeight=.25;key.inSlope=key.outSlope=0;tracks[lane].keys.push_back(key);
    }
    return tracks;
}
void InstallBone(Scene& f,bool turn) {
    const ModelKeyState native(f.first);
    auto source=bone_curve::AuthoringState::Capture(f.first.raw());
    bone_curve::NativeKeyView view{&f.first,mdl::BoneKeyCapacity(f.first.m->boneKeys),
        [](const void* p,std::uint32_t slot)noexcept->const mdl::BoneKey*{
            const auto& fixture=*static_cast<const Fixture*>(p);
            return slot<mdl::BoneKeyCapacity(fixture.m->boneKeys)?&fixture.m->boneKeys[slot]:nullptr;},
        [](const void* p,std::uint32_t slot,bone_scale::Matrix& scale,bone_scale::Curve& curve)noexcept{
            const auto& fixture=*static_cast<const Fixture*>(p);
            if(slot>=mdl::BoneKeyCapacity(fixture.m->boneKeys))return false;
            auto* raw=reinterpret_cast<const unsigned char*>(fixture.m.get());
            scale=bone_scale::Key(raw,int(slot));curve=bone_scale::KeyCurve(raw,int(slot));return true;}};
    Check(source->Replace(0,BoneTracks(turn),view),"prepare Free Euler authoring against the existing exact native quaternion mirror");
    auto install=bone_curve::AuthoringRestore::Prepare(f.first.raw(),*source);
    Check(install&&install->Publish()&&native.Matches(f.first),"install author layer without modifying native endpoints, selection or pose");
    bone_curve::Tracks effective;Check(bone_curve::ReadDetached(f.first.raw(),0,effective),"canonical detached read sees Free Euler author");
    Check(effective[4].keys[1].value==(turn?360:0),"effective Euler middle retains its full turn rather than quaternion-normalized zero");
    for(unsigned slot:{2u,3u})Check(!std::memcmp(f.first.m->boneKeys[0].rotation,f.first.m->boneKeys[slot].rotation,4*sizeof(float)),
        "rotation loop fixture really has identical native quaternion endpoint bytes");
}
animation_curve::NamedTrack MorphTrack(Scene& f,bool overshoot) {
    animation_curve::NamedTrack named;
    named.id=morph_curve::StableId(0,f.first.morphs[0].name);
    for(int frame:{0,10,30}){
        animation_curve::Key key;key.frame=frame;key.value=.25;
        key.tangent=animation_curve::Tangent::Free;key.interpolation=animation_curve::Interpolation::Bezier;
        key.inWeight=key.outWeight=.25;key.inSlope=key.outSlope=0;named.track.keys.push_back(key);
    }
    if(overshoot){named.track.keys[0].outSlope=.08;named.track.keys[1].inSlope=-.08;}
    return named;
}
void InstallMorph(Scene& f,bool overshoot) {
    const auto track=MorphTrack(f,overshoot);std::string error;
    Check(morph_curve::Write(f.first.raw(),{track},error),"ordinary morph writer installs equal endpoints and valid Free Bezier handles");
    unsigned current=0;for(unsigned hop=0;;++hop){
        Check(hop<mdl::MorphKeyCapacity(f.first.m->morphKeys)&&f.first.m->morphKeys[current].value==.25f,
            "every native morph endpoint is exactly equal despite authored Bezier shape");
        if(!f.first.m->morphKeys[current].next)break;current=f.first.m->morphKeys[current].next;
    }
}
std::vector<std::uint8_t> MorphBytes(Scene& f) {
    std::vector<animation_curve::Track> tracks;std::wstring error;
    Check(morph_curve::ReadManyDetached(*f.app,{0},tracks,error)&&tracks.size()==1,"read detached effective morph author curve");
    std::vector<animation_curve::NamedTrack> named{{"morph",tracks.front()}};std::vector<std::uint8_t> bytes;std::string detail;
    Check(animation_curve::Encode(named,bytes,detail),"encode exact morph author handles for unused-key regression");return bytes;
}
void BoneTurnProtection() {
    Scene f;InstallBone(f,true);NativePoseTestBinding binding(*f.app,f.history);
    const ModelKeyState before(f.first),peer(f.second);const auto author=bone_curve::AuthoringState::Capture(f.first.raw());
    const auto digest=f.first.Digest(),ring=PoseRingDigest(f.first);auto change=unused_key_checks::Plan(f,2);
    Check(change&&before.Matches(f.first)&&f.first.Digest()==digest&&
        bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*author),
        "finding redundant native endpoints preserves a 0-360-0 author rotation and leaves its live source untouched");
    {unused_key_checks::Report report;CmdFileMenu(f.app.get(),f.app->MainWindow(),222,0);
        Check(report.seen==1,"actual 222 completes its Free Euler protection case");}
    Check(f.history.UndoCount()==1&&f.first.m->boneKeys[2].frame==10&&f.first.m->boneKeys[3].frame==30&&
        f.first.m->morphKeys[1].frame==0&&f.first.m->displayKeys[1].frame==0,
        "actual 222 keeps the native-identical full rotation while deleting only the genuinely constant other families");
    Check(bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*author)&&peer.Matches(f.second)&&
        f.first.bones[0].trans[0]==91.f&&f.first.morphs[0].value==.91f&&PoseRingDigest(f.first)==ring,
        "Free Euler protection never rewrites author handles, peer model, unregistered pose or old rings");
    const ModelKeyState after(f.first);
    Check(f.history.History(false)==MMS_OK&&before.Matches(f.first)&&bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*author),
        "unused-key undo restores the other families without normalizing the preserved full turn");
    Check(f.history.History(true)==MMS_OK&&after.Matches(f.first)&&bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*author),
        "unused-key redo still preserves the complete Free Euler author");
}
void MorphOvershootProtection() {
    Scene f;InstallMorph(f,true);NativePoseTestBinding binding(*f.app,f.history);
    std::vector<animation_curve::Track> effective;std::wstring error;
    Check(morph_curve::ReadManyDetached(*f.app,{0},effective,error)&&effective.size()==1&&
        animation_curve::Evaluate(effective[0],5).value>.25,
        "equal-endpoint morph author actually overshoots between keys");
    const auto pristine=MorphBytes(f);morph_curve::AuthoredLayers layers;std::string detail;
    Check(morph_curve::CaptureLayers(f.first.raw(),layers,detail),"capture pristine and derived morph maps before planning");
    const ModelKeyState before(f.first);const auto digest=f.first.Digest();auto change=unused_key_checks::Plan(f,2);
    Check(change&&f.first.Digest()==digest&&before.Matches(f.first)&&morph_curve::LayersUnchanged(f.first.raw(),layers),
        "overshoot detection uses detached authoring and does not synchronize or modify live maps");
    {unused_key_checks::Report report;CmdFileMenu(f.app.get(),f.app->MainWindow(),222,0);
        Check(report.seen==1,"actual 222 completes equal-morph-endpoint Bezier protection");}
    Check(f.history.UndoCount()==1&&f.first.m->morphKeys[1].frame==10&&f.first.m->boneKeys[2].frame==0&&
        f.first.m->displayKeys[1].frame==0&&MorphBytes(f)==pristine,
        "actual 222 preserves effective morph overshoot and deletes only the two constant families");
    const ModelKeyState after(f.first);
    Check(f.history.History(false)==MMS_OK&&before.Matches(f.first)&&MorphBytes(f)==pristine,"overshoot survives exact unused undo");
    Check(f.history.History(true)==MMS_OK&&after.Matches(f.first)&&MorphBytes(f)==pristine,"overshoot survives exact unused redo");
}
void ConstantFreeRoundTrip() {
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly}){
        Scene f;InstallBone(f,false);InstallMorph(f,false);NativePoseTestBinding binding(*f.app,f.history);
        Check(f.history.SetMode(mode)==MMS_OK,"constant Free deletion works in each recording mode");
        const ModelKeyState before(f.first),peer(f.second);const auto bone=bone_curve::AuthoringState::Capture(f.first.raw());
        const auto morph=MorphBytes(f);auto change=unused_key_checks::Plan(f,3);ModelKeyCommit(f,change);
        Check(f.history.UndoCount()==1&&f.first.m->boneKeys[2].frame==0&&f.first.m->morphKeys[1].frame==0&&f.first.m->displayKeys[1].frame==0,
            "constant Free tracks are removable instead of conservatively blocking every authored track");
        const ModelKeyState after(f.first);const auto boneAfter=bone_curve::AuthoringState::Capture(f.first.raw());const auto morphAfter=MorphBytes(f);
        Check(f.history.History(false)==MMS_OK&&before.Matches(f.first)&&bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*bone)&&MorphBytes(f)==morph,
            "one unused undo restores exact constant Free bone and morph handles with native keys");
        Check(f.history.History(true)==MMS_OK&&after.Matches(f.first)&&bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*boneAfter)&&MorphBytes(f)==morphAfter&&peer.Matches(f.second),
            "one unused redo restores the immutable reduced authors without changing the peer model");
    }
}
void NonfiniteSourceAndRejectedAuthor() {
    for(unsigned family=0;family<2;++family){
        Scene f;
        if(family==0)f.first.m->boneKeys[2].position[0]=std::numeric_limits<float>::quiet_NaN();
        else f.first.m->morphKeys[1].value=std::numeric_limits<float>::infinity();
        const ModelKeyState before(f.first),peer(f.second);const auto digest=f.first.Digest();std::uint32_t count=99;
        std::shared_ptr<HistoryChange> change;
        const auto result=PlanUnusedModelKeyDeletion(*f.app,0,false,change,count);
        Check((result==MMS_OK&&count==2&&change)||(result==MMS_E_ARGUMENT&&!count&&!change)||
            (result==MMS_E_NOT_READY&&!count&&!change),"nonfinite native input is rejected or excluded from the redundant set");
        Check(before.Matches(f.first)&&peer.Matches(f.second)&&f.first.Digest()==digest&&!f.history.UndoCount(),
            "nonfinite unused scan has no native marking, pose, history or live source side effects");
        if(result==MMS_OK){ModelKeyCommit(f,change);
            Check(family==0?f.first.m->boneKeys[2].frame==10:f.first.m->morphKeys[1].frame==10,
                "an admitted deletion of other tracks never removes the nonfinite offending key");
            Check(f.history.History(false)==MMS_OK&&before.Matches(f.first),"undo after a nonfinite exclusion restores exact native bytes");}
    }
    Scene f;InstallBone(f,true);InstallMorph(f,true);const ModelKeyState before(f.first);
    const auto bone=bone_curve::AuthoringState::Capture(f.first.raw());const auto morph=MorphBytes(f);const auto digest=f.first.Digest();
    auto invalid=BoneTracks(true);invalid[4].keys[1].outSlope=std::numeric_limits<double>::quiet_NaN();std::string error;
    Check(!bone_curve::Write(f.first.raw(),0,invalid,error),"nonfinite Free bone author is rejected before modifying its native mirror");
    auto invalidMorph=MorphTrack(f,true);invalidMorph.track.keys[0].outSlope=std::numeric_limits<double>::infinity();
    Check(!morph_curve::Write(f.first.raw(),{invalidMorph},error),"nonfinite morph Bezier author is rejected before modifying layers");
    Check(before.Matches(f.first)&&f.first.Digest()==digest&&bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*bone)&&MorphBytes(f)==morph,
        "invalid author requests leave existing valid full-turn and overshoot authoring exact");
    auto change=unused_key_checks::Plan(f,1);
    Check(change&&before.Matches(f.first),"after rejected nonfinite author requests only the constant display key remains removable");
}
}
void NativeUnusedCurveChecks(){
    unused_curve_checks::BoneTurnProtection();unused_curve_checks::MorphOvershootProtection();
    unused_curve_checks::ConstantFreeRoundTrip();unused_curve_checks::NonfiniteSourceAndRejectedAuthor();
}
}
