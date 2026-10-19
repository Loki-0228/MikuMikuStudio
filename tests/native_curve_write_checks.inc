#include "../src/plugins/native_curve_write.hpp"
#include "mikumikustudio/pmm_extension.hpp"
#include <limits>
namespace {
struct CurveWriteFixture {
    std::unique_ptr<MMDApp> app=std::make_unique<MMDApp>();mdl::AccessoryRecord accessory{};
    plugins::StorageStore storage;plugins::SharedHistory history{storage};
    CurveWriteFixture(){
        ::new(static_cast<void*>(&app->state)) MMDAppState{};app->EnglishUI()=2;
        app->CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>(8);app->LightKeys()=mdl::KeyPoolCreate<mdl::LightKey>(8);
        app->ShadowKeys()=mdl::KeyPoolCreate<mdl::SelfShadowKey>(8);app->GravityKeys()=mdl::KeyPoolCreate<mdl::GravityKey>(8);
        app->AccessoryKeys(0)=mdl::KeyPoolCreate<mdl::AccessoryKey>(8);app->AccessorySlot(0)=&accessory;
        accessory.parentModel=-1;accessory.scale=accessory.opacity=1;accessory.visible=1;accessory.order=0;app->AccessoryRenderSplitOrder()=1;
        plugins::SceneObjectIdentity(&accessory);app->CameraParentModel()=-1;
        for(unsigned i=0;i<3;++i){const unsigned frame=i==0?0:i==1?12:30;
            const auto links=[&](auto& key){key.frame=frame;key.previous=i?i-1:0;key.next=i<2?i+1:0;key.selected=std::uint8_t(i+1);};
            auto& c=app->CameraKeys()[i];links(c);c.eye[0]=float(i)*3.25f;c.eye[1]=float(i)*2.75f;c.fov=45+int(i)*4;c.distance=-30-float(i);c.parentModel=-1;
            for(int lane=0;lane<6;++lane){c.interpolation[0][lane]=20;c.interpolation[1][lane]=20;c.interpolation[2][lane]=107;c.interpolation[3][lane]=107;}
            auto& l=app->LightKeys()[i];links(l);l.direction[0]=float(i)*.137918f;for(int axis=0;axis<3;++axis)l.color[axis]=.5f;
            auto& s=app->ShadowKeys()[i];links(s);s.mode=1;s.distance=.02f+float(i)*.01f;
            auto& g=app->GravityKeys()[i];links(g);g.acceleration=9.8f+float(i);g.direction[1]=-1;g.noise=i;
            auto& a=app->AccessoryKeys(0)[i];links(a);a.parentModel=-1;a.position[0]=float(i);a.scale=1+.1f*i;a.opacity=1;a.visible=1;
        }
        app->CameraKeys()[7].reserved2[1]=43;app->LightKeys()[7].reserved1[2]=41;
        app->CameraPositionY()=99;app->CameraFov()=45;app->CameraDistance()=-30;app->LightColor()[0]=.5f;
        app->GravityMagnitude()=9.8f;app->CurrentFrame()=7;app->LastRegisteredFrame()=9;
        app->CameraTrackCursor()=app->LightTrackCursor()=app->ShadowTrackCursor()=app->GravityTrackCursor()=app->AccessoryTrackCursor(0)=2;
        app->CameraTrackActive()=app->LightTrackActive()=app->ShadowTrackActive()=app->GravityTrackActive()=app->AccessoryTrackActive(0)=1;
    }
    ~CurveWriteFixture(){history.Reset();history.Collect();curve_host::Reset(app.get());plugins::ForgetSceneObject(&accessory);
        mdl::KeyPoolDestroy(app->CameraKeys());mdl::KeyPoolDestroy(app->LightKeys());mdl::KeyPoolDestroy(app->ShadowKeys());mdl::KeyPoolDestroy(app->GravityKeys());mdl::KeyPoolDestroy(app->AccessoryKeys(0));app->AccessorySlot(0)=nullptr;}
    std::vector<unsigned char> Native(unsigned kind)const{
        const auto bytes=[](const auto* keys){const auto* first=reinterpret_cast<const unsigned char*>(keys);return std::vector<unsigned char>(first,first+mdl::KeyPoolCapacity(keys)*sizeof(*keys));};
        switch(kind){case 0:return bytes(app->CameraKeys());case 1:return bytes(app->LightKeys());case 2:return bytes(app->ShadowKeys());case 3:return bytes(app->GravityKeys());default:return bytes(app->AccessoryKeys(0));}
    }
    std::uint64_t Digest()const{
        std::uint64_t hash=1469598103934665603ull;const auto add=[&](const void* data,std::size_t n){const auto* bytes=static_cast<const unsigned char*>(data);while(n--){hash^=*bytes++;hash*=1099511628211ull;}};
        add(&app->state,sizeof app->state);add(&accessory,sizeof accessory);add(&app->SceneLight(),sizeof(D3DLIGHT9));
        for(unsigned kind=0;kind<5;++kind){const auto bytes=Native(kind);add(bytes.data(),bytes.size());}return hash;
    }
    std::vector<curve_editor::Channel> Channels(unsigned kind,bool change=true){std::vector<curve_editor::Channel> channels;std::wstring error;
        Check(curve_host::Read(*app,curve_host::Kind(kind),0,channels,error),"curve write fixture reads current authoring");
        if(change){const auto c=kind==2?1u:0u;channels[c].track.keys[1].value+=kind==2?.015:2.0;}return channels;}
    std::shared_ptr<plugins::HistoryChange> Plan(unsigned kind,const std::vector<curve_editor::Channel>& channels){std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanGlobalCurveWrite(*app,curve_host::Kind(kind),0,channels,change)==MMS_OK,"curve replacement plans successfully");return change;}
    void Commit(const std::shared_ptr<plugins::HistoryChange>& change){std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(history.PrepareNative("curves",change,transaction,plugins::SharedHistory::NativeIntent::ExplicitCommit)==MMS_OK,"prepare explicit curve replacement in shared history");
        failAfter=0;const auto status=history.CommitNative(std::move(transaction));failAfter=-1;Check(status==MMS_OK,"curve native pool author pose and cursor publish without allocation");}
};
void CurveWriteRoundTrip(){
    for(unsigned kind=0;kind<5;++kind)for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        CurveWriteFixture f,oracle;Check(f.history.SetMode(mode)==MMS_OK,"curve replacement history recording mode");
        const auto unchanged=f.Channels(kind,false);const auto before=f.Native(kind);const auto source=f.Digest();
        Check(!f.Plan(kind,unchanged)&&f.Digest()==source,"unchanged curve write is a private no-op");
        const auto input=f.Channels(kind);auto change=f.Plan(kind,input);Check(change&&f.Digest()==source,"curve replacement does not publish while planning");
        std::wstring error;Check(curve_host::Write(*oracle.app,curve_host::Kind(kind),0,input,error),"run original unbound mirror as a native-byte oracle");
        f.Commit(change);Check(f.Native(kind)==oracle.Native(kind)&&f.history.UndoCount()==1,"first explicit curve publication matches original mirror bytes in both modes");
        Check(f.app->CurrentFrame()==7&&f.app->CameraPositionY()==99,"curve publication retains playhead and unrelated unregistered camera components");
        const auto after=f.Native(kind);f.app->CurrentFrame()=20;
        Check(f.history.History(false)==MMS_OK&&f.Native(kind)==before&&!curve_host::Has(f.app.get(),curve_host::Kind(kind)),
            "undo restores original native bytes and absence of authored state at another playhead");
        Check(f.app->CurrentFrame()==20&&f.app->CameraPositionY()==99,"curve undo evaluates only owned pose components without seeking");
        Check(f.history.History(true)==MMS_OK&&f.Native(kind)==after&&curve_host::Has(f.app.get(),curve_host::Kind(kind)),"redo restores exact mirrored native data and authored state");
        auto second=f.Channels(kind);f.Commit(f.Plan(kind,second));
        Check(f.history.UndoCount()==2&&f.history.History(false)==MMS_OK&&f.Native(kind)==after,"later curve write undoes to earlier author lineage");
        Check(f.history.History(false)==MMS_OK&&f.Native(kind)==before,"earlier curve history remains traversable after later write undo");
        Check(f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK,"two replacement records redo in the common journal order");
    }
}
void CurveWriteRefusals(){
    for(unsigned kind=0;kind<5;++kind){CurveWriteFixture f;const auto channels=f.Channels(kind);const auto source=f.Digest();
        auto change=f.Plan(kind,channels);std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        Check(change->Prepare(true,candidate)==MMS_OK&&f.Digest()==source,"curve candidate preparation stays entirely private");candidate.reset();
        Check(f.Digest()==source&&f.history.UndoCount()==0,"cancelled curve candidate releases detached data without publishing");
        Check(f.history.Configure(plugins::SharedHistory::RecordingMode::ExplicitOnly,8,1)==MMS_OK,"tiny curve history budget");
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("curves",change,transaction)==MMS_E_MEMORY&&!transaction&&f.Digest()==source,"budget refusal leaves all five source families intact");
    }
    {
        CurveWriteFixture f;auto channels=f.Channels(0);auto change=f.Plan(0,channels);
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;Check(f.history.PrepareNative("curves",change,transaction)==MMS_OK,"prepare stale curve candidate");
        f.app->CurrentFrame()=8;const auto before=f.Digest();
        Check(f.history.CommitNative(std::move(transaction))==MMS_E_STALE&&f.Digest()==before&&f.history.UndoCount()==0,"final curve validation rejects a changed playhead without partial mutation");
    }
    {
        CurveWriteFixture f;auto channels=f.Channels(0);auto change=f.Plan(0,channels);f.app->CameraKeys()[7].reserved2[1]^=1;
        std::unique_ptr<plugins::PreparedHistoryChange> candidate;const auto before=f.Digest();
        Check(change->Prepare(true,candidate)==MMS_E_STALE&&!candidate&&f.Digest()==before,"initial source freezing includes unused native pool bytes");
    }
    {
        CurveWriteFixture f;auto channels=f.Channels(4);f.app->AccessoryKeys(1)=f.app->AccessoryKeys(0);std::shared_ptr<plugins::HistoryChange> change;
        const auto result=plugins::PlanGlobalCurveWrite(*f.app,curve_host::Kind::Accessory,0,channels,change);f.app->AccessoryKeys(1)=nullptr;
        Check(result==MMS_E_NOT_READY&&!change,"aliased accessory pools refuse before private mirroring");
    }
}
void CurveWriteFailures(){
    for(unsigned kind=0;kind<5;++kind){unsigned failed=0;
        for(long allocation=0;allocation<384;++allocation){CurveWriteFixture f;const auto channels=f.Channels(kind);const auto before=f.Digest();
            std::shared_ptr<plugins::HistoryChange> change;std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
            failAfter=allocation;auto result=plugins::PlanGlobalCurveWrite(*f.app,curve_host::Kind(kind),0,channels,change);
            if(result==MMS_OK)result=plugins::Boundary([&]{return f.history.PrepareNative("curves",change,transaction);});failAfter=-1;
            Check(f.Digest()==before&&f.history.UndoCount()==0&&f.history.RedoCount()==0,"every curve preparation allocation failure preserves application and journal");
            if(result==MMS_OK){Check(transaction&&failed>5,"curve failure sweep reaches a complete candidate");break;}
            Check(result==MMS_E_MEMORY&&!transaction,"curve allocation failures report memory and no publication token");++failed;
            Check(allocation<383,"curve allocation sweep terminates");
        }
    }
}
void CurveWriteStaleAuthoring(){
    CurveWriteFixture f;std::wstring error;auto seed=f.Channels(0);
    Check(curve_host::Write(*f.app,curve_host::Kind::Camera,0,seed,error),"seed an existing author before a legacy native edit");
    curve_host::AuthoringTable raw;Check(curve_host::CaptureAuthoring(*f.app,raw,error),"capture raw authored channels and baseline");
    f.app->CameraKeys()[1].eye[0]+=1.25f;
    const auto nativeBefore=f.Native(0);const auto source=f.Digest();
    plugins::NativePoseTestBinding binding(*f.app,f.history);
    auto channels=f.Channels(0,false);
    const auto originalValue=float(channels[0].sample(channels[0],f.app->CurrentFrame()).value);
    Check(curve_host::AuthoringUnchanged(*f.app,raw)&&f.Digest()==source,
        "bound canonical read reconciles a detached copy without changing raw author baseline or fingerprint");
    channels[0].track.keys[1].value+=.75;auto change=f.Plan(0,channels);
    std::unique_ptr<plugins::PreparedHistoryChange> candidate;
    Check(change->Prepare(true,candidate)==MMS_OK&&f.Digest()==source&&curve_host::AuthoringUnchanged(*f.app,raw),
        "preparing a replacement over stale native authoring leaves original raw state intact");
    candidate.reset();Check(f.Digest()==source&&curve_host::AuthoringUnchanged(*f.app,raw),"cancelling stale-author preparation restores no live state because it never published");
    f.Commit(change);const auto nativeAfter=f.Native(0);
    Check(f.history.History(false)==MMS_OK&&f.Native(0)==nativeBefore&&curve_host::AuthoringUnchanged(*f.app,raw)&&f.app->CameraPositionX()==originalValue,
        "undo restores exact raw authoring while evaluating the reconciled target copy at the current playhead");
    channels=f.Channels(0,false);Check(curve_host::AuthoringUnchanged(*f.app,raw),"post-undo bound read remains detached for the original stale baseline");
    Check(f.history.History(true)==MMS_OK&&f.Native(0)==nativeAfter,"redo accepts restored raw author lineage after detached canonical reads");
    f.app->CameraKeys()[1].next=1;std::vector<curve_editor::Channel> invalid;
    Check(!curve_host::Read(*f.app,curve_host::Kind::Camera,0,invalid,error),"bound read refuses an invalid native chain even when authored state exists");
}
void CurveWriteMetadataOwnership(){
    {
        CurveWriteFixture f;auto channels=f.Channels(0,false);std::wstring error;std::size_t index=0;
        Check(curve_host::InsertPreservingShape(channels,0,7,index,error),"prepare exact camera insertion with native peer interval wrappers");
        f.Commit(f.Plan(0,channels));
        Check(f.app->CameraPositionY()==99,"equivalent native interval metadata on an unchanged peer does not own its unregistered pose");
        Check(f.history.History(false)==MMS_OK&&f.app->CameraPositionY()==99&&f.history.History(true)==MMS_OK&&f.app->CameraPositionY()==99,
            "undo and redo of exact insertion preserve unregistered peer pose components");
    }
    {
        CurveWriteFixture f;auto channels=f.Channels(0,false);std::wstring error;std::size_t index=0;
        Check(curve_host::InsertPreservingShape(channels,0,6,index,error)&&curve_host::Write(*f.app,curve_host::Kind::Camera,0,channels,error),
            "seed authored camera with preserved intervals on unchanged peer");
        auto input=f.Channels(0,false);Check(!input[1].preservedCameraIntervals.empty(),"seed retains peer native source interval metadata");
        const auto beforeValue=float(input[1].sample(input[1],7).value);
        input[1].preservedCameraIntervals.front().controls[1]=0;input[1].preservedCameraIntervals.front().controls[3]=127;
        const auto afterValue=float(input[1].sample(input[1],7).value);Check(beforeValue!=afterValue,"metadata-only source control edit changes sampled value");
        const auto nativeBefore=f.Native(0);f.app->CameraPositionY()=99;f.app->CameraPositionZ()=123;
        f.Commit(f.Plan(0,input));
        Check(f.Native(0)==nativeBefore&&f.app->CameraPositionY()==afterValue&&f.app->CameraPositionZ()==123,
            "metadata-only replacement updates the owned sampled component without native key or unrelated pose edits");
        Check(f.history.History(false)==MMS_OK&&f.app->CameraPositionY()==beforeValue&&f.app->CameraPositionZ()==123,
            "metadata-only undo evaluates the original preserved source at the current playhead");
        Check(f.history.History(true)==MMS_OK&&f.app->CameraPositionY()==afterValue&&f.app->CameraPositionZ()==123,
            "metadata-only redo restores exact source interval semantics");
    }
}
void CurveWriteGravityNativeFallback(){
    CurveWriteFixture f;auto& a=f.app->GravityKeys()[0];auto& b=f.app->GravityKeys()[1];
    a.noise=-7;b.noise=4;a.acceleration=9.812345f;b.acceleration=12.312349f;
    const std::uint32_t leftMode=0x12345600u,rightMode=0x9abcde01u;
    std::memcpy(&a.noiseEnabled,&leftMode,sizeof leftMode);std::memcpy(&b.noiseEnabled,&rightMode,sizeof rightMode);
    auto channels=f.Channels(3,false);for(auto& key:channels[4].track.keys)key.value=2;channels[5].track.keys[1].value=0;
    const float fraction=7.f/12.f;const auto expectedNoise=int(fraction*float(b.noise-a.noise))+a.noise;
    f.app->GravityNoise()=-101;f.app->GravityNoiseModeWord()=0x76543201u;
    f.Commit(f.Plan(3,channels));
    Check(f.history.History(false)==MMS_OK&&f.app->GravityNoise()==expectedNoise&&f.app->GravityNoiseModeWord()==rightMode,
        "unauthored gravity undo preserves signed native noise truncation and all four bytes of the right-key mode");
    Check(f.history.History(true)==MMS_OK,"signed native gravity source remains redoable after exact fallback evaluation");
}
void CurveWriteAccessoryLifetime(){
    CurveWriteFixture f;const auto before=f.Native(4);f.Commit(f.Plan(4,f.Channels(4)));const auto after=f.Native(4);
    auto owner=plugins::RetainSceneObject(*f.app,MMS_OBJECT_ACCESSORY,0);const auto identity=plugins::SceneObjectIdentity(&f.accessory);
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(f.history.PrepareNative("remove",owner->CaptureRemoval(4096),transaction)==MMS_OK&&f.history.CommitNative(std::move(transaction))==MMS_OK&&!f.app->AccessorySlot(0),
        "interleave retained accessory deletion after authored curve replacement");
    Check(f.history.History(false)==MMS_OK&&f.app->AccessorySlot(0)==&f.accessory&&plugins::SceneObjectIdentity(&f.accessory).generation!=identity.generation,
        "accessory restoration retains object and pool identity with a newer generation");
    Check(f.history.History(false)==MMS_OK&&f.Native(4)==before&&f.history.History(true)==MMS_OK&&f.Native(4)==after,
        "curve replacement traverses a correctly restored accessory identity");
    Check(f.history.History(true)==MMS_OK&&!f.app->AccessorySlot(0)&&f.history.History(false)==MMS_OK,
        "later accessory deletion redo remains ordered and leaves fixture restored for teardown");
}
void CurveWriteInterleavedRegistration(){
    CurveWriteFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=f.Native(0);
    f.Commit(f.Plan(0,f.Channels(0)));const auto first=f.Native(0);
    f.app->CameraPositionX()=31.5f;std::shared_ptr<plugins::HistoryChange> registration;
    Check(plugins::PlanCameraRegistration(*f.app,18,false,registration)==MMS_OK&&registration,"prepare a native camera registration between two authored curve replacements");
    f.Commit(registration);const auto middle=f.Native(0);f.Commit(f.Plan(0,f.Channels(0)));const auto last=f.Native(0);
    Check(f.history.UndoCount()==3&&f.history.History(false)==MMS_OK&&f.Native(0)==middle,
        "latest authored replacement undoes across a native registration author lineage");
    Check(f.history.History(false)==MMS_OK&&f.Native(0)==first&&f.history.History(false)==MMS_OK&&f.Native(0)==before,
        "native registration then earlier authored replacement undo to the original source");
    Check(f.history.History(true)==MMS_OK&&f.Native(0)==first&&f.history.History(true)==MMS_OK&&f.Native(0)==middle&&
        f.history.History(true)==MMS_OK&&f.Native(0)==last,"curve native registration curve records redo in shared journal order");
}
void CurveWriteGrowth(){
    CurveWriteFixture f;auto channels=f.Channels(1,false);const auto before=f.Native(1);const auto identity=mdl::KeyPoolIdentity(f.app->LightKeys());
    for(int frame=1;frame<18;++frame){if(frame==12)continue;auto key=channels[0].track.keys.front();key.frame=frame;key.value=frame*.25;
        const auto at=std::lower_bound(channels[0].track.keys.begin(),channels[0].track.keys.end(),frame,[](const auto& point,int value){return point.frame<value;});
        channels[0].track.keys.insert(at,key);}
    f.Commit(f.Plan(1,channels));const auto after=f.Native(1);const auto capacity=mdl::KeyPoolCapacity(f.app->LightKeys());
    Check(capacity>8&&mdl::KeyPoolIdentity(f.app->LightKeys())==identity,"private curve replacement grows a native pool without changing its stable identity");
    Check(f.history.History(false)==MMS_OK&&mdl::KeyPoolCapacity(f.app->LightKeys())==capacity&&mdl::KeyPoolIdentity(f.app->LightKeys())==identity&&
        !std::memcmp(f.app->LightKeys(),before.data(),before.size()),"growth undo retains new allocation and restores every original native byte");
    Check(f.history.History(true)==MMS_OK&&f.Native(1)==after,"growth redo resolves the preserved pool identity rather than the old allocation address");
}
void CurveWriteProjectionRefresh(){
    CurveWriteFixture f;auto channels=f.Channels(0,false);channels[6].track.keys[1].value-=20;
    f.Commit(f.Plan(0,channels));Check(f.app->cameraProjectionRefreshPending,"camera distance replacement requests projection refresh without seeking");
    f.app->cameraProjectionRefreshPending=false;
    Check(f.history.History(false)==MMS_OK&&f.app->cameraProjectionRefreshPending,"camera distance undo requests projection refresh at the current playhead");
    f.app->cameraProjectionRefreshPending=false;
    Check(f.history.History(true)==MMS_OK&&f.app->cameraProjectionRefreshPending,"camera distance redo requests projection refresh at the current playhead");
}
void CurveWriteMetadataSourceGuards(){
    for(unsigned variant=0;variant<4;++variant){
        CurveWriteFixture f;std::wstring error;auto seed=f.Channels(0,false);
        Check(curve_host::Write(*f.app,curve_host::Kind::Camera,0,seed,error),"seed canonical authoring for metadata source guards");
        plugins::NativePoseTestBinding binding(*f.app,f.history);
        auto pending=f.Plan(0,f.Channels(0));std::unique_ptr<plugins::PreparedHistoryChange> prepared;
        Check(pending->Prepare(true,prepared)==MMS_OK,"prepare replacement before an independent metadata edit");
        auto metadata=f.Channels(0,false);
        if(variant==0)metadata[0].sample={};
        else if(variant==1)metadata[0].interpolationGroup="independent camera group";
        else if(variant==2)metadata[0].group=2;
        else metadata[0].collections.push_back({"first",L"first collection"});
        auto external=f.Plan(0,metadata);std::unique_ptr<plugins::PreparedHistoryChange> edit;
        Check(external&&external->Prepare(true,edit)==MMS_OK,"same-key sampler or interpolation-group edit is a real authored replacement");
        const auto native=f.Native(0);edit->Publish();
        Check(!prepared->IsCurrent()&&f.Native(0)==native,"prepared source token refuses a metadata-only replacement even with identical native bytes");
        prepared.reset();
        // Publish a journal-owned key edit, then modify only metadata outside
        // that journal. Its replay must freeze semantic author data, not just
        // the native pool or a shared author identity.
        f.Commit(f.Plan(0,f.Channels(0)));const auto original=f.Channels(0,false);
        metadata=original;
        if(variant==0)metadata[0].sample=seed[0].sample;
        else if(variant==1)metadata[0].interpolationGroup="another camera group";
        else if(variant==2)metadata[0].group=3;
        else metadata[0].collections.front().label=L"changed collection label";
        external=f.Plan(0,metadata);Check(external&&external->Prepare(true,edit)==MMS_OK,"prepare an independent metadata edit after a journal-owned replacement");
        edit->Publish();const auto source=f.Digest();
        Check(f.history.History(false)==MMS_E_STALE&&f.history.UndoCount()==1&&f.Digest()==source,
            "committed replay refuses sampler or interpolation-group changes in the same author lineage");
        external=f.Plan(0,original);Check(external&&external->Prepare(true,edit)==MMS_OK,"prepare restoration of the captured metadata semantics");edit->Publish();
        Check(f.history.History(false)==MMS_OK,"replay accepts equivalent restored metadata despite newer internal author revisions");
    }
}
void CurveWriteOrganizationMetadata(){
    CurveWriteFixture f;std::wstring error;auto input=f.Channels(0,false);
    Check(curve_host::Write(*f.app,curve_host::Kind::Camera,0,input,error),"seed authored organization metadata fixture");
    curve_host::AuthoringTable before;Check(curve_host::CaptureAuthoring(*f.app,before,error),"capture original tree organization");
    const auto pose=f.app->CameraPositionY();const auto native=f.Native(0);
    input[0].group=2;input[0].collections.push_back({"workspace",L"workspace collection"});f.Commit(f.Plan(0,input));
    const auto committed=f.Channels(0,false);
    Check(f.Native(0)==native&&f.app->CameraPositionY()==pose&&committed[0].group==2&&committed[0].collections.size()==1&&
        committed[0].collections[0].id=="workspace"&&committed[0].collections[0].label==L"workspace collection",
        "authored organization metadata is retained without taking ownership of unrelated live poses");
    auto display=committed;display[0].selected=!display[0].selected;display[0].label=L"display only";display[0].unit=L"display unit";
    const auto source=f.Digest();Check(!f.Plan(0,display)&&f.Digest()==source&&f.history.UndoCount()==1,
        "display labels units and channel visibility do not create authored history records");
    Check(f.history.History(false)==MMS_OK&&curve_host::AuthoringUnchanged(*f.app,before)&&f.app->CameraPositionY()==pose,
        "organization metadata undo restores the original author tree and preserves unregistered poses");
    Check(f.history.History(true)==MMS_OK&&f.Channels(0,false)[0].collections.size()==1,
        "organization metadata redo restores retained tree ancestors");
}
void CurveWriteUnsafeNativeFallback(){
    for(unsigned variant=0;variant<4;++variant){
        CurveWriteFixture f;const unsigned kind=variant==1?1:variant==2?4:3;
        if(variant==0){f.app->GravityKeys()[0].noise=INT32_MIN;f.app->GravityKeys()[1].noise=INT32_MAX;}
        else if(variant==1){f.app->LightKeys()[0].direction[0]=-(std::numeric_limits<float>::max)();f.app->LightKeys()[1].direction[0]=(std::numeric_limits<float>::max)();}
        else if(variant==2){f.app->AccessoryKeys(0)[0].scale=-(std::numeric_limits<float>::max)();f.app->AccessoryKeys(0)[1].scale=(std::numeric_limits<float>::max)();}
        else{auto* keys=f.app->GravityKeys();keys[0].noise=0;keys[1].noise=INT32_MAX;keys[1].frame=100000000;keys[2].frame=100000002;keys[2].noise=INT32_MAX;}
        auto channels=f.Channels(kind,false);
        if(kind==3)for(auto& key:channels[4].track.keys)key.value=2;
        else if(kind==1)for(auto& key:channels[0].track.keys)key.value=.5;
        else for(auto& key:channels[6].track.keys)key.value=1;
        const auto source=f.Digest();std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanGlobalCurveWrite(*f.app,curve_host::Kind(kind),0,channels,change)==MMS_E_ARGUMENT&&!change&&f.Digest()==source&&f.history.UndoCount()==0,
            "unsafe native fallback interpolation refuses initial replacement before a record can become impossible to undo");
    }
}
void CurveWriteSparseRegistrationGrowth(){
    CurveWriteFixture f;mdl::KeyPoolDestroy(f.app->CameraKeys());f.app->CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>(2);
    auto& first=f.app->CameraKeys()[0];auto& last=f.app->CameraKeys()[1];first.fov=45;first.distance=-30;first.parentModel=-1;
    last=first;last.frame=20;last.eye[0]=10;first.next=1;last.previous=0;
    for(auto* key:{&first,&last})for(unsigned lane=0;lane<6;++lane){key->interpolation[0][lane]=20;key->interpolation[1][lane]=5;key->interpolation[2][lane]=110;key->interpolation[3][lane]=120;}
    f.app->LastRegisteredFrame()=20;f.app->CurrentFrame()=0;f.app->CameraTrackCursor()=0;
    plugins::NativePoseTestBinding binding(*f.app,f.history);
    const auto snapshot=[&]{pmm_extension::Document document;curve_host::Capture(*f.app,document);};
    const auto seek=[&](int frame){f.app->CurrentFrame()=frame;curve_host::Apply(f.app.get(),curve_host::Kind::Camera,frame);};
    const auto insert=[&](unsigned lane,int frame,double value){auto channels=f.Channels(0,false);animation_curve::Key key;key.frame=frame;key.value=value;
        auto& track=channels[lane].track;const auto at=std::lower_bound(track.keys.begin(),track.keys.end(),frame,[](const auto& key,int f){return key.frame<f;});
        track.keys.insert(at,key);animation_curve::UpdateAutomatic(track);
        for(auto& channel:channels)channel.collections={{"object",L"camera"},{"position",L"position"}};
        auto change=f.Plan(0,channels);f.Commit(change);return change;};
    insert(0,10,4);for(unsigned i=0;i<mdl::KeyPoolCapacity(f.app->CameraKeys());++i)f.app->CameraKeys()[i].selected=f.app->CameraKeys()[i].frame==10?1:0;
    snapshot();Check(mdl::KeyPoolCapacity(f.app->CameraKeys())==3,"first sparse camera edit privately grows the two-slot native pool");
    seek(12);f.app->CameraPositionX()=6.5f;f.app->CameraPositionY()=-3.25f;f.app->CameraPositionZ()=2.5f;
    std::shared_ptr<plugins::HistoryChange> registration;Check(plugins::PlanCameraRegistration(*f.app,12,false,registration)==MMS_OK&&registration,"native registration grows the sparse authored pool a second time");
    f.Commit(registration);snapshot();const auto registered=f.Native(0);seek(16);f.app->CameraPositionX()=91.5f;f.app->CameraPositionZ()=71.25f;
    insert(1,30,-8);for(unsigned i=0;i<mdl::KeyPoolCapacity(f.app->CameraKeys());++i)f.app->CameraKeys()[i].selected=f.app->CameraKeys()[i].frame==30?1:0;
    snapshot();seek(5);snapshot();
    const auto result=f.history.History(false);if(result!=MMS_OK)std::fprintf(stderr,"DIAG sparse registration growth undo status=%d count=%zu redo=%zu horizon=%u\n",result,f.history.UndoCount(),f.history.RedoCount(),f.app->LastRegisteredFrame());
    auto restored=f.Native(0);auto expected=registered;
    for(std::size_t offset=offsetof(mdl::CameraKey,selected);offset<restored.size();offset+=sizeof(mdl::CameraKey)){restored[offset]=0;expected[offset]=0;}
    Check(result==MMS_OK&&restored==expected&&f.app->LastRegisteredFrame()==20,"sparse curve native growth then second curve undo survives editor selection Capture and Apply at another frame");
    seek(17);snapshot();Check(f.history.History(false)==MMS_OK,"grown native registrar undo remains available after second curve undo");
    seek(3);snapshot();Check(f.history.History(false)==MMS_OK,"first two-slot curve edit undoes after capture and native registration growth");
}
void CurveWriteFrameSelection(){
    for(bool remove:{false,true}){
        CurveWriteFixture f;auto channels=f.Channels(0,false);
        if(remove){for(auto& channel:channels)channel.track.keys.erase(channel.track.keys.begin()+1);}
        else{animation_curve::Key key;key.frame=6;key.value=1;channels[0].track.keys.insert(channels[0].track.keys.begin()+1,key);}
        f.Commit(f.Plan(0,channels));
        const auto select=[&](unsigned root,unsigned middle,unsigned last){unsigned index=0;for(;;){auto& key=f.app->CameraKeys()[index];
            key.selected=std::uint8_t(key.frame==0?root:key.frame==12?middle:key.frame==30?last:5);if(!key.next)break;index=key.next;}};
        const auto mark=[&](unsigned frame){unsigned index=0;for(;;){const auto& key=f.app->CameraKeys()[index];if(key.frame==frame)return unsigned(key.selected);if(!key.next)break;index=key.next;}return 0u;};
        select(9,7,13);Check(f.history.History(false)==MMS_OK,"curve undo accepts later root and multiple user-selected frames");
        Check(mark(0)==9&&mark(12)==(remove?0u:7u)&&mark(30)==13,"curve undo maps raw selection bytes by frame and leaves restored missing frames unselected");
        for(std::size_t i=3;i<mdl::KeyPoolCapacity(f.app->CameraKeys());++i)
            Check(f.app->CameraKeys()[i].frame||!f.app->CameraKeys()[i].selected,"root frame selection never spreads into empty spare slots");
        select(0,11,17);Check(f.history.History(true)==MMS_OK,"curve redo accepts a second independent multi-selection");
        Check(mark(0)==0&&mark(30)==17&&mark(12)==(remove?0u:11u)&&mark(6)==0,
            "curve redo keeps frame identities through dense reordering and does not resurrect selection on absent frames");
        auto change=f.Plan(0,f.Channels(0));std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        Check(change->Prepare(true,candidate)==MMS_OK&&candidate->IsCurrent(),"prepare short selection-sensitive curve candidate");
        f.app->CameraKeys()[0].selected^=1;Check(!candidate->IsCurrent(),"short publication token still freezes every native selection byte");
    }
    CurveWriteFixture f;auto channels=f.Channels(3,false);channels[5].track.keys[1].value=1;f.Commit(f.Plan(3,channels));
    f.app->GravityKeys()[0].selected=9;f.app->GravityKeys()[1].selected=77;f.app->GravityKeys()[2].selected=0;
    Check(f.history.History(false)==MMS_OK&&f.app->GravityNoiseModeWord()==(77u<<8),
        "gravity fallback samples the four-byte right-key mode after final selection projection");
    Check(f.history.History(true)==MMS_OK&&f.app->GravityNoiseModeWord()==(77u<<8),
        "authored gravity redo preserves the projected native high mode bytes");
}
void NativeCurveWriteChecks(){
    std::puts("  curve write: private mirroring and common history");CurveWriteRoundTrip();CurveWriteRefusals();
    std::puts("  curve write: stale raw author and exact native metadata ownership");CurveWriteStaleAuthoring();CurveWriteMetadataOwnership();
    std::puts("  curve write: gravity native semantics and accessory lifetime");CurveWriteGravityNativeFallback();CurveWriteAccessoryLifetime();
    std::puts("  curve write: interleaved native registration and stable pool growth");CurveWriteInterleavedRegistration();CurveWriteGrowth();CurveWriteProjectionRefresh();CurveWriteMetadataSourceGuards();CurveWriteOrganizationMetadata();CurveWriteUnsafeNativeFallback();CurveWriteSparseRegistrationGrowth();CurveWriteFrameSelection();
    std::puts("  curve write: private allocation failure sweep");CurveWriteFailures();
}
}
