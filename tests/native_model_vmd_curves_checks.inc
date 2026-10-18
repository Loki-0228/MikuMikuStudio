// Include after native_model_vmd_checks.inc. These integration checks use the
// real unbound LoadVmdMotion as an oracle, then exercise the bound planner and
// SharedHistory. No renderer device, visible window, or source re-open on undo.
#include "mikudancestudio/d3d_wrapper.hpp"
namespace model_vmd_curve_integration {
namespace ac = mikudancestudio::animation_curve;
namespace p = mikudancestudio::plugins;
struct VmdCurveFixture : ModelPasteFixture {
    std::wstring boneName=L"\u9aa8\u9abc\u957f\u540d\u5c3e\u6bb5",morphName=L"\u8868\u60c5\u957f\u540d\u5c3e\u6bb5";
    std::unique_ptr<D3DRenderer> renderer=std::make_unique<D3DRenderer>();
    VmdCurveFixture() {
        std::memset(renderer.get(),0,sizeof(*renderer));app->Renderer()=renderer.get();
        app->CurrentFrame()=0;app->PlaybackPhysicsMode()=2;app->LastRegisteredFrame()=0;
        for(auto* f:{&first,&second}) {
            bone_curve::Forget(f->raw());morph_curve::Forget(f->raw());bone_scale::Forget(f->raw());
            auto& m=*f->m;m.maxFrame=0;m.searchCursor=5;m.loadComplete=1;m.physicsMode=2;
            std::strcpy(m.name,"fixture");
            std::memset(m.boneKeys,0,mdl::BoneKeyCapacity(m.boneKeys)*sizeof(mdl::BoneKey));
            std::memset(m.morphKeys,0,mdl::MorphKeyCapacity(m.morphKeys)*sizeof(mdl::MorphKey));
            for(unsigned i=0;i<f->bones.size();++i) {
                auto& bone=f->bones[i];bone={};bone.type=mdl::BoneType::RotateMove;
                bone.parent=bone.slotIndex=-1;bone.rotQuat[3]=1;
                for(unsigned diagonal=0;diagonal<4;++diagonal)bone.matInit[diagonal*5]=1;
                std::strcpy(bone.name,i?"target":"peer");m.boneKeys[i].rotation[3]=1;
                for(unsigned lane=0;lane<4;++lane) {
                    m.boneKeys[i].interpolation[lane]=m.boneKeys[i].interpolation[lane+4]=20;
                    m.boneKeys[i].interpolation[lane+8]=m.boneKeys[i].interpolation[lane+12]=107;
                }
            }
            f->bones[1].jpText=const_cast<wchar_t*>(boneName.c_str());
            std::strcpy(f->morphs[0].name,"face");f->morphs[0].jpText=const_cast<wchar_t*>(morphName.c_str());
            f->morphs[0].value=.375f;f->dirty={};f->selected={};f->selected[1]=1;
            for(unsigned i=0;i<4;++i) {
                auto& key=m.displayKeys[i];auto* ik=key.ikStates;auto* selectors=key.selectorStates;
                key={};key.ikStates=ik;key.selectorStates=selectors;key.visible=1;
                for(unsigned j=0;j<m.ikChainCount;++j)ik[j]=1;
                for(unsigned j=0;j<m.boneOrderCount;++j)selectors[j]={-1,0};
            }
            for(unsigned i=0;i<m.ikChainCount;++i)m.ikChains[i].enabled=1;
            for(unsigned i=0;i<m.boneOrderCount;++i) {
                auto& order=static_cast<mdl::BoneOrderEntry*>(m.boneOrderTable)[i];order={};
                order.boneIndex=int(i%2);order.linkedModel=-1;
            }
        }
        ClearSource();
    }
    ~VmdCurveFixture() {
        history.Reset();history.Collect();
        for(auto* f:{&first,&second}) {
            bone_curve::Forget(f->raw());f->bones[1].jpText=nullptr;f->morphs[0].jpText=nullptr;
            DisplayTestFreeGrowth(*f);
        }
        app->Renderer()=nullptr;
    }
    int Legacy(const VmdDecodeFile& file,unsigned offset) {
        Check(p::BoundNativePoseHistory(app.get())==nullptr,"VMD oracle uses the real unbound loader");
        const auto window=app->MainWindow();const auto selectedSlot=app->SelectedModelSlot();
        const auto frame=app->CurrentFrame();app->MainWindow()=nullptr;app->SelectedModelSlot()=1;app->CurrentFrame()=int(offset);
        // Avoid code-page dependent path conversion while retaining the ANSI
        // public loader entry used by the old command path.
        const auto path=file.path.string();const int result=LoadVmdMotion(app.get(),path.c_str());
        app->MainWindow()=window;app->SelectedModelSlot()=selectedSlot;app->CurrentFrame()=frame;return result;
    }
};
std::string HexName(const char* name) {
    constexpr char digits[]="0123456789abcdef";std::string out;
    for(unsigned i=0;i<20;++i){const auto c=static_cast<unsigned char>(name[i]);out+=digits[c>>4];out+=digits[c&15];}return out;
}
ac::Track Track(std::initializer_list<std::pair<int,double>> points,bool scale=false) {
    ac::Track result;for(const auto& point:points) {
        ac::Key key;key.frame=point.first;key.value=point.second;key.tangent=ac::Tangent::Free;
        key.interpolation=ac::Interpolation::Bezier;key.inWeight=.25;key.outWeight=.2;
        key.inSlope=scale?0:.03125;key.outSlope=scale?0:-.015625;result.keys.push_back(key);
    }return result;
}
struct Motion {
    VmdDecodeBytes standard,morph,bone,bytes;bone_scale_io::VmdExtension scales;
    extended_names::MotionNames names;std::size_t namesEnd=0,morphEnd=0,boneEnd=0;
    Motion(const VmdCurveFixture& fixture,int version=2,bool unicode=true,bool standardKeys=true,bool boneTail=true,bool morphTail=true,bool scaleTail=true) {
        const auto placeholder=extended_names::MotionIndexName(0);std::array<char,20> boneRaw{},morphRaw{};
        if(unicode){std::memcpy(boneRaw.data(),placeholder.data(),placeholder.size());std::memcpy(morphRaw.data(),placeholder.data(),placeholder.size());}
        else{std::memcpy(boneRaw.data(),fixture.first.bones[1].name,20);std::memcpy(morphRaw.data(),fixture.first.morphs[0].name,20);}
        VmdDecodeText(standard,version==1?"Vocaloid Motion Data file":"Vocaloid Motion Data 0002",30);
        VmdDecodeText(standard,"fixture",version==1?10:20);VmdDecodePut(standard,standardKeys?2u:0u);
        if(standardKeys)for(unsigned frame:{0u,10u}) {
            standard.insert(standard.end(),boneRaw.begin(),boneRaw.begin()+15);VmdDecodePut(standard,frame);
            for(float value:{frame?7.f:3.f,0.f,0.f,0.f,0.f,0.f,1.f})VmdDecodePut(standard,value);
            std::array<std::uint8_t,64> controls{};
            for(unsigned lane=0;lane<4;++lane)for(unsigned control=0;control<4;++control)
                controls[lane*16+control*4]=control<2?20:107;
            standard.insert(standard.end(),controls.begin(),controls.end());
        }
        VmdDecodePut(standard,standardKeys?2u:0u);
        if(standardKeys)for(unsigned frame:{0u,10u}) {
            standard.insert(standard.end(),morphRaw.begin(),morphRaw.begin()+15);VmdDecodePut(standard,frame);VmdDecodePut(standard,frame?.75f:.125f);
        }
        VmdDecodePut(standard,0u);VmdDecodePut(standard,0u);if(version==2)VmdDecodePut(standard,0u);VmdDecodePut(standard,0u);
        names.model=L"fixture";names.bones={fixture.boneName};names.morphs={fixture.morphName};
        if(standardKeys){names.boneOwners={0,0};names.morphOwners={0,0};}
        bytes=unicode?ModelVmdWithNames(standard,names):standard;namesEnd=bytes.size();
        std::string error;
        if(morphTail) {
            std::vector<std::uint8_t> payload;
            Check(morph_curve::EncodePayload({{morph_curve::StableId(0,morphRaw.data()),Track({{0,.125},{10,.75}})}},payload,error)&&
                  morph_curve::EncodeVmdBlock(payload,morph,error),"encode independent exact morph author tail");
            bytes.insert(bytes.end(),morph.begin(),morph.end());
        }
        morphEnd=bytes.size();
        if(boneTail) {
            std::vector<ac::NamedTrack> tracks;
            for(unsigned lane=0;lane<9;++lane) {
                const auto value=lane>=6?1.:0.;auto track=Track({{0,value},{10,value},{30,value}},lane>=6);
                if(!lane){track.keys[0].value=3;track.keys[1].value=7;track.keys[2].value=11;}
                if(lane==6){track.keys[0].value=1;track.keys[1].value=2;track.keys[2].value=3;}
                tracks.push_back({"bone/0/"+std::to_string(lane)+"/"+HexName(boneRaw.data()),std::move(track)});
            }
            std::vector<std::uint8_t> payload;Check(ac::Encode(tracks,payload,error),"encode independent nine-lane bone author payload");
            bone={'M','D','S','B','C','R','V','1'};VmdDecodePut(bone,std::uint32_t(payload.size()));bone.insert(bone.end(),payload.begin(),payload.end());
            bytes.insert(bytes.end(),bone.begin(),bone.end());
        }
        boneEnd=bytes.size();
        if(scaleTail&&standardKeys) {
            bone_scale_io::VmdExtension::Entry entry;std::memcpy(entry.name.data(),boneRaw.data(),entry.name.size());entry.frame=10;entry.scale=Scale(2);entry.scale.v[1]=.125f;
            entry.curve={7,31,88,119};scales.entries.push_back(entry);bytes=ModelVmdWithScale(bytes,scales);
        }
    }
};
struct File : VmdDecodeFile {
    explicit File(const VmdDecodeBytes& bytes) {
        path=std::filesystem::absolute("vmd-model-curves-"+std::to_string(GetCurrentProcessId())+".vmd");Write(bytes);
    }
    File(const File&)=delete;File& operator=(const File&)=delete;
};
VmdDecodeBytes Authors(Fixture& fixture) {
    std::vector<ac::NamedTrack> tracks;bone_curve::Tracks bones;
    if(bone_curve::ReadDetached(fixture.raw(),1,bones))for(unsigned lane=0;lane<9;++lane)tracks.push_back({"b/"+std::to_string(lane),bones[lane]});
    morph_curve::AuthoredLayers layers;std::string error;std::vector<ac::NamedTrack> morphs;
    Check(morph_curve::CaptureLayers(fixture.raw(),layers,error)&&morph_curve::LayersTracks(layers,morphs,error),"capture raw pristine morph authoring for comparison");
    for(auto& track:morphs)tracks.push_back(std::move(track));VmdDecodeBytes result;
    Check(ac::Encode(tracks,result,error),"encode independent author snapshot");return result;
}
struct Snapshot {
    ModelKeyState keys;bone_scale::AuthoringState scales;VmdDecodeBytes authors;
    std::uint32_t maximum,last;int cursor;
    Snapshot(VmdCurveFixture& f):keys(f.first),scales(bone_scale::AuthoringState::Capture(f.first.raw())),authors(Authors(f.first)),
        maximum(f.first.m->maxFrame),last(f.app->LastRegisteredFrame()),cursor(f.first.m->searchCursor){}
    bool Matches(VmdCurveFixture& f) const {
        return keys.Matches(f.first)&&scales.Equals(bone_scale::AuthoringState::Capture(f.first.raw()))&&authors==Authors(f.first)&&
            maximum==f.first.m->maxFrame&&last==f.app->LastRegisteredFrame()&&cursor==f.first.m->searchCursor;
    }
};
void Parity() {
    for(int version:{1,2})for(bool unicode:{false,true})for(unsigned offset:{0u,5u}) {
        std::printf("  model VMD author integration version=%d Unicode=%u offset=%u\n",version,unsigned(unicode),offset);
        VmdCurveFixture f;Motion motion(f,version,unicode);auto file=File(motion.bytes);p::ModelVmdSource source;
        Check(p::DecodeModelVmdSource(nullptr,file.path.c_str(),offset,source)==MMS_OK&&source.source.morphCurveBlock==motion.morph&&source.source.boneCurveBlock==motion.bone,
            "immutable model decoder retains both author envelopes byte-exact");
        Check(source.version==version&&source.source.bones.size()==2&&source.source.morphs.size()==2&&source.scales.entries.size()==1,
            "VMD1/2 standard counts coexist with exact curves and a CRC-bound scale footer");
        Check(p::ResolveModelVmdTargets(*f.app,0,source)==MMS_OK,"resolve complete model VMD identities without TLS");
        if(unicode)Check(source.source.curveTargetsResolved&&source.source.boneCurveTargets==std::vector<int>({1})&&source.source.morphCurveTargets==std::vector<int>({0}),
            "curve dictionary owner indices map to Unicode destination indices separately from record owners");
        f.first.bones[0].trans[0]=321.25f;f.first.dirty[0]=1;bone_scale::SetPose(f.first.raw(),0,Scale(9));
        const Snapshot before(f);const auto digest=f.first.Digest();p::ModelPasteReport report;std::shared_ptr<p::HistoryChange> change;
        Check(p::PlanModelVmdImport(*f.app,0,source,change,&report)==MMS_OK&&change&&!report.boneCurvesSkipped&&f.first.Digest()==digest&&before.Matches(f),
            "whole model author plan is private and does not reconcile live author caches");
        ModelPasteCommit(f,change);const auto last=f.app->LastRegisteredFrame();
        Check(f.Legacy(file,offset)==int(f.second.m->boneCount),"real old model loader accepts identical version and tail ordering");
        Check(ModelKeyState(f.second).Matches(f.first)&&Authors(f.second)==Authors(f.first)&&f.second.m->maxFrame==f.first.m->maxFrame&&f.second.m->searchCursor==f.first.m->searchCursor,
            "all native bytes marks author tangents and metadata match the real old loader");
        for(unsigned slot=0;slot<mdl::BoneKeyCapacity(f.first.m->boneKeys);++slot)
            Check(bone_scale::Key(f.first.raw(),int(slot))==bone_scale::Key(f.second.raw(),int(slot))&&bone_scale::KeyCurve(f.first.raw(),int(slot))==bone_scale::KeyCurve(f.second.raw(),int(slot)),
                "free bone mirrors use the imported scale basis before their own author write");
        Check(last==10+offset&&f.app->LastRegisteredFrame()==last&&f.first.m->maxFrame==30+offset,
            "tail-only frames enlarge maximum without enlarging the pre-tail registration extent");
        Check(f.first.bones[0].trans[0]==321.25f&&f.first.dirty[0]==1&&bone_scale::Pose(f.first.raw(),0)==Scale(9),"model author import preserves a peer unregistered pose");
        const Snapshot after(f);source={};Check(DeleteFileW(file.path.c_str()),"delete actual VMD source before history traversal");
        f.app->CurrentFrame()=17;f.app->SelectedModelSlot()=1;f.app->CameraMode()=1;
        Check(f.history.History(false)==MMS_OK&&before.Matches(f)&&f.app->CurrentFrame()==17,"single common undo restores all standard and author families with source offline");
        Check(f.history.History(true)==MMS_OK&&after.Matches(f)&&f.app->CurrentFrame()==17&&f.first.bones[0].trans[0]==321.25f,
            "single common redo owns raw curves and scale across playhead model and mode changes");
    }
}
void ConsumptionAndFallback() {
    for(unsigned variant=0;variant<5;++variant) {
        VmdCurveFixture f;Motion motion(f);auto bytes=motion.bytes;
        if(variant==0)bytes[motion.namesEnd+12]^=1; // Bad morph CRC hides every later tail.
        if(variant==1) { // Valid bone length but undecodable payload still permits following scale.
            bytes.assign(motion.bytes.begin(),motion.bytes.begin()+motion.boneEnd);bytes[motion.morphEnd+12]^=1;bytes=ModelVmdWithScale(bytes,motion.scales);
        }
        if(variant==2) {bytes.assign(motion.bytes.begin(),motion.bytes.begin()+motion.morphEnd);bytes.insert(bytes.end(),motion.bone.begin(),motion.bone.begin()+15);}
        if(variant==3)bytes.resize(motion.namesEnd+15); // Incomplete morph envelope.
        if(variant==4) {bytes.assign(motion.bytes.begin(),motion.bytes.begin()+motion.namesEnd);bytes.push_back(0);bytes.insert(bytes.end(),motion.morph.begin(),motion.morph.end());}
        auto file=File(bytes);p::ModelVmdSource source;
        Check(p::DecodeModelVmdSource(nullptr,file.path.c_str(),0,source)==MMS_OK,"bad optional author tails preserve complete standard model records");
        const bool morphValid=variant==1||variant==2;
        Check(source.source.morphCurveBlock.empty()!=morphValid&&source.source.boneCurveBlock.empty()==(variant!=1)&&source.scales.entries.empty()==(variant!=1),
            "CRC and truncation consume exactly the old loader boundaries");
        p::ModelPasteReport report;std::shared_ptr<p::HistoryChange> change;
        Check(p::PlanModelVmdImport(*f.app,0,source,change,&report)==MMS_OK&&change&&report.boneCurvesSkipped==(variant==1),"only a consumed incompatible bone payload requests the legacy warning");
        ModelPasteCommit(f,change);const auto expectedLast=f.app->LastRegisteredFrame();
        Check(f.Legacy(file,0)==2&&ModelKeyState(f.second).Matches(f.first)&&Authors(f.second)==Authors(f.first)&&f.app->LastRegisteredFrame()==expectedLast,
            "ignored or skipped tails match real old loader standard and remaining author results");
        for(unsigned slot=0;slot<mdl::BoneKeyCapacity(f.first.m->boneKeys);++slot)
            Check(bone_scale::Key(f.first.raw(),int(slot))==bone_scale::Key(f.second.raw(),int(slot)),"bone skip retains the independently valid scale footer");
    }
}
void TailOnlyAndAuthorOnly() {
    {
        VmdCurveFixture f;Motion motion(f,2,true,false,true,false,false);auto file=File(motion.bytes);p::ModelVmdSource source;
        Check(p::DecodeModelVmdSource(nullptr,file.path.c_str(),0,source)==MMS_OK&&source.source.bones.empty()&&source.source.morphs.empty(),"bone author tail may exist without standard model records");
        const Snapshot before(f);std::shared_ptr<p::HistoryChange> change;p::ModelPasteReport report;
        Check(p::PlanModelVmdImport(*f.app,0,source,change,&report)==MMS_OK&&change&&!report.boneCurvesSkipped,"tail-only source is not discarded by the aggregate empty-record fast path");
        ModelPasteCommit(f,change);Check(f.first.m->maxFrame==30&&f.app->LastRegisteredFrame()==0&&f.first.m->searchCursor==2,"tail-only source preserves the old cursor and registration-extent split");
        Check(f.Legacy(file,0)==2&&ModelKeyState(f.second).Matches(f.first)&&Authors(f.second)==Authors(f.first),"tail-only native mirrors and authoring match the real old loader");
        Check(f.history.History(false)==MMS_OK&&before.Matches(f),"tail-only import has a complete independent inverse");
    }
    {
        VmdCurveFixture f;Motion plain(f,2,true,true,false,false,false),authored(f,2,true,true,false,true,false);auto file=File(plain.bytes);p::ModelVmdSource source;
        Check(p::DecodeModelVmdSource(nullptr,file.path.c_str(),0,source)==MMS_OK,"decode standard seed for author-only history");std::shared_ptr<p::HistoryChange> change;
        Check(p::PlanModelVmdImport(*f.app,0,source,change)==MMS_OK&&change,"plan standard seed before exact authoring");ModelPasteCommit(f,change);
        const Snapshot before(f);file.Write(authored.bytes);Check(p::DecodeModelVmdSource(nullptr,file.path.c_str(),0,source)==MMS_OK,"decode author-only change over identical standard bytes");
        Check(p::PlanModelVmdImport(*f.app,0,source,change)==MMS_OK&&change,"exact morph tangents alone produce a nonempty common history node");ModelPasteCommit(f,change);
        Check(before.keys.Matches(f.first)&&before.authors!=Authors(f.first)&&f.history.UndoCount()==2,"author-only history does not fabricate a native key difference");
        const Snapshot after(f);Check(f.history.History(false)==MMS_OK&&before.Matches(f)&&f.history.History(true)==MMS_OK&&after.Matches(f),"author-only node restores raw author tangents in both directions");
        Check(p::PlanModelVmdImport(*f.app,0,source,change)==MMS_OK&&!change&&f.history.UndoCount()==2,"identical complete author import is a no-op");
    }
}
void PreparationFailures() {
    VmdCurveFixture f;Motion motion(f);auto file=File(motion.bytes);p::ModelVmdSource source;
    bool completed=false;unsigned failures=0;
    for(long limit=0;limit<512;++limit) {
        p::ModelVmdSource sentinel;sentinel.version=99;failAfter=limit;const auto status=p::DecodeModelVmdSource(nullptr,file.path.c_str(),0,sentinel);failAfter=-1;
        if(status==MMS_OK){source=std::move(sentinel);completed=true;break;}
        ++failures;Check(status==MMS_E_MEMORY&&sentinel.version==99&&sentinel.source.boneCurveBlock.empty(),"whole-tail decoder OOM preserves the caller destination");
    }
    Check(completed&&failures>5,"decoder allocation sweep reaches both raw tails and scale ownership");
    const Snapshot before(f);const auto digest=f.first.Digest();std::shared_ptr<p::HistoryChange> change;completed=false;failures=0;
    for(long limit=0;limit<4096;++limit) {
        failAfter=limit;const auto status=p::PlanModelVmdImport(*f.app,0,source,change);failAfter=-1;
        Check(f.first.Digest()==digest&&before.Matches(f)&&!f.history.UndoCount(),"any whole-tail planning failure preserves all native author and history state");
        if(status==MMS_OK){completed=bool(change);break;}++failures;Check(status==MMS_E_MEMORY&&!change,"whole-tail OOM returns no partially prepared model import");
    }
    Check(completed&&failures>10,"whole-tail planning sweep reaches all author family preparation");
    p::SharedHistory limited(f.storage,{},32,1);std::unique_ptr<p::SharedHistory::NativeTransaction> transaction;
    Check(limited.PrepareNative("VMD author budget",change,transaction)==MMS_E_MEMORY&&!transaction&&before.Matches(f),"history byte budget includes private author data before any publication");
    Check(f.history.PrepareNative("cancel VMD author",change,transaction)==MMS_OK&&transaction,"whole VMD import prepares a cancelable aggregate");
    failAfter=0;transaction.reset();failAfter=-1;Check(before.Matches(f)&&!f.history.UndoCount(),"cancel prepared tails without allocating or exposing intermediate standard keys");
    Check(DeleteFileW(file.path.c_str()),"source can disappear before the first publication");source={};ModelPasteCommit(f,change);const Snapshot after(f);
    failAfter=0;const auto undo=f.history.History(false);failAfter=-1;
    Check(undo==MMS_E_MEMORY&&after.Matches(f)&&f.history.UndoCount()==1,"failed inverse allocation preserves every imported family and common cursor");
    Check(f.history.History(false)==MMS_OK&&before.Matches(f),"complete inverse survives a prior memory failure");
    failAfter=0;const auto redo=f.history.History(true);failAfter=-1;
    Check(redo==MMS_E_MEMORY&&before.Matches(f)&&f.history.RedoCount()==1,"failed redo allocation preserves offline author data and redo branch");
    Check(f.history.History(true)==MMS_OK&&after.Matches(f),"offline redo restores both raw author families after allocation failure");
}
void AliasAdmission() {
    VmdCurveFixture f;Motion motion(f);auto file=File(motion.bytes);p::ModelVmdSource source;
    Check(p::DecodeModelVmdSource(nullptr,file.path.c_str(),0,source)==MMS_OK,"decode valid whole-tail source for alias rejection");
    for(unsigned kind=0;kind<3;++kind) {
        auto* ownMorph=f.first.m->morphKeys;auto* peerMorph=f.second.m->morphKeys;auto* cursors=f.first.m->boneKeyCursors;
        auto* links=f.firstIk[0].links;const auto linksCount=f.firstIk[0].linkCount;
        if(kind==0)f.second.m->morphKeys=ownMorph;
        if(kind==1)f.first.m->boneKeyCursors=reinterpret_cast<std::uint32_t*>(&f.first.morphs[0].value);
        if(kind==2){f.firstIk[0].links=reinterpret_cast<decltype(links)>(f.first.m->boneKeys);f.firstIk[0].linkCount=1;}
        const auto digest=f.first.Digest();std::shared_ptr<p::HistoryChange> change;
        const auto status=p::PlanModelVmdImport(*f.app,0,source,change);
        Check(status!=MMS_OK&&!change&&f.first.Digest()==digest&&!f.history.UndoCount(),"cross-model pool and nested cursor or IK aliases reject the entire model VMD before writes");
        f.second.m->morphKeys=peerMorph;f.first.m->boneKeyCursors=cursors;f.firstIk[0].links=links;f.firstIk[0].linkCount=linksCount;
    }
    std::shared_ptr<p::HistoryChange> change;std::unique_ptr<p::SharedHistory::NativeTransaction> transaction;
    Check(p::PlanModelVmdImport(*f.app,0,source,change)==MMS_OK&&change&&f.history.PrepareNative("late VMD peer alias",change,transaction)==MMS_OK,
        "whole VMD token initially prepares with independent peer storage");
    auto* peer=f.second.m->morphKeys;f.second.m->morphKeys=f.first.m->morphKeys;const auto digest=f.first.Digest();
    const auto status=CommitRegistrationNoAllocation(f.history,std::move(transaction));f.second.m->morphKeys=peer;
    Check(status==MMS_E_STALE&&f.first.Digest()==digest&&!f.history.UndoCount(),"peer storage aliased after preparation invalidates the short whole-import token before publication");
}
void MixedChronology() {
    for(auto mode:{p::SharedHistory::RecordingMode::AllOperations,p::SharedHistory::RecordingMode::ExplicitOnly}) {
        VmdCurveFixture f;Check(f.history.SetMode(mode)==MMS_OK,"model VMD integration uses either common recording mode");
        Motion motion(f);auto file=File(motion.bytes);p::ModelVmdSource source;
        Check(p::DecodeModelVmdSource(nullptr,file.path.c_str(),0,source)==MMS_OK,"decode first authored model VMD in mixed chronology");
        const Snapshot pristine(f);const ModelKeyState peerPristine(f.second);std::shared_ptr<p::HistoryChange> change;
        Check(p::PlanModelVmdImport(*f.app,0,source,change)==MMS_OK&&change,"plan first complete author import in mixed chronology");ModelPasteCommit(f,change);
        const Snapshot first(f);const auto peerBone=f.first.bones[0];
        f.app->CurrentFrame()=13;f.second.bones[0].trans[0]=17;f.second.dirty[0]=1;
        Check(p::PlanDisplayRegistration(*f.app,1,13,change)==MMS_OK&&change,"real second-model native registration shares the VMD history");
        ModelPasteCommit(f,change);const ModelKeyState peerRegistered(f.second);const Snapshot middle(f);
        std::vector<std::uint8_t> payload;std::vector<ac::NamedTrack> tracks;std::string error;
        Check(morph_curve::DecodeVmdBlock(source.source.morphCurveBlock.data(),source.source.morphCurveBlock.size(),payload)!=0&&
              morph_curve::DecodePayload(payload.data(),payload.size(),tracks,error)&&tracks.size()==1,"decode exact author to prepare second independent VMD");
        // The old importer normalizes the first imported key's handles. Edit
        // the interval-ending key so this is a real retained author change.
        tracks[0].track.keys[1].inSlope=.0625;
        Check(morph_curve::EncodePayload(tracks,payload,error)&&morph_curve::EncodeVmdBlock(payload,source.source.morphCurveBlock,error),"encode second VMD author edit with identical standard values");
        f.app->CurrentFrame()=7;
        p::ModelPasteReport report;const auto secondStatus=p::PlanModelVmdImport(*f.app,0,source,change,&report);
        if(secondStatus!=MMS_OK||!change) {
            std::printf("DIAG mixed second author mode=%u status=%d changed=%u skipped=%u frame=%d camera=%u playback=%u step=%u mutation=%u pending=%u undo=%zu redo=%zu source-offset=%u bone=%zu morph=%zu bone-block=%zu morph-block=%zu max=%u last=%u cursor=%d\n",
                unsigned(mode),int(secondStatus),unsigned(bool(change)),unsigned(report.boneCurvesSkipped),f.app->CurrentFrame(),unsigned(f.app->CameraMode()),
                unsigned(f.app->PlaybackActive()),unsigned(f.app->FrameStepPlayback()),unsigned(f.app->SceneMutationInProgress()),unsigned(f.app->state.recordPlaybackStartPending),
                f.history.UndoCount(),f.history.RedoCount(),source.source.frameOffset,source.source.bones.size(),source.source.morphs.size(),
                source.source.boneCurveBlock.size(),source.source.morphCurveBlock.size(),f.first.m->maxFrame,f.app->LastRegisteredFrame(),f.first.m->searchCursor);
        }
        Check(secondStatus==MMS_OK&&change,"second exact author import follows another model's real native operation");ModelPasteCommit(f,change);
        const Snapshot last(f);Check(f.history.UndoCount()==3,"two VMD imports and a cross-model registrar occupy three common nodes");
        source={};Check(DeleteFileW(file.path.c_str()),"mixed common history no longer has a source VMD file");
        const Snapshot* expectedUndo[]={&middle,&first,&pristine};const Snapshot* expectedRedo[]={&first,&middle,&last};
        for(unsigned step=0;step<3;++step) {
            f.app->CurrentFrame()=int(5+step*11);const int frame=f.app->CurrentFrame();
            Check(f.history.History(false)==MMS_OK&&expectedUndo[step]->Matches(f)&&f.app->CurrentFrame()==frame,"mixed VMD/native undo restores exact raw authors marks and range at a changed playhead");
            Check((step?peerPristine:peerRegistered).Matches(f.second),"mixed undo restores only the correct second-model native node");
            Check(!std::memcmp(&f.first.bones[0],&peerBone,sizeof(peerBone)),"mixed author undo leaves the first model's unrelated bone pose untouched");
        }
        for(unsigned step=0;step<3;++step) {
            f.app->CurrentFrame()=int(4+step*13);const int frame=f.app->CurrentFrame();
            Check(f.history.History(true)==MMS_OK&&expectedRedo[step]->Matches(f)&&f.app->CurrentFrame()==frame,"mixed VMD/native redo restores exact raw authors marks and range with source offline");
            Check((step?peerRegistered:peerPristine).Matches(f.second),"mixed redo publishes the second model only at its own chronological step");
            Check(!std::memcmp(&f.first.bones[0],&peerBone,sizeof(peerBone)),"mixed author redo preserves an unrelated bone's native pose bytes");
        }
    }
}
}
namespace {
void NativeModelVmdCurvesChecks() {
    model_vmd_curve_integration::Parity();model_vmd_curve_integration::ConsumptionAndFallback();
    model_vmd_curve_integration::TailOnlyAndAuthorOnly();model_vmd_curve_integration::PreparationFailures();
    model_vmd_curve_integration::AliasAdmission();model_vmd_curve_integration::MixedChronology();
}
}
