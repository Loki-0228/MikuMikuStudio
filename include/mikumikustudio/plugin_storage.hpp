#pragma once
#include <memory>
namespace mikumikustudio {
class MMDApp;
namespace plugins {
class ProjectSave {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit ProjectSave(MMDApp*);
    ~ProjectSave();
    bool Publish(const wchar_t* staged, const wchar_t* destination, bool copy);
};
bool PrepareProjectLoad(MMDApp*, const wchar_t*);
// Called at the first destructive step, before releasing any old scene data.
bool BeginProjectSceneReplacement(MMDApp*);
void CommitProjectLoad(MMDApp*);
void CancelProjectLoad(MMDApp*);
bool ResetPluginProject(MMDApp*);
void PluginProjectSaved(MMDApp*, const wchar_t*);
bool DeletePluginBackup(const wchar_t*);
}
}
