#include "mikudancestudio/model_import.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikudancestudio/import_texture_cache.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/path_workspace.hpp"
#include "mikudancestudio/physics_scene.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include <cstdlib>
#include <cwchar>
#include <utility>

namespace mikudancestudio::model_import {
namespace {
thread_local std::vector<Message>* messages = nullptr;
class Capture {
    std::vector<Message>* previous_;
public:
    explicit Capture(std::vector<Message>& value) noexcept : previous_(messages) { messages = &value; }
    ~Capture() { messages = previous_; }
};
bool Absolute(const wchar_t* path) noexcept {
    if (!path) return false;
    const size_t n = wcsnlen(path, 256);
    return n > 2 && n < 256 && ((path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) ||
        (path[0] == L'\\' && path[1] == L'\\'));
}
}
bool Preparing() noexcept { return messages != nullptr; }
int MessageA(HWND owner, LPCSTR text, LPCSTR title, UINT flags) {
    if (!messages) return MessageBoxA(owner, text, title, flags);
    Message value; value.textA = text ? text : ""; value.titleA = title ? title : ""; value.flags = flags;
    messages->push_back(std::move(value)); return IDOK;
}
int MessageW(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags) {
    if (!messages) return MessageBoxW(owner, text, title, flags);
    Message value; value.textW = text ? text : L""; value.titleW = title ? title : L"";
    value.flags = flags; value.wide = true; messages->push_back(std::move(value)); return IDOK;
}
bool ShowMessages(HWND owner, const std::vector<Message>& values) {
    for (const auto& value : values) {
        const int result = value.wide ? MessageBoxW(owner, value.textW.c_str(), value.titleW.c_str(), value.flags)
            : MessageBoxA(owner, value.textA.c_str(), value.titleA.c_str(), value.flags);
        if (!result || result == IDCANCEL || result == IDNO || result == IDABORT) return false;
    }
    return true;
}
Prepared::~Prepared() { if (model_) { DisposePrivate(model_); std::free(model_); } }
Prepared::Prepared(Prepared&& other) noexcept
    : model_(std::exchange(other.model_, nullptr)), messages_(std::move(other.messages_)) {}
Prepared& Prepared::operator=(Prepared&& other) noexcept {
    if (this != &other) {
        Prepared replacement(std::move(other));
        std::swap(model_, replacement.model_); messages_.swap(replacement.messages_);
    }
    return *this;
}
unsigned char* Prepared::Release() noexcept { return std::exchange(model_, nullptr); }
Result Prepare(MMDApp& app, const wchar_t* path, bool showInfo, PhysicsScene& detached,
               import_texture_cache::Preparation& textures, Prepared& out) noexcept {
    if (out.Object() || !Absolute(path)) return Result::InvalidPath;
    if (!app.Renderer() || !textures.IsFor(*app.Renderer()) || !detached.world || !app.Physics() ||
        &detached == app.Physics() || detached.world == app.Physics()->world ||
        GetModuleHandleW(L"MMEffect.dll") || GetModuleHandleW(L"MMHack.dll")) return Result::NotReady;
    try {
        Prepared next;
        next.model_ = static_cast<unsigned char*>(std::calloc(1, mdl::kSize));
        if (!next.model_) return Result::Memory;
        ModelInitDefaults(next.model_);
        auto paths = app.PathWorkspace();
        Capture capture(next.messages_);
        import_texture_cache::ScopedCapture textureCapture(textures);
        const bool loaded = ModelLoadPMD(next.model_, app.MainWindow(), path, app.Renderer(), 1,
                          showInfo ? 1 : 0, app.EnglishUI(), &detached, paths);
        if (textures.Status() != MMS_OK) return textures.Status() == MMS_E_MEMORY ? Result::Memory : Result::TextureFailure;
        if (!loaded) return Result::InvalidModel;
        const auto& model = *mdl::Mdl(next.model_);
        if (model.rigidCount && !model.rigidTable) return Result::InvalidModel;
        for (uint32_t i = 0; i < model.rigidCount; ++i)
            if (!model.rigidTable[i].body) return Result::InvalidModel;
        out = std::move(next); return Result::Ready;
    } catch (const std::bad_alloc&) { return Result::Memory; }
      catch (...) { return Result::InvalidModel; }
}
}
#endif
