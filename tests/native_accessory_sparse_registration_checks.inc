// Include after native_vmd_accessory_checks.inc and all accessory/global fixtures.
namespace mikudancestudio { int PasteAccessoryKeyRecord(MMDApp*,void*,int); }
namespace {
using AccessorySparseChannels=std::vector<curve_editor::Channel>;
AccessorySparseChannels AccessorySparseRead(MMDApp& app){
    AccessorySparseChannels channels;std::wstring error;
    Check(curve_host::Read(app,curve_host::Kind::Accessory,7,channels,error),"read accessory sparse authored channels");return channels;
}
bool AccessorySparseSame(const AccessorySparseChannels& a,const AccessorySparseChannels& b){
    if(a.size()!=b.size())return false;
    for(std::size_t c=0;c<a.size();++c){
        if(a[c].id!=b[c].id||a[c].track.discrete!=b[c].track.discrete||a[c].nativeControls!=b[c].nativeControls||a[c].controlGeometry!=b[c].controlGeometry||a[c].track.keys.size()!=b[c].track.keys.size())return false;
        for(std::size_t i=0;i<a[c].track.keys.size();++i){const auto& x=a[c].track.keys[i];const auto& y=b[c].track.keys[i];
            if(x.frame!=y.frame||x.value!=y.value||x.inSlope!=y.inSlope||x.outSlope!=y.outSlope||x.inWeight!=y.inWeight||x.outWeight!=y.outWeight||x.interpolation!=y.interpolation||x.tangent!=y.tangent)return false;}
    }return true;
}
bool AccessorySparseHas(const curve_editor::Channel& channel,int frame){return std::any_of(channel.track.keys.begin(),channel.track.keys.end(),[&](const auto& key){return key.frame==frame;});}
AccessorySparseChannels AccessorySparseSeed(AccessoryRegistrationFixture& f,const std::vector<int>& frames){
    auto channels=AccessorySparseRead(*f.app);Check(channels.size()==12,"accessory sparse fixture exposes all twelve authoring lanes");
    auto& keys=channels[0].track.keys;keys.erase(std::remove_if(keys.begin(),keys.end(),[&](const auto& key){return std::find(frames.begin(),frames.end(),key.frame)!=frames.end();}),keys.end());
    channels[1].track.keys[0].tangent=animation_curve::Tangent::Free;channels[1].track.keys[0].outWeight=.21;channels[1].track.keys[0].outSlope=.37;
    std::wstring error;Check(curve_host::Write(*f.app,curve_host::Kind::Accessory,7,channels,error),"seed sparse authoring without removing the union native frame");
    auto seeded=AccessorySparseRead(*f.app);
    for(int frame:frames)Check(!AccessorySparseHas(seeded[0],frame)&&GlobalPasteAt(f.app->AccessoryKeys(7),unsigned(frame)).frame==unsigned(frame),"native accessory union frame survives a deleted per-channel authored key");
    return seeded;
}
void AccessorySparseSetPose(AccessoryRegistrationFixture& f,unsigned frame){
    const auto& key=GlobalPasteAt(f.app->AccessoryKeys(7),frame);auto& object=f.accessory;
    std::memcpy(object.position,key.position,sizeof(object.position));std::memcpy(object.rotation,key.rotation,sizeof(object.rotation));
    object.visible=key.visible;object.shadowEnabled=key.shadowEnabled;object.parentModel=key.parentModel;object.parentBone=key.parentBone;object.scale=key.scale;object.opacity=key.opacity;
}
plugins::GlobalPasteSource AccessorySparseSource(AccessoryRegistrationFixture& f,unsigned offset,const std::vector<int>& frames){
    plugins::GlobalPasteSource source;source.frameOffset=offset;
    for(int frame:frames){const auto& key=GlobalPasteAt(f.app->AccessoryKeys(7),unsigned(frame));mdl::AccessoryClipboardKey item{};
        item.slot=7;item.frameOffset=unsigned(frame)-offset;item.visible=key.visible;item.shadowEnabled=key.shadowEnabled;item.parentModel=key.parentModel;item.parentBone=key.parentBone;
        std::memcpy(item.position,key.position,sizeof(item.position));std::memcpy(item.rotation,key.rotation,sizeof(item.rotation));std::memcpy(item.reserved1,key.reserved,sizeof(item.reserved1));item.scale=key.scale;item.opacity=key.opacity;source.accessories.push_back(item);}
    return source;
}
void AccessorySparseCommit(AccessoryRegistrationFixture& f,const std::shared_ptr<plugins::HistoryChange>& change){
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
    Check(change&&f.history.PrepareNative("accessory sparse explicit",change,token)==MMS_OK,"reserve authored-only accessory registration transaction");
    Check(CommitRegistrationNoAllocation(f.history,std::move(token))==MMS_OK,"authored-only accessory publication allocates nothing");
}
void AccessorySparseFullFrames(const AccessorySparseChannels& channels,const std::vector<int>& frames){
    for(const auto& channel:channels)for(int frame:frames)Check(AccessorySparseHas(channel,frame),"explicit accessory registration restores its target in every authored lane");
}
void AccessorySparseLegacyChecks(){
    std::puts("  accessory sparse authoring: unbound exact-hit paste/VMD and bound notification isolation");
    for(bool vmd:{false,true}){
        AccessoryRegistrationFixture f;f.app->AccessoryKeys(7)[5].frame=12;
        std::memset(f.accessory.name,0,sizeof(f.accessory.name));std::memcpy(f.accessory.name,"legacy-sparse",13);
        const auto before=AccessorySparseSeed(f,{12});f.app->CurrentFrame()=7;
        Check(!plugins::BoundNativePoseHistory(f.app.get()),"legacy sparse accessory test uses the unbound production path");
        const auto native=AccessoryRegistrationKeys(*f.app);const auto source=AccessorySparseSource(f,7,{12});auto item=source.accessories[0];
        Check(item.frameOffset==5&&!AccessorySparseHas(before[0],12)&&AccessorySparseHas(before[1],12),"legacy sparse fixture retains native frame twelve and another lane while first lane is absent");
        if(vmd){
            accessory_motion_io::Track track;track.name="legacy-sparse";accessory_motion_io::Key key;
            key.frame=item.frameOffset;key.visible=item.visible;key.shadowEnabled=item.shadowEnabled;key.parentModel=item.parentModel;key.parentBone=item.parentBone;
            std::memcpy(key.position,item.position,sizeof(key.position));std::memcpy(key.rotation,item.rotation,sizeof(key.rotation));key.scale=item.scale;key.opacity=item.opacity;track.keys.push_back(key);
            const auto bytes=accessory_motion_io::Encode({track});std::vector<accessory_motion_io::Track> decoded;
            Check(accessory_motion_io::Decode(bytes.data(),bytes.size(),decoded)&&decoded.size()==1,"decode actual accessory VMD wire block for legacy Apply");
            Check(accessory_motion_io::Apply(f.app.get(),decoded)==1,"legacy accessory VMD Apply matches target and reaches exact-hit registrar");
        }else Check(PasteAccessoryKeyRecord(f.app.get(),&item,0)==1,"legacy low-level accessory paste accepts the same native frame payload");
        const auto after=AccessorySparseRead(*f.app);AccessorySparseFullFrames(after,{12});
        auto actual=AccessoryRegistrationKeys(*f.app);auto expected=native;for(auto& key:actual)key.selected=0;for(auto& key:expected)key.selected=0;
        Check(actual.size()==expected.size()&&!std::memcmp(actual.data(),expected.data(),actual.size()*sizeof(mdl::AccessoryKey)),"unbound sparse repair changes authoring without changing any native frame payload or links");
        Check(after[1].track.keys[0].outSlope==before[1].track.keys[0].outSlope&&after[1].track.keys[0].outWeight==before[1].track.keys[0].outWeight&&!f.history.UndoCount(),"legacy sparse repair preserves unrelated handles and does not invent a separate history journal");
    }
    {
        AccessoryRegistrationFixture f;f.app->AccessoryKeys(7)[5].frame=12;const auto before=AccessorySparseSeed(f,{12});const auto keys=AccessoryRegistrationKeys(*f.app);
        {plugins::NativePoseTestBinding binding(*f.app,f.history);
            curve_host::NotifyLegacyRegistered(f.app.get(),curve_host::Kind::Accessory,7,12);
            Check(AccessorySparseSame(before,AccessorySparseRead(*f.app))&&AccessoryRegistrationKeysEqual(*f.app,keys)&&!f.history.UndoCount(),"legacy bridge is strictly read-only when the common history binding is active");}
        curve_host::NotifyRegistered(f.app.get(),curve_host::Kind::Accessory,7);
        Check(AccessorySparseSame(before,AccessorySparseRead(*f.app)),"ordinary curve notification never repairs authored keys after a controlled commit");
    }
}

void NativeAccessorySparseRegistrationChecks(){
    using namespace plugins;AccessorySparseLegacyChecks();std::puts("  accessory sparse authoring: same native registration is a real authored edit");
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly})for(int frame:{10,30}){
        AccessoryRegistrationFixture f;const auto before=AccessorySparseSeed(f,{10,30});f.app->CurrentFrame()=frame;AccessorySparseSetPose(f,unsigned(frame));
        const auto keys=AccessoryRegistrationKeys(*f.app);const auto marks=AccessoryRegistrationMarks(*f.app);const auto pose=f.accessory;
        Check(f.history.SetMode(mode)==MMS_OK,"configure sparse explicit accessory recording mode");std::shared_ptr<HistoryChange> change;
        Check(PlanAccessoryRegistration(*f.app,7,AccessoryRegistrationInputOf(f.accessory),change)==MMS_OK&&change&&AccessoryRegistrationKeysEqual(*f.app,keys)&&AccessorySparseSame(before,AccessorySparseRead(*f.app)),"same native payload plans missing authored registration without live writes");
        AccessorySparseCommit(f,change);const auto after=AccessorySparseRead(*f.app);AccessorySparseFullFrames(after,{frame});
        Check(!AccessorySparseHas(after[0],frame==10?30:10),"explicit registration does not restore unrelated sparse frames");
        Check(after[1].track.keys[0].outWeight==before[1].track.keys[0].outWeight&&after[1].track.keys[0].outSlope==before[1].track.keys[0].outSlope,"registration retains existing neighboring authored handles");
        Check(f.history.UndoCount()==1&&AccessoryRegistrationPoseEqual(f.accessory,pose),"authored-only explicit registration creates one node in either recording mode");
        std::shared_ptr<HistoryChange> noop;Check(PlanAccessoryRegistration(*f.app,7,AccessoryRegistrationInputOf(f.accessory),noop)==MMS_OK&&!noop,"repeat same registration becomes a true no-op once authoring is complete");
        f.app->CurrentFrame()=19;Check(f.history.History(false)==MMS_OK&&AccessorySparseSame(before,AccessorySparseRead(*f.app))&&AccessoryRegistrationKeysEqual(*f.app,keys)&&AccessoryRegistrationMarks(*f.app)==marks,"undo restores exact sparse keys handles native pool and global marks");
        f.app->CurrentFrame()=23;Check(f.history.History(true)==MMS_OK&&AccessorySparseSame(after,AccessorySparseRead(*f.app))&&f.app->CurrentFrame()==23,"redo reconstructs explicit authored frames at a changed playhead");
    }
    std::puts("  accessory sparse authoring: independent-offset paste and immutable VMD share registration");
    for(bool vmd:{false,true})for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly}){
        AccessoryRegistrationFixture f;const auto before=AccessorySparseSeed(f,{10,30});Check(f.history.SetMode(mode)==MMS_OK,"configure sparse paste recording mode");
        f.app->CurrentFrame()=7;f.app->CameraMode()=1;const auto keys=AccessoryRegistrationKeys(*f.app);const auto marks=AccessoryRegistrationMarks(*f.app);
        auto source=AccessorySparseSource(f,5,{10,30});source.accessories.push_back(source.accessories[0]);std::shared_ptr<HistoryChange> change;
        VmdDecodeFile file;
        if(vmd){
            std::memset(f.accessory.name,0,sizeof(f.accessory.name));std::memcpy(f.accessory.name,"sparse",6);
            VmdDecodeBytes bytes;VmdDecodeText(bytes,"Vocaloid Motion Data 0002",30);VmdDecodeText(bytes,"\x83\x4a\x83\x81\x83\x89\x81\x45\x8f\xc6\x96\xbe",20);
            for(unsigned section=0;section<6;++section)VmdDecodePut(bytes,0u);
            accessory_motion_io::Track track;track.name="sparse";
            for(const auto& item:source.accessories){accessory_motion_io::Key key;key.frame=item.frameOffset+source.frameOffset-unsigned(f.app->CurrentFrame());key.visible=item.visible;key.shadowEnabled=item.shadowEnabled;key.parentModel=item.parentModel;key.parentBone=item.parentBone;key.scale=item.scale;key.opacity=item.opacity;std::memcpy(key.position,item.position,sizeof(key.position));std::memcpy(key.rotation,item.rotation,sizeof(key.rotation));track.keys.push_back(key);}
            // The accessory wire codec requires sorted unique frames.
            track.keys.pop_back();const auto tail=accessory_motion_io::Encode({track});bytes.insert(bytes.end(),tail.begin(),tail.end());file.Write(bytes);
            Check(PlanGlobalVmdImport(*f.app,nullptr,file.path.c_str(),change)==MMS_OK&&change,"same-valued VMD accessory records restore sparse authoring in one private transaction");
        }else Check(PlanGlobalKeyPaste(*f.app,source,change)==MMS_OK&&change,"same-valued paste with offset independent of playhead restores sparse authoring");
        Check(AccessoryRegistrationKeysEqual(*f.app,keys)&&AccessorySparseSame(before,AccessorySparseRead(*f.app)),"paste/import planning never publishes sparse authoring early");
        AccessorySparseCommit(f,change);const auto after=AccessorySparseRead(*f.app);AccessorySparseFullFrames(after,{10,30});Check(f.history.UndoCount()==1&&f.app->CurrentFrame()==7,"all explicit accessory frames publish in one node and preserve playhead");
        std::shared_ptr<HistoryChange> noop;
        Check((vmd?PlanGlobalVmdImport(*f.app,nullptr,file.path.c_str(),noop):PlanGlobalKeyPaste(*f.app,source,noop))==MMS_OK&&!noop,"identical paste/import becomes a no-op after restoring all authored keys");
        source={};if(vmd)Check(DeleteFileW(file.path.c_str()),"delete sparse accessory VMD source before history traversal");
        f.app->CurrentFrame()=22;Check(f.history.History(false)==MMS_OK&&AccessorySparseSame(before,AccessorySparseRead(*f.app))&&AccessoryRegistrationKeysEqual(*f.app,keys)&&AccessoryRegistrationMarks(*f.app)==marks,"paste/import undo restores exact sparse authored state and native selection");
        f.app->CurrentFrame()=24;f.app->CameraMode()=0;f.app->SelectedAccessorySlot()=8;
        Check(f.history.History(true)==MMS_OK&&AccessorySparseSame(after,AccessorySparseRead(*f.app))&&f.app->CurrentFrame()==24,"paste/import redo owns original explicit frames after source destruction and selection change");
    }
    std::puts("  accessory sparse authoring: memory budget OOM cancellation and author revision guards");
    for(bool paste:{false,true}){
        AccessoryRegistrationFixture f;const auto before=AccessorySparseSeed(f,{10});f.app->CurrentFrame()=10;AccessorySparseSetPose(f,10);
        const auto keys=AccessoryRegistrationKeys(*f.app);const auto marks=AccessoryRegistrationMarks(*f.app);const auto pose=f.accessory;
        const auto source=AccessorySparseSource(f,5,{10});const auto input=AccessoryRegistrationInputOf(f.accessory);std::shared_ptr<HistoryChange> change;bool ready=false;
        for(long allowance=0;allowance<2048;++allowance){failAfter=allowance;const auto result=paste?PlanGlobalKeyPaste(*f.app,source,change):PlanAccessoryRegistration(*f.app,7,input,change);failAfter=-1;
            Check(AccessoryRegistrationKeysEqual(*f.app,keys)&&AccessorySparseSame(before,AccessorySparseRead(*f.app))&&AccessoryRegistrationMarks(*f.app)==marks&&AccessoryRegistrationPoseEqual(f.accessory,pose),"sparse plan allocation failure preserves native authoring selection and pose");
            if(result==MMS_OK){ready=bool(change);break;}Check(result==MMS_E_MEMORY&&!change,"sparse plan OOM returns no partial record");}
        Check(ready,"sparse authored-only plan allocation sweep reaches success");ready=false;
        for(long allowance=0;allowance<2048;++allowance){std::unique_ptr<PreparedHistoryChange> candidate;failAfter=allowance;const auto result=change->Prepare(true,candidate);failAfter=-1;
            Check(AccessoryRegistrationKeysEqual(*f.app,keys)&&AccessorySparseSame(before,AccessorySparseRead(*f.app))&&AccessoryRegistrationMarks(*f.app)==marks&&AccessoryRegistrationPoseEqual(f.accessory,pose),"sparse prepared target allocation and cancellation never publish partial authoring");
            if(result==MMS_OK){ready=bool(candidate);candidate.reset();break;}Check(result==MMS_E_MEMORY&&!candidate,"sparse prepare OOM returns no partial candidate");}
        Check(ready,"sparse candidate allocation sweep reaches complete author target");
        SharedHistory limited(f.storage,{},32,1);std::unique_ptr<SharedHistory::NativeTransaction> token;
        Check(limited.PrepareNative("sparse authored budget",change,token)==MMS_E_MEMORY&&!token&&AccessorySparseSame(before,AccessorySparseRead(*f.app)),"retained sparse author seed and explicit frames participate in history memory admission");
        Check(f.history.PrepareNative("sparse cancel",change,token)==MMS_OK,"reserve cancelable sparse author transaction");token.reset();
        Check(!f.history.UndoCount()&&AccessorySparseSame(before,AccessorySparseRead(*f.app)),"canceling sparse transaction leaves deleted authored keys absent");
        Check(f.history.PrepareNative("sparse author stale",change,token)==MMS_OK,"prepare sparse transaction before a competing curve author changes");
        auto replacement=before;replacement[1].track.keys[0].outSlope+=.125;std::wstring error;Check(curve_host::Write(*f.app,curve_host::Kind::Accessory,7,replacement,error),"publish competing curve author without changing native key values");const auto current=AccessorySparseRead(*f.app);
        Check(f.history.CommitNative(std::move(token))==MMS_E_STALE&&!f.history.UndoCount()&&AccessorySparseSame(current,AccessorySparseRead(*f.app)),"stale sparse transaction never overwrites the competing author");
    }
}
}
