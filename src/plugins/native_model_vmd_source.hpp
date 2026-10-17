#pragma once
#include "native_model_paste.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikudancestudio/extended_names.hpp"
#include <string>
namespace mikudancestudio::plugins {
struct ModelVmdSource {
    ModelPasteSource source;
    int version=0;
    std::string modelName;
    bool hasUnicodeNames=false;
    extended_names::MotionNames names;
};
// Reads once into private storage. Failure preserves out and all live state.
MmsResult DecodeModelVmdSource(const char* ansiPath,const wchar_t* widePath,
    std::uint32_t offset,ModelVmdSource& out) noexcept;
// Resolve the optional dictionary directly; never installs imported-target
// thread-local pointers or replaces the application's clipboard.
MmsResult ResolveModelVmdTargets(MMDApp&,unsigned slot,ModelVmdSource&) noexcept;
MmsResult PlanModelVmdImport(MMDApp&,unsigned slot,const ModelVmdSource&,
    std::shared_ptr<HistoryChange>&) noexcept;
}
#endif
