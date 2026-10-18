#pragma once
// Real hidden model editors bound to the production common journal. Include
// after bound_curve_history_checks.hpp; no renderer or source file is needed.
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/bone_curve.hpp"
#include "mikudancestudio/bone_scale.hpp"
#include "mikudancestudio/undo_storage.hpp"
#include <array>

namespace bound_model_curve_history_checks {
namespace p=mikudancestudio::plugins;
namespace bc=mikudancestudio::bone_curve;
namespace mc=mikudancestudio::morph_curve;
namespace shared=bound_curve_history_checks;
struct Model {
    std::unique_ptr<mdl::ModelRecord> record=std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord,2> bones{};
    std::array<mdl::MorphRecord,3> morphs{};
    std::array<mdl::IkChain,1> ik{};
    std::array<unsigned char,2> selected{{1,0}},dirty{};
    std::array<std::uint32_t,2> boneCursors{};
    std::array<unsigned char,2> boneActive{};
    std::array<std::uint32_t,3> morphCursors{};
    std::array<unsigned char,3> morphActive{};
    unsigned char* Raw()const{return reinterpret_cast<unsigned char*>(record.get());}
    explicit Model(unsigned number){
        auto& m=*record;m.boneCount=2;m.boneTable=bones.data();m.boneSelection=selected.data();m.bonePhysicsState=dirty.data();
        m.boneKeyCursors=boneCursors.data();m.boneTrackActive=boneActive.data();m.morphKeyCursors=morphCursors.data();m.morphTrackActive=morphActive.data();
        m.boneKeys=mdl::BoneKeyPoolCreate(8);m.morphKeys=mdl::MorphKeyPoolCreate(8);m.displayKeys=mdl::KeyPoolCreate<mdl::DisplayKey>(4);
        m.morphCount=3;m.morphs=morphs.data();m.ikChainCount=1;m.ikChains=ik.data();m.selectedBone=0;m.maxFrame=20;m.loadComplete=1;
        m.physicsMode=2;for(auto& index:m.selectedMorphs)index=-1;m.selectedMorphs[1]=0;
        std::snprintf(m.name,sizeof(m.name),"bound-model-%u",number);
        wchar_t temp[MAX_PATH]{};Check(GetTempPathW(MAX_PATH,temp)>0,"model-history fixture has a local temporary path");
        std::swprintf(m.path,sizeof(m.path)/sizeof(*m.path),L"%lsMMS-ABI-offline-%lu-%u.pmx",temp,static_cast<unsigned long>(GetCurrentProcessId()),number);
        Check(GetFileAttributesW(m.path)==INVALID_FILE_ATTRIBUTES,"model-history source file is absent before all edits");
        ik[0].boneIndex=0;ik[0].targetBone=1;ik[0].enabled=1;
        for(unsigned i=0;i<2;++i){auto& bone=bones[i];bone.parent=bone.slotIndex=bone.tailIdx=-1;bone.type=mdl::BoneType::Move;bone.rotQuat[3]=1;
            std::snprintf(bone.name,sizeof(bone.name),"bone%u",i);
            for(int cell:{0,5,10,15})bone.matInit[cell]=bone.matWorld[cell]=1;
            auto& first=m.boneKeys[i];auto& last=m.boneKeys[2+i];first.rotation[3]=last.rotation[3]=1;first.next=2+i;last.previous=i;last.frame=20;
            first.allocated=last.allocated=i==0?1:0;last.position[0]=i==0?10.f:-2.f;last.position[1]=i==0?4.f:7.f;
            for(unsigned lane=0;lane<4;++lane){last.interpolation[lane]=20;last.interpolation[4+lane]=10;last.interpolation[8+lane]=110;last.interpolation[12+lane]=100;}
        }
        for(unsigned i=0;i<3;++i){auto& morph=morphs[i];morph.type=1;morph.panel=i==2?mdl::MorphPanel::system:mdl::MorphPanel::eye;
            std::snprintf(morph.name,sizeof(morph.name),"face%u",i);m.morphKeys[i].value=.1f*float(i);morph.value=m.morphKeys[i].value;
            if(i<2){auto& last=m.morphKeys[3+i];m.morphKeys[i].next=3+i;last.previous=i;last.frame=20;last.value=i==0?.8f:.4f;}
        }
        m.morphKeys[2].allocated=7; // Hidden system-morph view selection is unrelated to every edited source.
        for(unsigned i=0;i<4;++i){auto& key=m.displayKeys[i];key.ikStates=new unsigned char[1]{1};key.selectorStates=new mdl::BoneReference[1]{{-1,0}};key.visible=1;}
        m.displayKeys[0].next=1;m.displayKeys[1].previous=0;m.displayKeys[1].frame=20;
    }
    ~Model(){
        bc::Forget(Raw());mc::Forget(Raw());bone_scale::Forget(Raw());
        for(auto& ring:record->undoRings)for(auto& entry:ring.slots){undo_storage::Free(entry.bonePose);undo_storage::Free(entry.auxiliaryPose);}
        for(std::size_t i=0;i<mdl::KeyPoolCapacity(record->displayKeys);++i){delete[] record->displayKeys[i].ikStates;delete[] record->displayKeys[i].selectorStates;}
        mdl::BoneKeyPoolDestroy(record->boneKeys);mdl::MorphKeyPoolDestroy(record->morphKeys);mdl::KeyPoolDestroy(record->displayKeys);
    }
};
struct Fixture:shared::Fixture {
    Model first{0},second{1};MMDApp* previous=g_Block;
    explicit Fixture(p::SharedHistory::RecordingMode mode=p::SharedHistory::RecordingMode::AllOperations):shared::Fixture(mode){
        ce::CloseOwned(app->MainWindow());editor=nullptr;app->CameraMode()=0;app->ModelSlot(0)=first.Raw();app->ModelSlot(1)=second.Raw();app->SelectedModelSlot()=0;app->LastRegisteredFrame()=20;
        first.record->hwnd=second.record->hwnd=app->MainWindow();p::SceneObjectIdentity(first.Raw());p::SceneObjectIdentity(second.Raw());g_Block=app.get();
        const int ids[]={panel::kReadoutPosYEdit,panel::kReadoutPosZEdit,panel::kReadoutRotXEdit,panel::kReadoutRotYEdit,panel::kReadoutRotZEdit,
            panel::kReadoutScaleXEdit,panel::kReadoutScaleYEdit,panel::kReadoutScaleZEdit,panel::kReadoutDistEdit};
        for(int id:ids)if(!GetDlgItem(app->MainWindow(),id))Check(CreateWindowW(L"EDIT",L"0",WS_CHILD,0,0,100,20,app->MainWindow(),reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr)!=nullptr,
            "model-history owner has real native bone readout edits");
        for(int id:{panel::kInterpCurveCombo,panel::kPhysicsFrameCheckbox,panel::kPhysicsCheckbox})if(!GetDlgItem(app->MainWindow(),id)){
            const bool combo=id==panel::kInterpCurveCombo;const auto control=CreateWindowW(combo?L"COMBOBOX":L"BUTTON",L"0",WS_CHILD|(combo?CBS_DROPDOWNLIST:BS_CHECKBOX),
                0,0,100,20,app->MainWindow(),reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
            Check(control!=nullptr,"model-history owner has real native interpolation controls");
            if(combo){SendMessageW(control,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"X"));SendMessageW(control,CB_SETCURSEL,0,0);}
        }
    }
    ~Fixture(){
        ce::CloseOwned(app->MainWindow());editor=nullptr;Check(history.Reset()==MMS_OK,"model-history journal releases retained models before fixture destruction");history.Collect();
        p::ForgetSceneObject(first.Raw());p::ForgetSceneObject(second.Raw());app->ModelSlot(0)=app->ModelSlot(1)=nullptr;g_Block=previous;
    }
    Model& At(unsigned slot){return slot?second:first;}
    void FindEditor(){
        editor=nullptr;EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM context)->BOOL{auto& f=*reinterpret_cast<Fixture*>(context);wchar_t name[96]{};
            GetClassNameW(window,name,96);if(GetWindow(window,GW_OWNER)==f.app->MainWindow()&&!std::wcscmp(name,L"MikuDanceStudioCurveEditorWindow"))f.editor=window;return TRUE;},reinterpret_cast<LPARAM>(this));
        Check(editor&&GetDlgItem(editor,shared::canvasId),"real bound model curve window opens on the production host");
        std::uint64_t state[5]{};Check(SendMessageW(editor,WM_APP+397,0,reinterpret_cast<LPARAM>(state))&&state[0]==0&&state[1]==0&&state[3],
            "model editor uses the real external journal and retains no local undo stack");
    }
    void Morph(unsigned slot=0,unsigned lane=0){ce::CloseOwned(app->MainWindow());app->SelectedModelSlot()=static_cast<unsigned char>(slot);curve_host::OpenMorph(app.get(),1,false);FindEditor();Select(0,int(lane));}
    void Bones(unsigned slot=0,int source=0,int lane=0){ce::CloseOwned(app->MainWindow());app->SelectedModelSlot()=static_cast<unsigned char>(slot);curve_host::OpenInterpolation(app.get(),false);FindEditor();Select(source,lane);}
    void Seek(int frame){SetDlgItemTextW(editor,1024,std::to_wstring(frame).c_str());Click(1025);Check(app->CurrentFrame()==frame,"bound model editor shares the real current-frame transport");}
    void Value(int frame,double value){Add(frame,value);SetDlgItemTextW(editor,shared::valueField,std::to_wstring(value).c_str());SetFocus(GetDlgItem(editor,shared::valueField));
        Click(shared::applyButton);Check(IsWindowEnabled(editor)&&GetFocus()==GetDlgItem(editor,shared::valueField),"bound model field publication keeps text focus and window enabled");}
    void Controls(const std::array<unsigned,4>& values){for(unsigned i=0;i<4;++i)SetDlgItemTextW(editor,1096+int(i),std::to_wstring(values[i]).c_str());
        const auto input=GetDlgItem(editor,1096);SetFocus(input);SendMessageW(input,WM_KEYDOWN,VK_RETURN,1);
        Check(IsWindowEnabled(editor)&&GetFocus()==input,"bound native handle publication keeps its coordinate-field focus");}
    void CancelDrag(std::size_t key){const auto point=SendMessageW(editor,WM_APP+300,key,0);const auto canvas=GetDlgItem(editor,shared::canvasId);
        SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,point);SendMessageW(canvas,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(LOWORD(point)+18,HIWORD(point)-10));SendMessageW(canvas,WM_KEYDOWN,VK_ESCAPE,1);
        Check(!GetCapture(),"cancelled model-curve preview releases real mouse capture");}
};
template<class Key>std::vector<Key> Rows(const Key* pool,std::size_t capacity,unsigned heads){std::vector<Key> result;
    for(unsigned root=0;root<heads;++root){unsigned index=root;for(std::size_t hops=0;;++hops){Check(index<capacity&&hops<capacity,"model checkpoint has bounded native key chains");
        auto key=pool[index];key.allocated=0;result.push_back(key);if(!key.next)break;index=key.next;}}return result;}
