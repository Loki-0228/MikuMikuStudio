#include "mikumikustudio/default_effect.hpp"
#include "mikumikustudio/d3dx_effect.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mikumikustudio {
namespace {
using CreateCompiler = HRESULT(WINAPI*)(LPCVOID, UINT, const void*, const void*,
                                       DWORD, void**, void**);
struct ComBuffer {
    void* value = nullptr;
    ~ComBuffer() { if (value) static_cast<IUnknown*>(value)->Release(); }
    const char* Data() const {
        using Fn = const char*(WINAPI*)(void*);
        return d3dx::EffectMethod<Fn, 3>(value)(value);
    }
    DWORD Size() const {
        using Fn = DWORD(WINAPI*)(void*);
        return d3dx::EffectMethod<Fn, 4>(value)(value);
    }
};
struct Compilation {
    std::vector<char> source, binary;
    std::string error;
    CreateCompiler create = nullptr;
    HMODULE module = nullptr;
    HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<bool> done{false};
    bool started = false; // UI thread only.
    int resource = 0;
    HRESULT result = E_FAIL;
    ULONGLONG elapsed = 0;
    ~Compilation() {
        if (completed) CloseHandle(completed);
        if (module) FreeLibrary(module);
    }
};
// UI-thread ownership. Workers retain only their own source/result object,
// never an application, window, renderer or Direct3D device pointer.
std::unordered_map<D3DRenderer*, std::shared_ptr<Compilation>> pending;

void Compile(const std::shared_ptr<Compilation>& work) noexcept {
    const auto start = GetTickCount64();
    try {
        ComBuffer compiler, errors, bytecode;
        work->result = work->create(work->source.data(), UINT(work->source.size()),
                                   nullptr, nullptr, 0, &compiler.value, &errors.value);
        if (FAILED(work->result) && errors.value)
            work->error.assign(errors.Data(), errors.Size());
        if (SUCCEEDED(work->result) && compiler.value) {
            ComBuffer compileErrors;
            // ID3DXEffectCompiler::CompileEffect, June 2010 SDK slot 59.
            using Fn = HRESULT(WINAPI*)(void*, DWORD, void**, void**);
            work->result = d3dx::EffectMethod<Fn, 59>(compiler.value)(
                compiler.value, 0, &bytecode.value, &compileErrors.value);
            if (compileErrors.value)
                work->error.assign(compileErrors.Data(), compileErrors.Size());
            if (SUCCEEDED(work->result) && bytecode.value)
                work->binary.assign(bytecode.Data(), bytecode.Data() + bytecode.Size());
        }
        if (work->binary.empty() && SUCCEEDED(work->result)) work->result = E_FAIL;
    } catch (...) {
        work->result = E_OUTOFMEMORY;
    }
    work->elapsed = GetTickCount64() - start;
    work->done.store(true, std::memory_order_release);
    SetEvent(work->completed);
}

bool HasSceneObjects(const MMDApp* app) {
    for (int i = 0; i < kModelSlotCount; ++i)
        if (app->ModelSlot(i)) return true;
    for (int i = 0; i < 255; ++i)
        if (app->ObjectSlot(i)) return true;
    return false;
}

void SetCompatibilityParameters(D3DRenderer* renderer, int resource) {
    if (resource != 117) return;
    // Preserve the compatibility profile's original parameter setup.
    DWORD desc[6]{};
    using GetDesc = HRESULT(WINAPI*)(void*, int, void*);
    using SetInt = HRESULT(WINAPI*)(void*, const char*, int);
    d3dx::EffectMethod<GetDesc, 17>(renderer->effect)(renderer->effect, 0, desc);
    d3dx::EffectMethod<SetInt, 26>(renderer->effect)(renderer->effect, "SKII1",
                                                  desc[0] == 'r' ? 600 : 500);
    renderer->shaderModel3 = static_cast<std::uint8_t>(desc[0] == 'r');
}
}

