#include "native_pose_scope.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "scene_object_history.hpp"
#include "mikudancestudio/native_edit_history.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/plugin_scene.hpp"

namespace mikudancestudio::plugins {
namespace {
thread_local NativePoseTestBinding* binding=nullptr;
MmsResult PoseResult(native_history::Result result) noexcept {
    return result==native_history::Result::Ready?MMS_OK:
        result==native_history::Result::Memory?MMS_E_MEMORY:MMS_E_STALE;
}
// The extra allowance covers the shared ownership control block, not an exact
// CRT allocation measurement. Resident model allocations belong to the caller.
constexpr std::uint64_t kSharedBlockAllowance=4*sizeof(void*);
struct PoseChange final:HistoryChange {
    MMDApp& app;
    unsigned slot;
    unsigned char* model;
    std::shared_ptr<RetainedSceneObject> lifetime;
    std::unique_ptr<native_history::OwnedPoseEdit> record;
    PoseChange(MMDApp& a,unsigned s):app(a),slot(s),model(a.ModelSlot(s)),lifetime(RetainSceneObject(a,MMS_OBJECT_MODEL,s)) {}
    bool Present() const noexcept {
        return lifetime->Present()&&app.ModelSlot(slot)==model&&SceneObjectPresent(model,lifetime->Id());
    }
    struct Candidate final:PreparedHistoryChange {
        MMDApp& app;
        unsigned slot;
        unsigned char* model;
        std::shared_ptr<RetainedSceneObject> lifetime;
        native_history::ModelEdit edit;
        explicit Candidate(const PoseChange& source):app(source.app),slot(source.slot),model(source.model),lifetime(source.lifetime) {}
        bool IsCurrent() const noexcept override {
            return lifetime->Present()&&app.ModelSlot(slot)==model&&SceneObjectPresent(model,lifetime->Id())&&edit.IsCurrent(app.CurrentFrame());
        }
        void Publish() noexcept override { edit.Publish(app.CurrentFrame()); }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();if(!record||!Present())return MMS_E_STALE;
        return Boundary([&]() -> MmsResult {
            auto next=std::make_unique<Candidate>(*this);
            const auto result=record->Prepare(model,app.CurrentFrame(),forward,next->edit);
            if(result!=native_history::Result::Ready)return PoseResult(result);
            out=std::move(next);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        const auto payload=record?record->RetainedBytes():0;
        constexpr auto own=sizeof(PoseChange)+kSharedBlockAllowance;
        return payload>UINT64_MAX-own?UINT64_MAX:payload+own;
    }
};
}
struct NativePoseScope::Impl {
    SharedHistory& history;
    std::shared_ptr<PoseChange> change;
    native_history::PoseEditCapture capture;
    std::unique_ptr<SharedHistory::AppliedTransaction> transaction;
    bool active=false;
    explicit Impl(SharedHistory& value):history(value) {}
    bool Cancel() noexcept {
        if(!active)return true;
        // Check membership before the pointer-based capture can dereference it.
        const bool restored=change->Present()&&capture.Rollback();
        if(!restored)history.Poison();
        active=false;transaction.reset();return restored;
    }
    ~Impl(){Cancel();}
};
NativePoseScope::NativePoseScope()=default;
NativePoseScope::~NativePoseScope()=default;
MmsResult NativePoseScope::Begin(MMDApp& app,SharedHistory& history,unsigned slot,std::string_view label) noexcept {
    if(impl_)return MMS_E_BUSY;
    // Validate thread and the host gate before reading the model. There are no
    // callbacks or window messages between this probe and the final reserve.
    if(auto result=history.Enter())return result;
    history.Leave();
    if(slot>=kModelSlotCount||!app.ModelSlot(slot)||app.PlaybackActive()||app.FrameStepPlayback()||
        app.state.recordPlaybackStartPending||app.SceneMutationInProgress())return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        auto next=std::make_unique<Impl>(history);next->change=std::make_shared<PoseChange>(app,slot);
        const auto captured=next->capture.Begin(next->change->model);
        if(captured!=native_history::Result::Ready)return PoseResult(captured);
        const auto payload=next->capture.MaximumRetainedBytes();
        constexpr auto own=sizeof(PoseChange)+kSharedBlockAllowance;
        if(payload>UINT64_MAX-own)return MMS_E_MEMORY;
        if(auto result=history.ReserveAppliedNative(std::string(label),payload+own,next->transaction))return result;
        next->active=true;impl_=std::move(next);return MMS_OK;
    });
}
MmsResult NativePoseScope::Commit(bool& changed) noexcept {
    changed=false;if(!impl_||!impl_->active)return MMS_E_NOT_READY;
    auto& scope=*impl_;
    MmsResult result=MMS_OK;
    if(!scope.change->Present())result=MMS_E_STALE;
    else {
        const auto captured=scope.capture.Finish(1,scope.change->record);
        if(captured==native_history::Result::Empty){scope.active=false;scope.transaction.reset();impl_.reset();return MMS_OK;}
        if(captured!=native_history::Result::Ready)result=PoseResult(captured);
        else result=scope.history.PrepareAppliedNative(*scope.transaction,scope.change);
        if(result==MMS_OK)result=scope.history.CommitAppliedNative(*scope.transaction);
    }
    if(result!=MMS_OK){if(!scope.Cancel())result=MMS_E_FAILED;impl_.reset();return result;}
    scope.active=false;scope.transaction.reset();impl_.reset();changed=true;return MMS_OK;
}
bool NativePoseScope::Cancel() noexcept {
    if(!impl_)return true;
    const bool result=impl_->Cancel();impl_.reset();return result;
}
NativePoseTestBinding::NativePoseTestBinding(MMDApp& app,SharedHistory& history) noexcept
    :app_(&app),history_(&history),previous_(binding){binding=this;}
NativePoseTestBinding::~NativePoseTestBinding(){binding=previous_;}
SharedHistory* BoundNativePoseHistory(MMDApp* app) noexcept { return binding&&binding->app_==app?binding->history_:nullptr; }
}
#endif
