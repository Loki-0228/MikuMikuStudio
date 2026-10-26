#pragma once
#include <memory>
#include <vector>
#include <mms/scene.h>
namespace mikumikustudio {
class MMDApp;
namespace pmm_extension { struct Document; }
namespace plugins {
class SceneStore;
class EvaluationStore;
std::shared_ptr<SceneStore> CreateSceneStore(MMDApp*);
std::shared_ptr<EvaluationStore> CreateEvaluationStore(MMDApp*);
void ForgetSceneObject(const void*);
MmsObjectRef SceneObjectIdentity(const void*);
// Inspect an existing live identity without creating one or allocating.
// Failure leaves the caller's output unchanged.
bool FindSceneObjectIdentity(const void*, MmsObjectRef&) noexcept;
MmsObjectRef CreateAbsentSceneObject(const void*);
bool SceneObjectPresent(const void*, MmsId) noexcept;
void ResetSceneIdentity();
void SaveSceneIdentity(const MMDApp&, pmm_extension::Document&);
void LoadSceneIdentity(MMDApp&, const pmm_extension::Document&,
                       const std::vector<int>& adaptedModelSlots = {});
}
}
