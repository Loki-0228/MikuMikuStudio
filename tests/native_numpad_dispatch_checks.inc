// Include after native_numpad_view_checks.inc; drive the real guarded history
// dispatcher as well as the direct journal API used by the numeric tests.
#include "../src/plugins/native_object_commands.hpp"
#include "mikudancestudio/bake_ui_lock.hpp"
namespace {
void NativeNumpadDispatchChecks(){
    using Mode=plugins::SharedHistory::RecordingMode;
    std::puts("  numpad dispatch: authorized history mutation/UI locks and unrelated locks");
    MMDApp* retired=nullptr;
    {NumpadFixture authored;retired=authored.app.get();NumpadResetAuthor(authored);
        Check(curve_host::Has(retired,curve_host::Kind::Camera),"numpad fixture lifecycle begins with an owned camera authoring entry");}
    // Has checks the address-keyed table only; do not dereference the retired
    // app. This catches a stale entry even if an allocator does not reuse it.
    Check(!curve_host::Has(retired,curve_host::Kind::Camera),"destroying an authored numpad fixture removes its app-keyed curves before releasing the native pool");
    {NumpadFixture fresh;fresh.Branch(2);std::shared_ptr<plugins::HistoryChange> change;
        Check(!curve_host::Has(fresh.app.get(),curve_host::Kind::Camera)&&plugins::PlanViewportNumpadPresets(*fresh.app,1u,change)==MMS_OK&&change,
            "a newly allocated numpad app captures native model-zero state without a foreign authored-pool lineage");}
    for(unsigned branch=0;branch<3;++branch)for(unsigned mask:{1u<<4,1u})for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(bool deferred:{false,true}){
        NumpadFixture f;f.Branch(branch,CameraAttachmentReference::SelectedBone);plugins::NativePoseTestBinding binding(*f.app,f.history);
        const auto before=NumpadRead(*f.app);const auto expected=NumpadExpected(f,mask,branch!=2,CameraAttachmentReference::SelectedBone);
        bool changed=false;const auto seeded=plugins::AdjustViewportNumpadPresets(*f.app,f.history,std::uint16_t(mask),changed);
        if(seeded!=MMS_OK||!changed||f.history.UndoCount()!=1){
            std::printf("DIAG numpad dispatch seed status=%d changed=%u undo=%zu mode=%u branch=%u mask=%u deferred=%u capture=%p uiLock=%u mutation=%u\n",
                int(seeded),unsigned(changed),f.history.UndoCount(),unsigned(f.history.Mode()),branch,mask,unsigned(deferred),static_cast<void*>(GetCapture()),unsigned(bake::UiLocked()),unsigned(f.app->SceneMutationInProgress()));std::fflush(stdout);
        }
        Check(seeded==MMS_OK&&changed&&f.history.UndoCount()==1,
            "seed a retained numpad adjustment before selecting either recording mode for history dispatch");
        Check(f.history.SetMode(mode)==MMS_OK,"guarded preset replay accepts either current recording policy");
        const auto after=NumpadRead(*f.app);Check(NumpadSame(after,expected),"guarded replay fixture starts from the exact independent preset oracle");
        f.app->CurrentFrame()=53;f.app->CameraMode()=branch==0?0:1;f.app->state.followCameraEnabled=0;f.app->SelectedModelSlot()=1;
        const auto entries=f.history.UndoCount();
        {
            ScopedSceneMutation mutation(*f.app);const auto stable=NumpadRead(*f.app);
            Check(!plugins::NativeObjectPublicationAuthorized(f.app.get())&&f.history.History(false)==MMS_E_BUSY&&
                plugins::RunBoundObjectHistory(*f.app,f.history,false,deferred)==MMS_E_BUSY&&NumpadSame(NumpadRead(*f.app),stable)&&f.history.UndoCount()==entries,
                "an ordinary scene-mutation scope cannot replay a preset or impersonate the authorized object-history publication");
            std::shared_ptr<plugins::HistoryChange> initial;Check(plugins::PlanViewportNumpadPresets(*f.app,1u<<6,initial)==MMS_E_BUSY&&!initial,
                "ordinary scene mutation also refuses initial preset planning");
        }
        {
            bake::ScopedUiLock lock(f.app->MainWindow());const auto stable=NumpadRead(*f.app);
            Check(!plugins::NativeObjectPublicationAuthorized(f.app.get())&&f.history.History(false)==MMS_E_BUSY&&
                plugins::RunBoundObjectHistory(*f.app,f.history,false,deferred)==MMS_E_BUSY&&NumpadSame(NumpadRead(*f.app),stable)&&f.history.UndoCount()==entries,
                "an ordinary UI lock still rejects preset history and cannot receive the dispatcher's publication exception");
            std::shared_ptr<plugins::HistoryChange> initial;Check(plugins::PlanViewportNumpadPresets(*f.app,1u<<6,initial)==MMS_E_BUSY&&!initial,
                "ordinary UI locking also refuses initial preset planning");
        }
        for(unsigned repeat=0;repeat<3;++repeat){
            Check(plugins::RunBoundObjectHistory(*f.app,f.history,false,deferred)==MMS_OK&&NumpadSame(NumpadRead(*f.app),before)&&
                f.history.UndoCount()==0&&f.history.RedoCount()==1&&f.app->CurrentFrame()==53,
                "real object-history dispatch undoes the preset under its authorized scene and UI guards without seeking");
            Check(!plugins::NativeObjectOperationActive(f.app.get())&&!plugins::NativeObjectPublicationAuthorized(f.app.get())&&!f.app->SceneMutationInProgress()&&!bake::UiLocked(),
                "history dispatch releases every operation, publication, mutation and UI guard before returning");
            Check(plugins::RunBoundObjectHistory(*f.app,f.history,true,deferred)==MMS_OK&&NumpadSame(NumpadRead(*f.app),after)&&
                f.history.UndoCount()==1&&!f.history.RedoCount()&&f.app->CurrentFrame()==53,
                "real object-history dispatch redoes the frozen preset in either recording mode and both immediate/deferred UI paths");
        }
    }
}
}
