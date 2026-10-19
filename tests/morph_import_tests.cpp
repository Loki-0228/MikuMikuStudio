// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace mikumikustudio {
void PostLoadInit(unsigned char*);
}
using namespace mikumikustudio;
namespace {
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
using Record = std::array<unsigned char, 40>;
Record Key(const char* name, std::uint32_t frame, float value) {
    Record r{};
    std::memcpy(r.data(), name, (std::min)(std::strlen(name), std::size_t(19)));
    std::memcpy(r.data() + 32, &frame, 4);
    std::memcpy(r.data() + 36, &value, 4);
    return r;
}
void RegistrarRegression() {
    // A bad typed stride lands in reserved memory, rather than silently
    // reading another allocation. Keep the last valid record at the boundary.
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    auto* region = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 65536, MEM_RESERVE, PAGE_NOACCESS));
    Check(region != nullptr, "reserve guarded morph records");
    Check(VirtualAlloc(region, info.dwPageSize, MEM_COMMIT, PAGE_READWRITE) != nullptr,
          "commit first guard page");
    auto* morphs = reinterpret_cast<mdl::MorphRecord*>(
        region + info.dwPageSize - 3 * sizeof(mdl::MorphRecord));
    std::strcpy(morphs[0].name, "first");
    std::strcpy(morphs[1].name, "second");
    std::strcpy(morphs[2].name, "last");
    auto model = std::make_unique<mdl::ModelRecord>();
    std::vector<mdl::MorphKey> keys(mdl::kMorphKeyCapacity);
    model->morphs = morphs; model->morphCount = 3;
    model->morphKeys = keys.data(); model->searchCursor = 3;
    auto* bytes = reinterpret_cast<unsigned char*>(model.get());
    for (const auto& r : {Key("last", 20, .8f), Key("last", 10, .4f),
                          Key("last", 20, .9f), Key("second", 0, .2f),
                          Key("missing", 30, 1.f)})
        Check(RegisterMorphKeyFromRecord(bytes, r.data(), 0), "register key safely");
    Check(keys[1].value == .2f && keys[1].allocated, "frame zero on second morph");
    const auto first = keys[2].next;
    const auto second = keys[first].next;
    Check(first >= 3 && keys[first].frame == 10 && keys[first].value == .4f,
          "insert earlier key into correct track");
    Check(second >= 3 && keys[second].frame == 20 && keys[second].value == .9f,
          "replace existing key in correct track");
    Check(keys[second].next == 0 && model->maxFrame == 20 && !keys[0].allocated,
          "unmatched names leave tracks unchanged");
    model->morphKeys = nullptr;
    Check(!RegisterMorphKeyFromRecord(bytes, Key("last", 2, 0.f).data(), 0),
          "missing key arena fails safely");
    VirtualFree(region, 0, MEM_RELEASE);
}

void RegistrarGrowthRegression() {
    auto model = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::MorphRecord, 2> morphs{};
    std::strcpy(morphs[0].name, "first");
    std::strcpy(morphs[1].name, "last");
    model->morphCount = 2;
    model->morphs = morphs.data();
    model->physicsMode = 2;
    model->morphKeys = mdl::MorphKeyPoolCreate(mdl::kMorphKeyCapacity);
    for (std::size_t i = 2; i < mdl::kMorphKeyCapacity; ++i) model->morphKeys[i].frame = 1;
    auto* bytes = reinterpret_cast<unsigned char*>(model.get());
    RegisterMorphKeyCurrent(bytes, 1, 12);
    auto slot = model->morphKeys[1].next;
    Check(slot >= mdl::kMorphKeyCapacity && model->morphKeys[slot].frame == 12,
          "current-value registration grows a full morph pool");
    model->searchCursor = static_cast<int>(mdl::kBoneKeyCapacity);
    Check(RegisterMorphKeyFromRecord(bytes, Key("first", 30, .8f).data(), 0),
          "record registration accepts a shared cursor left past the morph pool");
    slot = model->morphKeys[0].next;
    Check(slot >= mdl::kMorphKeyCapacity && model->morphKeys[slot].frame == 30 &&
              model->morphKeys[slot].value == .8f,
          "record registration writes the grown pool");
    ResetMorphKeyCursor(bytes);
    Check(std::size_t(model->searchCursor) < mdl::MorphKeyCapacity(*model),
          "reset finds free space beyond the former upper limit");
    mdl::MorphKeyPoolDestroy(model->morphKeys);
}

