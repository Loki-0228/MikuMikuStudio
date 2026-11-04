#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <tuple>
#include <type_traits>
#include <utility>

namespace {
int observed = 0;
std::array<std::uintptr_t, 3> arguments{};
template<class T> std::uintptr_t Bits(T value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<std::uintptr_t>(value);
    else return static_cast<std::uintptr_t>(value);
}
template<class R, class... A> R Observe(int ordinal, const std::tuple<A...>& args) {
    observed = ordinal; arguments = {};
    std::apply([](auto... value) { size_t i = 0; ((arguments[i++] = Bits(value)), ...); }, args);
    if constexpr (std::is_void_v<R>) return;
    else if constexpr (std::is_pointer_v<R>) return reinterpret_cast<R>(std::uintptr_t(0x10000 + ordinal * 32));
    else if constexpr (std::is_same_v<R, float>) return ordinal + 0.375f;
    else if constexpr (std::is_same_v<R, std::uint32_t>) return 0x80000000u | unsigned(ordinal);
    else return -ordinal * 19;
}
float buffer[16]{};
template<class T> T Sample(size_t index) {
    if constexpr (std::is_pointer_v<T>) return buffer + index;
    else return static_cast<T>(-17 - int(index));
}
template<class R, class... A, size_t... I>
bool CheckCall(int ordinal, R(__cdecl* actual)(A...), R(__cdecl* direct)(A...), std::index_sequence<I...>) {
    auto args = std::make_tuple(Sample<A>(I)...);
    if constexpr (std::is_void_v<R>) {
        std::apply(direct, args); const auto expected = arguments; observed = 0; arguments = {};
        std::apply(actual, args); return observed == ordinal && arguments == expected;
    } else {
        const auto result = std::apply(direct, args); const auto expected = arguments; observed = 0; arguments = {};
        const auto forwarded = std::apply(actual, args);
        return observed == ordinal && arguments == expected && forwarded == result;
    }
}
template<class R, class... A> bool Check(int ordinal, FARPROC p, R(__cdecl* direct)(A...)) {
    return p && CheckCall(ordinal, reinterpret_cast<R(__cdecl*)(A...)>(p), direct, std::index_sequence_for<A...>{});
}
}
extern "C" {
#define MMS_ADAPTER_EXPORT(ordinal, name, ret, params, args) \
    ret __cdecl name params { return Observe<ret>(ordinal, std::make_tuple args); }
#include "../../src/legacy_adapter/exports.inc"
#undef MMS_ADAPTER_EXPORT
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || GetModuleHandleW(L"MikuMikuDance.exe")) return 2;
    HMODULE module = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
#ifdef MMS_ADAPTER_MISSING_EXPORTS
    if (module || GetLastError() != ERROR_DLL_INIT_FAILED) return 3;
    std::puts("PASS adapter rejects a host without all 37 exports");
#else
    if (!module) { std::printf("adapter load error %lu\n", GetLastError()); return 4; }
    unsigned checked = 0;
#define MMS_ADAPTER_EXPORT(ordinal, name, ret, params, args) \
    { auto p = GetProcAddress(module, #name); \
      if (p != GetProcAddress(module, MAKEINTRESOURCEA(ordinal)) || !Check(ordinal, p, &name)) { \
        std::printf("FAIL %s ordinal %d\n", #name, ordinal); return 5; } ++checked; }
#include "../../src/legacy_adapter/exports.inc"
#undef MMS_ADAPTER_EXPORT
    FreeLibrary(module);
    std::printf("PASS renamed host forwards all %u exports by name and ordinal\n", checked);
#endif
    return 0;
}