bool QueueDefaultEffect(D3DRenderer* renderer, int resource) {
    CancelDefaultEffectCompilation(renderer);
    auto& api = d3dx::Get();
    if (!api.Load()) return false;
    // MME captures this resource call and requires the default effect before
    // its first BeginScene. Keep that plug-in's original initialization path.
    if (GetModuleHandleW(L"MMEffect.dll")) {
        ComBuffer errors;
        const auto start = GetTickCount64();
        const auto result = api.createEffectFromResA(renderer->device, nullptr,
            MAKEINTRESOURCEA(resource), nullptr, nullptr, 0, nullptr,
            reinterpret_cast<void**>(&renderer->effect), &errors.value);
        if (FAILED(result) || !renderer->effect) {
            if (renderer->effect) {
                reinterpret_cast<IUnknown*>(renderer->effect)->Release();
                renderer->effect = nullptr;
            }
            runtime_log::Write("DEFAULT_EFFECT MME failed resource=%d hr=0x%08lX %s",
                resource, static_cast<unsigned long>(result), errors.value ? errors.Data() : "");
            return false;
        }
        SetCompatibilityParameters(renderer, resource);
        runtime_log::Write("DEFAULT_EFFECT MME ready resource=%d synchronous_ms=%llu",
                          resource, GetTickCount64() - start);
        return true;
    }
    try {
        auto work = std::make_shared<Compilation>();
        work->resource = resource;
        work->create = reinterpret_cast<CreateCompiler>(
            GetProcAddress(api.module, "D3DXCreateEffectCompiler"));
        if (!work->completed || !work->create || !GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(work->create), &work->module)) return false;
        const auto info = FindResourceW(nullptr, MAKEINTRESOURCEW(resource), MAKEINTRESOURCEW(10));
        const DWORD size = info ? SizeofResource(nullptr, info) : 0;
        const auto data = info ? LoadResource(nullptr, info) : nullptr;
        const auto* bytes = data ? static_cast<const char*>(LockResource(data)) : nullptr;
        if (!bytes || !size) return false;
        work->source.assign(bytes, bytes + size);
        // Full builds embed D3DX bytecode. Retain the asynchronous source path
        // for legacy resources and builds without the resource generator.
        const unsigned char compiledHeader[] = {0x01, 0x09, 0xff, 0xfe};
        if (size >= sizeof compiledHeader && !std::memcmp(bytes, compiledHeader, sizeof compiledHeader)) {
            work->binary = std::move(work->source);
            work->result = S_OK;
            work->done.store(true, std::memory_order_release);
            SetEvent(work->completed);
        }
        pending.emplace(renderer, std::move(work));
        runtime_log::Write("DEFAULT_EFFECT queued resource=%d", resource);
        return true;
    } catch (...) {
        return false;
    }
}

void StartDefaultEffectCompilation(D3DRenderer* renderer) {
    const auto found = pending.find(renderer);
    if (found == pending.end() || found->second->started || found->second->done.load(std::memory_order_acquire)) return;
    const auto work = found->second;
    work->started = true;
    runtime_log::Write("DEFAULT_EFFECT compile begin resource=%d", work->resource);
    try {
        std::thread([work] { Compile(work); }).detach();
    } catch (...) {
        work->result = E_OUTOFMEMORY;
        work->done.store(true, std::memory_order_release);
        SetEvent(work->completed);
    }
}

void UpdateDefaultEffect(MMDApp* app) {
    auto* renderer = app->Renderer();
    const auto found = pending.find(renderer);
    if (found == pending.end()) return;
    const auto work = found->second;
    // Empty scenes and disabled self-shadow need no effect. While source-only
    // resources compile, interactive previews use the existing fixed renderer.
    if (!HasSceneObjects(app) || !app->state.selfShadowEnabled || app->SelfShadowMode() <= 0) return;
    StartDefaultEffectCompilation(renderer);
    // An export must not write temporary fallback shading into its first frame.
    if (app->RecordingWindow() && !work->done.load(std::memory_order_acquire))
        WaitForSingleObject(work->completed, INFINITE);
    if (!work->done.load(std::memory_order_acquire)) return;
    // Resizing while compilation runs is safe: the worker owns no device
    // resources. Delay publication if the UI thread is recovering the device.
    if (!renderer->device || FAILED(renderer->device->TestCooperativeLevel())) return;
    auto result = work->result;
    ComBuffer errors;
    const auto attachStart = GetTickCount64();
    if (SUCCEEDED(result)) {
        auto& api = d3dx::Get();
        if (api.Load() && api.createEffect) {
            result = api.createEffect(renderer->device, work->binary.data(),
                UINT(work->binary.size()), nullptr, nullptr, 0, nullptr,
                reinterpret_cast<void**>(&renderer->effect), &errors.value);
        } else result = E_NOINTERFACE;
    }
    if (FAILED(result) || !renderer->effect) {
        if (renderer->effect) {
            reinterpret_cast<IUnknown*>(renderer->effect)->Release();
            renderer->effect = nullptr;
        }
        renderer->postProcessEnabled = 0;
        app->SelfShadowMode() = 0;
        app->state.selfShadowEnabled = 0;
        runtime_log::Write("DEFAULT_EFFECT failed resource=%d hr=0x%08lX %s",
            work->resource, static_cast<unsigned long>(result),
            errors.value ? errors.Data() : work->error.c_str());
    } else {
        SetCompatibilityParameters(renderer, work->resource);
        runtime_log::Write("DEFAULT_EFFECT ready resource=%d compile_ms=%llu attach_ms=%llu",
            work->resource, work->elapsed, GetTickCount64() - attachStart);
    }
    pending.erase(found);
    app->state.messageSeen = 1;
}

void CancelDefaultEffectCompilation(D3DRenderer* renderer) {
    // A detached CPU-only worker may finish privately after its window closes.
    // Erasing the UI entry prevents publication into a freed/reused renderer.
    pending.erase(renderer);
}
}
