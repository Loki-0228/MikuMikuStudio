// Included only by the real PMM host. This cohort owns every model/project it
// writes and never joins the default reversible-encoding suite.
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/ui_message.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include <algorithm>
#include <commdlg.h>
#include <cwchar>

namespace pmm_identity_host {
using Bytes = std::vector<unsigned char>;
struct Name { std::string jp, en; };
struct Layout { std::vector<Name> bones, morphs; };
struct Fixture {
    std::filesystem::path source, project;
    std::string bone, morph;
    std::wstring boneText, morphText;
    float seed = 0;
    Bytes original;
};
constexpr char kBoneEnglish[] = "target-bone-en";
constexpr char kMorphEnglish[] = "target-face-en";

struct Wire {
    Bytes bytes;
    void U8(unsigned value) { Check(value <= 255, "PMD byte is bounded"); bytes.push_back(static_cast<unsigned char>(value)); }
    void U16(unsigned value) { Check(value <= 65535, "PMD word is bounded"); U8(value & 255); U8(value >> 8); }
    void U32(std::uint32_t value) { for (unsigned i = 0; i < 4; ++i) U8((value >> (8 * i)) & 255); }
    void F32(float value) { static_assert(sizeof(float) == 4); std::uint32_t bits = 0; std::memcpy(&bits, &value, 4); U32(bits); }
    void Fixed(const std::string& text, std::size_t capacity) {
        Check(text.size() <= capacity && text.find('\0') == std::string::npos, "fixture fixed field fits without hidden NULs");
        bytes.insert(bytes.end(), text.begin(), text.end()); bytes.insert(bytes.end(), capacity - text.size(), 0);
    }
};
void WriteNew(const std::filesystem::path& path, const Bytes& bytes) {
    Check(bytes.size() < MAXDWORD, "owned identity fixture fits a bounded write");
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "create a new identity fixture without replacing any existing file");
    DWORD written = 0; const BOOL ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    const BOOL flushed = ok && written == bytes.size() ? FlushFileBuffers(file) : FALSE;
    const BOOL closed = CloseHandle(file);
    Check(ok && written == bytes.size() && flushed && closed, "write the complete new identity fixture");
}
Layout SourceLayout(const Fixture& fixture) {
    return {{{fixture.bone, kBoneEnglish}, {"control-bone", "control-bone-en"}},
            {{"base", ""}, {fixture.morph, kMorphEnglish}, {"control-face", "control-face-en"}}};
}
Layout ReorderedLayout(const Fixture& fixture) {
    return {{{"control-bone", "control-bone-en"}, {"extra-bone", "extra-bone-en"}, {fixture.bone, kBoneEnglish}},
            {{"base", ""}, {"control-face", "control-face-en"}, {"extra-face", "extra-face-en"}, {fixture.morph, kMorphEnglish}}};
}
void WritePmd(const std::filesystem::path& path, const Layout& layout) {
    Check(!layout.bones.empty() && layout.morphs.size() >= 2 && layout.morphs[0].jp == "base", "complete PMD layout has bones, a base morph and an ordinary morph");
    Wire wire; wire.bytes = {'P', 'm', 'd'}; wire.F32(1.f);
    wire.Fixed("identity-model", 20); wire.Fixed("Owned full-width identity regression", 256);
    wire.U32(3);
    const std::array<std::array<float, 3>, 3> points{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}};
    for (const auto& point : points) {
        for (float value : point) wire.F32(value);
        for (float value : {0.f, 0.f, 1.f, point[0], point[1]}) wire.F32(value);
        wire.U16(0); wire.U16(0); wire.U8(100); wire.U8(0);
    }
    wire.U32(3); wire.U16(0); wire.U16(1); wire.U16(2);
    wire.U32(1);
    for (float value : {.7f, .6f, .5f, 1.f, 1.f, 0.f, 0.f, 0.f, .1f, .1f, .1f}) wire.F32(value);
    wire.U8(0); wire.U8(0); wire.U32(3); wire.Fixed("", 20);
    wire.U16(static_cast<unsigned>(layout.bones.size()));
    for (const auto& name : layout.bones) {
        wire.Fixed(name.jp, 20); wire.U16(65535); wire.U16(0); wire.U8(1); wire.U16(0);
        wire.F32(0); wire.F32(0); wire.F32(0);
    }
    wire.U16(0); // IK chains
    wire.U16(static_cast<unsigned>(layout.morphs.size()));
    for (std::size_t i = 0; i < layout.morphs.size(); ++i) {
        wire.Fixed(layout.morphs[i].jp, 20); wire.U32(i == 0 ? 3 : 1); wire.U8(i == 0 ? 0 : 1);
        const std::size_t entries = i == 0 ? 3 : 1;
        for (std::size_t entry = 0; entry < entries; ++entry) {
            wire.U32(static_cast<std::uint32_t>(i == 0 ? entry : (i - 1) % 3));
            for (unsigned axis = 0; axis < 3; ++axis) wire.F32(i == 0 ? points[entry][axis] : axis == 0 ? .125f * float(i) : 0.f);
        }
    }
    // No display references: they must not copy an unterminated identity into
    // an unrelated display record before the actual PMM writer is exercised.
    wire.U8(0); wire.U8(0); wire.U32(0);
    wire.U8(1); wire.Fixed("Identity model", 20); wire.Fixed("Owned English identity fixture", 256);
    for (const auto& name : layout.bones) wire.Fixed(name.en, 20);
    for (std::size_t i = 1; i < layout.morphs.size(); ++i) wire.Fixed(layout.morphs[i].en, 20);
    for (unsigned i = 1; i <= 10; ++i) {
        char toon[20]{}; std::snprintf(toon, sizeof toon, "toon%02u.bmp", i); wire.Fixed(toon, 100);
    }
    wire.U32(0); wire.U32(0); // Complete rigid-body and joint tails.
    WriteNew(path, wire.bytes);
}
template<std::size_t N> std::string Bounded(const char (&text)[N]) { return std::string(text, strnlen_s(text, N)); }
void Names(const mdl::ModelRecord& model, const Layout& layout, std::size_t bone, std::size_t morph, const Fixture& fixture) {
    static_assert(offsetof(mdl::BoneRecord, nameEn) == 20 && offsetof(mdl::MorphRecord, nameEn) == 20);
    Check(model.vertexCount == 3 && model.indexCount == 3 && model.materialCount == 1 &&
          model.boneCount == layout.bones.size() && model.morphCount == layout.morphs.size(), "real PMD loader accepts complete triangle, material, bone and morph sections");
    for (std::size_t i = 0; i < layout.bones.size(); ++i)
        Check(Bounded(model.boneTable[i].name) == layout.bones[i].jp && Bounded(model.boneTable[i].nameEn) == layout.bones[i].en, "actual loaded bone JP and adjacent English fields match the independent fixed-width fixture");
    for (std::size_t i = 0; i < layout.morphs.size(); ++i)
        Check(Bounded(model.morphs[i].name) == layout.morphs[i].jp && Bounded(model.morphs[i].nameEn) == layout.morphs[i].en, "actual loaded morph JP and adjacent English fields match the independent fixed-width fixture");
    Check(strnlen_s(model.boneTable[bone].name, 20) == 20 && model.boneTable[bone].nameEn[0] &&
          strnlen_s(model.morphs[morph].name, 20) == 20 && model.morphs[morph].nameEn[0], "actual target JP fields occupy all 20 bytes and their immediately adjacent English fields are nonempty");
    Check(text_encoding::BoneName(model.boneTable[bone], false) == fixture.boneText &&
          text_encoding::MorphName(model.morphs[morph], false) == fixture.morphText &&
          text_encoding::BoneName(model.boneTable[bone], true) == L"target-bone-en" &&
          text_encoding::MorphName(model.morphs[morph], true) == L"target-face-en", "full-width CP932 identities and English names stay separate after actual source load");
}
void SeedKeys(mdl::ModelRecord& model, float seed) {
    Check(model.boneCount == 2 && model.morphCount == 3 && mdl::BoneKeyCapacity(model) > 3 && mdl::MorphKeyCapacity(model) > 4, "source supplies separate target/control tracks and their real key capacity");
    for (unsigned i = 0; i < 2; ++i) {
        auto& dense = model.boneKeys[i]; dense.position[0] = i == 0 ? seed + .25f : -seed - .5f;
        const unsigned index = 2 + i; model.boneKeys[index] = dense;
        auto& key = model.boneKeys[index]; key.frame = 17; key.previous = i; key.next = 0; key.allocated = 1;
        key.position[0] = i == 0 ? seed + 100.5f : -seed - 200.75f; dense.next = index;
    }
    for (unsigned i = 1; i <= 2; ++i) {
        auto& dense = model.morphKeys[i]; dense.value = i == 1 ? .25f : .625f;
        const unsigned index = 2 + i; model.morphKeys[index] = dense;
        auto& key = model.morphKeys[index]; key.frame = 17; key.previous = i; key.next = 0; key.allocated = 1;
        key.value = i == 1 ? .375f : .875f; dense.next = index;
    }
    model.maxFrame = 17;
}
void BoneTrack(const mdl::ModelRecord& model, unsigned dense, float initial, float later) {
    const auto& first = model.boneKeys[dense]; const auto index = first.next;
    Check(first.frame == 0 && first.position[0] == initial && index >= model.boneCount && index < mdl::BoneKeyCapacity(model), "bone target keeps its independent dense key and a valid sparse-key link");
    const auto& key = model.boneKeys[index];
    Check(key.frame == 17 && key.position[0] == later && key.previous == dense && !key.next && key.allocated, "bone frame-17 key belongs to the correct source identity after real PMM mapping");
}
void MorphTrack(const mdl::ModelRecord& model, unsigned dense, float initial, float later) {
    const auto& first = model.morphKeys[dense]; const auto index = first.next;
    Check(first.frame == 0 && first.value == initial && index >= model.morphCount && index < mdl::MorphKeyCapacity(model), "morph target keeps its independent dense key and a valid sparse-key link");
    const auto& key = model.morphKeys[index];
    Check(key.frame == 17 && key.value == later && key.previous == dense && !key.next && key.allocated, "morph frame-17 key belongs to the correct source identity after real PMM mapping");
}
void Keys(const mdl::ModelRecord& model, const Fixture& fixture, bool reordered, bool target = true) {
    const unsigned bone = reordered ? 2 : 0, morph = reordered ? 3 : 1;
    if (target) { BoneTrack(model, bone, fixture.seed + .25f, fixture.seed + 100.5f); MorphTrack(model, morph, .25f, .375f); }
    else Check(model.boneKeys[bone].position[0] == 0 && !model.boneKeys[bone].next && model.morphKeys[morph].value == 0 && !model.morphKeys[morph].next, "noncanonical appended identity cannot claim the target's dense or sparse keys");
    BoneTrack(model, reordered ? 0 : 1, -fixture.seed - .5f, -fixture.seed - 200.75f);
    MorphTrack(model, reordered ? 1 : 2, .625f, .875f);
    if (reordered) Check(model.boneKeys[1].position[0] == 0 && !model.boneKeys[1].next && model.morphKeys[2].value == 0 && !model.morphKeys[2].next, "newly inserted unrelated tracks receive none of the old target/control keys");
}
void Trailer(const Bytes& bytes) {
    Check(bytes.size() >= 32 && !std::memcmp(bytes.data() + bytes.size() - 32, "MMSXPMM2", 8), "identity cohort retains the existing MMS-only 32-byte trailer magic");
}
void Direct(MMDApp& app, Fixture& fixture) {
    ResetAppState(&app); WritePmd(fixture.source, SourceLayout(fixture)); fixture.original = EncodingFileBytes(fixture.source);
    LoadModelFile(&app, fixture.source.c_str(), false); Check(app.ModelSlot(0) != nullptr, "load the actual owned full-width PMD through the production loader");
    auto& model = *mdl::Mdl(app.ModelSlot(0)); Names(model, SourceLayout(fixture), 0, 1, fixture); SeedKeys(model, fixture.seed);
    const auto report = save_compatibility::InspectPmm(app);
    Check(!report.invalidData && !report.needsNewPmm && !report.namesNeedExtension, "real complete 20-byte PMD identities fit PMM v2 without a false Unicode warning");
    const auto motion = save_compatibility::InspectMotion(model);
    Check(motion.namesNeedExtension && !motion.canCopyLegacy, "20-byte PMD acceptance does not relax the independent 15-byte legacy VMD identity limit");
    Check(SaveSceneCopy(&app, fixture.project.c_str()), "save the full-width PMD keys with the real PMM serializer");
    const auto prefix = ReadEncodingPrefix(fixture.project);
    Check(prefix.bones.size() == 2 && prefix.morphs.size() == 3, "actual PMM has both complete independent identity tables");
    std::printf("PMD_IDENTITY_STANDARD bone=%zu morph=%zu expected=20 fixture=%ls\n", prefix.bones[0].size(), prefix.morphs[1].size(), fixture.source.filename().c_str());
    Check(prefix.bones == std::vector<std::string>{fixture.bone, "control-bone"}, "standard PMM bone table contains exactly the 20 JP bytes without adjacent English bytes");
    Check(prefix.morphs == std::vector<std::string>{"base", fixture.morph, "control-face"}, "standard PMM morph table contains exactly the 20 JP bytes without adjacent English bytes");
    Trailer(EncodingFileBytes(fixture.project)); Read(fixture.project); Load(app, fixture.project);
    Check(app.ModelSlot(0) != nullptr, "reopen actual full-width PMD project");
    const auto& restored = *mdl::Mdl(app.ModelSlot(0)); Names(restored, SourceLayout(fixture), 0, 1, fixture); Keys(restored, fixture, false);
    Check(EncodingFileBytes(fixture.source) == fixture.original, "direct PMM save/load leaves every original PMD byte unchanged");
    std::printf("PASS direct PMD identity fixture=%ls\n", fixture.source.filename().c_str());
}

