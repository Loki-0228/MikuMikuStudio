// Guard the source against backwards reads while exercising the production resolver.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include "mikumikustudio/path_workspace.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "../src/io/project_relocation.hpp"
#include "../src/model/project_model_path.hpp"
#include "mikumikustudio/text_encoding.hpp"
using namespace mikumikustudio;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* text) {
    ++checks; if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", text); }
}

using project_relocation::RootMappings;
using project_relocation::Status;

void CheckEncodedModelPair(const std::filesystem::path& directory) {
    namespace fs = std::filesystem;
    const char* wireName = "\x83\x7e\x83\x4e.pmd"; // CP932 ミク.pmd
    const auto chosen = directory / L"new" / L"A" / L"ミク.pmd";
    const auto sibling = directory / L"new" / L"B" / L"ミク.pmd";
    const auto wire = [&](const char* parent) { return std::string(parent) + wireName; };
    RootMappings roots;
    auto reference = project_model::Locate(wire("old\\A\\").c_str(), directory, roots);
    Check(reference.Confirm(roots, chosen.native()), "manual filename identifies the missing CP932 spelling");
    fs::create_directories(sibling.parent_path());
    { std::ofstream file(sibling); file << "model"; }
    auto locateSibling = [&] { return project_model::Locate(wire("old\\B\\").c_str(), directory, roots); };
    const auto resolved = locateSibling();
    Check(!resolved.tryLegacy && resolved.exactPath == sibling.native(),
          "a unique existing CP932 sibling is found after a manual pairing");
#if !MMD_ORIGINAL_PROFILE
    std::wstring otherName;
    if (text_encoding::Decode(wireName, 936, otherName) && otherName != sibling.filename().native()) {
        const auto other = sibling.parent_path() / otherName;
        { std::ofstream file(other); file << "different model"; }
        const auto ambiguous = locateSibling();
        Check(!ambiguous.tryLegacy && ambiguous.exactPath.empty(),
              "two existing code-page spellings require an explicit choice");
        fs::remove(other);
    } else Check(false, "fixture supplies distinct valid CP932 and CP936 filenames");
    auto uncertainRoot = project_model::Locate(wire("old\\").append("\\same.pmd").c_str(), directory, roots);
    Check(!uncertainRoot.Confirm(roots, (directory / L"different" / L"same.pmd").native()),
          "matching the filename alone cannot decide between ambiguous original directories");
#endif
    fs::remove(sibling);
    fs::remove(sibling.parent_path());
    fs::remove(sibling.parent_path().parent_path());
}

void CheckCaseOnlyModelAlias(const std::filesystem::path& directory) {
    namespace fs = std::filesystem;
    const auto chosen = directory / LR"(new\Upper\Models\a.pmd)";
    const auto upperSibling = chosen.parent_path() / L"b.pmd";
    fs::create_directories(chosen.parent_path());
    { std::ofstream file(chosen); file << "selected model"; }
    { std::ofstream file(upperSibling); file << "upper sibling model"; }
    Check(fs::is_regular_file(chosen) && fs::is_regular_file(upperSibling),
          "the manual replacement and its sibling exist");
    Check(!fs::exists(directory / LR"(old\Pack\Models\a.pmd)") &&
          !fs::exists(directory / LR"(old\pack\Models\b.pmd)"),
          "both original model references are missing");

    RootMappings roots;
    const auto reference = project_model::Locate(R"(old\Pack\Models\a.pmd)", directory, roots);
    Check(reference.Confirm(roots, chosen.native()), "confirm the replacement for the Pack spelling");
    const auto alias = project_model::Locate(R"(old\pack\Models\b.pmd)", directory, roots);
    Check(!alias.tryLegacy && alias.exactPath.empty(),
          "a case-only root alias requires manual location even with an existing decoy");
    const auto exact = project_model::Locate(R"(old\Pack\Models\b.pmd)", directory, roots);
    Check(!exact.tryLegacy && exact.exactPath == project_relocation::NormalizeFile(upperSibling.native()),
          "the same-case sibling still resolves to the existing replacement");

    fs::remove(upperSibling);
    fs::remove(chosen);
    fs::remove(chosen.parent_path());
    fs::remove(directory / LR"(new\Upper)");
    fs::remove(directory / L"new");
}

