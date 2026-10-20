// Include after native_camera_interpolation_checks.inc. Hidden real controls
// exercise the ordinary byte oracle; RealMenu enters production menu 242.
#include "../src/plugins/native_camera_multiply.hpp"
#include "../src/plugins/edit_store.hpp"
#include <cstdio>
#include <limits>
namespace mikumikustudio {
void ApplyCameraFrameMultiplyDialog(MMDApp*, HWND);
void ReloadModels(MMDApp*);
void CmdFileMenu(MMDApp*, HWND, std::uint16_t, std::uint16_t);
}
namespace camera_multiply_checks {
using namespace plugins;
using MultiplyOptions = CameraMultiplyOptions;
using Pose = std::array<double, 12>;
Pose ReadPose(MMDApp& app) {
    Pose result{};
    for (unsigned axis=0;axis<3;++axis) { result[axis]=app.CameraPositionAxis(axis);result[axis+3]=app.CameraRotation()[axis]; }
    result[6]=app.CameraDistance();result[7]=app.CameraFov();result[8]=app.CameraPerspective();
    result[9]=app.CameraParentModel();result[10]=app.CameraParentBone();return result;
}
struct Fixture : camera_interpolation_checks::Fixture {
    HWND dialog=nullptr;
    Fixture() {
        app->HInstance()=GetModuleHandleW(nullptr);
        dialog=CreateWindowExW(0,L"STATIC",L"camera multiply scratch",WS_OVERLAPPED,0,0,400,240,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(dialog!=nullptr,"create hidden multiply scratch parent");
        for(int id=686;id<=701;++id)Check(CreateWindowExW(0,L"EDIT",L"",WS_CHILD,0,0,80,20,dialog,
            reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr)!=nullptr,"create real multiply edit ID");
        Set(MultiplyOptions{});
    }
    ~Fixture(){DestroyWindow(dialog);}
    void Set(const MultiplyOptions& options,HWND destination=nullptr) {
        if(!destination)destination=dialog;
        for(unsigned lane=0;lane<8;++lane)for(unsigned pair=0;pair<2;++pair){
            char text[64]{};std::snprintf(text,sizeof(text),"%.9g",double(pair?options.offset[lane]:options.scale[lane]));
            Check(SetDlgItemTextA(destination,686+int(2*lane+pair),text)!=FALSE,"populate native multiply pair with round-trip float text");
        }
    }
    std::shared_ptr<HistoryChange> Change(const MultiplyOptions& options) {
        NativeCameraMultiplyPlan plan;std::shared_ptr<HistoryChange> change;
        Check(plan.Begin(*app)==MMS_OK,"capture multiply source before modal");Set(options);
        Check(plan.ReadDialog(dialog)==MMS_OK&&plan.Take(change)==MMS_OK,"read real edit controls into private multiply candidate");return change;
    }
    void Oracle(const MultiplyOptions& options) {
        Set(options);auto* old=g_Block;g_Block=app.get();ApplyCameraFrameMultiplyDialog(app.get(),dialog);g_Block=old;
    }
    std::uint64_t ProtectedValues() const {
        std::uint64_t digest=14695981039346656037ull;
        const auto add=[&](const void* value,std::size_t bytes){auto* p=static_cast<const unsigned char*>(value);while(bytes--){digest^=*p++;digest*=1099511628211ull;}};
        add(&app->state,sizeof(app->state));add(&app->SceneLight(),sizeof(D3DLIGHT9));
        for(std::size_t slot=0;slot<mdl::KeyPoolCapacity(app->CameraKeys());++slot){auto key=app->CameraKeys()[slot];
            std::memset(key.eye,0,sizeof(key.eye));std::memset(key.target,0,sizeof(key.target));key.distance=0;key.fov=0;add(&key,sizeof(key));}
        const auto pool=[&](const auto* key){add(key,mdl::KeyPoolCapacity(key)*sizeof(*key));};
        pool(app->LightKeys());pool(app->ShadowKeys());pool(app->GravityKeys());for(unsigned slot=0;slot<255;++slot)pool(app->AccessoryKeys(slot));return digest;
    }
};
MultiplyOptions MixedOptions() {
    MultiplyOptions options;options.scale={1.25f,-.5f,2.f,.75f,-1.f,1.5f,.8f,1.25f};
    options.offset={3.25f,-2.5f,.125f,13.25f,-27.5f,33.75f,-4.5f,2.75f};return options;
}
void Commit(Fixture& f,const std::shared_ptr<HistoryChange>& change) {
    Check(bool(change),"multiply change is substantive");std::unique_ptr<SharedHistory::NativeTransaction> token;
    Check(f.history.PrepareNative("Multiply camera keyframes",change,token,SharedHistory::NativeIntent::ExplicitCommit)==MMS_OK,
        "multiply reserves one explicit common history node");
    failAfter=0;const auto result=f.history.CommitNative(std::move(token));failAfter=-1;
    Check(result==MMS_OK,"multiply native and author publication allocates nothing");
}
void LegacyParityAndDirtyPose() {
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly}) {
        Fixture f,oracle;NativePoseTestBinding binding(*f.app,f.history);
        Check(f.history.SetMode(mode)==MMS_OK,"select camera multiply history mode");
        const auto before=GlobalRegistrationKeys(f.app->CameraKeys());const auto untouched=f.ProtectedValues();
        auto change=f.Change(MixedOptions());oracle.Oracle(MixedOptions());Commit(f,change);
        Check(GlobalRegistrationSame(f.app->CameraKeys(),GlobalRegistrationKeys(oracle.app->CameraKeys()))&&
            !std::memcmp(&f.app->CameraKeys()[1],&before[1],sizeof(mdl::CameraKey)),
            "multiply matches actual unbound 242 bytes including root, signed degree offsets and truncating FOV, leaving unselected middle intact");
        Check(f.ProtectedValues()==untouched&&f.history.UndoCount()==1&&!f.history.LastPublicationRequiresPhysicsReset(),
            "dirty camera pose and every unrelated byte survive one explicit node in both modes");
        const auto after=GlobalRegistrationKeys(f.app->CameraKeys());
        f.app->CurrentFrame()=28;f.app->LastRegisteredFrame()=911;f.app->CameraPositionX()=917.25f;
        f.app->CameraRotation()[1]=-.713f;f.app->CameraDistance()=-817;f.app->CameraFov()=91.5f;
        for(unsigned slot=0;slot<3;++slot)f.app->CameraKeys()[slot].selected=std::uint8_t(slot+9);
        auto inverse=before,forward=after;for(unsigned slot=0;slot<3;++slot)inverse[slot].selected=forward[slot].selected=std::uint8_t(slot+9);
        const auto later=f.ProtectedValues();
        Check(f.history.History(false)==MMS_OK&&GlobalRegistrationSame(f.app->CameraKeys(),inverse)&&f.ProtectedValues()==later,
            "multiply undo preserves later dirty camera, selection, frame, last registered frame and cursors");
        Check(f.history.History(true)==MMS_OK&&GlobalRegistrationSame(f.app->CameraKeys(),forward)&&f.ProtectedValues()==later,
            "multiply redo replays immutable values without reevaluation or file IO");
    }
    for(int frame:{0,5,12,20,31,90}) {
        Fixture f,oracle;NativePoseTestBinding binding(*f.app,f.history);
        f.app->CurrentFrame()=oracle.app->CurrentFrame()=frame;
        ReloadModels(f.app.get());ReloadModels(oracle.app.get());
        const auto beforePose=ReadPose(*f.app);const auto before=GlobalRegistrationKeys(f.app->CameraKeys());
        const auto cursor=f.app->CameraTrackCursor();const auto active=f.app->CameraTrackActive();const auto last=f.app->LastRegisteredFrame();
        auto change=f.Change(MixedOptions());oracle.Oracle(MixedOptions());Commit(f,change);
        Check(ReadPose(*f.app)==ReadPose(*oracle.app)&&f.app->CurrentFrame()==frame&&f.app->LastRegisteredFrame()==last&&
            f.app->CameraTrackCursor()==cursor&&f.app->CameraTrackActive()==active,
            "clean pose matches actual ReloadModels at exact, interpolated and tail times without moving transport or cursor");
        const auto afterPose=ReadPose(*f.app);
        Check(f.history.History(false)==MMS_OK&&ReadPose(*f.app)==beforePose&&GlobalRegistrationSame(f.app->CameraKeys(),before),
            "clean camera undo locally restores the track sample");
        Check(f.history.History(true)==MMS_OK&&ReadPose(*f.app)==afterPose,"clean camera redo locally follows the changed sample");
    }
}
std::vector<curve_editor::Channel> Channels(MMDApp& app) {
    std::vector<curve_editor::Channel> channels;std::wstring error;
    Check(curve_host::Read(app,curve_host::Kind::Camera,0,channels,error),"read multiply camera author channels");return channels;
}
bool SameTrack(const curve_editor::Channel& a,const curve_editor::Channel& b) {
    std::vector<animation_curve::NamedTrack> aa{{a.id,a.track}},bb{{b.id,b.track}};
    std::vector<std::uint8_t> left,right;std::string error;
    Check(animation_curve::Encode(aa,left,error)&&animation_curve::Encode(bb,right,error),"encode exact multiply author track");
    return left==right&&a.nativeControls==b.nativeControls&&a.controlGeometry==b.controlGeometry&&a.preservedCameraIntervals==b.preservedCameraIntervals;
}
void AuthoringAndComposition() {
    for(int frame:{0,5,20,31}){
        Fixture clean;NativePoseTestBinding binding(*clean.app,clean.history);camera_interpolation_checks::Author(clean);
        clean.app->CurrentFrame()=frame;ReloadModels(clean.app.get());const auto beforePose=ReadPose(*clean.app);
        MultiplyOptions options;options.offset[0]=4.25f;Commit(clean,clean.Change(options));const auto afterPose=ReadPose(*clean.app);
        ReloadModels(clean.app.get());
        Check(ReadPose(*clean.app)==afterPose,"clean authored camera updates to the same sample that actual ReloadModels consumes");
        Check(clean.history.History(false)==MMS_OK&&ReadPose(*clean.app)==beforePose,"clean authored undo restores the pristine local sample");
        Check(clean.history.History(true)==MMS_OK&&ReadPose(*clean.app)==afterPose,"clean authored redo follows the preserved Free and Step curves");
    }
    Fixture f;NativePoseTestBinding binding(*f.app,f.history);camera_interpolation_checks::Author(f);
    auto channels=Channels(*f.app);
    // A lane can omit a native union row; another lane has its own extra key.
    // This exercises independent authored topology, not just native keys.
    channels[1].track.keys.erase(channels[1].track.keys.begin()+1);channels[1].nativeControls.clear();channels[1].controlGeometry.clear();channels[1].preservedCameraIntervals.clear();
    std::size_t at=0;std::string detail;
    Check(animation_curve::InsertPreservingShape(channels[2].track,20,at,detail)!=animation_curve::InsertResult::Rejected,
        "insert an independent authored camera frame");
    channels[2].nativeControls.clear();channels[2].controlGeometry.clear();channels[2].preservedCameraIntervals.clear();
    std::wstring error;Check(curve_host::Write(*f.app,curve_host::Kind::Camera,0,channels,error),"publish sparse free author fixture");
    for(std::size_t slot=0;slot<mdl::KeyPoolCapacity(f.app->CameraKeys());++slot)f.app->CameraKeys()[slot].selected=0;
    f.app->CameraKeys()[0].selected=2;f.app->CameraKeys()[2].selected=5;
    const auto pristine=Channels(*f.app);const auto beforeCurves=camera_interpolation_checks::Curves(*f.app);
    const auto savedBefore=camera_interpolation_checks::SavedCurves(*f.app),light=camera_interpolation_checks::Curves(*f.app,curve_host::Kind::Light);
    const auto before=GlobalRegistrationKeys(f.app->CameraKeys());const auto authoringCount=f.history.UndoCount();
    MultiplyOptions options;options.offset[0]=4.25f;
    auto change=f.Change(options);Commit(f,change);const auto after=GlobalRegistrationKeys(f.app->CameraKeys());const auto edited=Channels(*f.app);
    const auto afterCurves=camera_interpolation_checks::Curves(*f.app),savedAfter=camera_interpolation_checks::SavedCurves(*f.app);
    Check(edited[0].track.keys[0].outWeight==pristine[0].track.keys[0].outWeight&&
        edited[0].track.keys[0].outSlope==pristine[0].track.keys[0].outSlope&&
        edited[0].track.keys[1].inWeight==pristine[0].track.keys[1].inWeight&&
        edited[0].track.keys[1].inSlope==pristine[0].track.keys[1].inSlope,
        "native value edits keep authored Free handle geometry rather than rebuilding from native slopes");
    for(std::size_t lane=1;lane<pristine.size();++lane)Check(SameTrack(pristine[lane],edited[lane]),
        "unowned sparse lanes, Step, independent frames and interval metadata remain exact");
    Check(camera_interpolation_checks::Curves(*f.app,curve_host::Kind::Light)==light,"camera multiply leaves unrelated light authoring intact");
    // Genuine later native registration uses the same journal and can grow the
    // pool; undo it before replaying the earlier immutable multiply record.
    // The bound curve writes used by Author and the sparse fixture already
    // belong to this journal. The registration adds exactly one more node.
    const auto multiplyCount=f.history.UndoCount();
    Check(multiplyCount==authoringCount+1,"authored camera multiply adds exactly one node after fixture edits");
    auto registration=GlobalRegistrationPlan(f,0,80);GlobalRegistrationCommit(f,registration);
    const auto registrationCount=f.history.UndoCount();const auto registrationUndo=f.history.History(false);
    const bool registrationKeys=GlobalRegistrationSame(f.app->CameraKeys(),after);
    const auto registrationCurves=camera_interpolation_checks::Curves(*f.app);
    if(registrationCount!=multiplyCount+1||registrationUndo!=MMS_OK||f.history.UndoCount()!=multiplyCount||!registrationKeys||registrationCurves!=afterCurves){
        std::fprintf(stderr,"camera multiply composition count=%zu expectedCount=%zu undo=%d keys=%d curves=%d expectedBytes=%zu actualBytes=%zu\n",
            registrationCount,multiplyCount+1,int(registrationUndo),int(registrationKeys),int(registrationCurves==afterCurves),afterCurves.size(),registrationCurves.size());
        if(!registrationKeys)for(std::size_t slot=0;slot<after.size();++slot)if(std::memcmp(&f.app->CameraKeys()[slot],&after[slot],sizeof(mdl::CameraKey))){
            std::fprintf(stderr,"camera multiply composition first key difference slot=%zu\n",slot);break;}
        if(registrationCurves!=afterCurves)for(std::size_t i=0;i<(std::min)(registrationCurves.size(),afterCurves.size());++i)if(registrationCurves[i]!=afterCurves[i]){
            std::fprintf(stderr,"camera multiply composition first curve difference offset=%zu expected=%u actual=%u\n",i,unsigned(afterCurves[i]),unsigned(registrationCurves[i]));break;}
    }
    Check(registrationCount==multiplyCount+1&&registrationUndo==MMS_OK&&f.history.UndoCount()==multiplyCount&&registrationKeys&&registrationCurves==afterCurves,
        "undo later native registration restores multiply authoring");
    const auto identity=mdl::KeyPoolIdentity(f.app->CameraKeys());
    Check(mdl::KeyPoolEnsure(f.app->CameraKeys(),65)&&mdl::KeyPoolIdentity(f.app->CameraKeys())==identity,"grow camera allocation without replacing stable identity");
    f.app->CameraKeys()[64].selected=19;f.app->CameraKeys()[64].reserved2[1]=81;const auto spare=f.app->CameraKeys()[64];
    f.app->CameraPositionY()=-431.125f;const auto dirty=ReadPose(*f.app);
    Check(f.history.History(false)==MMS_OK&&GlobalRegistrationSame(f.app->CameraKeys(),before)&&
        camera_interpolation_checks::Curves(*f.app)==beforeCurves&&camera_interpolation_checks::SavedCurves(*f.app)==savedBefore&&
        !std::memcmp(&f.app->CameraKeys()[64],&spare,sizeof(spare))&&ReadPose(*f.app)==dirty,
        "undo after growth restores exact sparse authoring and PMM extension while preserving later spare bytes and dirty pose");
    Check(f.history.History(true)==MMS_OK&&GlobalRegistrationSame(f.app->CameraKeys(),after)&&
        camera_interpolation_checks::Curves(*f.app)==afterCurves&&camera_interpolation_checks::SavedCurves(*f.app)==savedAfter,
        "redo restores multiply authoring without dropping high precision handles or extra allocation");
    Check(f.history.History(true)==MMS_OK,"later native registration redo remains replayable after multiply");
}
void NoopInvalidStaleAndBudget() {
    {
        Fixture x;NativePoseTestBinding bound(*x.app,x.history);camera_interpolation_checks::Author(x);
        const auto before=x.Digest();const auto curves=camera_interpolation_checks::Curves(*x.app);
        const auto beforeUndo=x.history.UndoCount(),beforeRedo=x.history.RedoCount();
        bool complete=false;unsigned failures=0;
        for(long limit=0;limit<2048&&!complete;++limit){
            NativeCameraMultiplyPlan plan;std::shared_ptr<HistoryChange> change;
            failAfter=limit;auto result=plan.Begin(*x.app);if(result==MMS_OK)result=plan.Read(MixedOptions());
            if(result==MMS_OK)result=plan.Take(change);failAfter=-1;
            Check(x.Digest()==before&&camera_interpolation_checks::Curves(*x.app)==curves&&
                x.history.UndoCount()==beforeUndo&&x.history.RedoCount()==beforeRedo,
                "multiply capture and numeric preparation OOM cannot mutate keys free authoring pose or journal");
            if(result==MMS_OK){complete=bool(change);Check(complete,"capture sweep yields a complete immutable multiply record");}
            else{++failures;Check(result==MMS_E_MEMORY&&!change,"failed multiply capture or numeric preparation returns no partial record");}
        }
        Check(complete&&failures,"authored multiply capture allocation sweep reaches success");
    }
    Fixture f;NativePoseTestBinding binding(*f.app,f.history);Commit(f,f.Change(MixedOptions()));
    Check(f.history.History(false)==MMS_OK,"create retained redo branch for multiply refusals");const auto before=f.Digest();
    Check(!f.Change(MultiplyOptions{})&&f.history.RedoCount()==1&&f.Digest()==before,"identity multiply preserves redo and all state");
    {NativeCameraMultiplyPlan cancel;Check(cancel.Begin(*f.app)==MMS_OK,"begin cancellable multiply modal");}
    Check(f.history.RedoCount()==1&&f.Digest()==before,"cancelled multiply capture does not change history");
    for(unsigned slot=0;slot<3;++slot)f.app->CameraKeys()[slot].selected=0;
    const auto unselected=f.Digest();
    Check(!f.Change(MixedOptions())&&f.history.RedoCount()==1&&f.Digest()==unselected,"no selected camera keys is a true no-op");
    f.app->CameraKeys()[0].selected=2;f.app->CameraKeys()[2].selected=5;
    for(unsigned invalid=0;invalid<4;++invalid){
        NativeCameraMultiplyPlan plan;Check(plan.Begin(*f.app)==MMS_OK,"capture invalid multiply source");auto options=MixedOptions();
        if(invalid==0)options.scale[0]=std::numeric_limits<float>::quiet_NaN();
        if(invalid==1)options.offset[1]=std::numeric_limits<float>::infinity();
        if(invalid==2)options.scale[0]=std::numeric_limits<float>::max();
        if(invalid==3)options.scale[7]=std::numeric_limits<float>::max();
        std::shared_ptr<HistoryChange> rejected;
        Check(plan.Read(options)==MMS_E_ARGUMENT&&plan.Take(rejected)==MMS_E_NOT_READY&&!rejected&&f.Digest()==before&&f.history.RedoCount()==1,
            "NaN infinity floating overflow and integer overflow refuse atomically and preserve redo");
    }
    { MultiplyOptions wide;wide.offset[7]=500;auto change=f.Change(wide);Commit(f,change);
      Check(f.app->CameraKeys()[0].fov==545&&f.history.History(false)==MMS_OK,
          "native int32 FOV beyond author UI limits remains compatible and reversible"); }
    // Recreate the source after history replay; every modal source check must
    // happen before dereferencing a captured journal or publishing any values.
    for(unsigned mutation=0;mutation<5;++mutation){
        Fixture x;NativePoseTestBinding bound(*x.app,x.history);NativeCameraMultiplyPlan plan;Check(plan.Begin(*x.app)==MMS_OK,"capture stale multiply modal");
        if(mutation==0)x.app->CameraKeys()[2].selected^=1;
        if(mutation==1)x.app->CameraKeys()[1].eye[0]+=1;
        if(mutation==2)x.app->CameraPositionY()+=1;
        if(mutation==3)x.app->CurrentFrame()+=1;
        if(mutation==4)camera_interpolation_checks::Author(x);
        const auto unchanged=x.Digest();const auto beforeUndo=x.history.UndoCount(),beforeRedo=x.history.RedoCount();
        Check(plan.Read(MixedOptions())==MMS_E_STALE&&x.Digest()==unchanged&&
            x.history.UndoCount()==beforeUndo&&x.history.RedoCount()==beforeRedo,
            "modal selection key pose playhead or author revision changes cannot overwrite their newer source");
    }
    for(unsigned mutation=0;mutation<5;++mutation){
        Fixture x;NativePoseTestBinding bound(*x.app,x.history);auto edit=x.Change(MixedOptions());
        std::unique_ptr<SharedHistory::NativeTransaction> pending;
        Check(x.history.PrepareNative("stale publication",edit,pending)==MMS_OK,"reserve actual camera multiply publication");
        auto* original=x.app->CameraKeys();mdl::CameraKey* replacement=nullptr;
        if(mutation==0)x.app->CameraKeys()[1].distance-=1;
        if(mutation==1)x.app->CameraKeys()[2].selected^=1;
        if(mutation==2)x.app->CameraPositionY()+=1;
        if(mutation==3){replacement=mdl::KeyPoolCreate<mdl::CameraKey>(mdl::KeyPoolCapacity(original));
            std::memcpy(replacement,original,mdl::KeyPoolCapacity(original)*sizeof(*original));x.app->CameraKeys()=replacement;}
        if(mutation==4)camera_interpolation_checks::Author(x);
        const auto unchanged=x.Digest();const auto beforeUndo=x.history.UndoCount(),beforeRedo=x.history.RedoCount();
        failAfter=0;const auto result=x.history.CommitNative(std::move(pending));failAfter=-1;
        Check(result==MMS_E_STALE&&x.Digest()==unchanged&&x.history.UndoCount()==beforeUndo&&x.history.RedoCount()==beforeRedo,
            "last noalloc multiply check rejects newer keys marks pose pool identity and authoring without a partial write");
        if(replacement){x.app->CameraKeys()=original;mdl::KeyPoolDestroy(replacement);}
    }
    {
        Fixture x;NativeCameraMultiplyPlan detached;
        {NativePoseTestBinding bound(*x.app,x.history);Check(detached.Begin(*x.app)==MMS_OK,"capture before removing a binding");}
        Check(detached.Read(MixedOptions())==MMS_E_STALE,"unbound modal rejects before accessing its former journal");
    }
    auto change=f.Change(MixedOptions());const auto unchanged=f.Digest();const auto retained=change->RetainedBytes();
    Check(retained>sizeof(mdl::CameraKey)*3,"multiply charges captured keys authoring seeds patches and map");
    Check(f.history.Configure(SharedHistory::RecordingMode::ExplicitOnly,8,retained-1)==MMS_OK,"set budget below native multiply charge");
    std::unique_ptr<SharedHistory::NativeTransaction> token;
    Check(f.history.PrepareNative("budget",change,token)==MMS_E_MEMORY&&!token&&f.Digest()==unchanged,
        "insufficient multiply budget rejects before any native or author write");
    Check(f.history.Configure(SharedHistory::RecordingMode::ExplicitOnly,128,256ull*1024*1024)==MMS_OK,"restore normal history budget");
    unsigned failures=0;bool prepared=false;
    for(long limit=0;limit<2048&&!prepared;++limit){
        std::string label="multiply allocation sweep";failAfter=limit;const auto result=f.history.PrepareNative(std::move(label),change,token);failAfter=-1;
        Check(f.Digest()==unchanged&&!f.history.UndoCount(),"every failed multiply preparation leaves scene and journal unchanged");
        if(result==MMS_OK)prepared=true;else{++failures;Check(result==MMS_E_MEMORY&&!token,"multiply preparation OOM has no partial reservation");}
    }
    Check(prepared&&failures,"multiply prepare allocation sweep reaches a complete candidate");
    token.reset();Commit(f,change);const auto after=f.Digest();
    failAfter=0;const auto undo=f.history.History(false);failAfter=-1;
    Check(undo==MMS_E_MEMORY&&f.Digest()==after&&f.history.UndoCount()==1,"undo OOM preserves published multiply and journal");
    Check(f.history.History(false)==MMS_OK,"retry multiply undo after allocation refusal");
    failAfter=0;const auto redo=f.history.History(true);failAfter=-1;
    Check(redo==MMS_E_MEMORY&&f.Digest()==unchanged&&f.history.RedoCount()==1,"redo OOM preserves source and redo branch");
    Check(f.history.History(true)==MMS_OK,"retry multiply redo after allocation refusal");
    for(bool forward:{false,true}){
        std::unique_ptr<PreparedHistoryChange> candidate;Check(change->Prepare(forward,candidate)==MMS_OK&&candidate,"prepare isolated multiply replay");
        failAfter=0;const bool current=candidate->IsCurrent();if(current)candidate->Publish();failAfter=-1;
        Check(current&&f.Digest()==(forward?after:unchanged),"both multiply replay publications are allocation free");
    }
}
struct PluginState {
    int value=0,pending=0,before=0,after=42;
    MmsSerializerV1 Serializer(){
        MmsSerializerV1 table{Header<MmsSerializerV1>(),this};
        table.prepareLoad=[](void* p,MmsBytes bytes,std::uint32_t schema,MmsHandle* out)->MmsResult{
            auto& s=*static_cast<PluginState*>(p);if(schema&&(schema!=1||bytes.bytes!=sizeof(int)||!bytes.data))return MMS_E_ARGUMENT;
            s.pending=0;if(schema)std::memcpy(&s.pending,bytes.data,sizeof(int));*out=1;return MMS_OK;};
        table.commitLoad=[](void* p,MmsHandle){auto& s=*static_cast<PluginState*>(p);s.value=s.pending;};table.cancelLoad=[](void*,MmsHandle){};
        table.save=[](void* p,MmsBytes* bytes,std::uint32_t* schema,MmsHandle* lease)->MmsResult{*bytes={&static_cast<PluginState*>(p)->value,sizeof(int)};*schema=1;*lease=1;return MMS_OK;};
        table.releaseBytes=[](void*,MmsHandle){};return table;
    }
    MmsTransactionParticipantV1 Participant(MmsString id){
        MmsTransactionParticipantV1 table{Header<MmsTransactionParticipantV1>(),this,id};
        table.prepare=[](void* p)->MmsResult{auto& s=*static_cast<PluginState*>(p);return s.value==s.before?MMS_OK:MMS_E_STALE;};
        table.commit=[](void* p){auto& s=*static_cast<PluginState*>(p);s.value=s.after;};table.cancel=[](void*){};
        table.getUndoState=[](void* p,MmsBytes* before,MmsBytes* after,std::uint32_t* schema,MmsHandle* lease)->MmsResult{
            auto& s=*static_cast<PluginState*>(p);*before={&s.before,sizeof(int)};*after={&s.after,sizeof(int)};*schema=1;*lease=1;return MMS_OK;};
        table.releaseUndoState=[](void*,MmsHandle){};return table;
    }
};
void PluginCompositionAndBusy() {
    Fixture f;NativePoseTestBinding binding(*f.app,f.history);PluginState plugin;
    StorageClient storage;storage.id="test.camera-multiply";f.storage.Api(storage);auto serializer=plugin.Serializer();MmsHandle provider=0;
    Check(f.storage.Register(storage,Text(storage.id),&serializer,sizeof(serializer),&provider)==MMS_OK,"register real multiply plugin serializer");
    EditStore edits(f.history,f.storage);EditClient client;client.id=storage.id;auto api=edits.Api(client);
    auto change=f.Change(MixedOptions());const auto before=GlobalRegistrationKeys(f.app->CameraKeys());
    std::uint64_t revision=0;MmsHandle transaction=0;constexpr char label[]="plugin camera multiply";
    Check(f.history.Revision(revision)==MMS_OK&&api.begin(api.context,{label,sizeof(label)-1},revision,&transaction)==MMS_OK,"begin real plugin draft");
    NativeCameraMultiplyPlan busy;Check(busy.Begin(*f.app)==MMS_E_BUSY,"plugin draft excludes another multiply modal");
    auto participant=plugin.Participant(Text(client.id));
    Check(api.join(api.context,transaction,&participant)==MMS_OK&&edits.JoinNative(client,transaction,change)==MMS_OK&&
        api.prepare(api.context,transaction)==MMS_OK&&api.commit(api.context,transaction)==MMS_OK&&plugin.value==42&&f.history.UndoCount()==1,
        "plugin private state and native camera multiply publish as one real shared transaction");
    const auto after=GlobalRegistrationKeys(f.app->CameraKeys());
    auto light=GlobalRegistrationPlan(f,1,80);GlobalRegistrationCommit(f,light);
    const auto afterLight=GlobalRegistrationKeys(f.app->CameraKeys());
    Check(api.undo(api.context)==MMS_OK&&plugin.value==42&&GlobalRegistrationSame(f.app->CameraKeys(),after)&&
        api.undo(api.context)==MMS_OK&&plugin.value==0&&GlobalRegistrationSame(f.app->CameraKeys(),before),
        "one journal undoes later light edit then mixed plugin and camera values");
    Check(api.redo(api.context)==MMS_OK&&plugin.value==42&&GlobalRegistrationSame(f.app->CameraKeys(),after),
        "real plugin redo restores mixed native camera values and original marks");
    Check(api.redo(api.context)==MMS_OK&&plugin.value==42&&GlobalRegistrationSame(f.app->CameraKeys(),afterLight),
        "later native light registration replays its own global selection clear as well as values");
    Check(f.history.Reset()==MMS_OK,"retire plugin camera nodes before unregistering serializer");f.history.Collect();
    Check(f.storage.Unregister(storage,provider)==MMS_OK,"unregister test serializer after history cleanup");
}
Fixture* modalFixture=nullptr;
MultiplyOptions modalOptions;
bool modalCancel=false;
unsigned modalCount=0;
LRESULT CALLBACK ModalHook(int code,WPARAM wp,LPARAM lp) {
    if(code==HCBT_CREATEWND){auto* created=reinterpret_cast<CBT_CREATEWNDW*>(lp);
        if(!(created->lpcs->style&WS_CHILD)){created->lpcs->x=-32000;created->lpcs->y=-32000;}}
    if(code==HCBT_ACTIVATE)return 1;
    return CallNextHookEx(nullptr,code,wp,lp);
}
LRESULT CALLBACK ModalInitialized(int code,WPARAM wp,LPARAM lp) {
    const auto* message=reinterpret_cast<CWPRETSTRUCT*>(lp);
    if(code>=0&&message->message==WM_INITDIALOG){auto dialog=message->hwnd;
        Check(modalFixture&&GetDlgItem(dialog,686)&&GetDlgItem(dialog,701),"242 enters its actual native multiply dialog");
        ++modalCount;modalFixture->Set(modalOptions,dialog);PostMessageW(dialog,WM_COMMAND,modalCancel?IDCANCEL:IDOK,0);
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}
void RealMenu() {
    // This requires the same localized menu-242 resources as the product.
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly}){
        Fixture f,oracle;NativePoseTestBinding binding(*f.app,f.history);Check(f.history.SetMode(mode)==MMS_OK,"set real 242 recording mode");
        const auto before=GlobalRegistrationKeys(f.app->CameraKeys());const auto pose=ReadPose(*f.app);auto* previous=g_Block;g_Block=f.app.get();
        Check(FindResourceW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(0x291),MAKEINTRESOURCEW(5))!=nullptr,
            "real 242 English dialog resource is linked");
        Check(f.app->CameraMode()!=0,"real 242 is enabled in camera mode");
        {NativeCameraMultiplyPlan probe;const auto status=probe.Begin(*f.app);
         if(status)std::fprintf(stderr,"modal probe status=%d capture=%p opt=%u\n",status,GetCapture(),unsigned(f.app->state.optflag[0]));
         Check(status==MMS_OK,"real 242 has a valid plan before modal dispatch");}
        modalFixture=&f;modalOptions=MixedOptions();modalCancel=false;modalCount=0;
        const auto hook=SetWindowsHookExW(WH_CBT,ModalHook,nullptr,GetCurrentThreadId());Check(hook!=nullptr,"observe actual native 242 modal");
        const auto initialized=SetWindowsHookExW(WH_CALLWNDPROCRET,ModalInitialized,nullptr,GetCurrentThreadId());
        Check(initialized!=nullptr,"observe real modal initialization once, independently of repeated activation requests");
        SetLastError(ERROR_SUCCESS);CmdFileMenu(f.app.get(),f.app->MainWindow(),242,0);
        if(modalCount!=1)std::fprintf(stderr,"modal count=%u error=%lu instance=%p module=%p opt=%u\n",modalCount,GetLastError(),f.app->HInstance(),GetModuleHandleW(nullptr),unsigned(f.app->state.optflag[0]));
        Check(modalCount==1,"one real 242 command opens exactly one modal");
        oracle.Oracle(modalOptions);
        Check(f.history.UndoCount()==1&&f.app->SceneModified()&&ReadPose(*f.app)==pose&&
            GlobalRegistrationSame(f.app->CameraKeys(),GlobalRegistrationKeys(oracle.app->CameraKeys())),
            "real 242 OK commits one explicit edit with legacy key parity and dirty-pose preservation");
        Check(f.history.History(false)==MMS_OK&&GlobalRegistrationSame(f.app->CameraKeys(),before),"undo real modal camera multiply");
        const auto undo=f.history.UndoCount(),redo=f.history.RedoCount();modalCancel=true;
        CmdFileMenu(f.app.get(),f.app->MainWindow(),242,0);
        Check(f.history.UndoCount()==undo&&f.history.RedoCount()==redo&&GlobalRegistrationSame(f.app->CameraKeys(),before),"real 242 cancel keeps native source and redo");
        modalCancel=false;modalOptions=MultiplyOptions{};CmdFileMenu(f.app.get(),f.app->MainWindow(),242,0);
        Check(f.history.UndoCount()==undo&&f.history.RedoCount()==redo&&GlobalRegistrationSame(f.app->CameraKeys(),before),"real 242 equal OK preserves redo");
        UnhookWindowsHookEx(initialized);UnhookWindowsHookEx(hook);modalFixture=nullptr;g_Block=previous;
    }
}
}
void NativeCameraMultiplyChecks() {
    camera_multiply_checks::LegacyParityAndDirtyPose();camera_multiply_checks::AuthoringAndComposition();
    camera_multiply_checks::NoopInvalidStaleAndBudget();camera_multiply_checks::PluginCompositionAndBusy();
    camera_multiply_checks::RealMenu();
}
