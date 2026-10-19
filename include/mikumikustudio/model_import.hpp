#pragma once
#include "mikumikustudio/build_profile.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#if !MMD_ORIGINAL_PROFILE
#include <string>
#include <vector>
namespace mikumikustudio {
class MMDApp;
class PhysicsScene;
namespace import_texture_cache { class Preparation; }
namespace model_import {
enum class Result { Ready, InvalidPath, NotReady, InvalidModel, Memory, TextureFailure };
struct Message {
    std::string textA, titleA;
    std::wstring textW, titleW;
    UINT flags = 0;
    bool wide = false;
};
// Owns a model which has never occupied a scene slot. The detached physics
// scene passed to Prepare must outlive this object until Release/destruction.
class Prepared {
    unsigned char* model_ = nullptr;
    std::vector<Message> messages_;
    friend Result Prepare(MMDApp&, const wchar_t*, bool, PhysicsScene&,
                          import_texture_cache::Preparation&, Prepared&) noexcept;
public:
    Prepared() noexcept = default;
    ~Prepared();
    Prepared(Prepared&&) noexcept;
    Prepared& operator=(Prepared&&) noexcept;
    Prepared(const Prepared&) = delete;
    Prepared& operator=(const Prepared&) = delete;
    unsigned char* Object() const noexcept { return model_; }
    const std::vector<Message>& Messages() const noexcept { return messages_; }
    unsigned char* Release() noexcept;
};
// Does not publish slots, textures, physics, selection, dirty state, UI or cwd.
// Failure leaves out unchanged; successful messages are returned for explicit
// confirmation before publication. Existing opaque effect hooks are refused.
Result Prepare(MMDApp&, const wchar_t* absolutePath, bool showInfo, PhysicsScene& detached,
               import_texture_cache::Preparation&, Prepared& out) noexcept;
// Only call outside the message-free commit section, under the import operation
// guard. A cancelled model-information dialog cancels the whole import.
bool ShowMessages(HWND, const std::vector<Message>&);
// Internal parser hooks. The ordinary loader still uses the native dialogs.
bool Preparing() noexcept;
int MessageA(HWND, LPCSTR, LPCSTR, UINT);
int MessageW(HWND, LPCWSTR, LPCWSTR, UINT);
// Partial, unpublished model cleanup must not shut down the live DX12 renderer.
void DisposePrivate(unsigned char*) noexcept;
}
}
#else
namespace mikumikustudio::model_import {
inline bool Preparing() noexcept { return false; }
inline int MessageA(HWND h, LPCSTR t, LPCSTR c, UINT f) { return MessageBoxA(h,t,c,f); }
inline int MessageW(HWND h, LPCWSTR t, LPCWSTR c, UINT f) { return MessageBoxW(h,t,c,f); }
}
#endif