struct ExpectedDialogs {
    MMDApp& app; bool reselect; std::filesystem::path replacement;
    unsigned dialogs = 0, pickers = 0; bool ownMinhook = false;
    inline static ExpectedDialogs* active = nullptr;
    static INT_PTR Dialog(MMDApp* app, HINSTANCE, HWND owner, ProjectModelIssue issue, const wchar_t* label) {
        auto* self = active;
        Check(self && app == &self->app && owner == app->MainWindow() && issue == ProjectModelIssue::StructureMismatch && label &&
              (!std::wcscmp(label, L"identity-model") || !std::wcscmp(label, L"Identity model")), "only the exact owned structure-mismatch dialog may be answered by this cohort");
        ++self->dialogs; Check(self->dialogs <= (self->reselect ? 2u : 1u), "no unexpected repeated project mismatch is swallowed");
        return self->reselect && self->dialogs == 1 ? 3 : 1;
    }
    static BOOL Picker(LPOPENFILENAMEW request) {
        auto* self = active;
        Check(self && self->reselect && self->dialogs == 1 && self->pickers == 0 && request && request->hwndOwner == self->app.MainWindow() &&
              request->lpstrFile && request->nMaxFile > self->replacement.wstring().size() && (request->Flags & OFN_FILEMUSTEXIST) &&
              request->lpstrDefExt && !std::wcscmp(request->lpstrDefExt, L"pmd;pmx") && std::filesystem::is_regular_file(self->replacement), "only the expected real model picker receives the owned replacement PMD");
        ++self->pickers; wcscpy_s(request->lpstrFile, request->nMaxFile, self->replacement.c_str()); return TRUE;
    }
    ExpectedDialogs(MMDApp& value, bool choose, const std::filesystem::path& path = {}) : app(value), reselect(choose), replacement(path) {
        Check(!active, "one identity-dialog expectation owns its hooks"); active = this;
        const auto status = MH_Initialize(); ownMinhook = status == MH_OK;
        Check(ownMinhook || status == MH_ERROR_ALREADY_INITIALIZED, "reuse product MinHook initialization without taking its ownership");
        Check(MH_CreateHook(reinterpret_cast<void*>(&ShowProjectModelDialog), reinterpret_cast<void*>(&Dialog), nullptr) == MH_OK &&
              MH_CreateHook(reinterpret_cast<void*>(&GetUiOpenFileNameW), reinterpret_cast<void*>(&Picker), nullptr) == MH_OK, "control only two explicit production dialog adapters");
        Check(MH_EnableHook(reinterpret_cast<void*>(&ShowProjectModelDialog)) == MH_OK &&
              MH_EnableHook(reinterpret_cast<void*>(&GetUiOpenFileNameW)) == MH_OK, "enable bounded owned-fixture mismatch and picker expectations");
    }
    ~ExpectedDialogs() {
        MH_DisableHook(reinterpret_cast<void*>(&GetUiOpenFileNameW)); MH_RemoveHook(reinterpret_cast<void*>(&GetUiOpenFileNameW));
        MH_DisableHook(reinterpret_cast<void*>(&ShowProjectModelDialog)); MH_RemoveHook(reinterpret_cast<void*>(&ShowProjectModelDialog));
        active = nullptr; if (ownMinhook) MH_Uninitialize();
    }
    void Finish() const { Check(dialogs == (reselect ? 2u : 1u) && pickers == (reselect ? 1u : 0u), "actual PMM reconciliation took exactly the declared mismatch/reselection path"); }
};

