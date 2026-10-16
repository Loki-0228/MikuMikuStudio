#pragma once
#include <memory>
namespace mikudancestudio {
class MMDApp;
namespace pmm_extension { struct Document; }
namespace plugins {
class SceneStore;
std::shared_ptr<SceneStore> CreateSceneStore(MMDApp*);
void ForgetSceneObject(const void*);
void ResetSceneIdentity();
void SaveSceneIdentity(const MMDApp&, pmm_extension::Document&);
void LoadSceneIdentity(MMDApp&, const pmm_extension::Document&);
}
}