void ExpectPath(const RootMappings& roots, std::wstring_view original, std::wstring_view expected) {
    const auto result = roots.Infer(original);
    Check(result.status == Status::Candidate && result.path == expected,
          "mapped candidate preserves the exact expected directory tree and filename");
    if (result.status != Status::Candidate || result.path != expected)
        std::fprintf(stderr, "  original=%ls expected=%ls actual=%ls\n",
                     std::wstring(original).c_str(), std::wstring(expected).c_str(), result.path.c_str());
}

void CheckCaseSensitiveRoots() {
    RootMappings narrow;
    Check(narrow.AddConfirmed(LR"(D:\Old\Models\a.pmd)", LR"(E:\New\models\a.pmd)"),
          "confirm a replacement with a differently cased directory");
    ExpectPath(narrow, LR"(D:\Old\Models\b.pmd)", LR"(E:\New\models\b.pmd)");
    Check(narrow.Infer(LR"(D:\Old\Fx\main.fx)").status == Status::NoMatch,
          "a case-mismatched directory suffix cannot widen the rule to Old");

    const auto upper = LR"(D:\Old\Pack\Models\a.pmd)";
    const auto lower = LR"(D:\Old\pack\Models\a.pmd)";
    const auto upperSibling = LR"(D:\Old\Pack\Models\b.pmd)";
    const auto lowerSibling = LR"(D:\Old\pack\Models\b.pmd)";
    RootMappings distinct;
    Check(distinct.AddConfirmed(upper, LR"(E:\New\Upper\Models\a.pmd)"), "confirm the Pack root");
    const auto alias = distinct.Infer(lowerSibling);
    Check(alias.status == Status::Ambiguous && alias.path.empty(),
          "a case-only match gives an ambiguous result with no candidate");

    Check(distinct.AddConfirmed(lower, LR"(F:\New\Lower\Models\a.pmd)"), "confirm the pack root separately");
    ExpectPath(distinct, upperSibling, LR"(E:\New\Upper\Models\b.pmd)");
    ExpectPath(distinct, lowerSibling, LR"(F:\New\Lower\Models\b.pmd)");

    Check(distinct.AddConfirmed(upper, LR"(G:\Other\Upper\Models\a.pmd)"),
          "confirm a second destination for the exact Pack root");
    const auto conflict = distinct.Infer(upperSibling);
    Check(conflict.status == Status::Ambiguous && conflict.path.empty(),
          "different destinations for the same exact root remain ambiguous");
    ExpectPath(distinct, lowerSibling, LR"(F:\New\Lower\Models\b.pmd)");
}

