#include "../src/plugins/native_global_paste.hpp"
namespace {
struct GlobalPasteFixture : GlobalRegistrationFixture {
    mdl::AccessoryRecord accessory{};
    std::vector<mdl::CameraClipboardRecord> cameras;
    std::vector<mdl::LightClipboardRecord> lights;
    std::vector<mdl::ShadowClipboardRecord> shadows;
    std::vector<mdl::GravityClipboardRecord> gravity;
    std::vector<mdl::AccessoryClipboardKey> accessories;
    GlobalPasteFixture() {
        std::puts("    global fixture: base ready");std::fflush(stdout);
        app->AccessorySlot(7)=&accessory;accessory.parentModel=-1;accessory.scale=accessory.opacity=1;accessory.visible=1;
        plugins::SceneObjectIdentity(&accessory);
        for(unsigned slot=0;slot<255;++slot)for(std::size_t i=0;i<mdl::KeyPoolCapacity(app->AccessoryKeys(slot));++i) {
            auto& key=app->AccessoryKeys(slot)[i];key.parentModel=-1;key.scale=key.opacity=1;key.visible=1;
        }
        auto* ak=app->AccessoryKeys(7);auto* gk=app->GravityKeys();
        for(unsigned i=0;i<3;++i) {
            ak[i].frame=gk[i].frame=i==0?0:i==1?12:31;
            ak[i].previous=gk[i].previous=i?i-1:0;ak[i].next=gk[i].next=i<2?i+1:0;
            ak[i].position[0]=float(i);gk[i].acceleration=9.8f+float(i);gk[i].direction[1]=-1;gk[i].noise=int(i)*3;
        }
        for(unsigned i=0;i<3;++i) {
            mdl::CameraClipboardRecord c{};c.frame=i==1?8:0;c.fov=43+int(i);c.distance=-18-float(i);c.parentModel=-1;c.perspective=1;
            for(unsigned axis=0;axis<3;++axis){c.eye[axis]=float(i+axis);c.target[axis]=.07f*float(i+axis);}
            std::memset(c.interpolation,20,12);std::memset(c.interpolation+12,107,12);c.interpolation[4]=std::uint8_t(20+i);
            cameras.push_back(c);
            mdl::LightClipboardRecord l{};l.frame=c.frame;for(unsigned axis=0;axis<3;++axis){l.direction[axis]=.1f*float(i+axis);l.color[axis]=.3f+.1f*float(i);}lights.push_back(l);
            mdl::ShadowClipboardRecord s{};s.frame=c.frame;s.mode=std::uint8_t(i);s.distance=.025f+.01f*float(i);shadows.push_back(s);
            mdl::GravityClipboardRecord g{};g.frame=c.frame;g.acceleration=12.f+float(i);g.direction[1]=-1;g.noise=11+int(i);g.noiseEnabled=1;gravity.push_back(g);
            mdl::AccessoryClipboardKey a{};a.frameOffset=c.frame;a.slot=7;a.parentModel=-1;a.position[0]=5+float(i);a.rotation[1]=.1f*float(i);
            a.visible=1;a.shadowEnabled=1;a.scale=1.2f+float(i)*.1f;a.opacity=.8f-float(i)*.1f;accessories.push_back(a);
        }
        Bind(true);
        std::puts("    global fixture: sources ready");std::fflush(stdout);
    }
    ~GlobalPasteFixture() {
        history.Reset();history.Collect();plugins::ForgetSceneObject(&accessory);app->AccessorySlot(7)=nullptr;
        app->CameraClipboard()=nullptr;app->LightClipboard()=nullptr;app->ShadowClipboard()=nullptr;
        app->GravityClipboard()=nullptr;app->AccessoryClipboard()=nullptr;app->ClipboardCounts()={};
    }
    void Bind(bool five) {
        app->CameraClipboard()=cameras.data();app->LightClipboard()=lights.data();app->ShadowClipboard()=shadows.data();
        app->GravityClipboard()=five?gravity.data():nullptr;app->AccessoryClipboard()=five?accessories.data():nullptr;
        app->ClipboardCounts()={};auto& n=app->ClipboardCounts();n.cameras=unsigned(cameras.size());n.lights=unsigned(lights.size());n.shadows=unsigned(shadows.size());
        if(five){n.gravity=unsigned(gravity.size());n.accessories=unsigned(accessories.size());}
    }
    void ClearClipboard() {
        app->CameraClipboard()=nullptr;app->LightClipboard()=nullptr;app->ShadowClipboard()=nullptr;
        app->GravityClipboard()=nullptr;app->AccessoryClipboard()=nullptr;app->ClipboardCounts()={};
        cameras.clear();lights.clear();shadows.clear();gravity.clear();accessories.clear();
    }
};
struct GlobalPasteKeys {
    std::vector<mdl::CameraKey> camera;std::vector<mdl::LightKey> light;std::vector<mdl::SelfShadowKey> shadow;
    std::vector<mdl::GravityKey> gravity;std::array<std::vector<mdl::AccessoryKey>,255> accessory;
    explicit GlobalPasteKeys(MMDApp& app):camera(GlobalRegistrationKeys(app.CameraKeys())),light(GlobalRegistrationKeys(app.LightKeys())),
        shadow(GlobalRegistrationKeys(app.ShadowKeys())),gravity(GlobalRegistrationKeys(app.GravityKeys())) {
        for(unsigned i=0;i<255;++i)accessory[i]=GlobalRegistrationKeys(app.AccessoryKeys(i));
    }
    bool Same(MMDApp& app) const {
        if(!GlobalRegistrationSame(app.CameraKeys(),camera)||!GlobalRegistrationSame(app.LightKeys(),light)||
            !GlobalRegistrationSame(app.ShadowKeys(),shadow)||!GlobalRegistrationSame(app.GravityKeys(),gravity))return false;
        for(unsigned i=0;i<255;++i)if(!GlobalRegistrationSame(app.AccessoryKeys(i),accessory[i]))return false;
        return true;
    }
};
std::shared_ptr<plugins::HistoryChange> GlobalPastePlan(GlobalPasteFixture& f) {
    std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanGlobalKeyPaste(*f.app,change)==MMS_OK,"global clipboard privately plans");return change;
}
void GlobalPasteCommit(GlobalPasteFixture& f,const std::shared_ptr<plugins::HistoryChange>& change) {
    std::puts("    global commit: prepare");std::fflush(stdout);
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
    Check(f.history.PrepareNative("global clipboard paste",change,token,plugins::SharedHistory::NativeIntent::ExplicitCommit)==MMS_OK,"aggregate reserves one explicit global step");
    std::puts("    global commit: publish");std::fflush(stdout);
    failAfter=0;const auto result=f.history.CommitNative(std::move(token));failAfter=-1;
    std::puts("    global commit: published");std::fflush(stdout);
    Check(result==MMS_OK,"all global participants publish without allocation");
}
template<class K> const K& GlobalPasteAt(K* pool,unsigned frame) {
    unsigned at=0;for(std::size_t hop=0;pool[at].frame<frame&&pool[at].next;++hop){Check(hop<mdl::KeyPoolCapacity(pool),"bounded pasted key chain");at=pool[at].next;}
    Check(pool[at].frame==frame,"expected global clipboard frame exists");return pool[at];
}
void GlobalPasteRoundTripChecks() {
    for(bool five:{false,true})for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        std::printf("  global roundtrip five=%u mode=%u\n",unsigned(five),unsigned(mode));std::fflush(stdout);
        GlobalPasteFixture f;f.Bind(five);Check(f.history.SetMode(mode)==MMS_OK,"configure global paste history mode");
        const GlobalPasteKeys before(*f.app);const auto marks=AccessoryRegistrationMarks(*f.app);const auto last=f.app->LastRegisteredFrame();
        const auto cameraIdentity=mdl::KeyPoolIdentity(f.app->CameraKeys()),accessoryIdentity=mdl::KeyPoolIdentity(f.app->AccessoryKeys(7));
        std::puts("    global roundtrip: plan");std::fflush(stdout);
        const auto original=f.Digest();auto change=GlobalPastePlan(f);Check(change&&f.Digest()==original,"global aggregate preparation does not alter keys poses marks or metadata");
        GlobalPasteCommit(f,change);Check(f.history.UndoCount()==1&&f.app->CurrentFrame()==7&&f.app->LastRegisteredFrame()==15,"mixed global paste is one explicit step and updates last once");
        Check(GlobalPasteAt(f.app->CameraKeys(),7).fov==f.cameras[2].fov&&GlobalPasteAt(f.app->CameraKeys(),7).interpolation[0][4]==f.cameras[2].interpolation[4],"repeated camera source uses final record and retains all curve bytes");
        Check(GlobalPasteAt(f.app->LightKeys(),15).color[0]==f.lights[1].color[0]&&GlobalPasteAt(f.app->ShadowKeys(),7).mode==f.shadows[2].mode,"mixed light and shadow payloads survive aggregation");
        Check(mdl::KeyPoolCapacity(f.app->CameraKeys())>before.camera.size()&&mdl::KeyPoolIdentity(f.app->CameraKeys())==cameraIdentity,"full camera pool grows privately with stable identity");
        if(five)Check(GlobalPasteAt(f.app->GravityKeys(),7).noise==f.gravity[2].noise&&GlobalPasteAt(f.app->AccessoryKeys(7),7).opacity==f.accessories[2].opacity&&
            mdl::KeyPoolCapacity(f.app->AccessoryKeys(7))>before.accessory[7].size()&&mdl::KeyPoolIdentity(f.app->AccessoryKeys(7))==accessoryIdentity,"gravity and full accessory pool join the same transaction");
        for(unsigned slot=0;slot<255;++slot)if(slot!=7||!five)for(std::size_t i=0;i<mdl::KeyPoolCapacity(f.app->AccessoryKeys(slot));++i)
            Check(!f.app->AccessoryKeys(slot)[i].selected,"global paste clears every unrelated accessory mark");
        const GlobalPasteKeys after(*f.app);f.ClearClipboard();f.app->CurrentFrame()=23;f.app->CameraMode()=0;f.app->SelectedModelSlot()=113;f.app->SelectedAccessorySlot()=99;
        std::puts("    global roundtrip: undo");std::fflush(stdout);
        Check(f.history.History(false)==MMS_OK&&before.Same(*f.app)&&f.app->LastRegisteredFrame()==last&&f.app->CurrentFrame()==23,"global undo restores all old key payloads and 259 pool marks at another playhead");
        Check(f.history.History(true)==MMS_OK&&after.Same(*f.app)&&f.app->LastRegisteredFrame()==15&&f.app->CurrentFrame()==23,"global redo owns clipboard values across selection and playhead changes");
        Check(f.history.History(false)==MMS_OK,"global mixed history remains undoable");
        Check(AccessoryRegistrationMarks(*f.app).size()>=marks.size(),"grown selection storage remains owned after undo");
    }
}
void GlobalPasteZeroNoopChecks() {
    GlobalPasteFixture f;f.app->CurrentFrame()=0;f.Bind(true);auto change=GlobalPastePlan(f);Check(change!=nullptr,"frame zero global clipboard is planned");GlobalPasteCommit(f,change);
    Check(f.app->CameraKeys()[0].fov==f.cameras[2].fov&&f.app->GravityKeys()[0].noise==f.gravity[2].noise&&f.app->AccessoryKeys(7)[0].opacity==f.accessories[2].opacity,"frame zero overwrites owned head records");
    const auto undo=f.history.UndoCount();const auto digest=f.Digest();auto same=GlobalPastePlan(f);
    Check(!same&&f.history.UndoCount()==undo&&f.Digest()==digest,"identical five-family paste is a clean no-op");
    const auto oldLight=f.lights[2].color[0];f.lights[2].color[0]+=.1f;
    auto lightChange=GlobalPastePlan(f);Check(lightChange!=nullptr,"mixed clipboard can change only one participant");GlobalPasteCommit(f,lightChange);
    Check(f.app->CameraKeys()[0].selected&&f.app->GravityKeys()[0].selected&&f.app->AccessoryKeys(7)[0].selected,
        "unchanged source families remain selected when another pasted family changes");
    f.lights[2].color[0]=oldLight;
    Check(f.history.History(false)==MMS_OK&&f.history.RedoCount()==1,"seed redo after one-family change within mixed clipboard");
    const auto noOpBefore=f.Digest();same=GlobalPastePlan(f);
    Check(!same&&f.history.RedoCount()==1&&f.Digest()==noOpBefore,"identical mixed clipboard preserves an existing redo branch");
    Check(f.history.History(false)==MMS_OK,"seed global redo");
    f.ClearClipboard();const auto before=f.Digest();same=GlobalPastePlan(f);
    Check(!same&&f.history.RedoCount()==2&&f.Digest()==before,"empty global clipboard preserves redo and selection");
}
void GlobalPasteFailureChecks() {
    GlobalPasteFixture f;const auto before=f.Digest();std::shared_ptr<plugins::HistoryChange> change;
    f.accessories.back().slot=254;
    std::puts("    global failure: missing accessory plan");std::fflush(stdout);
    Check(plugins::PlanGlobalKeyPaste(*f.app,change)!=MMS_OK&&!change&&f.Digest()==before,"late missing accessory refuses all earlier global plans atomically");
    std::puts("    global failure: missing accessory preserved");std::fflush(stdout);
    f.accessories.back().slot=7;
    unsigned failures=0;bool success=false;
    for(long limit=0;limit<4096;++limit) {
        std::printf("    global failure: plan allowance=%ld\n",limit);std::fflush(stdout);
        failAfter=limit;const auto result=plugins::PlanGlobalKeyPaste(*f.app,change);failAfter=-1;
        if(result==MMS_OK){Check(change!=nullptr,"failure sweep eventually builds mixed plan");success=true;break;}
        ++failures;Check(!change&&f.Digest()==before,"failed global family preparation leaves all live fields intact");
    }
    Check(success&&failures>10,"global allocation sweep covers multiple participants");
    std::puts("    global failure: plan sweep completed, budget");std::fflush(stdout);
    Check(f.history.Configure(plugins::SharedHistory::RecordingMode::AllOperations,32,1)==MMS_OK,"set insufficient global history budget");
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
    Check(f.history.PrepareNative("over budget global paste",change,token)!=MMS_OK&&!token&&f.Digest()==before&&!f.history.UndoCount(),"aggregate budget failure cancels every participant before pool growth");
    Check(f.history.Configure(plugins::SharedHistory::RecordingMode::AllOperations,32,64ull*1024*1024)==MMS_OK,"restore sufficient global budget");
    unsigned preparedFailures=0;success=false;
    for(long limit=0;limit<4096;++limit) {
        std::printf("    global failure: candidate allowance=%ld\n",limit);std::fflush(stdout);
        // Construct the by-value API label before enabling the fault injector.
        // Otherwise allowance zero throws in the test caller's implicit
        // std::string conversion, outside PrepareNative's error boundary.
        std::string label="global candidate failure";
        failAfter=limit;const auto result=f.history.PrepareNative(std::move(label),change,token);failAfter=-1;
        if(result==MMS_OK){token.reset();success=true;break;}
        ++preparedFailures;Check(!token&&f.Digest()==before&&!f.history.UndoCount(),"late candidate failure does not publish early global participants");
    }
    Check(success&&preparedFailures>10,"global candidate failure sweep reaches final participant");
}
void GlobalPasteStaleChecks() {
    for(unsigned mutation=0;mutation<5;++mutation) {
        GlobalPasteFixture f;auto change=GlobalPastePlan(f);
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        Check(f.history.PrepareNative("stale mixed global paste",change,token)==MMS_OK,"prepare complete global candidate before staleness");
        auto* original=f.app->CameraKeys();mdl::CameraKey* replacement=nullptr;
        if(mutation==0){replacement=mdl::KeyPoolCreate<mdl::CameraKey>(mdl::KeyPoolCapacity(original));std::memcpy(replacement,original,mdl::KeyPoolCapacity(original)*sizeof(*original));f.app->CameraKeys()=replacement;}
        if(mutation==1)f.app->AccessorySlot(7)=nullptr;
        if(mutation==2)f.app->AccessoryKeys(254)[1].selected=99;
        if(mutation==3)++f.app->CurrentFrame();
        if(mutation==4)++f.app->LastRegisteredFrame();
        const auto before=f.Digest();
        Check(f.history.CommitNative(std::move(token))==MMS_E_STALE&&f.Digest()==before&&!f.history.UndoCount(),
            "changed pool identity accessory ownership shared marks playhead or metadata refuses entire global publication");
        if(replacement){f.app->CameraKeys()=original;mdl::KeyPoolDestroy(replacement);}
        f.app->AccessorySlot(7)=&f.accessory;
    }
}
void NativeGlobalPasteChecks(){
    std::puts("  global aggregate: roundtrip");std::fflush(stdout);GlobalPasteRoundTripChecks();
    std::puts("  global aggregate: zero/noop");std::fflush(stdout);GlobalPasteZeroNoopChecks();
    std::puts("  global aggregate: failures");std::fflush(stdout);GlobalPasteFailureChecks();
    std::puts("  global aggregate: stale");std::fflush(stdout);GlobalPasteStaleChecks();
}
}
