#pragma once

#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/plugin_scene.hpp"
#endif

namespace mikumikustudio {

// The import uses both the selected target and the current frame as input.
// A native picker or a model-name confirmation can dispatch scene edits.
class VmdImportTarget {
public:
    explicit VmdImportTarget(MMDApp& app)
        : owner_(app.MainWindow()), scene_(app.SceneGeneration()), frame_(app.CurrentFrame()),
          camera_(app.CameraMode() != 0), slot_(app.SelectedModelSlot()),
          model_(!camera_ && slot_ < kModelSlotCount ? app.ModelSlot(slot_) : nullptr) {
        if (model_) {
            const auto& record = *mdl::Mdl(model_);
            bones_ = record.boneTable;
            pool_ = record.boneKeys;
            poolId_ = mdl::BoneKeyPoolIdentity(pool_);
#if !MMD_ORIGINAL_PROFILE
            plugins::FindSceneObjectIdentity(model_, identity_);
#endif
        }
    }

    bool Current(MMDApp& app) const {
        return app.CurrentFrame() == frame_ && SameTarget(app);
    }

    bool SameTarget(MMDApp& app) const {
        if (app.MainWindow() != owner_ || (owner_ && !IsWindow(owner_)) ||
            app.SceneGeneration() != scene_ || app.SceneMutationInProgress() || app.sceneRecoveryPending ||
            (app.CameraMode() != 0) != camera_) return false;
        if (camera_) return true;
        if (!model_ || app.SelectedModelSlot() != slot_ || app.ModelSlot(slot_) != model_) return false;
#if !MMD_ORIGINAL_PROFILE
        if (identity_.id.lo || identity_.id.hi) {
            MmsObjectRef current{};
            if (!plugins::FindSceneObjectIdentity(model_, current) || current.id.lo != identity_.id.lo ||
                current.id.hi != identity_.id.hi || current.generation != identity_.generation) return false;
        }
#endif
        const auto& record = *mdl::Mdl(model_);
        return record.boneTable == bones_ && (poolId_ ? mdl::BoneKeyPoolIdentity(record.boneKeys) == poolId_
                                                    : record.boneKeys == pool_);
    }

private:
    HWND owner_;
    std::uint64_t scene_;
    int frame_;
    bool camera_;
    unsigned slot_;
    unsigned char* model_;
    const mdl::BoneRecord* bones_ = nullptr;
    const mdl::BoneKey* pool_ = nullptr;
    std::uint64_t poolId_ = 0;
#if !MMD_ORIGINAL_PROFILE
    MmsObjectRef identity_{};
#endif
};

} // namespace mikumikustudio
