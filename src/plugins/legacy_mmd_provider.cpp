#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <map>
#include <memory>
#include <mutex>

// Public binder ABI from oigami's MMDPlugin header. Weak bookkeeping is cleared
// before unloading a plugin, while its control-block destructors are mapped.
namespace mmp::detail {
struct IFuncBinderImpl {
    __declspec(dllexport) static void addPrev(std::shared_ptr<IFuncBinderImpl>, void*, void*);
    __declspec(dllexport) static void deleteFunc(void*);
protected:
    IFuncBinderImpl() = default;
    virtual ~IFuncBinderImpl() = default;
    virtual void add_prev(std::shared_ptr<IFuncBinderImpl>) = 0;
};
namespace {
std::mutex binderMutex;
std::map<void*, std::weak_ptr<IFuncBinderImpl>> binders;
}
void IFuncBinderImpl::addPrev(std::shared_ptr<IFuncBinderImpl> next, void* previous, void* replacement) {
    std::lock_guard<std::mutex> lock(binderMutex);
    if (auto it = binders.find(previous); it != binders.end())
        if (auto old = it->second.lock()) old->add_prev(next);
    binders[replacement] = next;
}
void IFuncBinderImpl::deleteFunc(void* replacement) {
    std::lock_guard<std::mutex> lock(binderMutex);
    binders.erase(replacement);
}
void ClearBinders() {
    std::lock_guard<std::mutex> lock(binderMutex);
    binders.clear();
}
}

extern "C" __declspec(dllexport) void __cdecl MMSClearLegacyHookBinders() { mmp::detail::ClearBinders(); }

__declspec(dllexport) void* __cdecl RewriteFunction(const char* module, const char* function, void* replacement, int ordinal) {
    using Rewrite = void*(__cdecl*)(const char*, const char*, void*, int);
    auto rewrite = reinterpret_cast<Rewrite>(GetProcAddress(GetModuleHandleW(nullptr), "MMSLegacyMmdRewriteFunction"));
    return rewrite ? rewrite(module, function, replacement, ordinal) : nullptr;
}

extern "C" __declspec(dllexport) HWND __cdecl getHWND() {
    using GetWindow = HWND(__cdecl*)();
    const auto getWindow = reinterpret_cast<GetWindow>(
        GetProcAddress(GetModuleHandleW(nullptr), "MMSLegacyMmdGetWindow"));
    return getWindow ? getWindow() : nullptr;
}

extern "C" __declspec(dllexport) int __cdecl createWM_APP_ID() {
    using Allocate = int(__cdecl*)();
    const auto allocate = reinterpret_cast<Allocate>(
        GetProcAddress(GetModuleHandleW(nullptr), "MMSLegacyMmdAllocateMenuId"));
    return allocate ? allocate() : -1;
}
