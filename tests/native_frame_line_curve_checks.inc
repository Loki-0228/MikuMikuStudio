// Include after native_frame_line_checks.inc. Uses the native global fixture.
namespace {
using FLCChannels=std::vector<curve_editor::Channel>;
using FLCBytes=std::vector<std::uint8_t>;
curve_host::Kind FLCKind(unsigned family){return family?curve_host::Kind::Light:curve_host::Kind::Camera;}
FLCChannels FLCRead(GlobalRegistrationFixture& f,unsigned family){FLCChannels channels;std::wstring error;
    Check(curve_host::Read(*f.app,FLCKind(family),0,channels,error),"read frame-line free authoring");return channels;}
FLCBytes FLCEncode(const FLCChannels& channels){std::vector<animation_curve::NamedTrack> tracks;
    for(const auto& channel:channels)tracks.push_back({channel.id,channel.track});FLCBytes bytes;std::string error;
    Check(animation_curve::Encode(tracks,bytes,error),"encode exact frame-line authored keys and handles");return bytes;}
const animation_curve::Key* FLCAt(const animation_curve::Track& track,int frame){
    const auto found=std::find_if(track.keys.begin(),track.keys.end(),[&](const auto& key){return key.frame==frame;});return found==track.keys.end()?nullptr:&*found;}
void FLCAuthor(GlobalRegistrationFixture& f,unsigned family,bool rootCollision=false){
    if(rootCollision){f.app->CameraKeys()[1].frame=1;f.app->LightKeys()[1].frame=1;}
    auto channels=FLCRead(f,family);
    auto& sparse=channels[0];sparse.track.keys.erase(sparse.track.keys.begin()+1);sparse.nativeControls.clear();sparse.controlGeometry.clear();sparse.sample={};
    sparse.track.keys[0].value=.123456789012;sparse.track.keys[1].value=3.987654321098;
    for(auto& key:sparse.track.keys){key.tangent=animation_curve::Tangent::Free;key.interpolation=animation_curve::Interpolation::Bezier;
        key.inWeight=.31;key.outWeight=.19;key.inSlope=-.017;key.outSlope=.013;}
    channels[0].selected=false;channels[0].group=7;channels[0].collections={{"free-track",L"free track metadata"}};
    auto& stepped=channels[1];stepped.track.keys.back().interpolation=animation_curve::Interpolation::Step;
    stepped.nativeControls.clear();stepped.controlGeometry.clear();stepped.sample={};
    if(!family){for(std::size_t i=0;i<channels[3].track.keys.size();++i)channels[3].track.keys[i].value=360.0*i+.000012345678;}
    std::wstring error;Check(curve_host::Write(*f.app,FLCKind(family),0,channels,error),"author sparse Free and Step channels with distinct metadata and high precision");
    f.app->SceneModified()=0;
}
int FLCMapped(int source,int threshold,bool insert,bool rootCollision){
    if(!source)return !insert&&threshold==0&&rootCollision?-1:0;
    if(source<threshold)return source;
    if(insert)return source+1;
    return source==threshold?-1:source-1;
}
void FLCCompare(const FLCChannels& source,const FLCChannels& target,int threshold,bool insert,bool collision){
    Check(source.size()==target.size(),"frame-line preserves every authored channel");
    for(std::size_t lane=0;lane<source.size();++lane){const auto& before=source[lane];const auto& after=target[lane];
        Check(before.id==after.id&&before.label==after.label&&before.unit==after.unit&&before.selected==after.selected&&before.group==after.group&&
            before.minimum==after.minimum&&before.maximum==after.maximum&&before.collections.size()==after.collections.size(),"frame-line preserves channel identity, labels, range, selection and grouping");
        for(std::size_t i=0;i<before.collections.size();++i)Check(before.collections[i].id==after.collections[i].id&&before.collections[i].label==after.collections[i].label,
            "frame-line retains authored collection paths");
        std::size_t count=0;
        for(const auto& old:before.track.keys){const int frame=FLCMapped(old.frame,threshold,insert,collision);if(frame<0)continue;++count;
            const auto* kept=FLCAt(after.track,frame);Check(kept&&kept->value==old.value&&
                (kept->tangent==old.tangent||(old.tangent==animation_curve::Tangent::Automatic&&kept->tangent==animation_curve::Tangent::Free))&&kept->interpolation==old.interpolation,
                "retained native-frame identity keeps exact authored values, tangent and interpolation modes");
            if(!(collision&&threshold==0&&!insert)&&old.frame!=1)Check(kept->inWeight==old.inWeight&&kept->outWeight==old.outWeight,
                "frame-line preserves normalized free handle weights");
        }
        if(collision&&threshold==0&&!insert&&!FLCAt(before.track,1)){
            ++count;const auto* root=FLCAt(after.track,0);const double sample=before.sample?before.sample(before,1).value:animation_curve::Evaluate(before.track,1).value;
            Check(root&&root->value==sample,"root collision on a sparse lane samples its original exact authoring before removing frame zero");
        }
        Check(after.track.keys.size()==count,"frame-line keeps sparse lanes sparse and adds only a required root anchor");
        for(std::size_t end=1;end<before.track.keys.size();++end){const auto& a=before.track.keys[end-1];const auto& b=before.track.keys[end];
            const int mappedA=FLCMapped(a.frame,threshold,insert,collision),mappedB=FLCMapped(b.frame,threshold,insert,collision);
            if(mappedA<0||mappedB<0)continue;const auto* left=FLCAt(after.track,mappedA);const auto* right=FLCAt(after.track,mappedB);
            if(before.track.discrete)continue;const double ratio=double(b.frame-a.frame)/double(mappedB-mappedA);
            if(before.nativeControls.empty())Check(std::abs(left->outSlope-a.outSlope*ratio)<1e-12&&std::abs(right->inSlope-b.inSlope*ratio)<1e-12,
                "retimed per-frame free slopes preserve old segment geometry by its old/new duration ratio");
            if(end<before.nativeControls.size()){
                const auto targetEnd=std::size_t(right-after.track.keys.data());
                Check(targetEnd<after.nativeControls.size()&&before.nativeControls[end]==after.nativeControls[targetEnd],"retained camera intervals keep all original raw control cells");
            }
        }
    }
}
void FLCAuthoredRoundTrips(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(unsigned family=0;family<2;++family)for(bool insert:{false,true})for(bool collision:{false,true})
        for(int threshold:{0,1,12,17,31,40})for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
        GlobalRegistrationFixture f;FLCAuthor(f,family,collision);Check(f.history.SetMode(mode)==MMS_OK,"configure explicit authored frame-line history mode");
        const auto source=FLCRead(f,family);const auto before=FLCEncode(source);const auto nativeCamera=GlobalRegistrationKeys(f.app->CameraKeys());
        const auto nativeLight=GlobalRegistrationKeys(f.app->LightKeys());const auto digest=f.Digest();
        const auto cameraX=f.app->CameraPositionX(),lightX=f.app->LightDirection()[0];std::shared_ptr<plugins::HistoryChange> change;
        Check(FrameLinePlan(f,family,threshold,insert,change)==MMS_OK&&f.Digest()==digest&&FLCEncode(FLCRead(f,family))==before,
            "complete authored frame mapping plans without reconciling or changing live native keys and poses");
        if(!change){Check(threshold==40,"authored frame-line above all author and native keys is a true no-op");continue;}
        FrameLineCommit(f.history,change);const auto after=FLCRead(f,family);const auto encodedAfter=FLCEncode(after);
        FLCCompare(source,after,threshold,insert,collision);
        Check(f.history.UndoCount()==1&&f.app->CurrentFrame()==7&&f.app->CameraPositionX()==cameraX&&f.app->LightDirection()[0]==lightX,
            "authored frame-line records one explicit node in both modes without seeking or replacing dirty author poses");
        for(int frame:{0,5,13,44}){f.app->CurrentFrame()=frame;f.app->CameraMode()=0;f.app->SelectedModelSlot()=1;
            Check(f.history.History(false)==MMS_OK&&GlobalRegistrationSame(f.app->CameraKeys(),nativeCamera)&&GlobalRegistrationSame(f.app->LightKeys(),nativeLight)&&
                FLCEncode(FLCRead(f,family))==before&&f.app->CurrentFrame()==frame,"undo exactly restores all sparse free lanes and native bytes at unrelated UI targets");
            Check(f.history.History(true)==MMS_OK&&FLCEncode(FLCRead(f,family))==encodedAfter&&f.app->CurrentFrame()==frame&&
                f.app->CameraPositionX()==cameraX&&f.app->LightDirection()[0]==lightX,"redo repeatedly derives the exact retimed authoring from its immutable seed and preserves dirty poses");
        }
    }
}
void FLCFailures(){
    for(unsigned family=0;family<2;++family){GlobalRegistrationFixture f;FLCAuthor(f,family);const auto before=f.Digest();const auto curves=FLCEncode(FLCRead(f,family));
        std::shared_ptr<plugins::HistoryChange> change;bool complete=false;
        for(long allowance=0;allowance<2048;++allowance){failAfter=allowance;const auto result=FrameLinePlan(f,family,12,true,change);failAfter=-1;
            Check(f.Digest()==before&&FLCEncode(FLCRead(f,family))==curves,"mapped planning OOM never changes native keys, free authoring or current pose");
            if(result==MMS_OK){complete=bool(change);break;}Check(result==MMS_E_MEMORY&&!change,"mapped frame-line planning reports allocation failure atomically");}
        Check(complete,"mapped planning allocation sweep reaches success");complete=false;
        for(long allowance=0;allowance<2048;++allowance){std::unique_ptr<plugins::PreparedHistoryChange> candidate;failAfter=allowance;
            const auto result=change->Prepare(true,candidate);failAfter=-1;
            Check(f.Digest()==before&&FLCEncode(FLCRead(f,family))==curves,"mapped candidate OOM and cancellation leave all authoring unchanged");
            if(result==MMS_OK){complete=candidate&&candidate->IsCurrent();break;}Check(result==MMS_E_MEMORY&&!candidate,"mapped candidate reports detached preparation OOM");}
        Check(complete,"mapped candidate allocation sweep reaches success");
        plugins::SharedHistory budget(f.storage,{},32,change->RetainedBytes()-1);std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        Check(budget.PrepareNative("mapped budget",change,token)==MMS_E_MEMORY&&!token&&f.Digest()==before,"owned frame map and native source dependencies are budgeted before publication");
        if(family)f.app->LightKeys()[0].direction[0]+=1;else f.app->CameraKeys()[0].eye[0]+=1;
        std::unique_ptr<plugins::PreparedHistoryChange> candidate;Check(change->Prepare(true,candidate)==MMS_E_STALE&&!candidate,
            "first frame-line preparation rejects an unchanged-frame native source modified since mapping");
        if(family)f.app->LightKeys()[0].direction[0]-=1;else f.app->CameraKeys()[0].eye[0]-=1;
        Check(change->Prepare(false,candidate)==MMS_E_STALE&&!candidate,"an unpublished frame-line cannot prepare reverse history");
    }
    for(unsigned family=0;family<2;++family){GlobalRegistrationFixture f;FLCAuthor(f,family);std::shared_ptr<plugins::HistoryChange> change;
        Check(FrameLinePlan(f,family,12,true,change)==MMS_OK&&change,"prepare mapped author metadata stale case");
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;Check(f.history.PrepareNative("mapped stale",change,token)==MMS_OK,"prepare frame-line before an independent author changes metadata");
        auto edited=FLCRead(f,family);edited[0].collections.push_back({"external",L"external editor"});std::wstring error;
        Check(curve_host::Write(*f.app,FLCKind(family),0,edited,error),"independent editor changes only authored collection metadata");const auto after=f.Digest();
        const auto external=FLCEncode(FLCRead(f,family));
        Check(CommitRegistrationNoAllocation(f.history,std::move(token))==MMS_E_STALE&&f.Digest()==after&&FLCEncode(FLCRead(f,family))==external&&!f.history.UndoCount(),
            "stale mapped transaction cannot overwrite independently authored metadata or publish native patches");
    }
}
void FLCLoadAuthorOnly(GlobalRegistrationFixture& f,unsigned family,int authorFrame){
    FLCAuthor(f,family);auto channels=FLCRead(f,family);channels[0].track.keys.back().frame=authorFrame;
    std::vector<animation_curve::NamedTrack> named;
    for(const auto& channel:channels)named.push_back({std::to_string(int(FLCKind(family)))+"/0/"+channel.id,channel.track});
    FLCBytes bytes;std::string error;Check(animation_curve::Encode(named,bytes,error),"encode a valid PMM authoring frame beyond the native mirror");
    pmm_extension::Document document;pmm_extension::Set(document,"org.mikudancestudio.animation_curves.v1",std::move(bytes));curve_host::Load(*f.app,document);
    Check(FLCAt(FLCRead(f,family)[0].track,authorFrame)!=nullptr,"PMM restores an independent authored frame without creating a native row");
}
std::array<float,12> FLCPose(MMDApp& app,unsigned family){std::array<float,12> pose{};
    if(family){for(unsigned axis=0;axis<3;++axis){pose[axis]=app.LightDirection()[axis];pose[axis+3]=app.LightColor()[axis];}}
    else{for(unsigned axis=0;axis<3;++axis){pose[axis]=app.CameraPositionAxis(axis);pose[axis+3]=app.CameraRotation()[axis];}
        pose[6]=app.CameraDistance();pose[7]=app.CameraFov();pose[8]=float(app.CameraPerspective());pose[9]=float(app.CameraParentModel());pose[10]=float(app.CameraParentBone());}
    return pose;
}
void FLCIndependentFramesAndEvaluation(){
    for(unsigned family=0;family<2;++family){GlobalRegistrationFixture f;FLCLoadAuthorOnly(f,family,55);
        const auto source=FLCEncode(FLCRead(f,family));const auto camera=GlobalRegistrationKeys(f.app->CameraKeys());const auto light=GlobalRegistrationKeys(f.app->LightKeys());
        const auto last=f.app->LastRegisteredFrame();std::shared_ptr<plugins::HistoryChange> change;
        Check(FrameLinePlan(f,family,40,true,change)==MMS_OK&&change,"author-only movement plans a transaction even when no native key needs a patch");
        FrameLineCommit(f.history,change);const auto target=FLCEncode(FLCRead(f,family));
        Check(FLCAt(FLCRead(f,family)[0].track,56)&&!FLCAt(FLCRead(f,family)[0].track,55)&&GlobalRegistrationSame(f.app->CameraKeys(),camera)&&
            GlobalRegistrationSame(f.app->LightKeys(),light)&&f.app->LastRegisteredFrame()==last,"author-only frame-line changes its free key while preserving every native byte and native maximum");
        for(unsigned repeat=0;repeat<5;++repeat)Check(f.history.History(false)==MMS_OK&&FLCEncode(FLCRead(f,family))==source&&
            f.history.History(true)==MMS_OK&&FLCEncode(FLCRead(f,family))==target,"author-only frame-line remains one exact reversible history transaction");
    }
    for(unsigned family=0;family<2;++family){GlobalRegistrationFixture f;FLCLoadAuthorOnly(f,family,frame_limits::maximum);
        const auto source=FLCEncode(FLCRead(f,family));const auto digest=f.Digest();std::shared_ptr<plugins::HistoryChange> change;
        Check(FrameLinePlan(f,family,40,true,change)==MMS_E_ARGUMENT&&!change&&f.Digest()==digest&&FLCEncode(FLCRead(f,family))==source,
            "unrepresentable author-only insertion is rejected before creating native or free authoring changes");
    }
    for(unsigned family=0;family<2;++family){GlobalRegistrationFixture f;FLCLoadAuthorOnly(f,family,1);
        const auto source=FLCRead(f,family);const auto before=FLCEncode(source);const auto cameraRoot=f.app->CameraKeys()[0];const auto lightRoot=f.app->LightKeys()[0];
        std::shared_ptr<plugins::HistoryChange> change;Check(FrameLinePlan(f,family,0,false,change)==MMS_OK&&change,
            "delete-zero maps a PMM-only frame one as the authored root winner even without a matching native row");
        FrameLineCommit(f.history,change);const auto target=FLCRead(f,family);FLCCompare(source,target,0,false,true);const auto after=FLCEncode(target);
        Check(target[0].track.keys.size()==1&&target[0].track.keys[0].value==source[0].track.keys.back().value&&
            !std::memcmp(&f.app->CameraKeys()[0],&cameraRoot,sizeof(cameraRoot))&&!std::memcmp(&f.app->LightKeys()[0],&lightRoot,sizeof(lightRoot)),
            "author-only root collision retains its exact winning value while native frame zero keeps the original legacy payload");
        for(unsigned repeat=0;repeat<4;++repeat)Check(f.history.History(false)==MMS_OK&&FLCEncode(FLCRead(f,family))==before&&
            f.history.History(true)==MMS_OK&&FLCEncode(FLCRead(f,family))==after,"author-only root collision repeatedly restores the exact original and winning sparse layers");
    }
    for(unsigned family=0;family<2;++family)for(bool insert:{false,true}){
        GlobalRegistrationFixture f;FLCAuthor(f,family);f.app->CurrentFrame()=7;
        curve_host::Apply(f.app.get(),FLCKind(family),7);const auto before=FLCPose(*f.app,family);std::shared_ptr<plugins::HistoryChange> change;
        Check(FrameLinePlan(f,family,12,insert,change)==MMS_OK&&change,"plan retime while current pose follows free authoring");FrameLineCommit(f.history,change);
        const auto target=FLCPose(*f.app,family);curve_host::Apply(f.app.get(),FLCKind(family),7);
        Check(target==FLCPose(*f.app,family)&&target!=before&&f.app->CurrentFrame()==7,"a pose following its authored track follows the retimed sample without moving the playhead");
        for(unsigned repeat=0;repeat<4;++repeat)Check(f.history.History(false)==MMS_OK&&FLCPose(*f.app,family)==before&&
            f.history.History(true)==MMS_OK&&FLCPose(*f.app,family)==target,"both history directions compare against the correct original or retimed source sample");
    }
}
void FLCAutomaticShape(){
    for(unsigned family=0;family<2;++family){GlobalRegistrationFixture f;FLCAuthor(f,family);auto channels=FLCRead(f,family);auto& automatic=channels[2];
        automatic.nativeControls.clear();automatic.controlGeometry.clear();automatic.sample={};
        const double values[]{.12,1.73,3.4};
        for(std::size_t i=0;i<automatic.track.keys.size();++i){auto& key=automatic.track.keys[i];key.value=values[i];
            key.tangent=animation_curve::Tangent::Automatic;key.interpolation=animation_curve::Interpolation::Bezier;}
        animation_curve::UpdateAutomatic(automatic.track);std::wstring error;
        Check(curve_host::Write(*f.app,FLCKind(family),0,channels,error),"author a monotone automatic track whose two adjacent durations will retime differently");
        const auto source=FLCRead(f,family);std::shared_ptr<plugins::HistoryChange> change;
        Check(FrameLinePlan(f,family,12,true,change)==MMS_OK&&change,"plan automatic tangent retime with unequal incoming and outgoing time scaling");
        FrameLineCommit(f.history,change);const auto target=FLCRead(f,family);const auto encoded=FLCEncode(target);FLCCompare(source,target,12,true,false);
        Check(std::any_of(target[2].track.keys.begin(),target[2].track.keys.end(),[](const auto& key){return key.tangent==animation_curve::Tangent::Free;}),
            "retime freezes an automatic tangent as Free when automatic recomputation would change preserved geometry");
        auto recomputed=target;for(auto& channel:recomputed)animation_curve::UpdateAutomatic(channel.track);
        // The first key has no incoming interval; the last has no outgoing
        // interval. Automatic refresh may rewrite only these unused slopes.
        // Canonicalize them on an encoding copy, keeping the raw refreshed
        // tracks below for independent sampling and every used field exact.
        auto encodingCopy=recomputed;
        for(std::size_t lane=0;lane<encodingCopy.size();++lane){auto& keys=encodingCopy[lane].track.keys;
            keys.front().inSlope=target[lane].track.keys.front().inSlope;keys.back().outSlope=target[lane].track.keys.back().outSlope;}
        if(FLCEncode(encodingCopy)!=encoded){
            for(std::size_t lane=0;lane<target.size();++lane)for(std::size_t index=0;index<target[lane].track.keys.size();++index){
                const auto& before=target[lane].track.keys[index];const auto& after=encodingCopy[lane].track.keys[index];
                if(before.value!=after.value||before.inSlope!=after.inSlope||before.outSlope!=after.outSlope||before.inWeight!=after.inWeight||before.outWeight!=after.outWeight||before.interpolation!=after.interpolation||before.tangent!=after.tangent)
                    std::printf("DIAG automatic family=%u lane=%zu key=%zu frame=%d tangent=%u/%u value=%.17g/%.17g inSlope=%.17g/%.17g usedIn=%u outSlope=%.17g/%.17g usedOut=%u inWeight=%.17g/%.17g outWeight=%.17g/%.17g\n",
                        family,lane,index,before.frame,unsigned(before.tangent),unsigned(after.tangent),before.value,after.value,before.inSlope,after.inSlope,unsigned(index>0),before.outSlope,after.outSlope,unsigned(index+1<target[lane].track.keys.size()),before.inWeight,after.inWeight,before.outWeight,after.outWeight);
            }std::fflush(stdout);
        }
        Check(FLCEncode(encodingCopy)==encoded,"automatic recomputation after retime preserves every effective handle and authored field exactly");
        for(double frame:{0.,.5,7.25,13.,20.125,32.})Check(animation_curve::Evaluate(target[2].track,frame).value==animation_curve::Evaluate(recomputed[2].track,frame).value,
            "automatic tangent refresh leaves retimed fractional samples unchanged");
        Check(target[0].track.keys.front().tangent==animation_curve::Tangent::Free&&target[0].track.keys.front().outWeight==.19,
            "automatic-mode protection preserves an independently authored Free lane");
    }
}
void NativeFrameLineCurveChecks(){
    std::puts("  frame lines: sparse Free/Step authoring, raw controls, root collision and exact repeated history");FLCAuthoredRoundTrips();
    std::puts("  frame lines: map budget, OOM, cancellation and stale native/authoring sources");FLCFailures();
    std::puts("  frame lines: independent PMM author frames, overflow and following-pose sampling");FLCIndependentFramesAndEvaluation();
    std::puts("  frame lines: automatic tangent refresh preserves retimed geometry");FLCAutomaticShape();
}
}
