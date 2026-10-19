// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/path_workspace.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/model.hpp"
#include "../src/io/pmm_io_common.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <locale.h>
#include <memory>

namespace {
void Check(bool value, const char* description) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", description); std::exit(1); }
}
LRESULT CALLBACK LossyUnicodeProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_APP + 1) return 12345;
    if (message == WM_SETTEXT) {
        char text[512]{};
        WideCharToMultiByte(CP_ACP, 0, reinterpret_cast<const wchar_t*>(lp), -1,
                            text, sizeof text, nullptr, nullptr);
        return DefWindowProcA(window, message, wp, reinterpret_cast<LPARAM>(text));
    }
    return DefWindowProcW(window, message, wp, lp);
}
}

int main() {
    using namespace mikumikustudio;
    namespace enc = text_encoding;
    namespace fs = std::filesystem;
    // Reproduce the application's Japanese CRT locale on a non-Japanese OS.
    _wsetlocale(LC_ALL, L"japanese");
    fs::path directory = fs::temp_directory_path() /
        (L"MDS-encoding-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directory(directory);
    PathResolutionWorkspace workspace{};
    char bytes[256];
    wchar_t resolved[256];
    const wchar_t* names[] = {L"plain.x", L"洛天依渲染.x", L"ステージ髪.x",
                             L"繁體場景.x", L"무대.x", L"混合_髪_무대_😀.x"};
    for (auto name : names) {
        fs::path path = directory / name;
        { std::ofstream file(path); file << "fixture"; }
        Check(enc::EncodePath(bytes, sizeof bytes, path.c_str()), "encode path without losing characters");
        // Fixed-field serialization, including zero padding, then reopen bytes.
        fs::path saved = directory / L"保存_日本語_😀.bin";
        { std::ofstream file(saved, std::ios::binary); file.write(bytes, sizeof bytes); }
        std::memset(bytes, 0xCC, sizeof bytes);
        { std::ifstream file(saved, std::ios::binary); file.read(bytes, sizeof bytes); }
        Check(ResolveAnsiUserFile(nullptr, bytes, resolved, _countof(resolved), workspace),
              "saved path resolves after reopening");
        Check(path == fs::path(resolved), "reopened path is exactly the original Unicode path");
        for (unsigned cp : {932u, 936u, 950u, 949u, 65001u}) {
            std::string legacy;
            if (!enc::EncodeExact(path.c_str(), cp, legacy)) continue;
            Check(ResolveAnsiUserFile(nullptr, legacy.c_str(), resolved, _countof(resolved), workspace),
                  "legacy code pages and untagged UTF-8 resolve by file existence");
            Check(path == fs::path(resolved), "legacy resolution preserves Unicode");
            Check(enc::EncodeExact(name, cp, legacy), "encode legacy filename");
            Check(enc::LegacyFilename(legacy.c_str(), resolved) == name,
                  "filename display uses resolved path to disambiguate legacy bytes");
        }
        char label[100];
        Check(enc::EncodeDisplay(label, sizeof label, name), "encode accessory display label");
        Check(enc::Display(label) == name, "accessory name save/read round trip");
        fs::remove(saved);
        fs::remove(path);
    }
    Check(enc::EncodePath(bytes, sizeof bytes, L"" ) && bytes[0] == 0,
          "empty paths remain empty rather than a percent sign");
    const std::wstring tooLong(256, L'中');
    Check(!enc::EncodePath(bytes, sizeof bytes, tooLong.c_str()) && bytes[0] == 0,
          "oversize paths fail instead of truncating a multibyte character");
    std::string encoded;
    Check(!enc::EncodeExact(L"😀", 932, encoded), "no question-mark substitutions");
    Check(!enc::EncodeExact(L"\u00a5", 932, encoded), "no yen-to-backslash best fit");
    Check(enc::EncodePath(bytes, sizeof bytes, L"混合😀.x") && enc::HasUtf8Bom(bytes),
          "non-legacy text carries an explicit UTF-8 marker");
    ConvertAnsiToWide(nullptr, bytes, resolved, _countof(resolved));
    Check(std::wcscmp(resolved, L"混合😀.x") == 0, "generic decoder understands marked UTF-8");
    Check(!ResolveAnsiUserFile(nullptr, "x", resolved, _countof(resolved), workspace),
          "short nonexistent paths do not underflow extension lookup");
    auto model = std::make_unique<mdl::ModelRecord>();
    wchar_t modelName[] = L"洛天依_髪_무대_😀";
    model->pmxTextBuffers[0] = modelName;
    std::strcpy(model->name, "lossy mirror");
    Check(enc::ModelName(*model, false) == modelName,
          "PMX display reads original Unicode rather than a lossy narrow mirror");
    wchar_t brokenEnglish[] = L"926\uFF61\uFF7E\uFF74\uE650桒魴\uFF84\uFF89﨓蝪\uFF7F";
    model->pmxTextBuffers[1] = brokenEnglish;
    std::wcscpy(model->path, L"C:\\模型\\石英式洛天依七夕.pmx");
    Check(enc::ModelLabel(*model, true) == brokenEnglish,
          "display preserves the selected model metadata without guessing corrections");
    Check(enc::ModelLabel(*model, false) == modelName,
          "native model name takes priority over the source filename");
    Check(enc::ModelName(*model, true) == brokenEnglish,
          "display labels do not rewrite serialized model metadata");
    wchar_t emptyName[] = L"";
    model->pmxTextBuffers[1] = emptyName;
    Check(enc::ModelName(*model, true) == modelName, "empty English model name falls back to the original");
    model->pmxTextBuffers[0] = emptyName;
    model->pmxTextBuffers[1] = brokenEnglish;
    Check(enc::ModelName(*model, false) == brokenEnglish, "empty original model name falls back to English");
    model->pmxTextBuffers[0] = modelName;
    enc::SetMaterialNames(*model, {{L"髪",L"Hair"},{L"衣服",L""},{L"",L"Eyes"}});
    Check(enc::MaterialName(*model,0,true)==L"Hair"&&enc::MaterialName(*model,0,false)==L"髪", "material names follow the requested language");
    Check(enc::MaterialName(*model,1,true)==L"衣服"&&enc::MaterialName(*model,2,false)==L"Eyes", "material names fall back both ways");
    enc::ForgetMaterialNames(*model);
    Check(enc::MaterialName(*model,0,true).empty(), "disposing metadata prevents stale material names on reused slots");
    std::wcscpy(model->path, L"D:/素材/髪_무대_😀.pmx");
    model->pmxTextBuffers[0] = model->pmxTextBuffers[1] = nullptr;
    model->name[0] = model->nameEn[0] = 0;
    Check(enc::ModelLabel(*model, true) == L"髪_무대_😀.pmx",
          "Unicode filenames and forward slashes remain intact");
    model->pmxTextBuffers[0] = modelName;
    model->path[0] = 0;
    Check(enc::ModelLabel(*model, false) == modelName,
          "models without a source path retain their metadata label");
    model->pmxTextBuffers[1] = nullptr;
    model->pmxTextBuffers[0] = nullptr;
    enc::EncodeExact(L"初音ミク", 932, encoded);
    std::strcpy(model->name, encoded.c_str());
    Check(enc::ModelName(*model, false) == L"初音ミク", "PMD name uses explicit Shift-JIS");
    WNDCLASSA windowClass{};
    windowClass.lpszClassName = "MDS encoding ANSI subclass test";
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpfnWndProc = DefWindowProcA;
    Check(RegisterClassA(&windowClass) != 0, "register legacy ANSI window");
    HWND window = CreateWindowA(windowClass.lpszClassName, "", 0,
                                0, 0, 1, 1, nullptr, nullptr, windowClass.hInstance, nullptr);
    Check(window != nullptr, "create legacy ANSI window");
    pmm_io::SetProjectWindowTitle(window, L"C:\\保存_日本語_😀.pmm");
    wchar_t title[512]{};
    GetWindowTextW(window, title, _countof(title));
    Check(std::wcscmp(title, L"MikuMikuDance [C:\\保存_日本語_😀.pmm]") == 0,
          "project title survives an ANSI window procedure");
    DestroyWindow(window);
    UnregisterClassA(windowClass.lpszClassName, windowClass.hInstance);
    WNDCLASSW lossyClass{};
    lossyClass.lpszClassName = L"MDS encoding lossy Unicode subclass test";
    lossyClass.hInstance = GetModuleHandleW(nullptr);
    lossyClass.lpfnWndProc = LossyUnicodeProc;
    Check(RegisterClassW(&lossyClass) != 0, "register lossy Unicode window");
    window = CreateWindowW(lossyClass.lpszClassName, L"", 0,
                           0, 0, 1, 1, nullptr, nullptr, lossyClass.hInstance, nullptr);
    Check(window && IsWindowUnicode(window), "plugin window reports Unicode");
    pmm_io::SetProjectWindowTitle(window, L"C:\\保存_日本語_😀.pmm");
    GetWindowTextW(window, title, _countof(title));
    Check(std::wcscmp(title, L"MikuMikuDance [C:\\保存_日本語_😀.pmm]") == 0,
          "project title survives internal ANSI conversion in a Unicode procedure");
    Check(SendMessageW(window, WM_APP + 1, 0, 0) == 12345,
          "non-text messages still reach the plugin procedure");
    DestroyWindow(window);
    UnregisterClassW(lossyClass.lpszClassName, lossyClass.hInstance);
    fs::remove(directory);
    std::puts("filename encoding regressions passed");
}
