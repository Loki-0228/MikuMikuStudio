// Included after copy fixtures and curve authoring fixtures.
namespace {
namespace model_copy_authoring_checks {
namespace batch = frame_copy_batch_checks;
namespace result = negative_copy_result_checks;
using Mode = plugins::SharedHistory::RecordingMode;
using CopyMode = frame_clipboard::CopyMode;
using Status = frame_clipboard::CopyBatchStatus;
using Snapshot = std::shared_ptr<const plugins::ModelCopySnapshot>;
constexpr long allowanceMax = 1000000;

bool Equal(const animation_curve::Track& a,const animation_curve::Track& b) {
    if(a.discrete!=b.discrete || a.keys.size()!=b.keys.size())return false;
    for(std::size_t i=0;i<a.keys.size();++i) {
        const auto& x=a.keys[i];const auto& y=b.keys[i];
        if(x.frame!=y.frame || x.value!=y.value || x.inSlope!=y.inSlope || x.outSlope!=y.outSlope ||
            x.inWeight!=y.inWeight || x.outWeight!=y.outWeight || x.interpolation!=y.interpolation || x.tangent!=y.tangent)return false;
    }
    return true;
}
bool Equal(const bone_curve::Tracks& a,const bone_curve::Tracks& b) {
    for(unsigned i=0;i<a.size();++i)if(!Equal(a[i],b[i]))return false;
    return true;
}
struct Fixture : batch::BatchFixture {
    bone_curve::Tracks bone=BoneVmdTracks();
    animation_curve::NamedTrack morph;
    explicit Fixture(Mode mode):BatchFixture(mode),morph(MorphAuthoredTrack(model.morphs[0],0)) {
        model.m->boneKeys[0].frame=model.m->boneKeys[1].frame=0;
        model.m->morphKeys[0].frame=model.m->displayKeys[0].frame=0;
        for(auto& b:model.bones)b.type=mdl::BoneType::RotateMove;
        std::string error;
        Check(bone_curve::Write(model.raw(),0,bone,error),"seed complete nine-lane bone authoring for production copy");
        Check(morph_curve::Write(model.raw(),{morph},error),"seed weighted Bezier morph authoring for production copy");
        // Read a canonical expected shape once before recording read-only oracles.
        Check(bone_curve::ReadDetached(model.raw(),0,bone),"read canonical detached bone fixture");
        std::vector<animation_curve::Track> tracks;std::wstring why;
        Check(morph_curve::ReadManyDetached(*app,{0},tracks,why) && tracks.size()==1,"read canonical detached morph fixture");
        morph.track=tracks[0];
        Select();
    }
    ~Fixture(){bone_curve::Forget(model.raw());}
    void Select() {
        SelectMode(CopyMode::Model);ClearRealCopyMarks(model);
        // Copy separated keys only. Neighbours are needed for exact handle
        // geometry, but are not additional selected native paste records.
        for(std::size_t i=0;i<mdl::BoneKeyCapacity(*model.m);++i)
            model.m->boneKeys[i].allocated=model.m->boneKeys[i].frame==20?1:0;
        for(std::size_t i=0;i<mdl::MorphKeyCapacity(model.m->morphKeys);++i)
            model.m->morphKeys[i].allocated=(model.m->morphKeys[i].frame==10 || model.m->morphKeys[i].frame==50)?1:0;
        negative_frames::SelectRange(app.get(),-70,-30);
    }
};
struct Before {
    result::ModelLive live;
    std::unique_ptr<bone_curve::AuthoringState> bones;
    bone_scale::AuthoringState scales;
    morph_curve::AuthoredLayers morphs;
    explicit Before(Fixture& f):live(f),bones(bone_curve::AuthoringState::Capture(f.model.raw())),
        scales(bone_scale::AuthoringState::Capture(f.model.raw())) {
        std::string error;Check(morph_curve::CaptureLayers(f.model.raw(),morphs,error),"capture unmodified raw morph layers");
    }
    void Expect(Fixture& f) const {
        live.Expect(f);
        Check(bones->Equals(*bone_curve::AuthoringState::Capture(f.model.raw())),"copy does not synchronize live bone authors or baselines");
        Check(scales.Equals(bone_scale::AuthoringState::Capture(f.model.raw())),"copy does not change source scale authoring");
        Check(morph_curve::LayersUnchanged(f.model.raw(),morphs),"copy does not synchronize pristine effective mirror or observed morph maps");
    }
};
long Command(Fixture& f,long allowance=allowanceMax) {
    Check(failAfter==-1 && !allocationAudit,"authoring command starts outside other injection scopes");
    const auto window=f.app->MainWindow();f.app->MainWindow()=f.controls.host;bool threw=false;
    failAfter=allowance;
    try{CmdControl400(f.app.get(),f.controls.host,420,0);}catch(...){threw=true;}
    const auto spent=allowance-failAfter;failAfter=-1;f.app->MainWindow()=window;
    Check(!threw,"production copy contains authoring allocation failures");return spent;
}
Snapshot Read(Fixture& f) {
    const auto copied=batch::Read(*f.app,CopyMode::Model,Status::Captured);
    Check(copied->model && copied->nativeFirst==10 && copied->first==-70,
        "authoring copy remains in the same signed native/negative receipt");
    return copied->model;
}
void Shape(const Snapshot& source,const bone_curve::Tracks& bone,const animation_curve::Track& morph) {
    Check(source && source->boneAuthoring.size()==1 && source->morphAuthoring.size()==1,
        "one owned author per selected source track, not per copied key");
    Check(source->boneAuthoring[0].bone==0 && Equal(source->boneAuthoring[0].tracks,bone),
        "owned bone author retains all nine lanes, sparse frames and exact tangent fields");
    Check(source->morphAuthoring[0].morph==0 && Equal(source->morphAuthoring[0].track,morph),
        "owned morph author retains complete Bezier shape including unselected neighbours");
    Check(source->payload.bones.size()==1 && source->payload.morphs.size()==2,
        "authoring neighbours never become selected native clipboard records");
}
void Ownership(Mode mode) {
    Snapshot held;bone_curve::Tracks bone;animation_curve::Track morph;
    {
        Fixture f(mode);const Before before(f);Command(f);before.Expect(f);held=Read(f);bone=f.bone;morph=f.morph.track;
        Shape(held,bone,morph);Check(plugins::ModelCopySnapshotCurrent(*f.app,*held),"copied author source is current");
        auto newBone=f.bone;newBone[0].keys.back().value+=3;
        auto newMorph=f.morph;newMorph.track.keys[1].outSlope+=.04;
        std::string error;Check(bone_curve::Write(f.model.raw(),0,newBone,error) && morph_curve::Write(f.model.raw(),{newMorph},error),
            "edit live authors after copy");
        f.app->CurrentFrame()=90;f.Select();
        Shape(held,bone,morph);Check(plugins::ModelCopySnapshotCurrent(*f.app,*held),
            "later author value edits do not invalidate the immutable source dependency guard");
        Command(f);const auto newer=Read(f);
        Check(!Equal(newer->boneAuthoring[0].tracks,bone) && !Equal(newer->morphAuthoring[0].track,morph),
            "a new real copy captures the new authoring while held data stays unchanged");
        Shape(held,bone,morph);
    }
    Shape(held,bone,morph);
}
void Failure(Mode mode) {
    Fixture f(mode);Command(f);const auto held=Read(f);const Before before(f);
    const auto cost=Command(f);Check(cost>20,"measure real authoring copy allocations after warmup");
    for(long allowance=0;allowance<cost;++allowance) {
        Command(f);const auto spent=Command(f,allowance);
        Check(spent==allowance,"each measured authoring preparation allocation is exercised");
        batch::Read(*f.app,CopyMode::Model,Status::Failed);
        Check(f.app->FrameClipboardCopy(CopyMode::Model).error==MMS_E_MEMORY,
            "authoring OOM retires the complete model copy with MEMORY");
        batch::EmptyNative(f,CopyMode::Model);f.controls.Expect(false,false);before.Expect(f);Shape(held,f.bone,f.morph.track);
    }
    Command(f);Shape(Read(f),f.bone,f.morph.track);before.Expect(f);
    std::printf("  model copy authoring OOM: mode=%u stages=%ld\n",unsigned(mode),cost);
}
}
void NativeModelCopyAuthoringChecks() {
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        model_copy_authoring_checks::Ownership(mode);model_copy_authoring_checks::Failure(mode);
    }
}
}
