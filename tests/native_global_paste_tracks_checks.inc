#include "../src/plugins/native_global_paste.hpp"
namespace mikudancestudio {
int RegisterCameraKey(MMDApp*,const void*,int);
int RegisterLightKey(MMDApp*,const void*);
int RegisterSelfShadowKey(MMDApp*,const void*);
}
namespace {
plugins::GlobalPasteSource GlobalPasteTracksSource(){
    plugins::GlobalPasteSource source;
    for(unsigned i=0;i<4;++i){const unsigned frame=i==3?20:i==0?12:i==1?20:70;
        mdl::CameraClipboardRecord camera{};camera.frame=frame;camera.distance=-40.f-float(i);camera.fov=40+int(i)*7;camera.parentModel=-1;camera.perspective=std::uint8_t(i%2);
        for(unsigned axis=0;axis<3;++axis){camera.eye[axis]=float(i+axis);camera.target[axis]=.12f*float(i+axis);}
        std::memset(camera.interpolation,20,12);std::memset(camera.interpolation+12,107,12);source.cameras.push_back(camera);
        mdl::LightClipboardRecord light{};light.frame=frame;for(unsigned axis=0;axis<3;++axis){light.direction[axis]=-.12f*float(i+axis+1);light.color[axis]=.13f*float(i+axis+1);}source.lights.push_back(light);
        mdl::ShadowClipboardRecord shadow{};shadow.frame=frame;shadow.mode=std::uint8_t(i%3);shadow.distance=.01f*float(i+1);source.shadows.push_back(shadow);
    }return source;
}
MmsResult GlobalPasteTracksPlan(GlobalRegistrationFixture& f,unsigned family,const plugins::GlobalPasteSource& source,plugins::GlobalPasteFamilyResult& result){
    return family==0?plugins::PlanCameraKeyPastePart(*f.app,source,result):family==1?plugins::PlanLightKeyPastePart(*f.app,source,result):plugins::PlanShadowKeyPastePart(*f.app,source,result);
}
void GlobalPasteTracksPublish(GlobalRegistrationFixture& f,plugins::GlobalKeySelection& selection,const std::shared_ptr<plugins::GlobalPastePart>& part,bool forward,bool first=false){
    std::unique_ptr<plugins::PreparedGlobalPastePart> candidate;
    Check(part->Prepare(forward,candidate)==MMS_OK&&candidate,"global paste track privately prepares");
    auto marks=selection.Prepare(forward,first);
    Check(candidate->IsCurrent()&&marks->IsCurrent(),"global paste tracks and shared marks validate before first write");
    failAfter=0;candidate->PublishOwned();marks->Publish();candidate->PublishSelection();failAfter=-1;
    Check(!candidate->IsCurrent(),"published global paste part cannot publish again");
}
void GlobalPasteTracksOracle(GlobalRegistrationFixture& f,unsigned family,const plugins::GlobalPasteSource& source){
    f.app->CurrentFrame()=int(source.frameOffset);ClearGlobalFrameSelection(f.app.get());
    if(family==0)for(const auto& key:source.cameras)Check(RegisterCameraKey(f.app.get(),&key,600000),"legacy camera clipboard registrar accepts payload");
    else if(family==1)for(const auto& key:source.lights)Check(RegisterLightKey(f.app.get(),&key),"legacy light clipboard registrar accepts payload");
    else for(const auto& key:source.shadows)Check(RegisterSelfShadowKey(f.app.get(),&key),"legacy shadow clipboard registrar accepts payload");
}
void NativeGlobalPasteTracksChecks(){
    std::puts("  global paste tracks: legacy multi-source parity");std::fflush(stdout);
    for(unsigned family=0;family<3;++family)for(unsigned offset:{0u,7u}){
        GlobalRegistrationFixture f,oracle;auto source=GlobalPasteTracksSource();source.frameOffset=offset;
        const auto camera=GlobalRegistrationKeys(f.app->CameraKeys());const auto light=GlobalRegistrationKeys(f.app->LightKeys());const auto shadow=GlobalRegistrationKeys(f.app->ShadowKeys());
        const auto before=f.Digest();plugins::GlobalPasteFamilyResult result;
        Check(GlobalPasteTracksPlan(f,family,source,result)==MMS_OK&&result.part&&result.dataChanged&&f.Digest()==before,"global batch plans all sources without live writes");
        plugins::GlobalKeySelection selection(*f.app);GlobalPasteTracksPublish(f,selection,result.part,true,true);GlobalPasteTracksOracle(oracle,family,source);
        Check(GlobalRegistrationSame(f.app->CameraKeys(),GlobalRegistrationKeys(oracle.app->CameraKeys()))&&GlobalRegistrationSame(f.app->LightKeys(),GlobalRegistrationKeys(oracle.app->LightKeys()))&&
            GlobalRegistrationSame(f.app->ShadowKeys(),GlobalRegistrationKeys(oracle.app->ShadowKeys())),"camera light shadow batches match old slot order and repeated source overwrite");
        Check(f.app->LastRegisteredFrame()==9&&result.maximumInsertedFrame==70+offset,"parts report inserted maximum and do not own shared last frame");
        Check(f.app->CameraPositionX()==4.125f&&f.app->LightDirection()[0]==-.21737f&&f.app->ShadowDistance()==.075f,"global paste parts preserve unregistered authored values");
        GlobalPasteTracksPublish(f,selection,result.part,false);
        Check(GlobalRegistrationSame(f.app->CameraKeys(),camera)&&GlobalRegistrationSame(f.app->LightKeys(),light)&&GlobalRegistrationSame(f.app->ShadowKeys(),shadow),"global paste undo restores complete key and shared mark snapshots");
        GlobalPasteTracksPublish(f,selection,result.part,true);
    }
    std::puts("  global paste tracks: frame zero and no-data source parts");std::fflush(stdout);
    for(unsigned family=0;family<3;++family){
        GlobalRegistrationFixture f;auto source=GlobalPasteTracksSource();source.cameras.resize(1);source.lights.resize(1);source.shadows.resize(1);
        source.cameras[0].frame=source.lights[0].frame=source.shadows[0].frame=0;
        plugins::GlobalPasteFamilyResult result;Check(GlobalPasteTracksPlan(f,family,source,result)==MMS_OK&&result.part&&result.dataChanged,"frame zero has explicit private root target");
        const auto next=family==0?f.app->CameraKeys()[0].next:family==1?f.app->LightKeys()[0].next:f.app->ShadowKeys()[0].next;
        plugins::GlobalKeySelection selection(*f.app);GlobalPasteTracksPublish(f,selection,result.part,true,true);
        Check((family==0?f.app->CameraKeys()[0].next:family==1?f.app->LightKeys()[0].next:f.app->ShadowKeys()[0].next)==next,"frame zero overwrite keeps existing nonzero chain");
        plugins::GlobalPasteFamilyResult same;Check(GlobalPasteTracksPlan(f,family,source,same)==MMS_OK&&same.part&&!same.dataChanged&&!same.maximumInsertedFrame,"equal source retains selection participant but reports whole-family data no-op");
    }
    std::puts("  global paste tracks: pristine authored curves and finite samples");std::fflush(stdout);
    for(unsigned family=0;family<3;++family){
        GlobalRegistrationFixture f;const auto kind=family==0?curve_host::Kind::Camera:family==1?curve_host::Kind::Light:curve_host::Kind::Shadow;
        std::vector<curve_editor::Channel> channels;std::wstring error;
        Check(curve_host::Read(*f.app,kind,0,channels,error),"read channels for global paste authoring fixture");
        const unsigned lane=family==2?1:0;auto& keys=channels[lane].track.keys;
        keys[0].outWeight=.19;keys[0].outSlope=family==2?.001:.173;keys[1].inWeight=.31;keys[1].inSlope=family==2?-.001:-.113;
        keys[1].interpolation=animation_curve::Interpolation::Bezier;
        Check(curve_host::Write(*f.app,kind,0,channels,error),"install custom global paste authoring handles");
        auto source=GlobalPasteTracksSource();plugins::GlobalPasteFamilyResult result;
        Check(GlobalPasteTracksPlan(f,family,source,result)==MMS_OK&&result.part,"global paste retains pristine authored track");
        plugins::GlobalKeySelection selection(*f.app);GlobalPasteTracksPublish(f,selection,result.part,true,true);
        std::vector<curve_editor::Channel> after;Check(curve_host::Read(*f.app,kind,0,after,error),"normal global reader reconciles pasted authoring");
        GlobalPasteTracksPublish(f,selection,result.part,false);
        std::vector<curve_editor::Channel> restored;Check(curve_host::Read(*f.app,kind,0,restored,error)&&restored[lane].track.keys[0].outWeight==.19&&
            restored[lane].track.keys[1].inWeight==.31,"global paste undo restores pristine custom handles after reader reconciliation");
    }
    {
        GlobalRegistrationFixture f;auto source=GlobalPasteTracksSource();const auto before=f.Digest();plugins::GlobalPasteFamilyResult result;
        source.cameras[0].parentModel=999;
        Check(plugins::PlanCameraKeyPastePart(*f.app,source,result)==MMS_E_ARGUMENT&&!result.part&&f.Digest()==before,"camera paste rejects absent parent before publication");
        source=GlobalPasteTracksSource();source.frameOffset=unsigned(frame_limits::maximum);
        Check(plugins::PlanLightKeyPastePart(*f.app,source,result)==MMS_E_ARGUMENT&&!result.part&&f.Digest()==before,"global clipboard frame overflow refuses entire part");
        source=GlobalPasteTracksSource();source.shadows[0].distance=std::numeric_limits<float>::quiet_NaN();
        Check(plugins::PlanShadowKeyPastePart(*f.app,source,result)==MMS_E_ARGUMENT&&!result.part&&f.Digest()==before,"global clipboard nonfinite value never changes scene");
    }
    {
        GlobalRegistrationFixture f;plugins::GlobalPasteSource source;source.lights.resize(2);
        source.lights[0].frame=0;source.lights[1].frame=12;
        source.lights[0].direction[0]=-std::numeric_limits<float>::max();source.lights[1].direction[0]=std::numeric_limits<float>::max();
        plugins::GlobalPasteFamilyResult result;const auto before=f.Digest();
        Check(plugins::PlanLightKeyPastePart(*f.app,source,result)==MMS_OK&&result.part,"finite extreme light sources prepare a private plan");
        std::unique_ptr<plugins::PreparedGlobalPastePart> candidate;
        Check(result.part->Prepare(true,candidate)==MMS_E_ARGUMENT&&!candidate&&f.Digest()==before,"nonfinite target interpolation rejects even while dirty live light would be retained");
    }
    std::puts("  global paste tracks: cancellation failures identity and ABA");std::fflush(stdout);
    for(unsigned family=0;family<3;++family){
        GlobalRegistrationFixture f;auto source=GlobalPasteTracksSource();const auto before=f.Digest();plugins::GlobalPasteFamilyResult result;bool success=false;
        for(long limit=0;limit<256;++limit){failAfter=limit;const auto status=GlobalPasteTracksPlan(f,family,source,result);failAfter=-1;
            Check(f.Digest()==before,"global paste planning OOM remains private");if(status==MMS_OK){success=bool(result.part);break;}Check(status==MMS_E_MEMORY&&!result.part,"global paste planning reports allocation refusal");}
        Check(success,"global paste planning failure sweep completes");success=false;
        for(long limit=0;limit<512;++limit){std::unique_ptr<plugins::PreparedGlobalPastePart> candidate;failAfter=limit;const auto status=result.part->Prepare(true,candidate);failAfter=-1;
            Check(f.Digest()==before,"global paste candidate OOM cancellation never publishes growth pose or curves");if(status==MMS_OK){Check(candidate&&candidate->IsCurrent(),"global paste candidate eventually complete");success=true;break;}Check(status==MMS_E_MEMORY&&!candidate,"global paste candidate reports allocation refusal");}
        Check(success,"global paste preparation failure sweep completes");
        plugins::GlobalKeySelection selection(*f.app);GlobalPasteTracksPublish(f,selection,result.part,true,true);
        std::unique_ptr<plugins::PreparedGlobalPastePart> stale;Check(result.part->Prepare(false,stale)==MMS_OK&&stale->IsCurrent(),"prepare candidate before global paste ABA");
        GlobalPasteTracksPublish(f,selection,result.part,false);GlobalPasteTracksPublish(f,selection,result.part,true);
        Check(!stale->IsCurrent(),"global paste part rejects ABA candidate after undo redo");stale.reset();
        if(family==0)Check(mdl::KeyPoolEnsure(f.app->CameraKeys(),1000),"grow camera pool after paste");
        else if(family==1)Check(mdl::KeyPoolEnsure(f.app->LightKeys(),1000),"grow light pool after paste");
        else Check(mdl::KeyPoolEnsure(f.app->ShadowKeys(),1000),"grow shadow pool after paste");
        GlobalPasteTracksPublish(f,selection,result.part,false);GlobalPasteTracksPublish(f,selection,result.part,true);
    }
}
}
