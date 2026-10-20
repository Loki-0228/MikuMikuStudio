#pragma once
#include <memory>
namespace mikumikustudio {
class MMDApp;
namespace plugins {
class ProjectSave {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    // Capture serializers on the UI thread. The snapshot owns its byte copies;
    // no plugin lease or live scene pointer is needed by PublishDisk.
    explicit ProjectSave(MMDApp*);
    ~ProjectSave();
    // One disk publication per snapshot. May run on a worker; the caller keeps
    // this object alive and joins that worker before Complete or destruction.
    bool PublishDisk(const wchar_t* staged, const wchar_t* destination, bool copy, bool keepPrevious = false) noexcept;
    // UI thread only, using the same destination/copy arguments as PublishDisk.
    // False means the runtime changed or completion is not ready; an already
    // published pair remains valid. The caller separately checks its scene
    // generation before completing. Never reloads plugin state or edits history.
    bool Complete(const wchar_t* destination, bool copy) noexcept;
    // Synchronous UI compatibility wrapper. Success describes disk publication,
    // even if a replaced runtime no longer accepts the completion metadata.
    bool Publish(const wchar_t* staged, const wchar_t* destination, bool copy);
};
bool PrepareProjectLoad(MMDApp*, const wchar_t*);
// Called at the first destructive step, before releasing any old scene data.
bool BeginProjectSceneReplacement(MMDApp*);
void CommitProjectLoad(MMDApp*);
void CancelProjectLoad(MMDApp*);
bool ResetPluginProject(MMDApp*);
void PluginProjectSaved(MMDApp*, const wchar_t*);
// Pure filesystem work; callers check native backup ownership and serialize
// retention for their backup directory before calling, including on a worker.
bool DeletePluginBackup(const wchar_t*);
}
}