void UnicodeComboRegression() {
    HWND parent = CreateWindowExW(0, L"STATIC", L"morph test", 0,
        0, 0, 300, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(parent != nullptr, "create hidden control owner");
    HWND combos[4]{};
    for (int lane = 0; lane < 4; ++lane) {
        combos[lane] = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | CBS_DROPDOWNLIST, 0, 0, 200, 200, parent,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(504 + 5 * lane)),
            GetModuleHandleW(nullptr), nullptr);
        Check(combos[lane] && IsWindowUnicode(combos[lane]), "Unicode morph combo");
    }
    std::array<mdl::MorphRecord, 4> morphs{};
    const wchar_t* fullName = L"まばたき_中文_長い表情名前_😀";
    morphs[0].jpText = const_cast<wchar_t*>(fullName);
    morphs[1].jpText = const_cast<wchar_t*>(fullName); // Duplicate display name.
    morphs[1].enText = const_cast<wchar_t*>(L"blink alternate");
    morphs[0].panel = morphs[1].panel = mdl::MorphPanel::eye;
    std::strcpy(morphs[2].name, "\x82\xA0"); // PMD CP932 'あ'.
    morphs[2].panel = mdl::MorphPanel::mouth;
    morphs[3].panel = mdl::MorphPanel::eyebrow;
    std::strcpy(morphs[3].name, "brow");
    mdl::BoneRecord bone{};
    auto model = std::make_unique<mdl::ModelRecord>();
    model->hwnd = parent; model->boneTable = &bone;
    model->morphs = morphs.data(); model->morphCount = 4;
    auto* bytes = reinterpret_cast<unsigned char*>(model.get());
    // Name language is a UI-language decision (0 JP / 1 EN / 2 CHS), taken from
    // the live UI language: the per-model physicsFlags byte used to drive it,
    // but PMX loads never set it and pmd_load stored the raw language byte, so
    // the tree panel and the combos could disagree (CHS showed English bone
    // names next to Japanese morph names).
    auto app = std::make_unique<MMDApp>();
    MMDApp* previousBlock = g_Block;
    g_Block = app.get();
    for (int language : {0, 1, 2}) {
        app->EnglishUI() = language;
        // A stale per-model flag must not override the UI language any more.
        model->physicsFlags = static_cast<unsigned char>(language == 1);
        model->selectedMorphs[0] = 999; model->selectedMorphs[1] = 1;
        model->selectedMorphs[2] = -1; model->selectedMorphs[3] = 0;
        PostLoadInit(bytes);
        wchar_t text[256]{};
        SendMessageW(combos[1], CB_GETLBTEXT, 0, reinterpret_cast<LPARAM>(text));
        Check(std::wstring(text) == fullName, "full PMX Unicode name and English fallback");
        SendMessageW(combos[1], CB_GETLBTEXT, 1, reinterpret_cast<LPARAM>(text));
        Check(std::wstring(text) == (language == 1 ? L"blink alternate" : fullName),
              "language switch preserves Unicode names");
        Check(SendMessageW(combos[1], CB_GETCURSEL, 0, 0) == 1 &&
              SendMessageW(combos[1], CB_GETITEMDATA, 0, 0) == 0 &&
              SendMessageW(combos[1], CB_GETITEMDATA, 1, 0) == 1,
              "duplicate names preserve distinct model indices");
        SendMessageW(combos[2], CB_GETLBTEXT, 0, reinterpret_cast<LPARAM>(text));
        Check(std::wstring(text) == L"あ", "legacy names decode as CP932");
        Check(model->selectedMorphs[0] == 3 && model->selectedMorphs[2] == 2 &&
              model->selectedMorphs[3] == -1, "stale and empty category selections reset safely");
    }
    // The reported CHS symptom: an English flag left over from a PMX load must
    // not flip the names while the UI is Chinese.
    app->EnglishUI() = 2;
    model->physicsFlags = 1;
    PostLoadInit(bytes);
    wchar_t chsText[256]{};
    SendMessageW(combos[1], CB_GETLBTEXT, 1, reinterpret_cast<LPARAM>(chsText));
    Check(std::wstring(chsText) == fullName,
          "Chinese UI keeps the model's own names despite a stale English flag");
    g_Block = previousBlock;
    DestroyWindow(parent);
}
void VsqLyricStorageRegression() {
    for (int tempos : {0, 1, 2}) {
        namespace fs = std::filesystem;
        const auto path = fs::temp_directory_path() /
            (L"MDS-vsq-layout-" + std::to_wstring(GetCurrentProcessId()) + L".vsq");
        const std::string lyric = "long-lyric-%s-%n-kept-literally";
        const std::string text = "[EventList]\n0=ID#0000\n480=ID#0001\n960=EOS\n"
            "[ID#0001]\nLength=480\nLyricHandle=h#00000001\n[h#00000001]\nL0=\"" + lyric + "\",\"a\",0\n";
        std::string track;
        for (int i = 0; i < tempos; ++i)
            track.append("\0\xFF\x51\x03\x07\xA1\x20", 7); // 120 BPM
        for (std::size_t pos = 0; pos < text.size(); pos += 100) {
            const auto part = text.substr(pos, 100);
            track.append("\0\xFF\x01", 3);
            track.push_back(static_cast<char>(8 + part.size()));
            track += "DM:0000:" + part;
        }
        track.append("\0\xFF\x2F\0", 4);
        {
            std::ofstream file(path, std::ios::binary);
            file.write("MThd\0\0\0\x06\0\0\0\x01\x01\xE0MTrk", 18);
            for (int shift : {24, 16, 8, 0}) file.put(char(track.size() >> shift));
            file.write(track.data(), track.size());
            Check(bool(file), "write synthetic VSQ fixture");
        }
        auto app = std::make_unique<MMDApp>();
        std::memset(&app->state, 0, sizeof(app->state));
        auto renderer = std::make_unique<D3DRenderer>();
        std::memset(renderer.get(), 0, sizeof(*renderer));
        app->Renderer() = renderer.get();
        auto model = std::make_unique<mdl::ModelRecord>();
        model->physicsMode = 2; // PMX: morph zero is editable, not the PMD base face.
        std::array<mdl::MorphRecord, 4> morphs{};
        const char* names[] = {"\x82\xA0", "\x82\xA2", "\x82\xA4", "\x82\xA8"};
        for (int i = 0; i < 4; ++i) std::strcpy(morphs[i].name, names[i]);
        std::vector<mdl::BoneKey> bones(mdl::kBoneKeyCapacity);
        std::vector<mdl::MorphKey> keys(mdl::kMorphKeyCapacity);
        std::vector<mdl::DisplayKey> displays(mdl::kDisplayKeyCapacity);
        model->boneKeys = bones.data();
        model->morphKeys = keys.data();
        model->displayKeys = displays.data();
        model->morphs = morphs.data();
        model->morphCount = 4;
        model->searchCursor = 4;
        app->ModelSlot(0) = reinterpret_cast<unsigned char*>(model.get());
        // The legacy loader changes the extension in its caller-provided buffer.
        std::wstring mutablePath = path.wstring();
        LoadVsqFile(app.get(), mutablePath.data());
        std::ifstream result{fs::path(mutablePath)};
        std::string line;
        std::getline(result, line);
        float start = 0, end = 0;
        char separator = 0;
        std::istringstream values(line);
        values >> start >> separator >> end;
        Check(start == .5f && separator == '-' && end == 1.f,
              "lyric storage never overlaps event timestamps");
        Check(line.find(lyric) != std::string::npos, "long lyric and percent signs remain literal");
        Check(model->maxFrame == 30, "VSQ event generates its final mouth key at frame 30");
        result.close();
        fs::remove(path);
        fs::remove(fs::path(mutablePath));
    }
}