struct Span { std::size_t begin, end; };
struct StandardTables { std::size_t path; std::vector<Span> bones, morphs; };
StandardTables Tables(const Bytes& bytes) {
    Trailer(bytes); Check(bytes.size() > 56 && !std::memcmp(bytes.data(), "Polygon Movie maker 0002", 24) && bytes[54] == 1 && bytes[55] == 0, "patch only the independent one-model slot-zero v2 fixture");
    std::size_t at = 56;
    const auto text = [&]() { Check(at < bytes.size(), "fixture text length exists"); const auto begin = at; const auto length = bytes[at++]; Check(length <= bytes.size() - at, "fixture text remains in the owned PMM"); at += length; return Span{begin, at}; };
    text(); text(); StandardTables result{at, {}, {}}; Check(bytes.size() - at > 256, "fixture model path and group marker exist"); at += 257;
    for (auto* table : {&result.bones, &result.morphs}) {
        Check(bytes.size() - at >= 4, "fixture name count exists"); std::uint32_t count = 0;
        for (unsigned i = 0; i < 4; ++i) count |= std::uint32_t(bytes[at++]) << (8 * i);
        Check(count < 256, "fixture table count is bounded");
        for (std::uint32_t i = 0; i < count; ++i) table->push_back(text());
    }
    Check(result.bones.size() == 2 && result.morphs.size() == 3, "patch only independently known source fixture name tables"); return result;
}
void PathCopy(const std::filesystem::path& source, const std::filesystem::path& output, const std::filesystem::path& model) {
    auto bytes = EncodingFileBytes(source); const auto tables = Tables(bytes); const auto before = bytes;
    char path[256]{}; Check(text_encoding::EncodePath(path, sizeof path, model.c_str()), "owned alternate model path fits its actual fixed field");
    std::memcpy(bytes.data() + tables.path, path, sizeof path);
    Check(std::equal(bytes.begin(), bytes.begin() + tables.path, before.begin()) &&
          std::equal(bytes.begin() + tables.path + sizeof path, bytes.end(), before.begin() + tables.path + sizeof path), "reference-copy construction changes only the fixed source-path bytes");
    WriteNew(output, bytes);
}
void AppendedCopy(const Fixture& fixture, const std::filesystem::path& output, unsigned mode) {
    const auto input = EncodingFileBytes(fixture.project); const auto tables = Tables(input);
    const auto append = [mode](std::string jp, std::string en) { if (mode == 1) en.pop_back(); if (mode == 2) en += "-extra"; return jp + en; };
    const std::array<std::pair<Span, std::string>, 2> edits{{{tables.bones[0], append(fixture.bone, kBoneEnglish)}, {tables.morphs[1], append(fixture.morph, kMorphEnglish)}}};
    const std::size_t trailer = input.size() - 32; std::uint64_t standard = 0;
    for (unsigned i = 0; i < 8; ++i) standard |= std::uint64_t(input[trailer + 8 + i]) << (8 * i);
    Check(standard <= trailer && standard >= edits[1].first.end, "old identity edits are wholly inside the independently framed standard body");
    Bytes bytes; std::size_t copied = 0;
    for (const auto& edit : edits) {
        Check(edit.first.begin >= copied && edit.first.end <= standard && edit.second.size() < 256, "only the ordered target identity spans may be replaced");
        bytes.insert(bytes.end(), input.begin() + copied, input.begin() + edit.first.begin);
        bytes.push_back(static_cast<unsigned char>(edit.second.size())); bytes.insert(bytes.end(), edit.second.begin(), edit.second.end()); copied = edit.first.end;
    }
    bytes.insert(bytes.end(), input.begin() + copied, input.end()); const auto delta = bytes.size() - input.size(); const auto newTrailer = bytes.size() - 32;
    for (unsigned i = 0; i < 8; ++i) bytes[newTrailer + 8 + i] = static_cast<unsigned char>(((standard + delta) >> (8 * i)) & 255);
    Check(std::equal(input.begin() + copied, input.begin() + standard, bytes.begin() + copied + delta), "all remaining standard project bytes survive old-file construction unchanged");
    Check(std::equal(input.begin() + standard, input.begin() + trailer, bytes.begin() + standard + delta) &&
          std::equal(input.begin() + trailer, input.begin() + trailer + 8, bytes.begin() + newTrailer) &&
          std::equal(input.begin() + trailer + 16, input.end(), bytes.begin() + newTrailer + 16), "MMS extension records, magic, extension size and checksum remain byte-identical; only standardSize advances");
    WriteNew(output, bytes); Read(output);
    const auto prefix = ReadEncodingPrefix(output);
    Check(prefix.bones == std::vector<std::string>{edits[0].second, "control-bone"} &&
          prefix.morphs == std::vector<std::string>{"base", edits[1].second, "control-face"}, "independent PMM parser confirms the exact full, partial or extra English suffix in both fixture tables");
}
void LegacyAndMapping(MMDApp& app, const Fixture& fixture, const std::filesystem::path& directory) {
    const auto stem = fixture.source.stem().wstring();
    for (unsigned mode = 0; mode < 3; ++mode) {
        const auto project = directory / (stem + L"-old-append-" + std::to_wstring(mode) + L".pmm"); AppendedCopy(fixture, project, mode);
        if (mode == 0) Load(app, project);
        else { ExpectedDialogs expected(app, false); Load(app, project); expected.Finish(); }
        Check(app.ModelSlot(0) != nullptr, "old-file identity reconciliation still loads the owned real source PMD");
        const auto& loaded = *mdl::Mdl(app.ModelSlot(0)); Names(loaded, SourceLayout(fixture), 0, 1, fixture); Keys(loaded, fixture, false, mode == 0);
    }
    const auto reordered = directory / (stem + L"-reordered.pmd"); const auto decoy = directory / (stem + L"-decoy.pmd");
    WritePmd(reordered, ReorderedLayout(fixture)); WritePmd(decoy, {{{"decoy-bone", "decoy-bone-en"}}, {{"base", ""}, {"decoy-face", "decoy-face-en"}}});
    const auto reorderedBytes = EncodingFileBytes(reordered), decoyBytes = EncodingFileBytes(decoy);
    for (bool reselect : {false, true}) {
        const auto project = directory / (stem + (reselect ? L"-reselect.pmm" : L"-remap.pmm"));
        PathCopy(fixture.project, project, reselect ? decoy : reordered);
        { ExpectedDialogs expected(app, reselect, reordered); Load(app, project); expected.Finish(); }
        Check(app.ModelSlot(0) != nullptr && std::filesystem::equivalent(mdl::Mdl(app.ModelSlot(0))->path, reordered), "real PMM mapping/reselection uses the owned reordered PMD on disk");
        const auto& loaded = *mdl::Mdl(app.ModelSlot(0)); Names(loaded, ReorderedLayout(fixture), 2, 3, fixture); Keys(loaded, fixture, true);
    }
    Check(EncodingFileBytes(fixture.source) == fixture.original && EncodingFileBytes(reordered) == reorderedBytes && EncodingFileBytes(decoy) == decoyBytes, "legacy loading, remapping and actual reselection leave every source PMD byte unchanged");
    std::printf("PASS bounded old append, rejected partial/extra append, named remap and actual reselection fixture=%ls\n", fixture.source.filename().c_str());
}
void Run(MMDApp& app, const std::filesystem::path& directory) {
    std::array<Fixture, 2> fixtures{};
    fixtures[0].bone = std::string(20, 'B'); fixtures[0].morph = std::string(20, 'M');
    fixtures[0].boneText = std::wstring(20, L'B'); fixtures[0].morphText = std::wstring(20, L'M');
    for (unsigned i = 0; i < 10; ++i) { fixtures[1].bone += "\x82\xa0"; fixtures[1].morph += "\x82\xa2"; }
    fixtures[1].boneText = std::wstring(10, L'あ'); fixtures[1].morphText = std::wstring(10, L'い');
    // Both direct cases precede any compatibility fixture or dialog hook, so
    // old production code first fails on its actual standard-table over-read.
    for (std::size_t i = 0; i < fixtures.size(); ++i) {
        auto& fixture = fixtures[i]; const auto stem = i == 0 ? L"identity-ascii20" : L"identity-cp93220";
        fixture.source = directory / (std::wstring(stem) + L".pmd"); fixture.project = directory / (std::wstring(stem) + L".pmm"); fixture.seed = float(11 + i * 10);
        Direct(app, fixture);
    }
    for (const auto& fixture : fixtures) LegacyAndMapping(app, fixture, directory);
    ResetAppState(&app); std::puts("PASS real full-width PMD identity save, reopen, old-file bounds and v2 reconciliation");
}
}
#endif