void CheckProjectRelocation() {
    const auto model = LR"(D:\旧工程\Models\A\body.pmx)";
    const auto selected = LR"(E:\新工程\Models\A\body.pmx)";
    const auto light = LR"(D:\旧工程\Effects\Light\main.fx)";
    const auto shade = LR"(D:\旧工程\Effects\Shade\main.fx)";
    RootMappings roots;
    Check(roots.AddConfirmed(model, selected), "accept confirmed model pair");
    // Later stream records can use this one pair, including same-name effects.
    ExpectPath(roots, LR"(d:/旧工程/Models/B/body.pmx)", LR"(E:\新工程\Models\B\body.pmx)");
    ExpectPath(roots, light, LR"(E:\新工程\Effects\Light\main.fx)");
    ExpectPath(roots, shade, LR"(E:\新工程\Effects\Shade\main.fx)");
    RootMappings renamed;
    const project_model::Reference renamedModel{{model}};
    Check(renamedModel.Confirm(renamed, LR"(E:\New\renamed.pmx)"),
          "a unique original can remember a manually selected renamed file");
    ExpectPath(renamed, model, LR"(E:\New\renamed.pmx)");
    Check(renamed.Infer(light).status == Status::NoMatch, "a renamed file supplies no evidence for sibling paths");
    ExpectPath(roots, LR"(D:\旧工程\Scenes\..\Effects\.\光\main.fx)", LR"(E:\新工程\Effects\光\main.fx)");
    for (auto outside : {LR"(D:\旧工程2\Effects\Light\main.fx)", LR"(F:\旧工程\Effects\Light\main.fx)",
                         LR"(D:\旧工程\..\Other\main.fx)"})
        Check(roots.Infer(outside).status == Status::NoMatch, "respect component and volume boundaries");

    RootMappings effectsFirst;
    Check(effectsFirst.AddConfirmed(light, LR"(E:\新工程\Effects\Light\main.fx)"), "accept effect as first pair");
    ExpectPath(effectsFirst, model, selected);
    Check(!effectsFirst.AddConfirmed(model, LR"(F:\else\renamed.pmx)"), "changed leaf gives no directory evidence");
    Check(!effectsFirst.AddConfirmed(model, LR"(F:else\body.pmx)"), "drive-relative selection gives no mapping");
    Check(!effectsFirst.AddConfirmed(LR"(D:\..\旧工程\Models\A\body.pmx)", selected),
          "old reference cannot escape its volume");
    ExpectPath(effectsFirst, model, selected);

    // A directory renamed inside the tree supports a narrower mapping only.
    const auto actor = LR"(D:\旧工程\Models\C\third.pmx)";
    const auto narrow = LR"(F:\replace\Actors\C\third.pmx)";
    const auto other = LR"(G:\other\Effects\Shade\main.fx)";
    for (bool reverse : {false, true}) {
        RootMappings overlap;
        if (reverse) {
            Check(overlap.AddConfirmed(actor, narrow), "add specific rule first");
            Check(overlap.AddConfirmed(shade, other), "add conflicting broad rule first");
            Check(overlap.AddConfirmed(model, selected), "add original broad rule last");
        } else {
            Check(overlap.AddConfirmed(model, selected), "add original broad rule first");
            Check(overlap.AddConfirmed(shade, other), "add conflicting broad rule second");
            Check(overlap.AddConfirmed(actor, narrow), "add specific rule after broad conflict");
        }
        ExpectPath(overlap, LR"(D:\旧工程\Models\B\body.pmx)", LR"(F:\replace\Actors\B\body.pmx)");
        const auto conflict = overlap.Infer(light);
        Check(conflict.status == Status::Ambiguous && conflict.path.empty(),
              "same-depth conflicting roots never choose either same-name effect");
    }
    RootMappings limited;
    Check(limited.AddConfirmed(actor, narrow), "infer only through the common C directory");
    Check(limited.Infer(light).status == Status::NoMatch, "stop before mismatched Models and Actors");

    RootMappings network;
    Check(network.AddConfirmed(LR"(\\srv\assets\Old\Fx\A.fx)", LR"(\\dest\library\New\Fx\A.fx)"),
          "accept absolute UNC pair");
    ExpectPath(network, LR"(\\SRV\ASSETS\Old\Models\B.pmx)", LR"(\\dest\library\New\Models\B.pmx)");
    Check(network.Infer(LR"(\\srv\other\Old\Models\B.pmx)").status == Status::NoMatch,
          "UNC share is part of the volume");
    RootMappings driveMove;
    Check(driveMove.AddConfirmed(LR"(D:\Show\Models\A.pmx)", LR"(E:\Show\Models\A.pmx)"),
          "same tree across drives");
    ExpectPath(driveMove, LR"(D:\Show\Fx\B.fx)", LR"(E:\Show\Fx\B.fx)");
    Check(driveMove.Infer(LR"(D:\AnotherShow\Fx\B.fx)").status == Status::NoMatch,
          "one confirmed pair cannot map the whole drive");
    Check(!driveMove.AddConfirmed(LR"(D:\A.pmx)", LR"(E:\A.pmx)"), "no named directory to anchor a root-level pair");

    for (auto unsafe : {L"main.fx", LR"(D:Old\main.fx)", LR"(\Old\main.fx)", LR"(\\srv\share)",
                        LR"(\\?\D:\旧工程\main.fx)", LR"(\\.\pipe\main.fx)", LR"(\\srv\pipe\Old\main.fx)",
                        LR"(\\srv\share\..\Old\main.fx)", LR"(D:\..\旧工程\main.fx)", LR"(D:\旧工程\.)",
                        LR"(D:\旧工程\main.fx:stream)", LR"(D:\旧工程\bad.\main.fx)", LR"(D:\旧工程\AUX .fx)",
                        LR"(D:\旧工程\LPT².fx)", LR"(D:\旧工程\*.fx)", LR"(D:\旧工程\)"}) {
        const auto rejected = roots.Infer(unsafe);
        Check(rejected.status == Status::InvalidPath && rejected.path.empty(),
              "unsafe or unanchored input cannot produce a candidate");
    }
    std::wstring embeddedNul(light);
    embeddedNul.push_back(0);
    embeddedNul += L".other";
    Check(roots.Infer(embeddedNul).status == Status::InvalidPath, "embedded NUL cannot truncate a reference");
    const auto expected = std::wstring(LR"(E:\新工程\Effects\Light\main.fx)");
    const auto tooShort = roots.Infer(light, expected.size());
    Check(tooShort.status == Status::TooLong && tooShort.path.empty(), "capacity includes the NUL, never truncates");
    Check(roots.Infer(light, expected.size() + 1).path == expected, "exact buffer capacity accepted");
    Check(roots.AddConfirmed(shade, LR"(e:/新工程/Effects/Shade/main.fx)"), "repeated evidence for same root");
    ExpectPath(roots, light, expected);
    Check(RootMappings{}.Infer(light).status == Status::NoMatch, "new project has no inherited mapping");
}
}
int main() {
    namespace fs = std::filesystem;
    CheckProjectRelocation();
    CheckCaseSensitiveRoots();
    const fs::path directory = fs::temp_directory_path() / (L"MMS-path-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directory(directory);
    CheckEncodedModelPair(directory);
    CheckCaseOnlyModelAlias(directory);
    struct RestoreDirectory {
        fs::path previous = fs::current_path();
        ~RestoreDirectory() { std::error_code ignored; fs::current_path(previous, ignored); }
    } restore;
    fs::current_path(directory);
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, system.dwPageSize * 2,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Check(memory != nullptr, "allocate guarded source string");
    if (!memory) return 1;
    DWORD previous = 0;
    Check(VirtualProtect(memory, system.dwPageSize, PAGE_NOACCESS, &previous) != FALSE,
        "guard page before source");
    auto* source = reinterpret_cast<wchar_t*>(memory + system.dwPageSize);
    PathResolutionWorkspace paths{};
    for (const auto* input : {L"", L"p", L"pm", L"pmd", L"wav", L"vac", L"WAV", L"file.pmd"}) {
        std::wcscpy(source, input);
        Check(ResolveUserFilePath(paths, source)[0] == L'\0',
            "missing short/basename path returns empty without a backwards read");
    }
    VirtualFree(memory, 0, MEM_RELEASE);
    for (const auto* folder : {L"Model", L"Wave", L"Accessory"})
        fs::create_directories(directory / L"UserFile" / folder);
    std::wcscpy(paths.executableDirectory, (directory.wstring() + L"\\").c_str());
    for (const auto* name : {L"fixture.pmd", L"fixture.wav", L"fixture.vac"}) {
        const auto* folder = std::wcscmp(name, L"fixture.pmd") == 0 ? L"Model" :
            std::wcscmp(name, L"fixture.wav") == 0 ? L"Wave" : L"Accessory";
        const fs::path file = directory / L"UserFile" / folder / name;
        { std::ofstream stream(file); stream << "fixture"; }
        Check(std::wstring(ResolveUserFilePath(paths, (std::wstring(L"missing\\") + name).c_str())) == file.wstring(),
            "missing normal path keeps existing UserFile fallback");
        Check(std::wstring(ResolveUserFilePath(paths, file.c_str())) == file.wstring(),
            "existing absolute file remains directly resolvable");
        fs::remove(file);
    }
    for (const auto* folder : {L"Model", L"Wave", L"Accessory"}) fs::remove(directory / L"UserFile" / folder);
    fs::remove(directory / L"UserFile"); fs::current_path(restore.previous); fs::remove(directory);
    std::printf("path_resolve_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
