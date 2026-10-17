// Included after the global registration fixtures. No renderer or live UI.
#include "../src/plugins/native_global_registration.hpp"
#include "../src/plugins/native_global_paste.hpp"
#include "../src/plugins/native_vmd_source.hpp"
#include <filesystem>
namespace {
using GSparseBytes=std::vector<std::uint8_t>;
curve_host::Kind GSparseKind(unsigned family){return family==0?curve_host::Kind::Camera:family==1?curve_host::Kind::Light:curve_host::Kind::Shadow;}
unsigned GSparseLane(unsigned family){return family==2?1:0;}
std::vector<curve_editor::Channel> GSparseRead(GlobalRegistrationFixture& f,unsigned family){
    std::vector<curve_editor::Channel> channels;std::wstring error;
    Check(curve_host::Read(*f.app,GSparseKind(family),0,channels,error),"read sparse global authoring");return channels;
}
GSparseBytes GSparseSnapshot(GlobalRegistrationFixture& f,unsigned family){
    const auto channels=GSparseRead(f,family);std::vector<animation_curve::NamedTrack> tracks;
    for(const auto& channel:channels)tracks.push_back({channel.id,channel.track});
    GSparseBytes result;std::string error;
    Check(animation_curve::Encode(tracks,result,error),"encode exact sparse global keys handles and interpolation");return result;
}
bool GSparseHas(const curve_editor::Channel& channel,int frame){
    return std::any_of(channel.track.keys.begin(),channel.track.keys.end(),[&](const auto& key){return key.frame==frame;});
}
template<class K> const K& GSparseAt(const K* keys,unsigned frame){
    unsigned current=0;for(std::size_t hops=0;keys[current].frame<frame&&keys[current].next;++hops){
        Check(hops<mdl::KeyPoolCapacity(keys),"sparse global fixture has bounded native chain");current=keys[current].next;
    }
    Check(keys[current].frame==frame,"sparse authored frame retains a union native key");return keys[current];
}
void GSparseSamePose(GlobalRegistrationFixture& f,unsigned family,unsigned frame=12){
    if(family==0){const auto& key=GSparseAt(f.app->CameraKeys(),frame);
        for(unsigned axis=0;axis<3;++axis){f.app->CameraPositionAxis(axis)=key.eye[axis];f.app->CameraRotation()[axis]=key.target[axis];}
        f.app->CameraDistance()=key.distance;f.app->CameraFov()=float(key.fov);f.app->CameraPerspective()=key.perspective;
        f.app->CameraParentModel()=key.parentModel;f.app->CameraParentBone()=key.parentBone;
    }else if(family==1){const auto& key=GSparseAt(f.app->LightKeys(),frame);
        std::memcpy(f.app->LightDirection(),key.direction,sizeof(key.direction));std::memcpy(f.app->LightColor(),key.color,sizeof(key.color));
    }else{const auto& key=GSparseAt(f.app->ShadowKeys(),frame);f.app->ShadowMode()=key.mode;f.app->ShadowDistance()=key.distance;}
}
void GSparseAuthor(GlobalRegistrationFixture& f,unsigned family){
    auto channels=GSparseRead(f,family);const auto lane=GSparseLane(family);auto& sparse=channels[lane].track.keys;
    sparse.erase(std::remove_if(sparse.begin(),sparse.end(),[](const auto& key){return key.frame==12;}),sparse.end());
    Check(sparse.size()==2,"sparse global fixture removes just one native-union frame from one authored lane");
    sparse.front().outSlope=family==2?.0007:.013;sparse.front().outWeight=.19;sparse.front().tangent=animation_curve::Tangent::Free;
    sparse.back().inSlope=family==2?-.0009:-.017;sparse.back().inWeight=.31;sparse.back().tangent=animation_curve::Tangent::Free;
    sparse.back().interpolation=animation_curve::Interpolation::Bezier;
    auto& unrelated=channels[family==2?0:1].track.keys;
    unrelated.erase(std::remove_if(unrelated.begin(),unrelated.end(),[](const auto& key){return key.frame==31;}),unrelated.end());
    unrelated.back().interpolation=animation_curve::Interpolation::Step;
    std::wstring error;Check(curve_host::Write(*f.app,GSparseKind(family),0,channels,error),"author sparse global lanes with Free handles and an unrelated Step interval");
    const auto read=GSparseRead(f,family);
    Check(!GSparseHas(read[lane],12)&&!GSparseHas(read[family==2?0:1],31),"ordinary read preserves genuinely sparse authored lane frames");
    GSparseSamePose(f,family);f.app->CurrentFrame()=7;
}
plugins::GlobalPasteSource GSparseSource(GlobalRegistrationFixture& f,unsigned family,unsigned offset=7){
    plugins::GlobalPasteSource source;source.frameOffset=offset;
    if(family==0){const auto& key=GSparseAt(f.app->CameraKeys(),12);mdl::CameraClipboardRecord record{};record.frame=12-offset;
        record.distance=key.distance;record.fov=key.fov;record.perspective=key.perspective;record.parentModel=key.parentModel;record.parentBone=key.parentBone;
        std::memcpy(record.eye,key.eye,sizeof(record.eye));std::memcpy(record.target,key.target,sizeof(record.target));
        std::memcpy(record.interpolation,key.interpolation,sizeof(record.interpolation));source.cameras.push_back(record);
    }else if(family==1){const auto& key=GSparseAt(f.app->LightKeys(),12);mdl::LightClipboardRecord record{};record.frame=12-offset;
        std::memcpy(record.direction,key.direction,sizeof(record.direction));std::memcpy(record.color,key.color,sizeof(record.color));source.lights.push_back(record);
    }else{const auto& key=GSparseAt(f.app->ShadowKeys(),12);mdl::ShadowClipboardRecord record{};record.frame=12-offset;
        record.mode=key.mode;record.distance=key.distance;source.shadows.push_back(record);}
    return source;
}
MmsResult GSparsePlan(GlobalRegistrationFixture& f,unsigned family,std::shared_ptr<plugins::HistoryChange>& out){
    return family==0?plugins::PlanCameraRegistration(*f.app,12,false,out):family==1?plugins::PlanLightRegistration(*f.app,12,out):
        plugins::PlanShadowRegistration(*f.app,12,f.app->ShadowDistance(),out);
}
MmsResult GSparsePart(GlobalRegistrationFixture& f,unsigned family,const plugins::GlobalPasteSource& source,plugins::GlobalPasteFamilyResult& out){
    return family==0?plugins::PlanCameraKeyPastePart(*f.app,source,out):family==1?plugins::PlanLightKeyPastePart(*f.app,source,out):
        plugins::PlanShadowKeyPastePart(*f.app,source,out);
}
void GSparseCompleted(GlobalRegistrationFixture& f,unsigned family){
    const auto channels=GSparseRead(f,family);
    Check(std::all_of(channels.begin(),channels.end(),[](const auto& lane){return GSparseHas(lane,12);}),"explicit global intent registers frame twelve on every lane");
    Check(!GSparseHas(channels[family==2?0:1],31),"explicit global registration does not fill unrelated missing frame thirty-one");
}
struct GSparseNative {
    std::vector<mdl::CameraKey> camera;std::vector<mdl::LightKey> light;std::vector<mdl::SelfShadowKey> shadow;
    explicit GSparseNative(GlobalRegistrationFixture& f):camera(GlobalRegistrationKeys(f.app->CameraKeys())),light(GlobalRegistrationKeys(f.app->LightKeys())),shadow(GlobalRegistrationKeys(f.app->ShadowKeys())){}
    bool Same(GlobalRegistrationFixture& f,bool marks=true)const{
        const auto equal=[&](const auto* live,const auto& saved){if(mdl::KeyPoolCapacity(live)<saved.size())return false;
            for(std::size_t i=0;i<saved.size();++i){auto a=live[i],b=saved[i];if(!marks)a.selected=b.selected=0;
                if(std::memcmp(&a,&b,sizeof(a)))return false;}return true;};
        return equal(f.app->CameraKeys(),camera)&&equal(f.app->LightKeys(),light)&&equal(f.app->ShadowKeys(),shadow);
    }
};
void GSparseRegistrationRoundTrip(){
    for(unsigned family=0;family<3;++family)for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        GlobalRegistrationFixture f;GSparseAuthor(f,family);const auto pristine=GSparseSnapshot(f,family);const GSparseNative native(f);
        const auto digest=f.Digest();const auto last=f.app->LastRegisteredFrame();
        Check(f.history.SetMode(mode)==MMS_OK,"sparse explicit global fixture configures history mode");
        std::shared_ptr<plugins::HistoryChange> change;
        Check(GSparsePlan(f,family,change)==MMS_OK&&change&&f.Digest()==digest&&GSparseSnapshot(f,family)==pristine,
            "same-valued native registration still privately plans a missing authored lane frame");
        GlobalRegistrationCommit(f,change);GSparseCompleted(f,family);const auto completed=GSparseSnapshot(f,family);
        Check(native.Same(f,false)&&f.app->CurrentFrame()==7&&f.app->LastRegisteredFrame()==last&&f.history.UndoCount()==1,
            "authoring-only explicit registration keeps native payload/playhead/last frame and records in both modes");
        f.app->CurrentFrame()=23;f.app->CameraMode()=0;
        Check(f.history.History(false)==MMS_OK&&GSparseSnapshot(f,family)==pristine&&native.Same(f)&&f.app->CurrentFrame()==23,
            "sparse registration undo restores exact original Free/Step curves and native marks at a different playhead");
        Check(f.history.History(true)==MMS_OK&&GSparseSnapshot(f,family)==completed&&f.app->CurrentFrame()==23,
            "sparse registration redo restores precisely the explicitly registered lane key without seeking");
        f.app->CameraMode()=1;GSparseSamePose(f,family);
        const auto redo=f.history.RedoCount(),undo=f.history.UndoCount();
        Check(GSparsePlan(f,family,change)==MMS_OK&&!change&&f.history.UndoCount()==undo&&f.history.RedoCount()==redo,
            "repeated identical registration becomes a complete no-op after authored lanes contain its frame");
    }
}
void GSparsePasteRoundTrip(){
    for(unsigned family=0;family<3;++family){
        GlobalRegistrationFixture f;GSparseAuthor(f,family);auto source=GSparseSource(f,family);const auto pristine=GSparseSnapshot(f,family);
        const GSparseNative native(f);const auto digest=f.Digest();plugins::GlobalPasteFamilyResult part;
        Check(GSparsePart(f,family,source,part)==MMS_OK&&part.part&&part.dataChanged&&!part.maximumInsertedFrame&&f.Digest()==digest,
            "same-valued global paste participant reports authoring-only data change with no native insertion");
        f.app->CurrentFrame()=5;std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanGlobalKeyPaste(*f.app,source,change)==MMS_OK&&change,"global paste aggregate retains authoring-only participant");
        source={};GlobalRegistrationCommit(f,change);GSparseCompleted(f,family);const auto completed=GSparseSnapshot(f,family);
        Check(native.Same(f,false)&&f.app->CurrentFrame()==5&&f.history.UndoCount()==1,"paste uses captured source offset instead of actual edit playhead and owns deleted source data");
        Check(f.history.History(false)==MMS_OK&&GSparseSnapshot(f,family)==pristine&&native.Same(f),"authoring-only paste undo restores original sparse curve and all native bytes");
        Check(f.history.History(true)==MMS_OK&&GSparseSnapshot(f,family)==completed,"authoring-only paste redo re-registers exact source frame");
        const auto current=GSparseSource(f,family);Check(GSparsePart(f,family,current,part)==MMS_OK&&part.part&&!part.dataChanged,
            "completed sparse paste family reports false dataChanged on a true repeat");
        Check(plugins::PlanGlobalKeyPaste(*f.app,current,change)==MMS_OK&&!change,"completed sparse paste aggregate is a true no-op");
    }
    {
        GlobalRegistrationFixture f;for(unsigned family=0;family<3;++family)GSparseAuthor(f,family);
        auto source=GSparseSource(f,0);source.lights=GSparseSource(f,1).lights;source.shadows=GSparseSource(f,2).shadows;
        const std::array<GSparseBytes,3> pristine={GSparseSnapshot(f,0),GSparseSnapshot(f,1),GSparseSnapshot(f,2)};
        const GSparseNative native(f);std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanGlobalKeyPaste(*f.app,source,change)==MMS_OK&&change,"mixed three-family sparse paste plans even without native payload differences");
        GlobalRegistrationCommit(f,change);for(unsigned family=0;family<3;++family)GSparseCompleted(f,family);
        Check(f.history.UndoCount()==1&&native.Same(f,false),"mixed authored-only families publish in one atomic global history node");
        Check(f.history.History(false)==MMS_OK&&native.Same(f),"mixed authoring-only paste restores all native marks together");
        for(unsigned family=0;family<3;++family)Check(GSparseSnapshot(f,family)==pristine[family],"mixed paste undo restores every sparse authoring family");
        Check(f.history.History(true)==MMS_OK,"mixed sparse family redo publishes all participants together");
        for(unsigned family=0;family<3;++family)GSparseCompleted(f,family);
    }
}
struct GSparseVmdFile {
    std::wstring path;GSparseBytes bytes;
    template<class T>void Put(const T& value){const auto* data=reinterpret_cast<const std::uint8_t*>(&value);bytes.insert(bytes.end(),data,data+sizeof(value));}
    void Raw(const void* pointer,std::size_t length){const auto* data=static_cast<const std::uint8_t*>(pointer);bytes.insert(bytes.end(),data,data+length);}
    explicit GSparseVmdFile(const plugins::GlobalPasteSource& source){
        static unsigned serial=0;path=(std::filesystem::temp_directory_path()/(L"mms-sparse-global-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++serial)+L".vmd")).native();
        char header[30]{};strcpy_s(header,"Vocaloid Motion Data 0002");Raw(header,sizeof(header));
        char name[20]{};std::memcpy(name,"\x83\x4a\x83\x81\x83\x89\x81\x45\x8f\xc6\x96\xbe",13);Raw(name,sizeof(name));Put(std::uint32_t(0));Put(std::uint32_t(0));
        Put(std::uint32_t(source.cameras.size()));for(const auto& key:source.cameras){Put(key.frame);Put(key.distance);Raw(key.eye,sizeof(key.eye));Raw(key.target,sizeof(key.target));
            for(unsigned lane=0;lane<6;++lane){Put(key.interpolation[lane]);Put(key.interpolation[lane+12]);Put(key.interpolation[lane+6]);Put(key.interpolation[lane+18]);}Put(key.fov);Put(key.perspective);}
        Put(std::uint32_t(source.lights.size()));for(const auto& key:source.lights){Put(key.frame);Raw(key.color,sizeof(key.color));Raw(key.direction,sizeof(key.direction));}
        Put(std::uint32_t(source.shadows.size()));for(const auto& key:source.shadows){Put(key.frame);Put(key.mode);Put(key.distance);}Put(std::uint32_t(0));
        FILE* file=nullptr;Check(!_wfopen_s(&file,path.c_str(),L"wb")&&file,"open sparse VMD source");Check(std::fwrite(bytes.data(),1,bytes.size(),file)==bytes.size(),"write sparse VMD source");Check(!std::fclose(file),"close sparse VMD source");
    }
    ~GSparseVmdFile(){DeleteFileW(path.c_str());}
};
void GSparseVmdRoundTrip(){
    for(unsigned family=0;family<3;++family){
        GlobalRegistrationFixture f;GSparseAuthor(f,family);const auto pristine=GSparseSnapshot(f,family);GSparseVmdFile file(GSparseSource(f,family));
        const GSparseNative native(f);const auto digest=f.Digest();std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanGlobalVmdImport(*f.app,nullptr,file.path.c_str(),change)==MMS_OK&&change&&f.Digest()==digest,
            "same-valued standard VMD privately retains explicit frame intent for sparse authored lanes");
        GlobalRegistrationCommit(f,change);GSparseCompleted(f,family);const auto completed=GSparseSnapshot(f,family);
        Check(native.Same(f,false)&&DeleteFileW(file.path.c_str()),"sparse VMD adds authoring frame without native data changes and allows source removal");
        f.app->CurrentFrame()=24;
        Check(f.history.History(false)==MMS_OK&&GSparseSnapshot(f,family)==pristine,"sparse VMD undo restores exact pre-import authored curves");
        Check(f.history.History(true)==MMS_OK&&GSparseSnapshot(f,family)==completed&&f.app->CurrentFrame()==24,"sparse VMD redo retains frame intent after source deletion without seeking");
    }
}
void GSparseFailures(){
    for(unsigned family=0;family<3;++family)for(bool paste:{false,true}){
        GlobalRegistrationFixture f;GSparseAuthor(f,family);const auto source=GSparseSource(f,family);const auto before=f.Digest();
        curve_host::AuthoringTable pristine;std::wstring error;Check(curve_host::CaptureAuthoring(*f.app,pristine,error),"capture sparse authoring before allocation sweep");
        std::shared_ptr<plugins::HistoryChange> change;bool completed=false;unsigned failures=0;
        for(long allowance=0;allowance<1024;++allowance){
            failAfter=allowance;const auto result=paste?plugins::PlanGlobalKeyPaste(*f.app,source,change):GSparsePlan(f,family,change);failAfter=-1;
            Check(f.Digest()==before&&curve_host::AuthoringUnchanged(*f.app,pristine),"sparse explicit planning failure preserves live native data and captured authoring");
            if(result==MMS_OK){completed=bool(change);break;}
            ++failures;Check(result==MMS_E_MEMORY&&!change,"sparse explicit planning OOM leaves empty result");
        }
        Check(completed&&failures>4,"sparse explicit planning sweep reaches complete owned frame intent");completed=false;failures=0;
        for(long allowance=0;allowance<2048;++allowance){std::unique_ptr<plugins::PreparedHistoryChange> candidate;
            failAfter=allowance;const auto result=change->Prepare(true,candidate);failAfter=-1;
            Check(f.Digest()==before&&curve_host::AuthoringUnchanged(*f.app,pristine),"sparse authoring prepare OOM or cancellation leaves all families untouched");
            if(result==MMS_OK){completed=candidate&&candidate->IsCurrent();break;}
            ++failures;Check(result==MMS_E_MEMORY&&!candidate,"sparse authoring preparation reports OOM without partial publication");
        }
        Check(completed&&failures>4,"sparse explicit prepare sweep reaches detached forced-key targets");
        plugins::SharedHistory limited(f.storage,{},16,change->RetainedBytes()-1);std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(limited.PrepareNative("sparse global budget",change,transaction)==MMS_E_MEMORY&&!transaction&&f.Digest()==before&&curve_host::AuthoringUnchanged(*f.app,pristine),
            "sparse frame vector and retained authoring cannot bypass history byte budget");
        Check(f.history.PrepareNative("stale sparse global",change,transaction)==MMS_OK,"reserve sparse global change before external authoring replacement");
        auto channels=GSparseRead(f,family);channels[GSparseLane(family)].track.keys.front().outWeight=.23;
        Check(curve_host::Write(*f.app,GSparseKind(family),0,channels,error),"replace sparse live authoring after preparation");
        const auto stale=f.Digest();const auto external=GSparseSnapshot(f,family);
        Check(f.history.CommitNative(std::move(transaction))==MMS_E_STALE&&f.Digest()==stale&&GSparseSnapshot(f,family)==external&&!f.history.UndoCount(),
            "stale sparse authoring transaction neither publishes forced keys nor overwrites another author");
    }
}
void NativeGlobalSparseRegistrationChecks(){
    std::puts("  global sparse registration: same-valued explicit frame intent");GSparseRegistrationRoundTrip();
    std::puts("  global sparse registration: family and aggregate paste");GSparsePasteRoundTrip();
    std::puts("  global sparse registration: immutable VMD frame intent");GSparseVmdRoundTrip();
    std::puts("  global sparse registration: OOM budget and stale guards");GSparseFailures();
}
}
