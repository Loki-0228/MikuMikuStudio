// Uses the actual command 500 and the real edit-probe DLL journal. A private
// native pool isolates authored setup from the previously loaded PMD/PMX data.
namespace bone_curve_sync_host_checks {
using Bytes=std::vector<std::uint8_t>;
bone_curve::Tracks Read(unsigned char* raw){bone_curve::Tracks out;
    Check(bone_curve::Read(raw,0,out),"real controlled bone keeps its free authoring readable");return out;}
Bytes Encode(const bone_curve::Tracks& tracks){std::vector<animation_curve::NamedTrack> named;
    for(unsigned lane=0;lane<tracks.size();++lane)named.push_back({std::to_string(lane),tracks[lane]});
    Bytes bytes;std::string error;Check(animation_curve::Encode(named,bytes,error),"encode exact real sparse bone values and handles");return bytes;}
bool Has(const animation_curve::Track& track,int frame){return std::any_of(track.keys.begin(),track.keys.end(),[&](const auto& key){return key.frame==frame;});}
unsigned At(const mdl::ModelRecord& model,unsigned frame){unsigned slot=0;
    for(std::size_t visited=0;visited<mdl::BoneKeyCapacity(model.boneKeys);++visited){const auto& key=model.boneKeys[slot];
        if(key.frame==frame)return slot;if(!key.next)break;slot=key.next;}
    Check(false,"real sparse bone fixture contains its union native frame");return 0;}
struct Original {
    MMDApp& app;plugins::SharedHistory& history;unsigned char* raw;mdl::ModelRecord& model;
    mdl::BoneKey* pool;const std::uint64_t identity;std::vector<mdl::BoneRecord> bones;
    Bytes selection,dirty,active,morphMarks,displayMarks;std::vector<std::uint32_t> cursors;
    std::unique_ptr<bone_curve::AuthoringState> curves;bone_scale::Snapshot scales;
    const int frame,last,camera,selected,search,selectedBone,modified,physicsReset,recorded,messageSeen;
    const unsigned maximum;const plugins::SharedHistory::RecordingMode mode;
    const LRESULT automaticCheckbox;std::array<std::uint32_t,2> undo;
    Original(MMDApp& a,plugins::SharedHistory& h):app(a),history(h),raw(a.ModelSlot(0)),model(*mdl::Mdl(raw)),pool(model.boneKeys),
        identity(mdl::BoneKeyPoolIdentity(pool)),bones(model.boneTable,model.boneTable+model.boneCount),
        selection(model.boneSelection,model.boneSelection+model.boneCount),dirty(model.bonePhysicsState,model.bonePhysicsState+model.boneCount),
        curves(bone_curve::AuthoringState::Capture(raw)),scales(bone_scale::Capture(raw)),frame(a.CurrentFrame()),last(a.LastRegisteredFrame()),
        camera(a.CameraMode()),selected(a.SelectedModelSlot()),search(model.searchCursor),selectedBone(model.selectedBone),modified(a.SceneModified()),
        physicsReset(a.PhysicsResetPending()),recorded(a.RecordedFrameCountByte1()),messageSeen(a.MessageSeen()),maximum(model.maxFrame),mode(h.Mode()),
        automaticCheckbox(SendDlgItemMessageW(a.MainWindow(),panel::kPhysicsFrameCheckbox,BM_GETCHECK,0,0)),undo{model.undoState[0],model.undoState[1]}{
        if(model.boneKeyCursors)cursors.assign(model.boneKeyCursors,model.boneKeyCursors+model.boneCount);
        if(model.boneTrackActive)active.assign(model.boneTrackActive,model.boneTrackActive+model.boneCount);
        for(std::size_t i=0;i<mdl::MorphKeyCapacity(model.morphKeys);++i)morphMarks.push_back(model.morphKeys[i].allocated);
        for(std::size_t i=0;i<mdl::KeyPoolCapacity(model.displayKeys);++i)displayMarks.push_back(model.displayKeys[i].allocated);
        auto* temporary=mdl::BoneKeyPoolCreate(mdl::BoneKeyCapacity(pool));
        std::memcpy(temporary,pool,mdl::BoneKeyCapacity(pool)*sizeof(*pool));model.boneKeys=temporary;
        Check(mdl::BoneKeyPoolIdentity(temporary)!=0,"private real bone fixture has the owned bone-pool identity required for controlled edits");
    }
    ~Original(){
        Check(history.Reset()==MMS_OK,"release real bone free-curve records before restoring their native pool");history.Collect();
        auto* temporary=model.boneKeys;model.boneKeys=pool;mdl::BoneKeyPoolDestroy(temporary);
        std::memcpy(model.boneTable,bones.data(),bones.size()*sizeof(bones[0]));
        std::memcpy(model.boneSelection,selection.data(),selection.size());std::memcpy(model.bonePhysicsState,dirty.data(),dirty.size());
        if(!cursors.empty())std::memcpy(model.boneKeyCursors,cursors.data(),cursors.size()*sizeof(cursors[0]));
        if(!active.empty())std::memcpy(model.boneTrackActive,active.data(),active.size());
        for(std::size_t i=0;i<morphMarks.size();++i)model.morphKeys[i].allocated=morphMarks[i];
        for(std::size_t i=0;i<displayMarks.size();++i)model.displayKeys[i].allocated=displayMarks[i];
        Check(bone_scale::Restore(raw,std::move(scales)),"restore real model's original scale keys, pose and curves");
        auto restore=bone_curve::AuthoringRestore::Prepare(raw,*curves);
        Check(restore&&restore->Publish(),"restore real model's original free authoring without touching its old undo rings");
        model.searchCursor=search;model.maxFrame=maximum;model.selectedBone=selectedBone;
        app.CurrentFrame()=frame;app.LastRegisteredFrame()=last;app.CameraMode()=static_cast<std::uint8_t>(camera);app.SelectedModelSlot()=static_cast<std::uint8_t>(selected);
        app.SceneModified()=modified;app.PhysicsResetPending()=physicsReset;app.RecordedFrameCountByte1()=recorded;app.MessageSeen()=messageSeen;
        SendDlgItemMessageW(app.MainWindow(),panel::kPhysicsFrameCheckbox,BM_SETCHECK,automaticCheckbox,0);
        Check(history.SetMode(mode)==MMS_OK&&model.boneKeys==pool&&mdl::BoneKeyPoolIdentity(pool)==identity&&
            model.undoState[0]==undo[0]&&model.undoState[1]==undo[1]&&bone_curve::AuthoringState::Capture(raw)->Equals(*curves),
            "real bone fixture restores original native identity, authored curves, legacy ring indices and recording mode");
    }
};
void Build(MMDApp& app){auto* raw=app.ModelSlot(0);auto& model=*mdl::Mdl(raw);auto& bone=model.boneTable[0];
    bone.type=mdl::BoneType::RotateMove;bone.hasRigidBody=0;bone.physicsDisabled=1;
    bone_curve::Tracks tracks;
    for(unsigned lane=0;lane<tracks.size();++lane)for(int frame:{0,12,31}){
        if(!lane&&frame==12)continue;animation_curve::Key key;key.frame=frame;
        key.value=lane>=6?1:lane==0?frame*.05:lane==4?frame*27.0+.000012:0;
        key.tangent=animation_curve::Tangent::Free;key.inWeight=.31;key.outWeight=.19;
        key.inSlope=key.outSlope=lane==0?.013:0;key.interpolation=lane==1?animation_curve::Interpolation::Step:animation_curve::Interpolation::Bezier;
        tracks[lane].keys.push_back(key);}
    std::string error;Check(bone_curve::Write(raw,0,tracks,error),"author real sparse bone channels with winding, Free handles and Step interpolation");
    std::memset(model.boneSelection,0,model.boneCount);model.boneSelection[0]=1;model.selectedBone=0;model.bonePhysicsState[0]=0;
    for(std::size_t i=0;i<mdl::BoneKeyCapacity(model.boneKeys);++i)model.boneKeys[i].allocated=0;
    for(std::size_t i=0;i<mdl::MorphKeyCapacity(model.morphKeys);++i)model.morphKeys[i].allocated=0;
    for(std::size_t i=0;i<mdl::KeyPoolCapacity(model.displayKeys);++i)model.displayKeys[i].allocated=0;
    const auto slot=At(model,12);const auto& key=model.boneKeys[slot];
    std::memcpy(bone.trans,key.position,sizeof(key.position));std::memcpy(bone.rotQuat,key.rotation,sizeof(key.rotation));
    Check(bone_scale::SetPose(raw,0,bone_scale::Key(raw,int(slot))),"set real author pose to the exact unchanged sparse native mirror");
    const auto read=Read(raw);Check(!Has(read[0],12)&&Has(read[1],12),"real bone union key exists while one authored lane remains sparse");
}
bool NativeSame(const mdl::ModelRecord& model,const std::vector<mdl::BoneKey>& expected,bool marks){
    if(mdl::BoneKeyCapacity(model.boneKeys)!=expected.size()){
        std::printf("DIAG bone native capacity actual=%zu expected=%zu\n",mdl::BoneKeyCapacity(model.boneKeys),expected.size());return false;}
    for(std::size_t i=0;i<expected.size();++i){auto a=model.boneKeys[i],b=expected[i];if(!marks)a.allocated=b.allocated=0;
        if(std::memcmp(&a,&b,sizeof(a))){std::size_t byte=0;const auto* actual=reinterpret_cast<const unsigned char*>(&a);const auto* source=reinterpret_cast<const unsigned char*>(&b);
            while(byte<sizeof(a)&&actual[byte]==source[byte])++byte;
            std::printf("DIAG bone native slot=%zu firstByte=%zu actualByte=%u expectedByte=%u frame=%u/%u prev=%u/%u next=%u/%u physics=%u/%u marks=%u/%u\n",
                i,byte,unsigned(actual[byte]),unsigned(source[byte]),a.frame,b.frame,a.previous,b.previous,a.next,b.next,unsigned(a.physicsDisabled),unsigned(b.physicsDisabled),unsigned(a.allocated),unsigned(b.allocated));
            for(unsigned axis=0;axis<3;++axis)std::printf("DIAG bone native pos%u %.9g/%.9g\n",axis,double(a.position[axis]),double(b.position[axis]));
            for(unsigned axis=0;axis<4;++axis)std::printf("DIAG bone native rot%u %.9g/%.9g\n",axis,double(a.rotation[axis]),double(b.rotation[axis]));
            return false;}}return true;
}
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()){
    using edit_history_host_checks::Menu;
    Check(app&&app->ModelSlot(0)&&!history.UndoCount()&&!history.RedoCount(),"real sparse bone starts with a loaded model and empty common history");
    const auto initial=value();Original original(*app,history);auto* raw=app->ModelSlot(0);auto& model=*mdl::Mdl(raw);
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        Build(*app);Check(history.SetMode(mode)==MMS_OK,"configure real explicit bone registration recording mode");
        app->CameraMode()=0;app->SelectedModelSlot()=0;app->CurrentFrame()=12;
        SendDlgItemMessageW(app->MainWindow(),panel::kPhysicsFrameCheckbox,BM_SETCHECK,BST_UNCHECKED,0);
        const auto before=Encode(Read(raw));const auto source=bone_curve::AuthoringState::Capture(raw);
        const std::vector<mdl::BoneKey> native(model.boneKeys,model.boneKeys+mdl::BoneKeyCapacity(model.boneKeys));
        const auto sourceScale=bone_scale::AuthoringState::Capture(raw);
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe prepared");SendMessageW(app->MainWindow(),WM_COMMAND,500,0);
        Check(history.UndoCount()==1&&Encode(Read(raw))==before&&NativeSame(model,native,true),"pending plugin transaction rejects real sparse bone registration atomically");
        Menu(app,L"Edit probe cancel");SendMessageW(app->MainWindow(),WM_COMMAND,500,0);
        const auto tracks=Read(raw);const auto after=Encode(tracks);const auto target=bone_curve::AuthoringState::Capture(raw);
        const bool allRegistered=std::all_of(tracks.begin(),tracks.end(),[](const auto& track){return Has(track,12);});
        const bool nativeSame=NativeSame(model,native,false);
        const bool registered=allRegistered&&after!=before&&nativeSame&&
            tracks[4].keys.back().value==837.000012&&history.UndoCount()==2&&value()==initial+1&&app->CurrentFrame()==12;
        if(!registered){
            std::printf("DIAG bone register mode=%u allRegistered=%u authorChanged=%u nativeSame=%u winding=%.17g expected=%.17g undo=%zu value=%d expectedValue=%d frame=%d\n",
                unsigned(mode),unsigned(allRegistered),unsigned(after!=before),unsigned(nativeSame),tracks[4].keys.back().value,837.000012,history.UndoCount(),value(),initial+1,app->CurrentFrame());
            for(unsigned lane=0;lane<tracks.size();++lane)std::printf("DIAG bone register lane=%u has12=%u count=%zu\n",lane,unsigned(Has(tracks[lane],12)),tracks[lane].keys.size());
            std::fflush(stdout);
        }
        Check(registered,
            "actual command 500 explicitly fills sparse free lanes while native values and distant Euler winding remain unchanged");
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");app->CameraMode()=1;app->SelectedModelSlot()=1;app->CurrentFrame()=23;
        Menu(app,L"Edit probe undo");
        Check(bone_curve::AuthoringState::Capture(raw)->Equals(*source)&&Encode(Read(raw))==before&&NativeSame(model,native,true)&&
            bone_scale::AuthoringState::Capture(raw).Equals(sourceScale)&&value()==initial+1&&app->CurrentFrame()==23,
            "actual bone undo restores exact sparse authoring and native marks across UI target changes without seeking");
        Menu(app,L"Edit probe undo");Check(value()==initial,"earlier real plugin edit remains separately undoable");
        Menu(app,L"Edit probe redo");Menu(app,L"Edit probe redo");
        Check(bone_curve::AuthoringState::Capture(raw)->Equals(*target)&&Encode(Read(raw))==after&&value()==initial+1&&app->CurrentFrame()==23,
            "actual bone redo restores the owned reconciled authoring after earlier plugin redo");
        Menu(app,L"Edit probe redo");Check(value()==initial+2&&history.UndoCount()==3,"later real plugin edit remains separately redoable");
        for(unsigned i=0;i<3;++i)Menu(app,L"Edit probe undo");
        Check(value()==initial&&history.Reset()==MMS_OK,"real sparse bone restores the plugin baseline");history.Collect();
    }
    std::puts("PASS actual sparse free-bone registration, exact undo/redo, plugin interleave and both history modes");
}
}
