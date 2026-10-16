#pragma once
#include <memory>
#include <mms/scene.h>
namespace mikudancestudio {
class MMDApp;
namespace pmm_extension { struct Document; }
namespace plugins {
class SceneStore;
std::shared_ptr<SceneStore> CreateSceneStore(MMDApp*);
void ForgetSceneObject(const void*);
MmsObjectRef SceneObjectIdentity(const void*);
MmsObjectRef CreateAbsentSceneObject(const void*);
bool SceneObjectPresent(const void*, MmsId) noexcept;
void ResetSceneIdentity();
void SaveSceneIdentity(const MMDApp&, pmm_extension::Document&);
void LoadSceneIdentity(MMDApp&, const pmm_extension::Document&);
}
}
