#include "../src/plugins/native_global_paste.hpp"
namespace mikumikustudio { int PasteAccessoryKeyRecord(MMDApp*,void*,int); }
namespace {
struct AccessoryPasteFixture : AccessoryRegistrationFixture {
    std::array<mdl::AccessoryClipboardKey,4> clipboard{};
    AccessoryPasteFixture() {
        for(unsigned i=0;i<clipboard.size();++i) {
            auto& key=clipboard[i];key.slot=i%2?8:7;key.frameOffset=i<2?0:i==2?10:20;
            key.parentModel=-1;key.visible=key.shadowEnabled=1;
            key.position[0]=1.25f+float(i);key.position[1]=-float(i);
            key.rotation[1]=.27f*float(i+1);key.scale=.5f+.25f*float(i);key.opacity=.25f+.1f*float(i);
            key.reserved1[0]=std::uint8_t(0xd0+i);key.reserved1[1]=std::uint8_t(0xe0+i);
        }
        app->AccessoryClipboard()=clipboard.data();app->ClipboardCounts()={};app->ClipboardCounts().accessories=unsigned(clipboard.size());
        app->CameraMode()=1;app->CurrentFrame()=20;
    }
    ~AccessoryPasteFixture() { app->AccessoryClipboard()=nullptr;app->ClipboardCounts()={}; }
    plugins::GlobalPasteSource Source()const {
        plugins::GlobalPasteSource source;source.frameOffset=unsigned(app->CurrentFrame());
        source.accessories.assign(clipboard.begin(),clipboard.end());return source;
    }
    std::shared_ptr<plugins::HistoryChange> Plan() {
        std::shared_ptr<plugins::HistoryChange> result;
        Check(plugins::PlanGlobalKeyPaste(*app,result)==MMS_OK&&result,"global accessory paste plans an atomic record");return result;
    }
};
void CommitAccessoryPaste(AccessoryPasteFixture& f,const std::shared_ptr<plugins::HistoryChange>& change) {
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
    Check(f.history.PrepareNative("accessory clipboard paste",change,tx)==MMS_OK,"global accessory paste prepares every private participant");
    Check(CommitRegistrationNoAllocation(f.history,std::move(tx))==MMS_OK,"global accessory paste publishes without allocations");
}
void NativeAccessoryPasteChecks() {
    std::puts("  accessory paste: legacy payload pose and multi-object parity");
    for(int frame:{0,10,20,60})for(bool bound:{false,true}) {
        AccessoryPasteFixture f,oracle;f.app->CurrentFrame()=oracle.app->CurrentFrame()=frame;
        if(bound)for(auto* fixture:{&f,&oracle})for(auto& record:fixture->clipboard){record.parentModel=1;record.parentBone=1;}
        const auto first=AccessoryRegistrationKeys(*f.app,7),second=AccessoryRegistrationKeys(*f.app,8);
        const auto marks=AccessoryRegistrationMarks(*f.app);const auto pose=f.accessory,other=f.other;
        const auto model=f.first.Digest();auto* firstPool=f.app->AccessoryKeys(7);auto* secondPool=f.app->AccessoryKeys(8);
        auto change=f.Plan();std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
        Check(f.history.PrepareNative("accessory parity",change,tx)==MMS_OK&&f.app->AccessoryKeys(7)==firstPool&&
            f.app->AccessoryKeys(8)==secondPool&&AccessoryRegistrationKeysEqual(*f.app,first,7)&&
            AccessoryRegistrationKeysEqual(*f.app,second,8)&&AccessoryRegistrationMarks(*f.app)==marks&&
            AccessoryRegistrationPoseEqual(f.accessory,pose)&&AccessoryRegistrationPoseEqual(f.other,other)&&f.app->LastRegisteredFrame()==15,
            "two accessory participants prepare pools poses parents and selection without live mutation");
        Check(CommitRegistrationNoAllocation(f.history,std::move(tx))==MMS_OK,"two accessory participants publish as one allocation-free transaction");
        ClearGlobalFrameSelection(oracle.app.get());
        for(auto& record:oracle.clipboard)Check(PasteAccessoryKeyRecord(oracle.app.get(),&record,0)==1,"native accessory oracle pastes clipboard record");
        ApplyAccessoryTrack(oracle.app.get(),7);ApplyAccessoryTrack(oracle.app.get(),8);
        for(int slot:{7,8}) {
            const auto expected=AccessoryRegistrationKeys(*oracle.app,slot);
            if(!AccessoryRegistrationKeysEqual(*f.app,expected,slot)) {
                const auto* actual=reinterpret_cast<const unsigned char*>(f.app->AccessoryKeys(slot));
                const auto* reference=reinterpret_cast<const unsigned char*>(expected.data());
                for(std::size_t byte=0;byte<expected.size()*sizeof(mdl::AccessoryKey);++byte)if(actual[byte]!=reference[byte]) {
                    std::printf("  accessory parity offset=%d bound=%d slot=%d key=%zu byte=%zu actual=%u expected=%u\n",
                        frame,int(bound),slot,byte/sizeof(mdl::AccessoryKey),byte%sizeof(mdl::AccessoryKey),unsigned(actual[byte]),unsigned(reference[byte]));break;
                }
            }
            Check(AccessoryRegistrationKeysEqual(*f.app,expected,slot),"accessory paste key links payload reserved bytes and marks match actual registrar");
        }
        Check(AccessoryRegistrationPoseEqual(f.accessory,oracle.accessory),"first accessory paste pose matches actual sampler");
        Check(AccessoryRegistrationPoseEqual(f.other,oracle.other),"second accessory paste pose matches actual sampler");
        Check(AccessoryRegistrationMarks(*f.app)==AccessoryRegistrationMarks(*oracle.app),"accessory paste global selection matches clear-then-register");
        Check(f.app->LastRegisteredFrame()==oracle.app->LastRegisteredFrame(),"accessory paste watermark matches real ordinary paste");
        Check(f.history.UndoCount()==1&&f.first.Digest()==model,"multi-accessory paste is one node and does not touch model dirty authoring");
        const auto afterFirst=AccessoryRegistrationKeys(*f.app,7),afterSecond=AccessoryRegistrationKeys(*f.app,8);
        f.clipboard={};f.app->ClipboardCounts()={};f.app->CurrentFrame()=17;
        Check(f.history.History(false)==MMS_OK&&AccessoryRegistrationKeysEqual(*f.app,first,7)&&
            AccessoryRegistrationKeysEqual(*f.app,second,8)&&AccessoryRegistrationMarks(*f.app)==marks,
            "multi-accessory undo restores keys and original global marks after clipboard replacement");
        f.app->CurrentFrame()=oracle.app->CurrentFrame()=25;
        ApplyAccessoryTrack(oracle.app.get(),7);ApplyAccessoryTrack(oracle.app.get(),8);
        Check(f.history.History(true)==MMS_OK&&AccessoryRegistrationKeysEqual(*f.app,afterFirst,7)&&
            AccessoryRegistrationKeysEqual(*f.app,afterSecond,8)&&AccessoryRegistrationPoseEqual(f.accessory,oracle.accessory)&&
            AccessoryRegistrationPoseEqual(f.other,oracle.other),"multi-accessory redo samples retained step transforms and linear scale opacity at new playhead");
    }
    std::puts("  accessory paste: growth cancellation no-op and guards");
    {
        AccessoryPasteFixture f;
        for(int slot:{7,8}) {
            auto* keys=f.app->AccessoryKeys(slot);std::memset(keys,0,8*sizeof(*keys));
            for(unsigned i=0;i<8;++i){keys[i].frame=i*10;keys[i].previous=i?i-1:0;keys[i].next=i<7?i+1:0;
                keys[i].parentModel=-1;keys[i].scale=keys[i].opacity=1;}
        }
        f.app->CurrentFrame()=90;const auto before=AccessoryRegistrationKeys(*f.app),other=AccessoryRegistrationKeys(*f.app,8);
        auto* pool=f.app->AccessoryKeys(7);const auto identity=mdl::KeyPoolIdentity(pool);
        auto source=f.Source();plugins::GlobalPasteFamilyResult part;
        bool planned=false;
        for(long allowance=0;allowance<1024&&!planned;++allowance) {
            failAfter=allowance;const auto status=plugins::PlanAccessoryKeyPastePart(*f.app,7,source,part);failAfter=-1;
            Check(f.app->AccessoryKeys(7)==pool&&AccessoryRegistrationKeysEqual(*f.app,before)&&
                AccessoryRegistrationKeysEqual(*f.app,other,8)&&!f.history.UndoCount(),"accessory planning faults leave both live objects and key pools untouched");
            if(status==MMS_OK){planned=true;part={};}
            else Check(status==MMS_E_MEMORY&&!part.part,"accessory planning allocation fault returns no partial part");
        }
        Check(planned,"accessory planning sweep reaches owners parents and retained free-curve authoring");
        Check(plugins::PlanAccessoryKeyPastePart(*f.app,7,source,part)==MMS_OK&&part.part&&part.dataChanged&&
            part.maximumInsertedFrame==100,"accessory part reports only its own insertion watermark");
        bool prepared=false;
        for(long allowance=0;allowance<2048&&!prepared;++allowance) {
            std::unique_ptr<plugins::PreparedGlobalPastePart> candidate;failAfter=allowance;
            const auto status=part.part->Prepare(true,candidate);failAfter=-1;
            Check(f.app->AccessoryKeys(7)==pool&&AccessoryRegistrationKeysEqual(*f.app,before)&&
                AccessoryRegistrationKeysEqual(*f.app,other,8)&&!f.history.UndoCount(),"accessory part preparation and cancellation never publish growth or cross-object edits");
            if(status==MMS_OK){prepared=true;candidate.reset();}else Check(status==MMS_E_MEMORY&&!candidate,"accessory part allocation fault reports memory");
        }
        Check(prepared,"accessory part allocation sweep reaches growth pose and curve candidates");
        CommitAccessoryPaste(f,f.Plan());
        Check(mdl::KeyPoolIdentity(f.app->AccessoryKeys(7))==identity&&mdl::KeyPoolCapacity(f.app->AccessoryKeys(7))>8&&
            mdl::KeyPoolCapacity(f.app->AccessoryKeys(8))>8,"both full accessory pools grow privately with preserved identity");
        Check(f.history.History(false)==MMS_OK&&AccessoryRegistrationKeysEqual(*f.app,before)&&AccessoryRegistrationKeysEqual(*f.app,other,8)&&
            !f.app->AccessoryKeys(7)[8].selected&&!f.app->AccessoryKeys(8)[8].selected,
            "grown accessory undo restores captured marks and clears its own new tail slots");
    }
    {
        AccessoryPasteFixture f;auto source=f.Source();plugins::GlobalPasteFamilyResult result;
        auto* sourcePool=f.app->AccessoryKeys(7);
        Check(plugins::PlanAccessoryKeyPastePart(*f.app,7,source,result)==MMS_OK&&result.part,"accessory stale fixture plans");
        std::unique_ptr<plugins::PreparedGlobalPastePart> candidate;
        Check(result.part->Prepare(true,candidate)==MMS_OK,"accessory stale fixture prepares");
        f.app->LightKeys()[7].selected=9;++f.app->LastRegisteredFrame();
        Check(candidate->IsCurrent(),"accessory part does not own shared selection or last-frame state");
        ++f.accessory.position[1];Check(!candidate->IsCurrent(),"accessory part detects changed own pose");--f.accessory.position[1];candidate.reset();
        auto* object=f.app->AccessorySlot(7);f.app->AccessorySlot(7)=&f.other;
        Check(result.part->Prepare(true,candidate)==MMS_E_STALE&&!candidate,"accessory part rejects object-slot replacement");f.app->AccessorySlot(7)=object;
        source.accessories[0].parentModel=254;
        Check(plugins::PlanAccessoryKeyPastePart(*f.app,7,source,result)==MMS_E_ARGUMENT&&!result.part&&f.app->AccessoryKeys(7)==sourcePool,
            "missing parent in one accessory record rejects the whole part without detaching or skipping");
        source=f.Source();source.accessories[0].scale=std::numeric_limits<float>::infinity();
        Check(plugins::PlanAccessoryKeyPastePart(*f.app,7,source,result)==MMS_E_ARGUMENT&&!result.part,
            "nonfinite accessory payload rejects before publication");
    }
    {
        AccessoryPasteFixture f;CommitAccessoryPaste(f,f.Plan());
        auto source=f.Source();plugins::GlobalPasteFamilyResult result;
        Check(plugins::PlanAccessoryKeyPastePart(*f.app,7,source,result)==MMS_OK&&result.part&&!result.dataChanged,
            "identical accessory payload is not counted as a data edit");
        std::shared_ptr<plugins::HistoryChange> noop;
        const auto before=AccessoryRegistrationKeys(*f.app);const auto count=f.history.UndoCount();
        Check(plugins::PlanGlobalKeyPaste(*f.app,noop)==MMS_OK&&!noop&&f.history.UndoCount()==count&&AccessoryRegistrationKeysEqual(*f.app,before),
            "all-identical accessory paste preserves history and marks");
        f.app->CurrentFrame()=37;auto change=f.Plan();plugins::SharedHistory limited(f.storage,{},32,1);
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
        Check(limited.PrepareNative("budget accessory paste",change,tx)==MMS_E_MEMORY&&!tx,
            "multi-accessory record payload must fit history budget before publication");
    }
    std::puts("  accessory paste: pristine free curves");
    {
        AccessoryPasteFixture f;std::wstring error;std::vector<curve_editor::Channel> channels;
        Check(curve_host::Read(*f.app,curve_host::Kind::Accessory,7,channels,error),"read accessory paste authoring channels");
        channels[0].track.keys[1].interpolation=animation_curve::Interpolation::Bezier;
        channels[0].track.keys[0].outWeight=.2;channels[0].track.keys[0].outSlope=1.2;
        Check(curve_host::Write(*f.app,curve_host::Kind::Accessory,7,channels,error),"seed weighted accessory paste free curve");
        CommitAccessoryPaste(f,f.Plan());std::vector<curve_editor::Channel> observed;
        Check(curve_host::Read(*f.app,curve_host::Kind::Accessory,7,observed,error),"ordinary reader reconciles pasted curve");
        Check(f.history.History(false)==MMS_OK,"accessory paste undo succeeds after curve reconciliation");
        Check(curve_host::Read(*f.app,curve_host::Kind::Accessory,7,observed,error)&&
            observed[0].track.keys[0].outWeight==.2&&observed[0].track.keys[0].outSlope==1.2,
            "accessory paste undo recovers original free-curve handles");
    }
}
}
