#include "../src/plugins/native_vmd_source.hpp"
namespace {
struct VmdAccessoryFixture:GlobalPasteFixture {
    mdl::AccessoryRecord second{};
    VmdAccessoryFixture(){
        std::memcpy(accessory.name,"same",5);accessory.order=2;
        second=accessory;second.order=1;app->AccessorySlot(3)=&second;plugins::SceneObjectIdentity(&second);
    }
    ~VmdAccessoryFixture(){history.Reset();history.Collect();plugins::ForgetSceneObject(&second);app->AccessorySlot(3)=nullptr;}
    std::vector<accessory_motion_io::Track> Tracks(unsigned keys=1){
        std::vector<accessory_motion_io::Track> result(3);result[0].name=result[1].name="same";result[2].name="unmatched";
        for(unsigned track=0;track<result.size();++track)for(unsigned i=0;i<keys;++i){accessory_motion_io::Key key;key.frame=i*5;key.position[0]=float((track+1)*10+i);key.rotation[1]=.13f*float(i);key.scale=1+.1f*float(i);key.opacity=.8f;result[track].keys.push_back(key);}return result;
    }
};
void VmdAccessoryTail(VmdGlobalFile& file,const std::vector<accessory_motion_io::Track>& tracks){const auto bytes=accessory_motion_io::Encode(tracks);file.bytes.insert(file.bytes.end(),bytes.begin(),bytes.end());file.Write();}
unsigned VmdAccessoryAt(MMDApp& app,unsigned slot,unsigned frame){const auto* keys=app.AccessoryKeys(slot);unsigned index=0;
    while(keys[index].frame<frame&&keys[index].next)index=keys[index].next;Check(keys[index].frame==frame,"imported accessory frame exists");return index;}
void NativeVmdAccessoryChecks(){
    std::puts("  VMD accessory: duplicate names panel order standard mix and deleted source");std::fflush(stdout);
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        VmdAccessoryFixture f;VmdGlobalFile file(f,2,true);VmdAccessoryTail(file,f.Tracks(4));f.ClearClipboard();
        const auto before=GlobalPasteKeys(*f.app);const auto aIdentity=mdl::KeyPoolIdentity(f.app->AccessoryKeys(3));const auto bIdentity=mdl::KeyPoolIdentity(f.app->AccessoryKeys(7));
        Check(f.history.SetMode(mode)==MMS_OK,"configure accessory VMD explicit history mode");std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanGlobalVmdImport(*f.app,nullptr,file.path.c_str(),change)==MMS_OK&&change&&before.Same(*f.app),"standard plus multiple accessory VMD privately plans one change");
        GlobalPasteCommit(f,change);
        Check(f.history.UndoCount()==1&&f.app->AccessoryKeys(3)[VmdAccessoryAt(*f.app,3,7)].position[0]==10&&f.app->AccessoryKeys(7)[VmdAccessoryAt(*f.app,7,7)].position[0]==20,
            "duplicate accessory source names bind in stable panel order and apply current frame offset once");
        Check(mdl::KeyPoolCapacity(f.app->AccessoryKeys(3))>3&&mdl::KeyPoolIdentity(f.app->AccessoryKeys(3))==aIdentity&&mdl::KeyPoolIdentity(f.app->AccessoryKeys(7))==bIdentity,
            "multi-key accessory VMD privately grows pools while preserving owner identities");
        const auto after=GlobalPasteKeys(*f.app);Check(DeleteFileW(file.path.c_str()),"delete VMD accessory source before history traversal");
        std::memcpy(f.second.name,"renamed",8);f.second.order=9;
        Check(f.history.History(false)==MMS_OK&&before.Same(*f.app)&&f.history.History(true)==MMS_OK&&after.Same(*f.app),
            "accessory VMD history owns resolved targets and does not rematch renamed reordered objects or reopen file");
    }
    std::puts("  VMD accessory: initial matching guards and parent normalization");std::fflush(stdout);
    {
        VmdAccessoryFixture f;VmdGlobalFile file(f,2,true);VmdAccessoryTail(file,f.Tracks());f.ClearClipboard();std::shared_ptr<plugins::HistoryChange> change;
        const auto before=GlobalPasteKeys(*f.app);
        Check(plugins::PlanGlobalVmdImport(*f.app,nullptr,file.path.c_str(),change)==MMS_OK&&change,"plan accessory mapping guard fixture");
        std::memcpy(f.second.name,"renamed",8);std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        Check(change->Prepare(true,candidate)==MMS_E_STALE&&!candidate&&before.Same(*f.app),"renaming before first publication refuses stale name matching");
        std::memcpy(f.second.name,"same",5);f.second.name[5]=0;
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        Check(f.history.PrepareNative("mapping",change,token)==MMS_OK,"prepare frozen accessory mapping");
        ++f.second.order;
        Check(f.history.CommitNative(std::move(token))==MMS_E_STALE&&before.Same(*f.app)&&!f.history.UndoCount(),"panel reorder after preparation refuses stale mapping without publication");
    }
    {
        VmdAccessoryFixture f;VmdGlobalFile file(f,2,true);auto tracks=f.Tracks();tracks[0].keys[0].parentModel=254;tracks[0].keys[0].parentBone=900;
        VmdAccessoryTail(file,tracks);f.ClearClipboard();std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanGlobalVmdImport(*f.app,nullptr,file.path.c_str(),change)==MMS_OK&&change,"missing accessory parent privately detaches");
        GlobalPasteCommit(f,change);const auto& key=f.app->AccessoryKeys(3)[VmdAccessoryAt(*f.app,3,7)];
        Check(key.parentModel==-1&&key.parentBone==0,"missing accessory parent normalizes both fields");
    }
    {
        VmdAccessoryFixture f;Fixture model;f.app->ModelSlot(0)=model.raw();plugins::SceneObjectIdentity(model.raw());
        VmdGlobalFile file(f,2,true);auto tracks=f.Tracks();tracks[0].keys[0].parentModel=0;tracks[0].keys[0].parentBone=900;
        VmdAccessoryTail(file,tracks);f.ClearClipboard();const GlobalPasteKeys before(*f.app);std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanGlobalVmdImport(*f.app,nullptr,file.path.c_str(),change)==MMS_E_ARGUMENT&&!change&&before.Same(*f.app),"existing accessory parent with invalid bone rejects all standard and accessory tracks");
        plugins::ForgetSceneObject(model.raw());f.app->ModelSlot(0)=nullptr;
    }
    {
        VmdAccessoryFixture f;VmdGlobalFile file(f,2,true);VmdAccessoryTail(file,f.Tracks(4));f.ClearClipboard();const GlobalPasteKeys before(*f.app);
        std::shared_ptr<plugins::HistoryChange> change;unsigned failures=0;bool success=false;
        for(long limit=0;limit<4096;++limit){failAfter=limit;const auto result=plugins::PlanGlobalVmdImport(*f.app,nullptr,file.path.c_str(),change);failAfter=-1;
            if(result==MMS_OK){success=bool(change);break;}++failures;Check(!change&&before.Same(*f.app),"accessory decode matching and growth OOM leaves live keys untouched");}
        Check(success&&failures>10,"accessory import allocation sweep reaches complete private change");
        plugins::SharedHistory budget(f.storage,{},32,1);std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        Check(budget.PrepareNative("accessory budget",change,token)==MMS_E_MEMORY&&!token&&before.Same(*f.app),"accessory mapping and growth count toward common history budget");
        Check(f.history.PrepareNative("accessory cancellation",change,token)==MMS_OK,"prepare cancelable complete accessory batch");token.reset();
        Check(before.Same(*f.app)&&!f.history.UndoCount(),"canceling accessory batch publishes no participant");
    }
}
}