struct Checkpoint {
    struct Stored {
        std::vector<mdl::BoneKey> bones;std::vector<mdl::MorphKey> morphs;
        struct Display {std::uint32_t frame,previous,next;std::uint8_t visible;std::array<std::uint8_t,3> reserved0,reserved1;std::uint8_t ik;mdl::BoneReference selector;};
        std::vector<Display> display;std::unique_ptr<bc::AuthoringState> boneAuthors;std::vector<std::uint8_t> morphAuthors;std::uint32_t maxFrame=0;
        explicit Stored(Model& model){const auto& m=*model.record;bones=Rows(m.boneKeys,mdl::BoneKeyCapacity(m.boneKeys),2);morphs=Rows(m.morphKeys,mdl::MorphKeyCapacity(m.morphKeys),3);
            unsigned index=0;for(std::size_t hops=0;;++hops){Check(index<mdl::KeyPoolCapacity(m.displayKeys)&&hops<mdl::KeyPoolCapacity(m.displayKeys),"model checkpoint has a bounded display chain");
                const auto& key=m.displayKeys[index];Display copy{key.frame,key.previous,key.next,key.visible,{},{},key.ikStates[0],key.selectorStates[0]};
                std::copy(std::begin(key.reserved0),std::end(key.reserved0),copy.reserved0.begin());std::copy(std::begin(key.reserved1),std::end(key.reserved1),copy.reserved1.begin());display.push_back(copy);
                if(!key.next)break;index=key.next;}
            boneAuthors=bc::AuthoringState::Capture(model.Raw());mc::AuthoredLayers layers;std::vector<ac::NamedTrack> tracks;std::string error;
            Check(mc::CaptureLayers(model.Raw(),layers,error)&&mc::LayersTracks(layers,tracks,error)&&ac::Encode(tracks,morphAuthors,error),"capture original morph authors without altering native data");maxFrame=m.maxFrame;
        }
        void Match(Model& model)const{const Stored actual(model);const auto exact=[](const auto& a,const auto& b){return a.size()==b.size()&&(a.empty()||!std::memcmp(a.data(),b.data(),a.size()*sizeof(a[0])));};
            const bool raw=exact(bones,actual.bones)&&exact(morphs,actual.morphs)&&maxFrame==actual.maxFrame;
            if(!raw)std::fprintf(stderr,"DIAG model=%s native bone rows %zu/%zu morph rows %zu/%zu maxFrame %u/%u (expected/actual)\n",model.record->name,
                bones.size(),actual.bones.size(),morphs.size(),actual.morphs.size(),unsigned(maxFrame),unsigned(actual.maxFrame));
            Check(raw,"common model history restores exact native bone/morph records and maximum frame apart from view selection");
            const bool boneExact=boneAuthors->Equals(*actual.boneAuthors),morphExact=morphAuthors==actual.morphAuthors;
            if(!boneExact||!morphExact)std::fprintf(stderr,"DIAG model=%s bone-authors-equal=%d morph-authors-equal=%d morph-author-bytes %zu/%zu\n",model.record->name,int(boneExact),int(morphExact),morphAuthors.size(),actual.morphAuthors.size());
            Check(boneExact&&morphExact,"common model history restores original bone and morph author data");
            Check(display.size()==actual.display.size(),"common model history preserves display frame count");
            for(std::size_t i=0;i<display.size();++i){const auto& a=display[i];const auto& b=actual.display[i];Check(a.frame==b.frame&&a.previous==b.previous&&a.next==b.next&&a.visible==b.visible&&
                a.reserved0==b.reserved0&&a.reserved1==b.reserved1&&a.ik==b.ik&&a.selector.modelIndex==b.selector.modelIndex&&a.selector.boneIndex==b.selector.boneIndex,
                "common model history restores display/IK values and keeps original selector payload");}
        }
    };
    Stored first,second;pmm_extension::Document document;std::uint32_t last;int plugin;
    explicit Checkpoint(Fixture& f):first(f.first),second(f.second),last(f.app->LastRegisteredFrame()),plugin(f.plugin.value){bc::CapturePmm(*f.app,document);mc::CapturePmm(*f.app,document);}
    void Match(Fixture& f)const{first.Match(f.first);second.Match(f.second);pmm_extension::Document current;bc::CapturePmm(*f.app,current);mc::CapturePmm(*f.app,current);
        Check(document.records.size()==current.records.size()&&last==f.app->LastRegisteredFrame()&&plugin==f.plugin.value,"model and plugin history restore one common checkpoint and frame horizon");
        for(std::size_t i=0;i<document.records.size();++i)Check(document.records[i].key==current.records[i].key&&document.records[i].value==current.records[i].value,"model-history PMM author records return byte for byte");}
};
bool MorphHas(Model& model,int morph,int frame){ac::Track track;std::string error;Check(mc::Read(model.Raw(),morph,track,error),"read real model expression curve");return std::any_of(track.keys.begin(),track.keys.end(),[&](const auto& key){return key.frame==frame;});}
void CommonChronology(){for(auto mode:{p::SharedHistory::RecordingMode::AllOperations,p::SharedHistory::RecordingMode::ExplicitOnly}){
    Fixture f(mode);f.stage="BoundModel/CommonChronology";std::vector<std::unique_ptr<Checkpoint>> states;states.push_back(std::make_unique<Checkpoint>(f));
    const auto save=[&](){states.push_back(std::make_unique<Checkpoint>(f));Check(f.history.UndoCount()==states.size()-1,"each actual model operation adds exactly one common node in both recording modes");};
    f.Morph();Check(TreeView_GetCount(GetDlgItem(f.editor,shared::channelTree))>=2,"real morph editor has user expression channels");f.Add(10,.65);Check(MorphHas(f.first,0,10),"real bound morph Add writes its native mirror");save();
    f.Add(10,.65);Check(f.history.UndoCount()==1,"reselecting an existing real morph key records no additional history");
    f.PluginEdit(7);save();
    f.Bones(0,0,0);f.Add(20,10);f.Controls({25,35,100,110});Check(f.first.record->boneKeys[2].interpolation[0]==25&&f.first.record->boneKeys[2].interpolation[4]==35,
        "real native bone editor changes raw MMD interpolation controls");save();
    f.Select(1,0);f.Add(10,5.5);Check(bc::Has(f.first.Raw(),0),"real free-curve source installs original bone author data");save();
    f.Select(1,9);f.Value(0,0);Check(f.first.record->displayKeys[0].visible==0&&f.first.record->displayKeys[0].ikStates[0]==1,"real discrete visibility edit preserves IK");save();
    f.Select(1,10);f.Value(20,0);Check(f.first.record->displayKeys[1].ikStates[0]==0&&f.first.record->displayKeys[0].ikStates[0]==1,"real discrete IK edit owns only its addressed frame");save();
    f.Select(1,11);f.Add(14,-4);Check(bc::Has(f.first.Raw(),1),"same free-curve source edits a different real bone");save();
    f.Morph(1);f.Add(12,.22);Check(MorphHas(f.second,0,12),"second model expression is edited through its real source");save();
    const auto count=states.size()-1;const auto selectedFirst=f.first.selected,selectedSecond=f.second.selected;
    for(std::size_t step=0;step<count;++step){f.Seek(3+int(step)*4);const auto frame=f.app->CurrentFrame();f.second.bones[1].trans[2]=101.25f+float(step);f.first.morphs[2].value=.9375f;
        f.History(false);if(f.history.UndoCount()!=count-step-1)f.DiagnoseHistory();Check(f.history.UndoCount()==count-step-1&&f.history.RedoCount()==step+1,"real model editor Ctrl Z traverses bone, morph, display/IK and plugin nodes in order");
        states[count-step-1]->Match(f);Check(f.app->CurrentFrame()==frame&&f.app->SelectedModelSlot()==1&&f.second.bones[1].trans[2]==101.25f+float(step)&&f.first.morphs[2].value==.9375f&&
            f.first.selected==selectedFirst&&f.second.selected==selectedSecond&&f.first.record->morphKeys[2].allocated==7&&f.second.record->morphKeys[2].allocated==7,
            "model undo preserves playhead, selected model/bones, hidden-morph selection and unrelated unregistered poses");}
    for(std::size_t step=0;step<count;++step){f.Seek(5+int(step)*3);const auto frame=f.app->CurrentFrame();f.second.bones[1].trans[2]=201.25f+float(step);f.first.morphs[2].value=.8125f;
        f.History(true);if(f.history.UndoCount()!=step+1)f.DiagnoseHistory();Check(f.history.UndoCount()==step+1&&f.history.RedoCount()==count-step-1,"real model editor Ctrl Y replays all model and plugin nodes in order");states[step+1]->Match(f);
        Check(f.app->CurrentFrame()==frame&&f.app->SelectedModelSlot()==1&&f.second.bones[1].trans[2]==201.25f+float(step)&&f.first.morphs[2].value==.8125f,
            "model redo samples owned data without changing time or unrelated unregistered poses");}
    for(auto* model:{&f.first,&f.second})Check(GetFileAttributesW(model->record->path)==INVALID_FILE_ATTRIBUTES,"model undo/redo completes while both original source files remain offline");
}}
void CancellationBudgetAndGuards(){
    Fixture f;f.stage="BoundModel/CancellationBudgetAndGuards";f.Morph();f.Add(10,.6);Checkpoint morph(f);const auto count=f.history.UndoCount();f.CancelDrag(1);morph.Match(f);
    Check(f.history.UndoCount()==count&&f.history.RedoCount()==0,"cancelled real morph preview has no common history entry");
    f.Bones(0,0,0);Checkpoint native(f);f.Add(11,2);native.Match(f);Check(f.history.UndoCount()==count,"native interpolation source rejects added frame/value data before any history or native write");
    f.Add(20,10);f.Controls({0,35,100,110});native.Match(f);Check(f.history.UndoCount()==count,"invalid real native control input has no partial write or history");
    f.Bones(0,1,0);f.Add(10,3.5);Checkpoint bone(f);const auto boneCount=f.history.UndoCount();f.CancelDrag(1);bone.Match(f);
    Check(f.history.UndoCount()==boneCount,"cancelled real free-bone preview restores all authored and native data");
    Check(f.history.Configure(f.history.Mode(),128,1)==MMS_OK,"set a deliberately insufficient model-history budget");Checkpoint limited(f);const auto undo=f.history.UndoCount(),redo=f.history.RedoCount();
    f.Add(15,6);limited.Match(f);Check(f.history.UndoCount()==undo&&f.history.RedoCount()==redo,"real free-bone write refuses budget without partial native/author changes");
    f.Morph();f.Add(15,.3);limited.Match(f);Check(!MorphHas(f.first,0,15)&&f.history.UndoCount()==undo&&f.history.RedoCount()==redo,"real morph write refuses budget without changing source or history");
    // Start a fresh journal to isolate lifetime admission from budget refusal.
    Check(f.history.Configure(f.history.Mode(),128,64ull*1024*1024)==MMS_OK,"restore ordinary history budget");f.Add(16,.45);const auto before=f.history.UndoCount();Checkpoint beforeSwitch(f);
    f.app->SelectedModelSlot()=1;SetDlgItemTextW(f.editor,shared::frameField,L"17");SetDlgItemTextW(f.editor,shared::valueField,L"0.8");f.Click(shared::addButton);
    Check(f.history.UndoCount()==before&&!IsWindowEnabled(GetDlgItem(f.editor,shared::addButton)),"stale selected-model source becomes read-only instead of redirecting an edit");beforeSwitch.Match(f);
    f.app->CurrentFrame()=27;Check(p::RunBoundObjectHistory(*f.app,f.history,false)==MMS_OK&&f.history.UndoCount()+1==before&&f.app->CurrentFrame()==27&&f.app->SelectedModelSlot()==1,
        "the real main common-history entry still undoes a model edit while its old editor is read-only");
}
void MalformedMorphCollections(){
    Fixture f;f.stage="BoundModel/MalformedMorphCollections";f.Morph();std::vector<ce::Channel> baseline;std::wstring error;
    Check(curve_host::ReadMorphChannels(*f.app,baseline,error)&&baseline.size()==2,"bound morph source exposes exactly its two user channels and omits the system channel");
    const auto identity=curve_host::MorphIdentityOf(*f.app,f.first.Raw());const Checkpoint before(f);
    for(int malformed=0;malformed<4;++malformed){auto target=baseline;target[0].track.keys.back().value=.7;
        if(malformed==0)target.pop_back();
        if(malformed==1){ce::Channel hidden;hidden.id=mc::StableId(2,f.first.morphs[2].name);std::string detail;
            Check(mc::ReadNative(f.first.Raw(),2,hidden.track,detail),"read the hidden native channel solely to construct a rejected malformed batch");target.push_back(std::move(hidden));}
        if(malformed==2)target[1].track.discrete=true;
        if(malformed==3)target[1].nativeControls.push_back({20,20,107,107});
        error.clear();Check(!curve_host::WriteMorphChannels(*f.app,identity,target,baseline,error)&&!error.empty(),
            "production morph source rejects an incomplete, hidden, discrete or raw-control peer before publishing the otherwise valid edit");
        Check(!f.history.UndoCount()&&!f.history.RedoCount(),"malformed morph collection never commits a partial common-history node");before.Match(f);
    }
}
void CurrentDisplayState(){
    Fixture f;f.stage="BoundModel/CurrentDisplayState";f.Bones(0,0,10);f.Seek(20);f.first.bones[1].trans[2]=91.25f;
    f.Value(20,0);Check(f.history.UndoCount()==1&&f.first.record->displayKeys[1].ikStates[0]==0&&f.first.ik[0].enabled==0&&f.app->CurrentFrame()==20&&f.first.bones[1].trans[2]==91.25f,
        "actual display-only IK edit immediately publishes the state at the current frame without re-seeking unregistered bones");
    f.History(false);Check(f.history.UndoCount()==0&&f.first.ik[0].enabled==1&&f.app->CurrentFrame()==20,"display-only IK undo immediately restores current chain enable state");
    f.Seek(0);f.History(true);Check(f.history.UndoCount()==1&&f.first.record->displayKeys[1].ikStates[0]==0&&f.first.ik[0].enabled==1&&f.app->CurrentFrame()==0,
        "IK redo at another frame samples that frame instead of restoring the original playback pose");
    f.Select(0,9);f.Value(0,0);Check(f.history.UndoCount()==2&&f.first.record->displayKeys[0].visible==0&&f.first.record->loadComplete==0,
        "actual display-only visibility edit immediately updates the current visible state");
    f.Seek(19);f.History(false);Check(f.history.UndoCount()==1&&f.first.record->loadComplete==1&&f.app->CurrentFrame()==19,"visibility undo samples the restored discrete state at a new frame");
    f.Seek(25);f.History(true);Check(f.history.UndoCount()==2&&f.first.record->displayKeys[0].visible==0&&f.first.record->loadComplete==1&&f.app->CurrentFrame()==25,
        "visibility redo preserves its earlier keyed value while displaying the later frame's state");
    // This verifies native display/IK publication. The fixture has no CCD
    // links or renderer; it deliberately does not claim viewport IK solving.
}
void Run(){CommonChronology();CancellationBudgetAndGuards();MalformedMorphCollections();CurrentDisplayState();}
} // namespace bound_model_curve_history_checks
