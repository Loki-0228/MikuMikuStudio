// Include after native_negative_copy_result_checks.inc. The parent TU supplies
// --negative-copy-source-only -> NativeNegativeCopySourceChecks().
// Synthetic fixtures only. This tests the complete private negative source API;
// production 420 and the future two-sided signed batch coordinator are separate.
// No interpolation/physics payload absent from State.keys is reconstructed.
// Public registration and PMM Load reject NaN: raw NaN is NOT dynamically covered.
// No private-Key access, forged CopySource, or production test backdoor is used.
#include "../src/animation/negative_copy_source.hpp"
#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace {
namespace negative_copy_source_checks {
namespace copy = global_copy_failure_checks;
namespace receipt = global_copy_snapshot_checks;
namespace result = negative_copy_result_checks;
namespace nf = negative_frames;
using Source = std::shared_ptr<const nf::CopySource>;
using Key = nf::CopySourceKey;
using Values = std::array<double,16>;
using Mode = plugins::SharedHistory::RecordingMode;
constexpr long allowanceMax = (std::numeric_limits<long>::max)();
static_assert(std::is_same<decltype(nf::CaptureCopySource(std::declval<MMDApp&>(),
    std::declval<Source&>())),MmsResult>::value,"capture returns the ABI result code");
static_assert(noexcept(nf::CaptureCopySource(std::declval<MMDApp&>(),std::declval<Source&>())),
    "capture contains allocation failures");
static_assert(noexcept(nf::CopySourceCurrent(std::declval<const MMDApp&>(),
    std::declval<const nf::CopySource&>())),"source validation never throws");

bool SameIdentity(const MmsObjectRef& a,const MmsObjectRef& b) {
    return a.id.lo==b.id.lo && a.id.hi==b.id.hi && a.generation==b.generation;
}
bool SameRaw(const Key& a,const Key& b) {
    return a.model==b.model && a.identity==b.identity && a.target==b.target && a.frame==b.frame &&
        a.morph==b.morph && a.selected==b.selected && a.clipboardOffset==b.clipboardOffset &&
        !std::memcmp(a.values.data(),b.values.data(),sizeof(a.values)) &&
        nf::authoring::Same(a.author,b.author);
}
bool SameKey(const Key& a,const Key& b) {
    return SameRaw(a,b) && a.name==b.name && a.occurrence==b.occurrence &&
        a.mappedBone==b.mappedBone && a.trackAvailable==b.trackAvailable;
}
bool SameReference(const nf::CopyObjectReference& a,const nf::CopyObjectReference& b) {
    return a.slot==b.slot && a.pointer==b.pointer && SameIdentity(a.identity,b.identity) &&
        a.referenced==b.referenced && a.available==b.available;
}
bool SameSource(const nf::CopySource& a,const nf::CopySource& b) {
    if(a.owner!=b.owner || a.mode!=b.mode || a.modelSlot!=b.modelSlot || a.sceneGeneration!=b.sceneGeneration ||
        a.hadState!=b.hadState || a.referencesAvailable!=b.referencesAvailable || a.first!=b.first ||
        a.lifetime.owner_before(b.lifetime) || b.lifetime.owner_before(a.lifetime) || a.keys.size()!=b.keys.size())return false;
    for(std::size_t i=0;i<a.keys.size();++i)if(!SameKey(a.keys[i],b.keys[i]))return false;
    for(unsigned i=0;i<255;++i) {
        if(!SameReference(a.accessories[i],b.accessories[i]) || !SameReference(a.models[i],b.models[i]))return false;
        const auto& x=a.models[i];const auto& y=b.models[i];
        if(x.tracks!=y.tracks || x.bones!=y.bones || x.morphs!=y.morphs || x.ikChains!=y.ikChains ||
            x.selectors!=y.selectors || x.boneCount!=y.boneCount || x.morphCount!=y.morphCount ||
            x.ikCount!=y.ikCount || x.selectorCount!=y.selectorCount)return false;
    }
    return true;
}
Source Capture(MMDApp& app) {
    Source out;Check(nf::CaptureCopySource(app,out)==MMS_OK && out,"successful capture owns a source, including empty selection");
    return out;
}
void Current(MMDApp& app,const Source& source,bool expected) {
    Check(source && failAfter==-1 && !allocationAudit,"Current injection starts outside other allocation audits");
    AllocationAudit audit;allocationAudit=&audit;failAfter=0;
    const bool current=nf::CopySourceCurrent(app,*source);
    const long remaining=failAfter;failAfter=-1;allocationAudit=nullptr;
    Check(current==expected && remaining==0 && !audit.outstanding && !audit.overflow,
        "Current returns the expected identity/lifetime result with allocation forbidden");
}
const Key& Row(const nf::CopySource& source,int target,int frame,bool morph=false) {
    const Key* found=nullptr;
    for(const auto& key:source.keys)if(key.target==target && key.frame==frame && key.morph==morph) {
        Check(!found,"source contains each selected row once");found=&key;
    }
    Check(found!=nullptr,"selected raw row remains in the source");return *found;
}
void Rows(const Source& source,const std::vector<Key>& expected) {
    Check(source->keys.size()==expected.size(),"capture contains all and only the selected active-timeline rows");
    std::int64_t first=0;
    for(const auto& key:expected) {
        Check(SameKey(Row(*source,key.target,key.frame,key.morph),key),
            "capture preserves every raw field, all sixteen values, name20, occurrence and track availability");
        first=std::min(first,std::int64_t(key.frame));
    }
    Check(source->first==first,"source first is the minimum selected signed frame, not playhead or native origin");
}
Key Expected(const unsigned char* model,const void* identity,int target,int frame,bool morph,Values values,
             const char* name=nullptr,unsigned occurrence=0,int mappedBone=-1) {
    Key key;key.model=model;key.identity=identity;key.target=target;key.frame=frame;key.morph=morph;
    key.selected=true;key.values=values;key.trackAvailable=true;key.occurrence=occurrence;key.mappedBone=mappedBone;
    if(name)std::copy_n(name,20,key.name.begin());return key;
}
void ObjectStamp(const nf::CopyObjectReference& ref,int slot,const void* pointer) {
    MmsObjectRef id{};
    Check(plugins::FindSceneObjectIdentity(pointer,id) && ref.referenced && ref.available && ref.slot==slot &&
        ref.pointer==pointer && SameIdentity(ref.identity,id),"reference saves an existing copy-time slot, pointer and object identity");
}
void ModelStamp(const nf::CopyModelReference& ref,int slot,Fixture& f,bool tracks) {
    ObjectStamp(ref,slot,f.raw());const auto& m=*f.m;
    Check(ref.tracks==tracks && ref.bones==m.boneTable && ref.boneCount==m.boneCount &&
        ref.morphs==m.morphs && ref.morphCount==m.morphCount && ref.ikChains==m.ikChains &&
        ref.ikCount==m.ikChainCount && ref.selectors==m.boneOrderTable && ref.selectorCount==m.boneOrderCount,
        "model stamp owns four table tokens/counts and distinguishes source tracks from parent-only references");
}

struct Parent {
    Fixture model;
    std::array<mdl::IkChain,1> chains{};
    std::array<mdl::BoneOrderEntry,1> selectors{};
    Parent() {
        model.m->ikChains=chains.data();model.m->boneOrderTable=selectors.data();
        selectors[0].linkedModel=selectors[0].linkedBone=-1;
        plugins::SceneObjectIdentity(model.raw());
    }
    ~Parent(){plugins::ForgetSceneObject(model.raw());model.m->ikChains=nullptr;model.m->boneOrderTable=nullptr;}
};
struct GlobalFixture : receipt::OwnedFixture {
    Parent cameraParent,accessoryParent;
    std::vector<Key> expected;
    explicit GlobalFixture(Mode mode) {
        Check(history.SetMode(mode)==MMS_OK,"set negative-source history mode");receipt::DetachParents(*this);
        app->ModelSlot(17)=cameraParent.model.raw();app->ModelSlot(19)=accessoryParent.model.raw();
        app->CameraParentModel()=17;app->CameraParentBone()=1;
        Values camera{};for(unsigned i=0;i<3;++i){camera[i]=app->CameraPositionAxis(i);camera[3+i]=app->CameraRotation()[i];}
        camera[6]=app->CameraDistance();camera[7]=app->CameraFov();camera[8]=app->CameraPerspective();camera[9]=17;camera[10]=1;
        Add(nf::Global::Camera,-90,camera);
        Values light{};for(unsigned i=0;i<3;++i){light[i]=app->LightDirection()[i];light[3+i]=app->LightColor()[i];}
        Add(nf::Global::Light,-150,light);
        app->SelfShadowMode()=2;app->ShadowDistance()=.125f;Add(nf::Global::Shadow,-30,Values{{2,.125}});
        app->GravityMagnitude()=9.25f;app->GravityNoise()=-13;app->GravityNoiseEnabled()=1;
        app->GravityDirection()[0]=.25f;app->GravityDirection()[1]=-.5f;app->GravityDirection()[2]=.75f;
        Add(nf::Global::Gravity,-110,Values{{9.25,.25,-.5,.75,-13,1}});
        for(unsigned i=0;i<2;++i) {
            auto& a=accessory[i];a.parentModel=i?-1:19;a.parentBone=i?-1:1;
            a.scale=1.25f+float(i);a.opacity=.625f;a.visible=1;a.shadowEnabled=std::uint8_t(i);
            Values values{};for(unsigned j=0;j<3;++j){a.position[j]=float(i*3+j)+.25f;a.rotation[j]=float(j)*.125f;
                values[j]=a.position[j];values[3+j]=a.rotation[j];}
            values[6]=a.scale;values[7]=a.opacity;values[8]=a.visible;values[9]=a.shadowEnabled;
            values[10]=a.parentModel;values[11]=a.parentBone;
            const int slot=i?254:7,frame=i?-50:-70;
            Check(nf::RegisterAccessory(app.get(),slot,frame),"register a real negative accessory row");
            expected.push_back(Expected(nullptr,&a,1000+slot,frame,false,values));
        }
        Check(nf::RegisterGlobal(app.get(),nf::Global::Light,-10) && history.History(false)==MMS_OK && history.RedoCount()==1,
            "global source has meaningful undo and redo before capture");
        nf::SelectRange(app.get(),-150,-20);app->CurrentFrame()=321;
    }
    void Add(nf::Global kind,int frame,Values values) {
        Check(nf::RegisterGlobal(app.get(),kind,frame),"register a real negative global row");
        expected.push_back(Expected(nullptr,app.get(),int(kind),frame,false,values));
    }
    ~GlobalFixture(){history.Reset();history.Collect();nf::Reset(app.get());app->ModelSlot(17)=app->ModelSlot(19)=nullptr;}
};
struct ModelFixture : result::ModelFixture {
    Parent parent;
    std::array<mdl::MorphRecord,2> morphs{};
    std::array<mdl::BoneOrderEntry,1> selectors{};
    std::vector<Key> expected;
    explicit ModelFixture(Mode mode):result::ModelFixture(mode) {
        app->ModelSlot(19)=parent.model.raw();plugins::SceneObjectIdentity(model.raw());
        model.m->morphs=morphs.data();model.m->morphCount=2;model.m->boneOrderTable=selectors.data();
        // Full-width non-NUL names exercise exactly 20 owned bytes and duplicate
        // occurrences even when preceding tracks have no selected source row.
        for(auto& bone:model.bones)std::memcpy(bone.name,"12345678901234567890",20);
        for(auto& morph:morphs)std::memcpy(morph.name,"abcdefghijklmnopqrst",20);
        chains[0].boneIndex=1;chains[0].enabled=0;
        selectors[0].boneIndex=1;selectors[0].linkedModel=19;selectors[0].linkedBone=1;
        for(int bone:{1,0}) {
            auto& b=model.bones[bone];for(unsigned j=0;j<3;++j)b.trans[j]=float(bone*4+j)+.125f;
            b.rotQuat[0]=.5f;b.rotQuat[1]=-.5f;b.rotQuat[2]=.5f;b.rotQuat[3]=.5f;
            bone_scale::Matrix scale;scale.v={{2,.125f,.25f,.375f,3,.5f,.625f,.75f,4}};
            Check(bone_scale::SetPose(model.raw(),bone,scale),"seed all nine stored scale components");
            Values values{};std::copy_n(b.trans,3,values.begin());std::copy_n(b.rotQuat,4,values.begin()+3);
            std::copy(scale.v.begin(),scale.v.end(),values.begin()+7);const int frame=bone?-120:-90;
            Check(nf::RegisterBone(model.raw(),bone,frame),"register a real negative bone row");
            expected.push_back(Expected(model.raw(),model.bones.data(),bone,frame,false,values,b.name,unsigned(bone)));
        }
        for(int morph:{1,0}) {
            const int frame=morph?-100:-80;const float value=morph?.625f:-.25f;
            Check(nf::RegisterMorph(model.raw(),morph,frame,value),"register a real negative morph row");
            expected.push_back(Expected(model.raw(),morphs.data(),morph,frame,true,Values{{value}},morphs[morph].name,unsigned(morph)));
        }
        model.m->loadComplete=1;
        Check(nf::RegisterDisplay(model.raw(),-60),"register display, IK and selector through the real registrar");
        expected.push_back(Expected(model.raw(),model.bones.data(),-1,-60,false,Values{{1}}));
        expected.push_back(Expected(model.raw(),model.bones.data(),-2,-60,false,Values{{0}},model.bones[1].name,1,1));
        expected.push_back(Expected(model.raw(),model.bones.data(),-1000000,-60,false,Values{{19,1}},model.bones[1].name,1,1));
        // Exclude the base fixture's -70/-40 bone rows without mutating private keys.
        nf::SelectRange(app.get(),1,2);
        for(const auto& key:expected) {
            // Re-registration marks only these seven keys selected, preserving
            // old unselected rows in the same private State.
            if(key.morph)Check(nf::RegisterMorph(model.raw(),key.target,key.frame,float(key.values[0])),"select authored morph row");
            else if(key.target>=0)Check(nf::RegisterBone(model.raw(),key.target,key.frame),"select authored bone row");
        }
        Check(nf::RegisterDisplay(model.raw(),-60),"select authored display group");
        Check(nf::RegisterMorph(model.raw(),0,-10,.875f) && history.History(false)==MMS_OK && history.RedoCount()==1,
            "model source has meaningful undo and redo before capture");
        app->CurrentFrame()=456;
    }
    ~ModelFixture() {
        history.Reset();history.Collect();nf::Reset(app.get());app->ModelSlot(19)=nullptr;
        model.m->morphs=model.morphs.data();model.m->morphCount=1;model.m->boneOrderTable=nullptr;
    }
};

struct ModelLive : result::ModelLive {
    std::vector<unsigned char> morphs,chains,selectors;
    std::array<bone_scale::Matrix,2> scales;
    std::uint64_t parent;
    explicit ModelLive(ModelFixture& f):result::ModelLive(f),
        morphs(copy::Bytes(f.morphs.data(),sizeof(f.morphs))),chains(copy::Bytes(f.chains.data(),sizeof(f.chains))),
        selectors(copy::Bytes(f.selectors.data(),sizeof(f.selectors))),
        scales{{bone_scale::Pose(f.model.raw(),0),bone_scale::Pose(f.model.raw(),1)}},parent(f.parent.model.Digest()) {}
    void Expect(ModelFixture& f) const {
        result::ModelLive::Expect(f);
        Check(morphs==copy::Bytes(f.morphs.data(),sizeof(f.morphs)) && chains==copy::Bytes(f.chains.data(),sizeof(f.chains)) &&
            selectors==copy::Bytes(f.selectors.data(),sizeof(f.selectors)) && parent==f.parent.model.Digest() &&
            scales[0]==bone_scale::Pose(f.model.raw(),0) && scales[1]==bone_scale::Pose(f.model.raw(),1),
            "capture preserves the expanded morph table, IK/selector values, parent and scale poses");
    }
};

// Own every native clipboard byte separately from the live-key/history fixture.
// CaptureCopySource must neither consume nor repair the legacy RawClip buffers.
struct RawClip {
    std::array<void*,10> pointers{};
    std::array<std::vector<unsigned char>,10> bytes;
    std::array<unsigned,8> counts{};
    frame_clipboard::GlobalCopyState receipt;
    explicit RawClip(MMDApp& app):receipt(app.GlobalClipboardCopy()) {
        const auto& n=app.ClipboardCounts();counts={{n.cameras,n.lights,n.shadows,n.gravity,n.accessories,n.bones,n.morphs,n.displays}};
        pointers={{app.CameraClipboard(),app.LightClipboard(),app.ShadowClipboard(),app.GravityClipboard(),app.AccessoryClipboard(),
            app.BoneClipboard(),app.MorphClipboard(),app.DisplayClipboard(),nullptr,nullptr}};
        const std::array<std::size_t,8> sizes{{sizeof(mdl::CameraClipboardRecord),sizeof(mdl::LightClipboardRecord),
            sizeof(mdl::ShadowClipboardRecord),sizeof(mdl::GravityClipboardRecord),sizeof(mdl::AccessoryClipboardKey),
            sizeof(mdl::BoneClipboardRecord),sizeof(mdl::MorphClipboardRecord),sizeof(mdl::DisplayClipboardRecord)}};
        for(unsigned i=0;i<8;++i)if(pointers[i] && counts[i])bytes[i]=copy::Bytes(pointers[i],sizes[i]*counts[i]);
        if(n.displays && app.DisplayClipboard()) {
            Check(n.displays==1,"synthetic RawClip owns one display record");const auto& d=*app.DisplayClipboard();
            pointers[8]=d.ikStates;pointers[9]=d.selectorStates;
            if(d.ikStates && d.ikCount)bytes[8]=copy::Bytes(d.ikStates,d.ikCount*sizeof(*d.ikStates));
            if(d.selectorStates && d.selectorCount)bytes[9]=copy::Bytes(d.selectorStates,d.selectorCount*sizeof(*d.selectorStates));
        }
    }
    void Expect(MMDApp& app) const {
        const RawClip now(app);
        Check(pointers==now.pointers && counts==now.counts && bytes==now.bytes,
            "source capture leaves every legacy RawClip allocation and byte unchanged");
        Check(receipt.attempt==now.receipt.attempt && receipt.sceneGeneration==now.receipt.sceneGeneration &&
            receipt.status==now.receipt.status && receipt.error==now.receipt.error && receipt.preparing==now.receipt.preparing &&
            receipt.snapshot==now.receipt.snapshot,"source capture does not publish or retire a production 420 receipt");
    }
};

void GlobalFields(Mode mode) {
    GlobalFixture f(mode);const copy::Snapshot before(f);const RawClip raw(*f.app);
    auto source=Capture(*f.app);before.Expect(f);raw.Expect(*f.app);Rows(source,f.expected);
    Check(source->owner==f.app.get() && source->mode==nf::CopySourceMode::Global && source->modelSlot==-1 &&
        source->sceneGeneration==f.app->SceneGeneration() && source->hadState && !source->lifetime.expired() &&
        source->referencesAvailable,"global source captures its owner, origin, generation and live State token");
    ObjectStamp(source->accessories[7],7,&f.accessory[0]);ObjectStamp(source->accessories[254],254,&f.accessory[1]);
    ModelStamp(source->models[17],17,f.cameraParent.model,false);ModelStamp(source->models[19],19,f.accessoryParent.model,false);
    for(unsigned i=0;i<255;++i) {
        Check(source->accessories[i].referenced==(i==7 || i==254),"only selected accessory slots are stamped");
        Check(source->models[i].referenced==(i==17 || i==19),"only selected global parent models are stamped");
    }
    Current(*f.app,source,true);const auto saved=*source;
    nf::SelectRange(f.app.get(),1,2);f.app->CurrentFrame()=-901;
    f.app->CameraDistance()=-123.25f;
    Check(nf::RegisterGlobal(f.app.get(),nf::Global::Camera,-90),"edit the live source value after capture");
    Current(*f.app,source,true);
    auto edited=Capture(*f.app);Check(edited->keys.size()==1 && edited->keys[0].values[6]==-123.25 &&
        SameSource(*source,saved),"recapture sees new selection/value while held source keeps all original bytes");
    f.SeedOld();f.app->CameraClipboard()->distance=777;nf::ClearClipboard(f.app.get());
    Check(SameSource(*source,saved),"overwriting raw native and clearing legacy negative clipboards cannot alter the source");
    const auto attempt=f.app->GlobalClipboardCopy().attempt;CmdControl400(f.app.get(),nullptr,420,0);
    Check(f.app->GlobalClipboardCopy().attempt==attempt+1 && SameSource(*source,saved),
        "a later real global 420 batch cannot retire or overwrite an independent negative source");
    Current(*f.app,source,true);
    f.app->CameraMode()=0;f.app->SelectedModelSlot()=17;f.app->CurrentFrame()=321;
    Check(nf::RegisterBone(f.cameraParent.model.raw(),0,-333),"register selected data in the other mode");
    Current(*f.app,source,true);f.app->CameraMode()=1;
    auto global=Capture(*f.app);Check(global->keys.size()==1 && !global->keys[0].model,
        "selected model keys never enter a global source");
    Check(SameSource(*source,saved),"later captures and opposite-mode data leave the held source immutable");
    auto other=std::make_unique<MMDApp>();Current(*other,source,false);
}
void ModelFields(Mode mode) {
    ModelFixture f(mode);const ModelLive before(f);const RawClip raw(*f.app);
    auto source=Capture(*f.app);before.Expect(f);raw.Expect(*f.app);Rows(source,f.expected);
    Check(source->owner==f.app.get() && source->mode==nf::CopySourceMode::Model && source->modelSlot==0 &&
        source->sceneGeneration==f.app->SceneGeneration() && source->hadState && !source->lifetime.expired() &&
        source->referencesAvailable,"model source captures model context and live State ownership");
    ModelStamp(source->models[0],0,f.model,true);ModelStamp(source->models[19],19,f.parent.model,false);
    for(unsigned i=0;i<255;++i)Check(!source->accessories[i].referenced && source->models[i].referenced==(i==0 || i==19),
        "model capture ignores global keys and stamps only its own model plus selector parent");
    const auto saved=*source;Current(*f.app,source,true);
    nf::SelectRange(f.app.get(),-120,-120);auto onlySecondBone=Capture(*f.app);
    Check(onlySecondBone->keys.size()==1 && onlySecondBone->keys[0].occurrence==1,
        "duplicate occurrence counts preceding tracks even when those tracks have no selected row");
    std::memset(f.model.bones[1].name,'B',20);std::memset(f.morphs[1].name,'M',20);
    nf::SelectRange(f.app.get(),1,2);f.model.bones[1].trans[0]=91.5f;
    Check(nf::RegisterBone(f.model.raw(),1,-120),"replace a real bone key after copy");
    auto edited=Capture(*f.app);Check(edited->keys.size()==1 && edited->keys[0].values[0]==91.5 &&
        edited->keys[0].name[19]=='B' && edited->keys[0].occurrence==0,"fresh metadata uses edited copy-time track name and occurrence");
    Check(nf::RegisterMorph(f.model.raw(),1,-100,.875f),"replace a real morph key after copy");
    auto editedMorph=Capture(*f.app);const auto& morph=Row(*editedMorph,1,-100,true);
    Check(morph.name[19]=='M' && morph.occurrence==0 && morph.values[0]==.875,
        "fresh morph metadata owns the edited name, occurrence and value");
    f.SeedOld();f.app->BoneClipboard()[0].frame=123;nf::ClearClipboard(f.app.get());
    f.app->CameraMode()=1;f.app->CurrentFrame()=912;
    Check(nf::RegisterGlobal(f.app.get(),nf::Global::Gravity,-444),"add selected global data after model capture");
    const auto attempt=f.app->GlobalClipboardCopy().attempt;CmdControl400(f.app.get(),nullptr,420,0);
    Check(f.app->GlobalClipboardCopy().attempt==attempt+1,"publish a later real clipboard batch in global mode");
    Current(*f.app,source,true);f.app->CameraMode()=0;f.app->SelectedModelSlot()=19;
    Current(*f.app,source,true);f.app->SelectedModelSlot()=0;
    auto model=Capture(*f.app);Check(model->keys.size()==2 && model->keys[0].model==f.model.raw() && model->keys[1].model==f.model.raw(),
        "selected global rows never enter a model source");
    Check(SameSource(*source,saved),"model values, names, selection, cursor, mode and RawClip changes cannot rewrite held bytes");
    auto other=std::make_unique<MMDApp>();Current(*other,source,false);
}

void StaleAccessories() {
    GlobalFixture f(Mode::AllOperations);nf::SelectRange(f.app.get(),-70,-70);
    auto live=Capture(*f.app);Check(live->keys.size()==1,"stale accessory case has exactly one selected row");
    const auto raw=live->keys[0];f.app->AccessorySlot(7)=nullptr;
    auto missing=Capture(*f.app);
    Check(missing->keys.size()==1 && SameRaw(missing->keys[0],raw) && !missing->keys[0].trackAvailable &&
        !missing->referencesAvailable && missing->accessories[7].referenced && !missing->accessories[7].available,
        "selected stale accessory is successful raw capture, never a successful empty source");
    Current(*f.app,live,false);Current(*f.app,missing,false);
    f.app->AccessorySlot(7)=&f.accessory[0];Current(*f.app,live,true);Current(*f.app,missing,false);
    mdl::AccessoryRecord replacement{};plugins::SceneObjectIdentity(&replacement);f.app->AccessorySlot(7)=&replacement;
    auto replaced=Capture(*f.app);Check(replaced->keys.size()==1 && SameRaw(replaced->keys[0],raw) &&
        !replaced->referencesAvailable,"replacement at the same slot cannot hide the selected old raw accessory row");
    Current(*f.app,live,false);Current(*f.app,replaced,false);f.app->AccessorySlot(7)=&f.accessory[0];
    plugins::ForgetSceneObject(&replacement);
    plugins::ForgetSceneObject(&f.accessory[0]);MmsObjectRef absent{};
    Check(!plugins::FindSceneObjectIdentity(&f.accessory[0],absent),"accessory identity is absent before capture");
    auto noIdentity=Capture(*f.app);
    Check(noIdentity->keys.size()==1 && SameRaw(noIdentity->keys[0],raw) && !noIdentity->referencesAvailable &&
        !plugins::FindSceneObjectIdentity(&f.accessory[0],absent),"capture uses Find and cannot create an accessory identity");
    Current(*f.app,noIdentity,false);plugins::SceneObjectIdentity(&f.accessory[0]);
    Current(*f.app,live,false);Current(*f.app,noIdentity,false);Current(*f.app,Capture(*f.app),true);
}

// Validate every pointer/count independently; tokens are restored before any
// fixture teardown or registrar can dereference the temporarily replaced table.
void Structure(MMDApp& app,const Source& source,Fixture& fixture,bool tracks) {
    auto& m=*fixture.m;std::array<mdl::BoneRecord,2> bones{};std::array<mdl::MorphRecord,2> morphs{};
    std::array<mdl::IkChain,1> chains{};std::array<mdl::BoneOrderEntry,1> selectors{};
    const auto boneTable=m.boneTable;m.boneTable=bones.data();Current(app,source,false);m.boneTable=boneTable;
    const auto boneCount=m.boneCount;++m.boneCount;Current(app,source,false);m.boneCount=boneCount;
    const auto morphTable=m.morphs;m.morphs=morphs.data();Current(app,source,!tracks);m.morphs=morphTable;
    const auto morphCount=m.morphCount;++m.morphCount;Current(app,source,!tracks);m.morphCount=morphCount;
    const auto ikTable=m.ikChains;m.ikChains=chains.data();Current(app,source,!tracks);m.ikChains=ikTable;
    const auto ikCount=m.ikChainCount;++m.ikChainCount;Current(app,source,!tracks);m.ikChainCount=ikCount;
    const auto selectorTable=m.boneOrderTable;m.boneOrderTable=selectors.data();Current(app,source,!tracks);m.boneOrderTable=selectorTable;
    const auto selectorCount=m.boneOrderCount;++m.boneOrderCount;Current(app,source,!tracks);m.boneOrderCount=selectorCount;
    Current(app,source,true);
}
void ParentReferences() {
    GlobalFixture f(Mode::AllOperations);auto source=Capture(*f.app);const auto saved=*source;
    Structure(*f.app,source,f.cameraParent.model,false);Structure(*f.app,source,f.accessoryParent.model,false);
    for(int slot:{17,19}) {
        auto* pointer=f.app->ModelSlot(slot);f.app->ModelSlot(slot)=nullptr;Current(*f.app,source,false);
        auto missing=Capture(*f.app);Rows(missing,f.expected);
        Check(!missing->referencesAvailable && missing->models[slot].referenced && !missing->models[slot].available,
            "missing camera/accessory parent keeps every raw key and marks its parent unavailable");
        f.app->ModelSlot(slot)=pointer;Current(*f.app,source,true);Current(*f.app,missing,false);
    }
    plugins::ForgetSceneObject(f.cameraParent.model.raw());MmsObjectRef absent{};
    auto missing=Capture(*f.app);Rows(missing,f.expected);
    Check(!plugins::FindSceneObjectIdentity(f.cameraParent.model.raw(),absent) && !missing->referencesAvailable,
        "capture cannot create a missing parent model identity");
    plugins::SceneObjectIdentity(f.cameraParent.model.raw());Current(*f.app,source,false);Current(*f.app,missing,false);
    Current(*f.app,Capture(*f.app),true);Check(SameSource(*source,saved),"parent identity reuse never changes retained bytes");
    auto transient=std::make_unique<Parent>();f.app->ModelSlot(17)=transient->model.raw();
    auto removable=Capture(*f.app);Current(*f.app,removable,true);
    f.app->ModelSlot(17)=nullptr;transient.reset();Current(*f.app,removable,false);
    Check(removable->keys.size()==6,"checking a destroyed parent uses the live slot before dereferencing cached tokens");
    f.app->ModelSlot(17)=f.cameraParent.model.raw();
}
void ModelReferences() {
    ModelFixture f(Mode::AllOperations);auto source=Capture(*f.app);const auto saved=*source;
    Structure(*f.app,source,f.model,true);Structure(*f.app,source,f.parent.model,false);
    f.app->ModelSlot(19)=nullptr;auto missingParent=Capture(*f.app);Rows(missingParent,f.expected);
    Check(!missingParent->referencesAvailable && missingParent->models[19].referenced && !missingParent->models[19].available,
        "selector's missing parent is stamped without dropping the selector key");
    Current(*f.app,source,false);f.app->ModelSlot(19)=f.parent.model.raw();Current(*f.app,source,true);Current(*f.app,missingParent,false);
    // A source still pointing at the same model retains raw rows even after its
    // identity-bearing bone/morph table has been replaced before capture.
    std::array<mdl::BoneRecord,2> bones=f.model.bones;std::array<mdl::MorphRecord,2> morphs=f.morphs;
    for(bool replaceMorph:{false,true}) {
        if(replaceMorph)f.model.m->morphs=morphs.data();else f.model.m->boneTable=bones.data();
        auto stale=Capture(*f.app);Check(stale->keys.size()==source->keys.size() && !stale->referencesAvailable,
            "same model with changed key identity table keeps the complete raw selection");
        for(const auto& key:source->keys) {
            const auto& now=Row(*stale,key.target,key.frame,key.morph);
            Check(SameRaw(now,key) && now.trackAvailable==(key.morph!=replaceMorph),
                "only rows whose bone/morph table identity changed lose track metadata availability");
        }
        Current(*f.app,source,false);Current(*f.app,stale,false);
        f.model.m->boneTable=f.model.bones.data();f.model.m->morphs=f.morphs.data();
        Current(*f.app,source,true);Current(*f.app,stale,false);
    }
    for(bool shrinkMorph:{false,true}) {
        if(shrinkMorph)f.model.m->morphCount=1;else f.model.m->boneCount=1;
        auto shortened=Capture(*f.app);Check(shortened->keys.size()==source->keys.size() && !shortened->referencesAvailable,
            "reduced track counts keep out-of-range selected rows in raw capture");
        for(const auto& key:source->keys)Check(SameRaw(Row(*shortened,key.target,key.frame,key.morph),key),
            "count changes cannot filter selected raw rows");
        Check(!Row(*shortened,1,shrinkMorph?-100:-120,shrinkMorph).trackAvailable,
            "a removed bone or morph track reports metadata unavailable");
        Current(*f.app,source,false);Current(*f.app,shortened,false);
        f.model.m->boneCount=2;f.model.m->morphCount=2;Current(*f.app,source,true);
    }
    for(int slot:{0,19}) {
        auto* pointer=f.app->ModelSlot(slot);plugins::ForgetSceneObject(pointer);MmsObjectRef absent{};
        auto missing=Capture(*f.app);Check(missing->keys.size()==source->keys.size() && !missing->referencesAvailable &&
            !plugins::FindSceneObjectIdentity(pointer,absent),"source and selector-parent capture do not create missing identities");
        for(const auto& key:source->keys)Check(SameRaw(Row(*missing,key.target,key.frame,key.morph),key),
            "missing model identity preserves every raw field");
        Current(*f.app,missing,false);plugins::SceneObjectIdentity(pointer);Current(*f.app,missing,false);
        Current(*f.app,source,false);Current(*f.app,Capture(*f.app),true);
    }
    Check(SameSource(*source,saved),"all source/parent structure and same-pointer identity changes preserve held snapshot bytes");
    f.selectors[0].linkedModel=0;
    Check(nf::RegisterDisplay(f.model.raw(),-60),"register a selector referring to its own source model");
    auto selfParent=Capture(*f.app);
    Check(selfParent->models[0].tracks && !selfParent->models[19].referenced,
        "a source also referenced as parent retains its stronger four-table tracks stamp");
    Structure(*f.app,selfParent,f.model,true);
}

void EmptyAndLifetime() {
    receipt::OwnedFixture f;receipt::DetachParents(f);const copy::Snapshot before(f);const RawClip raw(*f.app);
    auto none=Capture(*f.app);before.Expect(f);raw.Expect(*f.app);
    Check(none->keys.empty() && !none->hadState && none->lifetime.expired() && none->first==0 &&
        none->referencesAvailable,"no State is a successful empty source without creating State");Current(*f.app,none,true);
    Check(!Capture(*f.app)->hadState,"repeated empty captures do not materialize private negative State");
    f.SeedHistory();nf::SelectRange(f.app.get(),1,2);auto empty=Capture(*f.app);
    Check(empty->keys.empty() && empty->hadState && !empty->lifetime.expired() && empty->first==0,
        "empty selection with existing State captures that State lifetime");Current(*f.app,empty,true);
    nf::SelectRange(f.app.get(),-70,-40);auto source=Capture(*f.app);const auto saved=*source;
    nf::Reset(f.app.get());Current(*f.app,source,false);Current(*f.app,empty,false);
    // Existing history may retain the old token. Current must still reject the
    // removed/replaced State, rather than relying only on weak_ptr::expired().
    Check(SameSource(*source,saved),"Reset preserves owned raw source bytes while retiring the State");
    Check(nf::RegisterGlobal(f.app.get(),nf::Global::Camera,-40),"create a new State for the same application");
    Current(*f.app,source,false);auto fresh=Capture(*f.app);Current(*f.app,fresh,true);
    Check(source->lifetime.owner_before(fresh->lifetime) || fresh->lifetime.owner_before(source->lifetime),
        "a recreated State in the same app has a different lifetime identity");
    f.history.Reset();f.history.Collect();
    Check(source->lifetime.expired() && SameSource(*source,saved),"retiring old history releases the old State token, not its copied values");
    const auto freshBytes=*fresh;f.app->AdvanceSceneGeneration();Current(*f.app,fresh,false);
    Check(SameSource(*fresh,freshBytes),"scene generation retirement cannot alter retained source bytes");
    auto after=Capture(*f.app);Check(after->sceneGeneration==f.app->SceneGeneration(),"fresh capture uses the current scene generation");
    Source held;
    {GlobalFixture owner(Mode::AllOperations);held=Capture(*owner.app);}
    Check(held->lifetime.expired() && held->keys.size()==6,"source owns payload but does not keep fixture objects or State alive");
    Current(*f.app,held,false);
}
void WholeModelSelectorAndMappings() {
    ModelFixture f(Mode::AllOperations);
    // Production RebuildBoneOrder uses boneIndex=-1 for selector zero.
    f.selectors[0].boneIndex=-1;
    Check(nf::RegisterDisplay(f.model.raw(),-60),"register the production whole-model selector sentinel");
    nf::SelectRange(f.app.get(),-60,-60);
    auto source=Capture(*f.app);const auto saved=*source;
    const auto& root=Row(*source,-1000000,-60);
    Check(source->keys.size()==3 && root.trackAvailable && root.mappedBone==-1 &&
        root.name==std::array<char,20>{} && root.occurrence==0 && source->referencesAvailable,
        "whole-model selector needs no bone name and remains a complete usable source");
    ObjectStamp(source->models[19],19,f.parent.model.raw());Current(*f.app,source,true);
    f.selectors[0].boneIndex=0;Current(*f.app,source,false);
    f.selectors[0].boneIndex=-1;Current(*f.app,source,true);
    for(int bone:{0,99}) {
        f.chains[0].boneIndex=bone;Current(*f.app,source,false);
        f.chains[0].boneIndex=1;Current(*f.app,source,true);
    }
    Check(SameSource(*source,saved),"in-place IK/selector mapping changes never alter captured metadata");
    const auto parentsRecorded=[&](const Source& stale) {
        Check(stale->keys.size()==3 && !stale->referencesAvailable &&
            SameRaw(Row(*stale,-1000000,-60),root),"stale source keeps the whole raw selector row");
        ObjectStamp(stale->models[19],19,f.parent.model.raw());Current(*f.app,stale,false);
    };
    plugins::ForgetSceneObject(f.model.raw());parentsRecorded(Capture(*f.app));
    plugins::SceneObjectIdentity(f.model.raw());
    std::array<mdl::BoneRecord,2> replacement=f.model.bones;
    f.model.m->boneTable=replacement.data();parentsRecorded(Capture(*f.app));
    f.model.m->boneTable=f.model.bones.data();
    f.model.m->boneOrderTable=nullptr;parentsRecorded(Capture(*f.app));
    f.model.m->boneOrderTable=f.selectors.data();
    f.model.m->boneOrderCount=0;parentsRecorded(Capture(*f.app));
    f.model.m->boneOrderCount=1;Current(*f.app,Capture(*f.app),true);
}
void InvalidModel() {
    receipt::OwnedFixture f;auto previous=Capture(*f.app);f.app->CameraMode()=0;
    for(unsigned slot:{0u,254u,255u}) {
        f.app->SelectedModelSlot()=static_cast<std::uint8_t>(slot);Source out=previous;
        const copy::Snapshot before(f);const RawClip raw(*f.app);AllocationAudit audit;
        allocationAudit=&audit;failAfter=0;const auto code=nf::CaptureCopySource(*f.app,out);
        const auto remaining=failAfter;failAfter=-1;allocationAudit=nullptr;
        Check(code==MMS_E_NOT_READY && !out && remaining==0 && !audit.outstanding && !audit.overflow,
            "missing/out-of-range selected model returns NOT_READY before OOM and clears prepopulated output");
        before.Expect(f);raw.Expect(*f.app);
    }
    Parent valid;f.app->SelectedModelSlot()=0;f.app->ModelSlot(0)=valid.model.raw();
    auto empty=Capture(*f.app);Check(empty->keys.empty() && empty->mode==nf::CopySourceMode::Model && empty->modelSlot==0 &&
        !empty->hadState && empty->referencesAvailable,"valid selected model with no State succeeds as an empty model source");
    ModelStamp(empty->models[0],0,valid.model,true);Current(*f.app,empty,true);f.app->ModelSlot(0)=nullptr;
}

struct Attempt {MmsResult code=MMS_E_FAILED;long spent=0;bool exhausted=false,overflow=false;std::size_t outstanding=0;};
Attempt CaptureAt(MMDApp& app,Source& out,long allowance) {
    Check(failAfter==-1 && !allocationAudit,"capture injection starts outside other audits");
    AllocationAudit audit;Attempt attempt;allocationAudit=&audit;failAfter=allowance;
    attempt.code=nf::CaptureCopySource(app,out);attempt.spent=allowance-failAfter;attempt.exhausted=failAfter==0;
    failAfter=-1;allocationAudit=nullptr;attempt.outstanding=audit.outstanding;attempt.overflow=audit.overflow;return attempt;
}
template<class F,class Live> void AllocationFailures(F& f) {
    const Live before(f);const RawClip raw(*f.app);auto held=Capture(*f.app);const auto saved=*held;
    Source measured;const auto success=CaptureAt(*f.app,measured,allowanceMax);
    Check(success.code==MMS_OK && measured && !success.exhausted && !success.overflow && success.spent>0,
        "measure this fixture's successful capture allocation path");
    Check(SameSource(*held,*measured),"measuring capture preserves the complete source contract");before.Expect(f);raw.Expect(*f.app);
    for(long allowance=0;allowance<success.spent;++allowance) {
        Source out=held;const auto failed=CaptureAt(*f.app,out,allowance);
        Check(failed.code==MMS_E_MEMORY && !out && failed.exhausted && !failed.outstanding && !failed.overflow,
            "every measured capture OOM clears a prepopulated output and releases all partial allocations");
        before.Expect(f);raw.Expect(*f.app);Check(SameSource(*held,saved),"failed capture leaves caller-held source unchanged");
        Current(*f.app,held,true);
    }
    auto recovered=Capture(*f.app);Check(SameSource(*held,*recovered),"capture recovers completely after all measured OOM stages");
    before.Expect(f);raw.Expect(*f.app);
}
void Oom(Mode mode) {
    {GlobalFixture f(mode);AllocationFailures<GlobalFixture,copy::Snapshot>(f);}
    {ModelFixture f(mode);AllocationFailures<ModelFixture,ModelLive>(f);}
    // Empty capture still owns an envelope: its allocation failure must clear
    // an existing caller output without creating State or mutating history.
    receipt::OwnedFixture f;Check(f.history.SetMode(mode)==MMS_OK,"configure empty capture OOM mode");
    AllocationFailures<receipt::OwnedFixture,copy::Snapshot>(f);
}
void StoredClipboardOffsets() {
    GlobalFixture f(Mode::AllOperations);f.Select(false);
    // Empty native counts via the existing fixture route, then select three
    // real negative rows. Every destination remains strictly below zero.
    nf::SelectRange(f.app.get(),1,2);copy::Copy(f);nf::SelectRange(f.app.get(),-150,-90);
    auto original=Capture(*f.app);Check(original->keys.size()==3,"offset fixture selects light, gravity and camera");
    Check(nf::CopySelected(f.app.get())==nf::CopyResult::Copied,"prepare legacy negative-only offset fixture");
    f.app->CurrentFrame()=-500;Check(nf::Command(f.app.get(),421),"paste solely below zero to create stored nonzero offsets");
    auto pasted=Capture(*f.app);Check(pasted->keys.size()==3 && pasted->first==-500,"capture selects the three new negative rows");
    bool nonzero=false;
    for(const auto& key:original->keys) {
        auto expected=key;expected.clipboardOffset=std::int64_t(key.frame)-original->first;
        expected.frame=int(-500+expected.clipboardOffset);nonzero|=expected.clipboardOffset!=0;
        Check(SameKey(Row(*pasted,expected.target,expected.frame),expected),
            "capture copies actual stored clipboardOffset verbatim alongside the signed frame and all values");
    }
    Check(nonzero,"offset coverage includes nonzero stored offsets");Current(*f.app,pasted,true);
    // Keep only camera: first now equals its own frame, while the stored offset
    // remains 60. This detects accidental normalization to source.first.
    nf::SelectRange(f.app.get(),-440,-440);auto single=Capture(*f.app);
    Check(single->keys.size()==1 && single->first==-440 && single->keys[0].clipboardOffset==60,
        "source first and a row's stored legacy offset are independent fields");
}
} // namespace negative_copy_source_checks

void NativeNegativeCopySourceChecks() {
    using namespace negative_copy_source_checks;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){GlobalFields(mode);ModelFields(mode);Oom(mode);}
    StaleAccessories();ParentReferences();ModelReferences();EmptyAndLifetime();InvalidModel();StoredClipboardOffsets();
    WholeModelSelectorAndMappings();
}
} // namespace
