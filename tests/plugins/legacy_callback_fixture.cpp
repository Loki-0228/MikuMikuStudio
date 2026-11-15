// Compiled against the upstream declarations, independently of host slot constants.
#include "fixtures/legacy_callbacks.hpp"
#include <stdexcept>
namespace {
void Event(int value, HRESULT result = S_OK, const void* pointer = nullptr) {
    using Callback = void(__cdecl*)(int, HRESULT, const void*);
    auto callback = reinterpret_cast<Callback>(GetProcAddress(GetModuleHandleW(nullptr), "MMSFacadeTestEvent"));
    if (callback) callback(value, result, pointer);
}
#if MMD_TEST_ABI == 2
struct Plugin final : MMDPluginDLL2 {
    explicit Plugin(IDirect3DDevice9* device) { Event(20, S_OK, device); }
    ~Plugin() override { Event(23); }
    void Reset(D3DPRESENT_PARAMETERS* parameters) override {
        Event(21, S_OK, parameters);
        if (GetEnvironmentVariableW(L"MMDFACADE_TEST_THROW", nullptr, 0)) throw std::runtime_error("reset fixture");
    }
    void PostReset(D3DPRESENT_PARAMETERS* parameters, HRESULT& result) override {
        Event(22, result, parameters);
        if (GetEnvironmentVariableW(L"MMDFACADE_TEST_POST_THROW", nullptr, 0)) throw std::runtime_error("post fixture");
        result = S_OK; // Must not conceal a failed device reset from MMS.
    }
};
#else
struct Plugin final : MMDPluginDLL3 {
    explicit Plugin(IDirect3DDevice9*) { Event(30); }
    ~Plugin() override { Event(34); }
    const char* getPluginTitle() const override { return "ABI 3 regression observer"; }
    void start() override { Event(31); }
    void stop() override { Event(33); }
    void WndProc(CWPSTRUCT* message) override { if (message->message == WM_SHOWWINDOW) Event(32); }
};
#endif
}
extern "C" __declspec(dllexport) int __cdecl version() {
    return GetEnvironmentVariableW(L"MMDFACADE_TEST_WRONG_VERSION", nullptr, 0) ? 4 : MMD_TEST_ABI;
}
#if MMD_TEST_ABI == 2
extern "C" __declspec(dllexport) MMDPluginDLL2* __cdecl create2(IDirect3DDevice9* device) { return new Plugin(device); }
extern "C" __declspec(dllexport) void __cdecl destroy2(MMDPluginDLL2* object) { delete object; }
#else
extern "C" __declspec(dllexport) MMDPluginDLL3* __cdecl create3(IDirect3DDevice9* device) { return new Plugin(device); }
extern "C" __declspec(dllexport) void __cdecl destroy3(MMDPluginDLL3* object) { delete object; }
#endif