void VerifyVmd(const wchar_t* path) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    Check(bool(file), "open supplied VMD read-only");
    char header[30]{}; file.read(header, 30);
    Check(std::strncmp(header, "Vocaloid Motion Data 0002", 24) == 0, "VMD2 header");
    file.seekg(20, std::ios::cur);
    std::uint32_t count = 0; file.read(reinterpret_cast<char*>(&count), 4);
    file.seekg(static_cast<std::streamoff>(count) * 111, std::ios::cur);
    file.read(reinterpret_cast<char*>(&count), 4);
    Check(bool(file) && count < 20000, "bounded fixture morph count");
    std::vector<Record> records;
    std::vector<mdl::MorphRecord> morphs;
    std::map<std::string, std::size_t> names;
    std::vector<std::map<std::uint32_t, float>> expected;
    for (std::uint32_t i = 0; i < count; ++i) {
        Record r{}; file.read(reinterpret_cast<char*>(r.data()), 15);
        file.read(reinterpret_cast<char*>(r.data() + 32), 8);
        Check(bool(file), "complete fixture morph record");
        const std::string name(reinterpret_cast<char*>(r.data()));
        auto [it, inserted] = names.emplace(name, morphs.size());
        if (inserted) {
            mdl::MorphRecord morph{}; std::memcpy(morph.name, r.data(), 15);
            morphs.push_back(morph); expected.emplace_back();
        }
        std::uint32_t frame; float value;
        std::memcpy(&frame, r.data() + 32, 4); std::memcpy(&value, r.data() + 36, 4);
        expected[it->second][frame] = value;
        records.push_back(r);
    }
    auto model = std::make_unique<mdl::ModelRecord>();
    std::vector<mdl::MorphKey> keys(mdl::kMorphKeyCapacity);
    model->morphs = morphs.data(); model->morphCount = static_cast<std::uint32_t>(morphs.size());
    model->morphKeys = keys.data(); model->searchCursor = static_cast<int>(morphs.size());
    for (const auto& r : records)
        Check(RegisterMorphKeyFromRecord(reinterpret_cast<unsigned char*>(model.get()),
                                        r.data(), 0), "import supplied facial key");
    for (std::size_t m = 0; m < morphs.size(); ++m) {
        std::map<std::uint32_t, float> actual;
        std::size_t i = m, visited = 0;
        do {
            Check(i < keys.size() && ++visited <= keys.size(), "valid key chain");
            if (keys[i].allocated) actual[keys[i].frame] = keys[i].value;
            i = keys[i].next;
        } while (i != 0);
        Check(actual == expected[m], "every imported frame/value matches its source track");
    }
    std::printf("Verified %u VMD keys across %zu morph tracks.\n", count, morphs.size());
}
}
int wmain(int argc, wchar_t** argv) {
    RegistrarRegression();
    RegistrarGrowthRegression();
    UnicodeComboRegression();
    VsqLyricStorageRegression();
    if (argc > 1) VerifyVmd(argv[1]);
    std::puts("Morph import and Unicode control regressions passed.");
}
