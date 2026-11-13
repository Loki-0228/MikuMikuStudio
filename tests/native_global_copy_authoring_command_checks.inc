// Production 420 integration; include after the copy and curve fixtures.
namespace {
namespace global_copy_authoring_command_checks {
namespace copy = global_copy_failure_checks;
namespace receipt = global_copy_snapshot_checks;
namespace batch = frame_copy_batch_checks;
using Kind = curve_host::Kind;
using Mode = plugins::SharedHistory::RecordingMode;
using CopyMode = frame_clipboard::CopyMode;
using Status = frame_clipboard::CopyBatchStatus;
using Authors = std::vector<curve_host::CopyAuthoring>;
using Snapshot = std::shared_ptr<const plugins::GlobalCopySnapshot>;
constexpr long allowanceMax = 1000000;

bool Equal(const curve_editor::Channel& a,const curve_editor::Channel& b) {
    if(a.id!=b.id || a.track.discrete!=b.track.discrete || a.track.keys.size()!=b.track.keys.size() ||
        a.interpolationGroup!=b.interpolationGroup || a.nativeControls!=b.nativeControls ||
        a.controlGeometry!=b.controlGeometry || a.preservedCameraIntervals!=b.preservedCameraIntervals ||
        bool(a.sample)!=bool(b.sample))return false;
    for(std::size_t i=0;i<a.track.keys.size();++i) {
        const auto& x=a.track.keys[i];const auto& y=b.track.keys[i];
        if(x.frame!=y.frame || x.value!=y.value || x.interpolation!=y.interpolation || x.tangent!=y.tangent ||
            x.inSlope!=y.inSlope || x.outSlope!=y.outSlope || x.inWeight!=y.inWeight || x.outWeight!=y.outWeight)return false;
    }
    return true;
}
bool Equal(const Authors& a,const Authors& b) {
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i) {
        if(a[i].kind!=b[i].kind || a[i].slot!=b[i].slot || a[i].channels.size()!=b[i].channels.size())return false;
        for(std::size_t c=0;c<a[i].channels.size();++c)if(!Equal(a[i].channels[c],b[i].channels[c]))return false;
    }
    return true;
}
struct Fixture : receipt::OwnedFixture {
    Authors expected;
    std::vector<curve_editor::Channel> originalCamera;
    explicit Fixture(Mode mode) {
        receipt::DetachParents(*this);app->CameraMode()=1;
        // The raw-copy fixture has one deliberately unlinked nonzero sentinel.
        // Author writes require a valid native pool before the copy is tested.
        const auto clearSentinel=[](auto* pool){pool[mdl::KeyPoolCapacity(pool)-1]={};};
        clearSentinel(app->CameraKeys());clearSentinel(app->LightKeys());
        clearSentinel(app->ShadowKeys());clearSentinel(app->GravityKeys());
        clearSentinel(app->AccessoryKeys(7));clearSentinel(app->AccessoryKeys(254));
        for(auto& object:accessory){object.parentModel=-1;object.scale=object.opacity=1;}
        Check(history.SetMode(mode)==MMS_OK,"global author command history mode");
        for(const auto target:std::array<curve_host::CopyAuthoringTarget,6>{{
            {Kind::Camera,0},{Kind::Light,0},{Kind::Shadow,0},{Kind::Gravity,0},{Kind::Accessory,7},{Kind::Accessory,254}}}) {
            std::vector<curve_editor::Channel> channels;std::wstring error;
            Check(curve_host::Read(*app,target.kind,target.slot,channels,error),"read global command source for author setup");
            if(target.kind==Kind::Camera) {
                originalCamera=channels;std::size_t at=0;
                Check(curve_host::InsertPreservingShape(channels,0,13,at,error) &&
                    curve_host::InsertPreservingShape(channels,3,17,at,error),"seed exact fractional camera slices and linked rotation");
            } else {
                auto& track=channels[target.kind==Kind::Shadow?1:0].track;
                for(auto& key:track.keys) {
                    key.interpolation=animation_curve::Interpolation::Bezier;key.tangent=animation_curve::Tangent::Free;
                    key.inSlope=.013;key.outSlope=-.007;key.inWeight=.2;key.outWeight=.3;
                }
            }
            WriteAuthor(target.kind,target.slot,channels);
            Check(curve_host::Read(*app,target.kind,target.slot,channels,error),"read independent canonical source before copy");
            expected.push_back({target.kind,target.slot,std::move(channels)});
        }
        app->CurrentFrame()=-70;
        Check(negative_frames::RegisterGlobal(app.get(),negative_frames::Global::Light,-70),"seed negative light in author copy batch");
        Check(negative_frames::RegisterGlobal(app.get(),negative_frames::Global::Camera,-40),"seed negative camera in author copy batch");
        Check(negative_frames::RegisterGlobal(app.get(),negative_frames::Global::Shadow,-20) && history.History(false)==MMS_OK,
            "retain undo and redo while copying authored global keys");
        app->CurrentFrame()=7;SelectEnds();
    }
    ~Fixture(){curve_host::Reset(app.get());}
    void WriteAuthor(Kind kind,int slot,const std::vector<curve_editor::Channel>& channels) {
        // Setup uses the real planner and shared transaction without requiring
        // the unrelated curve-editor window admission or issuing UI refreshes.
        std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanGlobalCurveWrite(*app,kind,slot,channels,change)==MMS_OK && change,
            "seed global author through production planner");
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(history.PrepareNative("copy author setup",change,transaction,plugins::SharedHistory::NativeIntent::ExplicitCommit)==MMS_OK &&
            history.CommitNative(std::move(transaction))==MMS_OK,"publish global author setup through shared history");
    }
    void SelectEnds() {
        const auto mark=[](auto* pool) {
            const auto capacity=mdl::KeyPoolCapacity(pool);
            for(std::size_t i=0;i<capacity;++i)pool[i].selected=0;
            unsigned slot=0;for(std::size_t hop=0;hop<capacity;++hop) {
                if(!pool[slot].next){pool[slot].selected=1;return;}slot=pool[slot].next;
            }
            Check(false,"global author fixture ends in bounded native chain");
        };
        mark(app->CameraKeys());mark(app->LightKeys());mark(app->ShadowKeys());mark(app->GravityKeys());
        mark(app->AccessoryKeys(7));mark(app->AccessoryKeys(254));
        negative_frames::SelectRange(app.get(),-70,-40);
    }
};
struct Before {
    copy::Snapshot native;
    curve_host::AuthoringTable authors;
    explicit Before(Fixture& f):native(f) {
        std::wstring error;Check(curve_host::CaptureAuthoring(*f.app,authors,error),"capture complete original global author layers");
    }
    void Expect(Fixture& f) const {
        native.Expect(f);Check(curve_host::AuthoringUnchanged(*f.app,authors),"copy never reconciles or prunes the live global author table");
    }
};
long Command(Fixture& f,long allowance=allowanceMax) {
    Check(failAfter==-1 && !allocationAudit,"global author command starts outside failure scopes");
    const auto window=f.app->MainWindow();f.app->MainWindow()=f.controls.host;bool threw=false;
    failAfter=allowance;
    try{CmdControl400(f.app.get(),f.controls.host,420,0);}catch(...){threw=true;}
    const auto spent=allowance-failAfter;failAfter=-1;f.app->MainWindow()=window;
    Check(!threw,"global author copy contains all preparation failures");return spent;
}
Snapshot Read(Fixture& f) {
    const auto copied=batch::Read(*f.app,CopyMode::Global,Status::Captured);
    Check(copied->global && copied->nativeFirst==27 && copied->first==-70,
        "global authors remain inside one copy-time signed origin and receipt");
    return copied->global;
}
void Shape(const Snapshot& source,const Authors& expected) {
    Check(source && source->authoring.size()==6 && Equal(source->authoring,expected),
        "global receipt owns all requested complete authors and exact camera interval fields");
    Check(source->payload.cameras.size()==1 && source->payload.lights.size()==1 && source->payload.shadows.size()==1 &&
        source->payload.gravity.size()==1 && source->payload.accessories.size()==2,
        "unselected authoring neighbours never become additional clipboard payload records");
}
void Samples(const Snapshot& source,const std::vector<curve_editor::Channel>& original) {
    for(std::size_t c=0;c<8;++c)for(int tick=0;tick<=270;++tick) {
        const auto& copied=source->authoring[0].channels[c];
        const auto a=original[c].sample(original[c],tick/10.0),b=copied.sample(copied,tick/10.0);
        Check(a.value==b.value && std::abs(a.derivative-b.derivative)<=1e-12*std::max(1.,std::abs(a.derivative)),
            "copied exact native camera shape and velocity survive at fractional frames");
    }
}
void Ownership(Mode mode) {
    Snapshot held;Authors expected;std::vector<curve_editor::Channel> original;
    {
        Fixture f(mode);const Before before(f);Command(f);before.Expect(f);held=Read(f);expected=f.expected;original=f.originalCamera;
        Shape(held,expected);Samples(held,original);f.controls.Expect(true,true);
        Check(plugins::GlobalCopySnapshotCurrent(*f.app,*held),"complete global author receipt is eligible at its original identity");
        auto edited=f.expected[1].channels;edited[0].track.keys[1].outSlope+=.25;
        f.WriteAuthor(Kind::Light,0,edited);
        f.SelectEnds();Shape(held,expected);
        Check(plugins::GlobalCopySnapshotCurrent(*f.app,*held),"later author values do not invalidate owned source references");
        Command(f);Check(!Equal(Read(f)->authoring,expected),"new copy observes later author edits while held source remains unchanged");
        Shape(held,expected);curve_host::Reset(f.app.get());Shape(held,expected);Samples(held,original);
    }
    Shape(held,expected);Samples(held,original);
}
void Failure(Mode mode) {
    Fixture f(mode);Command(f);const auto held=Read(f);const Before before(f);const auto cost=Command(f);
    Check(cost>30 && cost<allowanceMax,"measure global command author capture allocation count");
    for(long allowance=0;allowance<cost;++allowance) {
        Command(f);Check(Command(f,allowance)==allowance,"exercise each global author copy allocation boundary");
        batch::Read(*f.app,CopyMode::Global,Status::Failed);
        Check(f.app->FrameClipboardCopy(CopyMode::Global).error==MMS_E_MEMORY,"failed author copy returns complete-batch memory failure");
        for(auto count:copy::Counts(*f.app))Check(count==0,"failed global author copy clears every active count");
        for(auto pointer:copy::Buffers(*f.app))Check(!pointer,"failed global author copy releases every active native buffer");
        f.controls.Expect(false,false);before.Expect(f);Shape(held,f.expected);
    }
    Command(f);Shape(Read(f),f.expected);before.Expect(f);
    std::printf("  global author command OOM: mode=%u stages=%ld\n",unsigned(mode),cost);
    // The raw copy producer can copy a selected valid row while an unselected
    // root is malformed. Complete author capture must reject that batch.
    f.app->CameraKeys()[0].frame=1;const Before malformed(f);Command(f);
    batch::Read(*f.app,CopyMode::Global,Status::Failed);
    Check(f.app->FrameClipboardCopy(CopyMode::Global).error==MMS_E_ARGUMENT,"invalid authored source chain rejects the whole real copy");
    malformed.Expect(f);f.controls.Expect(false,false);Shape(held,f.expected);
    f.app->CameraKeys()[0].frame=0;Command(f);Shape(Read(f),f.expected);before.Expect(f);
}
}
void NativeGlobalCopyAuthoringCommandChecks() {
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        global_copy_authoring_command_checks::Ownership(mode);global_copy_authoring_command_checks::Failure(mode);
    }
}
}
