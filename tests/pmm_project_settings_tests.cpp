#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/extended_names.hpp"
#include "mikumikustudio/display_capacity.hpp"
#include "mikumikustudio/save_compatibility.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include <memory>
// MAINTENANCE: regression-keep; PMM settings, portable processor matching and unknown fields.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/pmm_project_settings.hpp"
#include "mikumikustudio/view_options.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/model.hpp"

using namespace mikumikustudio;
namespace ext = mikumikustudio::pmm_extension;
namespace settings = mikumikustudio::pmm_settings;
namespace {
bool expectedInvalidSave=false;
bool invalidSaveDialogHandled=false;
unsigned invalidSaveDialogs=0;
// Five tiers, stored as their own enum values.  The five menu ids are
// contiguous from kTransparentSortPrecisionMenuFirst: Low = 304 .. Maximum = 307
// and Off = kTransparentSortPrecisionMenuOff = 308.
constexpr int kTierCount = 5;

// The retired toggle key of the earlier six-tier test format.  The settings
// module no longer declares it, so this file spells the key out to place it in a
// fixture as an ordinary unknown record: the loader never looks it up, and a
// real writer carries it over byte for byte.
constexpr char kRetiredToggleKey[] = "org.mikudancestudio.view.transparent_sort";

// One label per tier, indexed by the stored TransparentSortPrecision value:
// Low, Standard, High, Maximum, Off.
struct TierLabel {
    const wchar_t* zh;
    const wchar_t* en;
    const wchar_t* jp;
};
constexpr TierLabel kTierLabels[] = {
    {L"低", L"Low", L"低"},
    {L"标准", L"Standard", L"標準"},
    {L"高", L"High", L"高"},
    {L"最高", L"Maximum", L"最高"},
    {L"关闭（默认）", L"Off (default)", L"オフ（既定）"},
};

void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}
void MatchTypes() {
    const std::vector<gpu::Adapter> devices{
        {123, L"Discrete vendor A", false}, {567, L"Integrated vendor B", true},
        {890, L"Integrated vendor C", true}};
    const auto integrated = settings::ResolveProcessor(settings::Processor::Integrated, devices);
    const auto discrete = settings::ResolveProcessor(settings::Processor::Discrete, devices);
    Check(integrated.adapterId == 567 && integrated.adapterClass == skinning::AdapterClass::Integrated,
          "integrated class selects local integrated adapter");
    Check(discrete.adapterId == 123 && discrete.adapterClass == skinning::AdapterClass::Discrete,
          "discrete class selects local discrete adapter");
    Check(settings::ResolveProcessor(settings::Processor::Integrated, {devices[0]}).mode == skinning::Mode::Automatic,
          "missing integrated class falls back to automatic");
    Check(settings::ResolveProcessor(settings::Processor::Discrete, {devices[1]}).mode == skinning::Mode::Automatic,
          "missing discrete class falls back to automatic");
    Check(settings::ResolveProcessor(settings::Processor::Automatic, {}).mode == skinning::Mode::Automatic,
          "automatic remains automatic");
    Check(settings::ResolveProcessor(settings::Processor::Cpu, devices).mode == skinning::Mode::Cpu,
          "CPU remains CPU");
    Check(settings::ClassifyProcessor(integrated, devices) == settings::Processor::Integrated &&
          settings::ClassifyProcessor(discrete, devices) == settings::Processor::Discrete,
          "explicit local adapter converts to a portable class");
    auto otherComputer = devices;
    otherComputer[1].id = 9999; otherComputer[1].name = L"Unrelated integrated GPU";
    Check(settings::ResolveProcessor(settings::Processor::Integrated, otherComputer).adapterId == 9999,
          "matching ignores vendor, model, LUID and original device order");
    std::puts("PASS portable processor matching including missing classes");
}
int Open(const std::filesystem::path& path, int mode) {
    int fd = -1;
    Check(_wsopen_s(&fd, path.c_str(), mode | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE) == 0,
          "open isolated PMM");
    return fd;
}
void Save(MMDApp& app, const std::filesystem::path& path) {
    wcscpy_s(app.EnvFileName(), 256, path.c_str());
    app.SceneModified() = 1;
    SaveSceneFile(&app);
    Check(app.SceneModified() == 0 && std::filesystem::file_size(path) > 32, "real PMM save succeeds");
}
void Load(MMDApp& app, const std::filesystem::path& path) {
    wcscpy_s(app.EnvFileName(), 256, path.c_str());
    LoadSceneFile();
}
ext::Document Read(const std::filesystem::path& path, std::uint64_t* boundary = nullptr) {
    int fd = Open(path, _O_RDONLY);
    char magic[30]{}; Check(_read(fd, magic, 30) == 30, "read PMM header");
    Check(std::memcmp(magic, "Polygon Movie maker 0002", 24) == 0, "PMM version remains 0002");
    ext::Document doc; std::uint64_t size = 0;
    Check(ext::Read(fd, doc, size) == ext::ReadResult::Loaded, "valid project extension");
    Check(_telli64(fd) == 30, "extension read restores file position");
    _close(fd); if (boundary) *boundary = size; return doc;
}
// The submenu is found by the command id of its first tier instead of by
// position, so no unrelated popup can satisfy the search.
HMENU PrecisionPopup(HWND hwnd) {
    HMENU view = GetSubMenu(GetMenu(hwnd), 2);
    if (view == nullptr) return nullptr;
    for (int pos = 0; pos < GetMenuItemCount(view); ++pos) {
        HMENU sub = GetSubMenu(view, pos);
        if (sub != nullptr &&
            GetMenuState(sub, kTransparentSortPrecisionMenuFirst, MF_BYCOMMAND) !=
                static_cast<UINT>(-1))
            return sub;
    }
    return nullptr;
}
bool TierChecked(HWND hwnd, int tier) {
    HMENU popup = PrecisionPopup(hwnd);
    return popup != nullptr &&
           (GetMenuState(popup, kTransparentSortPrecisionMenuFirst + tier, MF_BYCOMMAND) &
            MF_CHECKED) != 0;
}
std::wstring ItemLabel(HMENU popup, UINT id) {
    wchar_t buffer[128]{};
    const int length = GetMenuStringW(popup, id, buffer, 128, MF_BYCOMMAND);
    return length > 0 ? std::wstring(buffer) : std::wstring();
}
// A fixture places exact record bytes, including the retired toggle key, which
// no current writer produces on its own.  The standard PMM prefix of `source` is
// copied and the record stream is appended straight through the extension codec.
void WriteFixture(const std::filesystem::path& source, const std::filesystem::path& target,
                  std::uint64_t standardSize, const ext::Document& document) {
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing);
    const int fd = Open(target, _O_RDWR);
    Check(_chsize_s(fd, static_cast<__int64>(standardSize)) == 0,
          "trim the fixture to its standard PMM prefix");
    Check(_lseeki64(fd, 0, SEEK_END) == static_cast<__int64>(standardSize),
          "fixture positioned at the end of the standard data");
    Check(ext::Append(fd, document), "append the fixture through the extension codec");
    _close(fd);
}
ext::Document FixtureRecords(const std::vector<std::uint8_t>* toggle,
                             const std::vector<std::uint8_t>* precision) {
    ext::Document document;
    if (precision != nullptr) ext::Set(document, settings::kSortingPrecisionKey, *precision);
    if (toggle != nullptr) ext::Set(document, kRetiredToggleKey, *toggle);
    return document;
}

void AntialiasRoundtrip(MMDApp& app, const std::filesystem::path& directory) {
    using A = antialias::Algorithm;
    const auto source = directory / L"antialias-source.pmm";
    const auto fixture = directory / L"antialias-future.pmm";
    const auto saved = directory / L"antialias-saved.pmm";
    for (unsigned mode = 0; mode < static_cast<unsigned>(A::Count); ++mode) {
        settings::Reset(app);
        const auto last = mode >= unsigned(A::Smaa) ? static_cast<A>(mode) : A::SmaaT2xEnhanced;
        antialias::ApplyProjectSettings(&app, false, static_cast<A>(mode), last);
        settings::AntialiasChanged(app);
        Save(app, source);
        const auto document = Read(source);
        Check(ext::Find(document, settings::kAntialiasKey)->value ==
            std::vector<std::uint8_t>{0, std::uint8_t(mode), std::uint8_t(last)},
            "PMM stores enabled, algorithm and remembered SMAA in three stable bytes");
        antialias::ApplyProjectSettings(&app, false, A::MmdOriginal, A::Smaa);
        Load(app, source);
        Check(!app.antialiasing.enabled && app.antialiasing.algorithm == static_cast<A>(mode) &&
            app.antialiasing.lastSmaaAlgorithm == last, "all AA choices survive real PMM save/load");
    }
    antialias::ApplyProjectSettings(&app, true, A::Fxaa, A::Smaa);
    settings::AntialiasChanged(app); Save(app, source); Load(app, source);
    Check(app.antialiasing.enabled && app.antialiasing.algorithm == A::Fxaa,
        "PMM restores the enabled AA state");
    wchar_t preference[32768]{};
    GetEnvironmentVariableW(L"MMS_ANTIALIAS_CONFIG", preference, 32768);
    Check(GetPrivateProfileIntW(L"Antialias", L"Algorithm", 999, preference) == 4 &&
        GetPrivateProfileIntW(L"Antialias", L"Enabled", 999, preference) == 0,
        "loading and saving project AA does not overwrite machine defaults");

    // Hardware fallback is runtime state and must not rewrite the saved request.
    antialias::ApplyProjectSettings(&app, false, A::SmaaT2xEnhanced, A::SmaaT2xEnhanced);
    settings::AntialiasChanged(app); Save(app, source); Load(app, source);
    antialias::ApplyProjectSettings(&app, false, A::Fxaa, A::Smaa);
    Save(app, saved);
    Check(ext::Find(Read(saved), settings::kAntialiasKey)->value ==
        std::vector<std::uint8_t>{0, 9, 9}, "temporary fallback retains the project's requested mode");

    std::uint64_t boundary = 0; auto document = Read(source, &boundary);
    for (const std::vector<std::uint8_t>& future : {
        std::vector<std::uint8_t>{1,250,9}, {1,9,99}, {1,9}, {2,9,9}}) {
        ext::Set(document, settings::kAntialiasKey, future);
        WriteFixture(source, fixture, boundary, document);
        Load(app, fixture); Save(app, saved);
        Check(ext::Find(Read(saved), settings::kAntialiasKey)->value == future,
            "unknown AA values and lengths pass through unchanged");
    }
    std::wstring error;
    Check(antialias::Select(&app, A::SmaaStandard, error), "explicit user AA selection succeeds");
    Save(app, saved);
    Check(ext::Find(Read(saved), settings::kAntialiasKey)->value ==
        std::vector<std::uint8_t>{0,7,7}, "explicit selection replaces only the unknown AA record");
    std::puts("PASS PMM anti-aliasing choices, local defaults, unknown data and fallback preservation");
}
}

void LegacyBindingMetadata() {
    auto app=std::make_unique<MMDApp>();auto model=std::make_unique<mdl::ModelRecord>();
    std::memset(&app->state,0,sizeof app->state);
    std::array<mdl::BoneRecord,2> bones{};std::array<mdl::MorphRecord,2> morphs{};
    model->boneTable=bones.data();model->boneCount=1;model->morphs=morphs.data();model->morphCount=1;
    std::strcpy(model->name,"Ronova");std::strcpy(model->nameEn,"New MMD Model");
    app->ModelSlot(0)=reinterpret_cast<unsigned char*>(model.get());
    const auto assign=[](auto& record,const wchar_t* name) {
        Check(extended_names::Assign(record.jpText,name),"allocate complete binding name");
        std::string encoded;
        Check(text_encoding::EncodeExact(name,932,encoded),"encode sample binding name without substitutions");
        std::memset(record.name,0,sizeof record.name);
        std::memcpy(record.name,encoded.data(),(std::min)(encoded.size(),sizeof(record.name)-1));
    };
    const auto expect=[&](bool extended,const char* message) {
        const auto report=save_compatibility::InspectPmm(*app);
        Check(!report.needsNewPmm&&report.namesNeedExtension==extended,message);
    };
    assign(bones[0],L"+FlycloakRootB CB A01");assign(morphs[0],L"face");
    Check(std::strcmp(bones[0].name,"+FlycloakRootB CB A")==0,"Ronova sample uses its original nineteen-byte binding");
    expect(false,"ordinary long ASCII bone identity does not require a compatibility copy");
    Check(save_compatibility::InspectMotion(*model).namesNeedExtension,
          "accepting a PMM binding does not relax VMD name limits");
    Check(!save_compatibility::CanSavePmd(*model),"accepting a PMM binding does not relax PMD name limits");
    ext::Document document;extended_names::CapturePmm(*app,document);
    Check(extended_names::Assign(bones[0].jpText,L"changed"),"alter full name before extension restore");
    extended_names::LoadPmm(*app,document);
    Check(text_encoding::BoneName(bones[0],false)==L"+FlycloakRootB CB A01",
          "complete source name remains in PMM metadata without a compatibility warning");
    for(const wchar_t* name : {L"1234567890123456789",L"12345678901234567890",L"あいうえおABCDEFGHIJK"}) {
        assign(bones[0],name);expect(false,"exact and canonical truncated CP932 bone bindings remain compatible");
        assign(morphs[0],name);expect(false,"exact and canonical truncated CP932 morph bindings remain compatible");
    }
    assign(bones[0],L"abcdefghijklmnopqrあ");expect(false,"a split 19-byte CP932 mirror still binds to the same model bytes");
    assign(bones[0],L"+FlycloakRootB CB A01");
    std::strcpy(bones[0].name,"+FlycloakRootB");expect(true,"an arbitrary shorter alias still requires an extension");
    assign(bones[0],L"+FlycloakRootB CB A01");
    Check(extended_names::Assign(bones[0].jpText,L"骨骼\U0001f31f"),"allocate non-CP932 binding name");
    expect(true,"Unicode replacement aliases still require an extension");
    assign(bones[0],L"+FlycloakRootB CB A01");assign(bones[1],L"+FlycloakRootB CB A02");model->boneCount=2;
    expect(true,"distinct bones with colliding canonical short names still require an extension");model->boneCount=1;
    assign(morphs[0],L"+FlycloakRootB CB A01");assign(morphs[1],L"+FlycloakRootB CB A02");model->morphCount=2;
    expect(true,"distinct morphs with colliding canonical short names still require an extension");
    app->ModelSlot(0)=nullptr;
    for(auto* p:model->pmxTextBuffers)std::free(p);
    for(auto& bone:bones){std::free(bone.jpText);std::free(bone.enText);}
    for(auto& morph:morphs){std::free(morph.jpText);std::free(morph.enText);}
    std::puts("PASS PMM canonical legacy binding names, collisions, and unchanged PMD/VMD limits");
}

// ---- real-PMX name and PMM version boundary -------------------------------
// The supplied RNW_Test.pmm contains a model bone whose full CP932 name is
// "+FlycloakRootB CB A01" (21 bytes) while the PMM stores its 19-byte mirror
// "+FlycloakRootB CB A". The old compatibility check treated every such
// truncation as unrepresentable. This fixture drives the real PMX loader so
// the mirror, the extension decision and the v3 boundary are checked against
// the bytes the loader actually produces, not a hand-written record.
template<class T> void PutPmx(std::ofstream& file, const T& value) {
    file.write(reinterpret_cast<const char*>(&value), sizeof value);
}
void PutPmxText(std::ofstream& file, const std::wstring& value) {
    PutPmx(file, static_cast<std::uint32_t>(value.size() * sizeof(wchar_t)));
    file.write(reinterpret_cast<const char*>(value.data()),
               static_cast<std::streamsize>(value.size() * sizeof(wchar_t)));
}
void WritePmmNameBoundaryPmx(const std::filesystem::path& path,
                           const std::vector<std::wstring>& names = {
                               L"1234567890123456789", L"+FlycloakRootB CB A01",
                               L"あいうえおかきくけこ", L"bone\U0001f600",
                               L"CollisionPrefixABCDEFG1", L"CollisionPrefixABCDEFG2"}) {
    std::ofstream file(path, std::ios::binary);
    file.write("PMX ", 4);
    PutPmx(file, 2.0f);
    const unsigned char header[8] = {0, 0, 1, 1, 1, 1, 1, 1};  // UTF-16, 1-byte indices
    PutPmx(file, static_cast<unsigned char>(sizeof header));
    file.write(reinterpret_cast<const char*>(header), sizeof header);
    PutPmxText(file, L"Boundary"); PutPmxText(file, L"Boundary");
    PutPmxText(file, L""); PutPmxText(file, L"");
    PutPmx(file, std::int32_t(0));  // texture table
    PutPmx(file, std::int32_t(0));  // vertices
    PutPmx(file, std::int32_t(0));  // surface indices
    PutPmx(file, std::int32_t(0));  // materials
    PutPmx(file, static_cast<std::int32_t>(names.size()));  // bones
    for (std::size_t i = 0; i < names.size(); ++i) {
        PutPmxText(file, names[i]);
        PutPmxText(file, L"en");
        PutPmx(file, float(i)); PutPmx(file, 0.0f); PutPmx(file, 0.0f);
        PutPmx(file, static_cast<signed char>(-1));  // parent
        PutPmx(file, std::int32_t(0));               // layer
        PutPmx(file, std::uint16_t(0));              // flags
        PutPmx(file, 0.0f); PutPmx(file, 1.0f); PutPmx(file, 0.0f);  // tail offset
    }
    PutPmx(file, std::int32_t(0));  // morphs
    PutPmx(file, std::int32_t(1));  // display frames
    PutPmxText(file, L"Root"); PutPmxText(file, L"Root");
    PutPmx(file, static_cast<unsigned char>(0));
    PutPmx(file, static_cast<std::int32_t>(names.size()));
    for (std::size_t i = 0; i < names.size(); ++i) {
        PutPmx(file, static_cast<unsigned char>(0));  // bone element
        PutPmx(file, static_cast<signed char>(i));
    }
    PutPmx(file, std::int32_t(0));  // rigid bodies
    PutPmx(file, std::int32_t(0));  // joints
    Check(bool(file), "write bones-only PMX boundary fixture");
}
void MeasuredNameBoundariesRoundtrip(MMDApp& app, const std::filesystem::path& directory) {
    // These input names match the native MMD measurements documented in
    // docs/reference/legacy-mmd-limits.md. Exercise MMS's actual save/load path:
    // the VMD 15-byte limit must not force a PMM v3 header or lose bone keys.
    const std::vector<std::wstring> names{
        std::wstring(15, L'B'), std::wstring(16, L'B'), std::wstring(19, L'B'),
        std::wstring(20, L'B'), std::wstring(21, L'B'),
        std::wstring(7, L'あ') + L"A", std::wstring(8, L'あ'),
        std::wstring(9, L'あ') + L"A", std::wstring(10, L'あ'), L"骨\U0001f31f"};
    for (std::size_t i = 0; i < names.size(); ++i) {
        ResetAppState(&app);
        const auto stem = L"measured-name-" + std::to_wstring(i);
        const auto pmx = directory / (stem + L".pmx");
        const auto pmm = directory / (stem + L".pmm");
        WritePmmNameBoundaryPmx(pmx, {names[i]});
        LoadModelFile(&app, pmx.c_str(), false);
        Check(app.ModelSlot(0) != nullptr, "load a measured name through the production PMX loader");
        auto& model = *mdl::Mdl(app.ModelSlot(0));
        Check(model.boneCount == 1, "measured-name fixture has one bone");
        const auto report = save_compatibility::InspectPmm(app);
        Check(!report.invalidData && !report.needsNewPmm,
              "measured names alone never upgrade PMM to v3");
        Check(report.namesNeedExtension == (i == names.size() - 1),
              "canonical CP932 names need no warning; a Unicode alias still does");
        const std::string mirror(model.boneTable[0].name);
        model.boneKeys[0].position[0] = float(i + 1);
        Check(SaveSceneCopy(&app, pmm.c_str()), "save the measured-name project");
        Read(pmm);  // Checks the actual 0002 header and extension integrity.
        Load(app, pmm);
        Check(app.ModelSlot(0) != nullptr, "reload the measured-name PMM");
        const auto& restored = *mdl::Mdl(app.ModelSlot(0));
        Check(restored.boneCount == 1 &&
              text_encoding::BoneName(restored.boneTable[0], false) == names[i] &&
              mirror == restored.boneTable[0].name &&
              restored.boneKeys[0].position[0] == float(i + 1),
              "PMM v2 roundtrip retains the full name, binding mirror and keyed value");
    }
    ResetAppState(&app);
    std::puts("PASS ten measured name cases keep PMM v2 and survive actual MMS save/load");
}
void PmxNameBoundaries(MMDApp& app, const std::filesystem::path& directory) {
    const auto pmx = directory / L"boundary-bones.pmx";
    WritePmmNameBoundaryPmx(pmx);
    LoadModelFile(&app, pmx.c_str(), false);
    auto* raw = app.ModelSlot(0);
    Check(raw != nullptr, "load the boundary PMX through the real PMX loader");
    auto& model = *mdl::Mdl(raw);
    Check(model.boneCount == 6, "boundary PMX supplies six bones");
    Check(std::strcmp(model.boneTable[0].name, "1234567890123456789") == 0,
          "a 19-byte CP932 name is stored unchanged");
    Check(std::strcmp(model.boneTable[1].name, "+FlycloakRootB CB A") == 0,
          "a 21-byte name keeps the 19-byte mirror used by the supplied PMM");
    std::string jp;
    Check(text_encoding::EncodeExact(L"あいうえおかきくけこ", 932, jp) && jp.size() == 20,
          "encode the 20-byte Japanese boundary name");
    Check(std::memcmp(model.boneTable[2].name, jp.data(), 19) == 0 &&
          model.boneTable[2].name[19] == '\0',
          "the loader keeps the 19-byte mirror when it splits a CP932 character");
    Check(text_encoding::BoneName(model.boneTable[2], false) == L"あいうえおかきくけこ",
          "the complete source name remains available beside the split mirror");

    const auto inspect = [&] { return save_compatibility::InspectPmm(app); };
    model.boneCount = 2;
    auto report = inspect();
    Check(!report.invalidData && !report.namesNeedExtension && !report.needsNewPmm,
          "19-byte and canonical 21-byte mirrors need no extension and select PMM v2");
    Check(report.canCopyLegacy, "canonical ASCII mirrors remain copyable");
    model.boneCount = 3;
    report = inspect();
    Check(!report.namesNeedExtension && !report.needsNewPmm,
          "a split 19-byte CP932 mirror still binds and needs no extension");
    model.boneCount = 4;
    report = inspect();
    Check(report.namesNeedExtension && !report.needsNewPmm,
          "a non-CP932 source name needs the extension but never selects v3");
    model.boneCount = 6;
    report = inspect();
    Check(report.namesNeedExtension && !report.needsNewPmm && !report.canCopyLegacy,
          "distinct full names sharing one mirror need the extension and cannot be copied");
    model.boneCount = 2;
    const auto canonical = directory / L"boundary-canonical.pmm";
    Check(SaveSceneCopy(&app, canonical.c_str()), "save the canonical boundary project");
    int fd = Open(canonical, _O_RDONLY);
    char header30[30]{};
    Check(_read(fd, header30, 30) == 30, "read the canonical PMM header");
    _close(fd);
    Check(std::memcmp(header30, "Polygon Movie maker 0002", 24) == 0,
          "canonical model names keep the PMM 0002 header");

    model.boneCount = 0;
    model.maxFrame = frame_limits::legacyMaximum;
    Check(!inspect().needsNewPmm && !inspect().invalidData,
          "the exact legacy frame still selects PMM v2");
    model.maxFrame = frame_limits::legacyMaximum + 1;
    Check(inspect().needsNewPmm, "one frame beyond the legacy maximum selects PMM v3");
    model.maxFrame = 0;
    mdl::SetDisplayCapacity(model, 255, 255);
    Check(!inspect().needsNewPmm, "255 display groups and facial rows select PMM v2");
    mdl::SetDisplayCapacity(model, 256, 255);
    Check(inspect().needsNewPmm, "256 display groups select PMM v3");
    mdl::SetDisplayCapacity(model, 255, 256);
    Check(inspect().needsNewPmm, "256 facial rows select PMM v3");
    mdl::SetDisplayCapacity(model, 0, 0);
    mdl::ForgetDisplayCapacity(model);
    wcscpy_s(model.path, L"D:\\model-\U0001f31f.pmx");
    Check(inspect().needsNewPmm, "a non-CP932 model path selects PMM v3");
    std::puts("PASS real PMX name mirrors and the strict PMM v3 selection boundary");
}

void AutomaticMetadata() {
    auto app=std::make_unique<MMDApp>();auto model=std::make_unique<mdl::ModelRecord>();
    std::memset(&app->state,0,sizeof app->state);
    std::array<mdl::BoneRecord,2> bones{};std::array<mdl::MorphRecord,1> morphs{};
    std::vector<mdl::DisplayGroup> groups(300);
    model->boneTable=bones.data();model->boneCount=2;model->morphs=morphs.data();model->morphCount=1;model->groupNames=groups.data();
    app->ModelSlot(0)=reinterpret_cast<unsigned char*>(model.get());
    mdl::SetDisplayCapacity(*model,300,512);groups[299].flags=1;
    const std::wstring first=L"共同前缀_超过十四字符_完整左名称_\U0001f31f",second=L"共同前缀_超过十四字符_完整右名称_\U0001f31f";
    Check(extended_names::Assign(bones[0].jpText,first)&&extended_names::Assign(bones[1].jpText,second),"allocate Unicode model metadata");
    std::strcpy(bones[0].name,"boneA");std::strcpy(bones[1].name,"boneB");std::strcpy(morphs[0].name,"face");
    text_encoding::SetDisplayGroupName(*model,groups[299],L"最后一个分组的完整名称",L"Last complete display group");
    ext::Document document;extended_names::CapturePmm(*app,document);
    Check(extended_names::Assign(bones[0].jpText,L"changed"),"change metadata before restore");groups[299].flags=0;
    extended_names::LoadPmm(*app,document);
    Check(text_encoding::BoneName(bones[0],false)==first&&text_encoding::BoneName(bones[1],false)==second,"PMM metadata restores distinct complete Unicode names");
    Check(mdl::DisplayGroupCount(*model)==300&&mdl::FacialFrameCount(*model)==512&&groups[299].flags==1,"expanded display counts and final group state survive PMM metadata");
    Check(save_compatibility::InspectPmm(*app).needsNewPmm,"group capacity beyond original PMM selects the new version");
    mdl::SetDisplayCapacity(*model,1,1);
    Check(extended_names::Assign(bones[0].jpText,L"boneA")&&extended_names::Assign(bones[1].jpText,L"boneB"),"restore exact legacy binding names");
    Check(extended_names::Assign(bones[0].enText,L"An English display label far beyond nineteen bytes"),"set a long English caption");
    Check(extended_names::Assign(model->pmxTextBuffers[0],L"A model caption longer than twenty bytes"),"set a length-prefixed PMM model caption");
    const auto ordinary=save_compatibility::InspectPmm(*app);
    Check(!ordinary.needsNewPmm&&!ordinary.namesNeedExtension,"English captions and display-group metadata do not trigger a PMM compatibility warning");
    Check(extended_names::Assign(bones[0].jpText,first),"restore a Unicode binding name");
    Check(save_compatibility::InspectPmm(*app).namesNeedExtension,"a real binding alias still requires Unicode extension data");
    app->ModelSlot(0)=nullptr;
    auto*& camera=app->CameraKeys();camera=mdl::KeyPoolCreate<mdl::CameraKey>();
    Check(!save_compatibility::InspectPmm(*app).needsNewPmm,"ordinary project prefers original PMM");
    Check(mdl::KeyPoolEnsure(camera,10001),"grow camera pool for format selection");
    Check(!save_compatibility::InspectPmm(*app).needsNewPmm,"unused expanded capacity does not force a new PMM");
    camera[0].next=10000;camera[10000].frame=21;camera[10000].previous=0;
    Check(!save_compatibility::InspectPmm(*app).needsNewPmm,"sparse high slots still fit legacy PMM after save compaction");
    for (unsigned i=1;i<10001;++i) camera[i].frame=i;
    Check(save_compatibility::InspectPmm(*app).needsNewPmm,"only actual live count beyond old capacity selects new PMM");
    mdl::KeyPoolDestroy(camera);
    mdl::ForgetDisplayCapacity(*model);text_encoding::ForgetDisplayGroupNames(*model);
    for(auto* p:model->pmxTextBuffers)std::free(p);
    for(auto& bone:bones){std::free(bone.jpText);std::free(bone.enText);}
    for(auto& morph:morphs){std::free(morph.jpText);std::free(morph.enText);}
    std::puts("PASS automatic Unicode metadata, display counts and legacy-first format selection");
}

template<class K> void TrackFormatBoundaries(MMDApp& app,K*& track,std::size_t original) {
    K* saved=track;
    if constexpr(std::is_same_v<K,mdl::BoneKey>)track=mdl::BoneKeyPoolCreate(original+1);
    else if constexpr(std::is_same_v<K,mdl::MorphKey>)track=mdl::MorphKeyPoolCreate(original+1);
    else track=mdl::KeyPoolCreate<K>(original+1);
    const auto expect=[&](bool extended,const char* message) {
        const auto report=save_compatibility::InspectPmm(app);
        Check(!report.invalidData && report.needsNewPmm==extended,message);
    };
    expect(false,"allocated capacity alone never selects v3");
    for(std::size_t i=0;i<original;++i){auto& k=track[i];k.frame=static_cast<std::uint32_t>(i);k.previous=i?static_cast<std::uint32_t>(i-1):0;k.next=i+1<original?static_cast<std::uint32_t>(i+1):0;}
    expect(false,"exactly the legacy live-key capacity still selects v2");
    track[original-1].next=static_cast<std::uint32_t>(original);
    track[original].frame=static_cast<std::uint32_t>(original);
    track[original].previous=static_cast<std::uint32_t>(original-1);
    expect(true,"one live key beyond the legacy limit selects v3");
    track[original].next=static_cast<std::uint32_t>(original+1);
    Check(save_compatibility::InspectPmm(app).invalidData,"validation continues after the legacy capacity is exceeded");
    std::memset(track,0,(original+1)*sizeof(K));
    track[0].next=static_cast<std::uint32_t>(original);
    track[original].frame=frame_limits::legacyMaximum;
    expect(false,"sparse high slot and exact legacy maximum frame select v2");
    ++track[original].frame;
    expect(true,"first extended frame selects v3");
    track[original].frame=frame_limits::maximum;
    expect(true,"largest supported frame selects v3");
    ++track[original].frame;
    const auto invalid=save_compatibility::InspectPmm(app);
    Check(invalid.invalidData&&!invalid.canCopyLegacy&&!invalid.reason.empty(),"unsupported frame cannot be saved using either PMM version");
    track[original].frame=21;
    track[original].previous=static_cast<std::uint32_t>(original+1);
    Check(save_compatibility::InspectPmm(app).invalidData,"out-of-range previous link is invalid");
    track[original].previous=1;
    Check(save_compatibility::InspectPmm(app).invalidData,"link to an empty slot is invalid");
    track[original].previous=0;
    expect(false,"repairing the link restores v2 eligibility");
    track[1].next=static_cast<std::uint32_t>(original+100);
    expect(false,"stale links in unused slots are not serialized or rejected");
    if constexpr(std::is_same_v<K,mdl::BoneKey>)mdl::BoneKeyPoolDestroy(track);
    else if constexpr(std::is_same_v<K,mdl::MorphKey>)mdl::MorphKeyPoolDestroy(track);
    else mdl::KeyPoolDestroy(track);
    track=saved;
}

void FormatTriggerBoundaries(MMDApp& app) {
    const auto expect=[&](bool extended,const char* message) {
        const auto report=save_compatibility::InspectPmm(app);
        Check(!report.invalidData&&report.needsNewPmm==extended,message);
    };
    expect(false,"initialized production scene is valid legacy PMM");
    TrackFormatBoundaries(app,app.CameraKeys(),10000);
    TrackFormatBoundaries(app,app.LightKeys(),10000);
    TrackFormatBoundaries(app,app.ShadowKeys(),10000);
    TrackFormatBoundaries(app,app.GravityKeys(),10000);
    auto model=std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord,1> bones{};std::array<mdl::MorphRecord,1> morphs{};
    std::array<mdl::DisplayGroup,256> groups{};
    std::strcpy(bones[0].name,"bone");std::strcpy(morphs[0].name,"face");
    model->boneTable=bones.data();model->boneCount=1;model->morphs=morphs.data();model->morphCount=1;
    model->boneKeys=mdl::BoneKeyPoolCreate(1);model->morphKeys=mdl::MorphKeyPoolCreate(1);
    model->displayKeys=mdl::KeyPoolCreate<mdl::DisplayKey>(1);model->groupNames=groups.data();
    Check(!app.ModelSlot(254),"format audit model slot is free");
    app.ModelSlot(254)=reinterpret_cast<unsigned char*>(model.get());
    TrackFormatBoundaries(app,model->boneKeys,mdl::kBoneKeyCapacity);
    TrackFormatBoundaries(app,model->morphKeys,mdl::kMorphKeyCapacity);
    TrackFormatBoundaries(app,model->displayKeys,mdl::kDisplayKeyCapacity);
    model->maxFrame=frame_limits::legacyMaximum;
    expect(false,"model timeline at legacy maximum selects v2");
    ++model->maxFrame;
    expect(true,"persisted model timeline beyond legacy maximum selects v3");
    model->maxFrame=0;
    mdl::SetDisplayCapacity(*model,255,255);expect(false,"255 display groups and facial rows still select v2");
    mdl::SetDisplayCapacity(*model,256,255);expect(true,"256 display groups select v3");
    mdl::SetDisplayCapacity(*model,255,256);expect(true,"256 facial rows select v3");
    mdl::SetDisplayCapacity(*model,0,0);
    wcscpy_s(model->path,L"D:\\model.pmx");expect(false,"legacy reference path selects v2");
    wcscpy_s(model->path,L"D:\\model-\U0001f31f.pmx");expect(true,"non-CP932 model reference selects v3");
    app.ModelSlot(254)=nullptr;
    mdl::ForgetDisplayCapacity(*model);
    mdl::BoneKeyPoolDestroy(model->boneKeys);mdl::MorphKeyPoolDestroy(model->morphKeys);mdl::KeyPoolDestroy(model->displayKeys);
    auto accessory=std::make_unique<mdl::AccessoryRecord>();
    Check(!app.AccessorySlot(254),"format audit accessory slot is free");
    auto* savedAccessoryKeys=app.AccessoryKeys(254);
    app.AccessorySlot(254)=accessory.get();app.AccessoryKeys(254)=mdl::KeyPoolCreate<mdl::AccessoryKey>(1);
    TrackFormatBoundaries(app,app.AccessoryKeys(254),10000);
    wcscpy_s(accessory->sourcePath,L"D:\\accessory-\U0001f31f.x");expect(true,"non-CP932 accessory reference selects v3");
    mdl::KeyPoolDestroy(app.AccessoryKeys(254));app.AccessorySlot(254)=nullptr;app.AccessoryKeys(254)=savedAccessoryKeys;
    for(auto* path:{app.WavePath(),app.AviBackgroundPath(),app.PictureBackgroundPath()}) {
        const std::wstring saved=path;
        wcscpy_s(path,256,L"D:\\media-\U0001f31f.png");expect(true,"each non-CP932 media reference selects v3");
        wcscpy_s(path,256,saved.c_str());
    }
    for(auto* frame:{&app.CurrentFrame(),&app.TimelineStartFrame()}) {
        const auto saved=*frame;
        *frame=frame_limits::legacyMaximum;expect(false,"exact legacy UI frame selects v2");
        ++*frame;expect(true,"first extended UI frame selects v3");
        *frame=-1;Check(save_compatibility::InspectPmm(app).invalidData,"negative UI frame is invalid");
        *frame=saved;
    }
    const auto last=app.LastRegisteredFrame();
    app.LastRegisteredFrame()=frame_limits::legacyMaximum;expect(false,"exact legacy last frame selects v2");
    ++app.LastRegisteredFrame();expect(true,"extended last frame selects v3");app.LastRegisteredFrame()=last;
    for(auto control:{panel::kPlayStartFrameEdit,panel::kPlayStopFrameEdit}) {
        char saved[256]{};GetDlgItemTextA(app.MainWindow(),control,saved,sizeof saved);
        SetDlgItemTextA(app.MainWindow(),control,"10000000");expect(false,"exact legacy playback bound selects v2");
        SetDlgItemTextA(app.MainWindow(),control,"10000001");expect(true,"extended playback bound selects v3");
        SetDlgItemTextA(app.MainWindow(),control,"2147483647");Check(save_compatibility::InspectPmm(app).invalidData,"unsupported playback bound is rejected before writing");
        SetDlgItemTextA(app.MainWindow(),control," ");expect(false,"blank playback range retains native zero-default behavior");
        SetDlgItemTextA(app.MainWindow(),control,saved);
    }
    expect(false,"all temporary extended data removed restores v2 eligibility");
    std::puts("PASS PMM format and invalid-data boundaries for all eight key pools, frames, counts and reference paths");
}

void InvalidFormatEscalation(MMDApp& app,const std::filesystem::path& directory) {
    auto* keys=app.CameraKeys();const auto head=keys[0];
    keys[0].next=static_cast<std::uint32_t>(mdl::KeyPoolCapacity(keys));
    const auto path=directory/L"invalid-links-must-not-become-v3.pmm";
    Check(!SaveSceneCopy(&app,path.c_str()),"corrupt links are refused rather than saved as PMM v3");
    Check(!std::filesystem::exists(path),"invalid data creates no PMM file");
    const auto existing=directory/L"invalid-links-preserve-existing.pmm";
    const char marker[]="existing PMM must remain untouched";
    int fd=Open(existing,_O_CREAT|_O_EXCL|_O_WRONLY);
    Check(_write(fd,marker,sizeof marker)==sizeof marker,"create existing-file preservation fixture");_close(fd);
    const std::wstring savedName=app.EnvFileName();const auto language=app.EnglishUI();
    wcscpy_s(app.EnvFileName(),256,existing.c_str());
    const auto timer=SetTimer(nullptr,0,20,[](HWND,UINT,UINT_PTR,DWORD) {
        if(!expectedInvalidSave||invalidSaveDialogHandled)return;
        EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM)->BOOL {
            wchar_t cls[32]{};GetClassNameW(window,cls,32);
            if(wcscmp(cls,L"#32770"))return TRUE;
            bool matched=false;
            EnumChildWindows(window,[](HWND child,LPARAM data)->BOOL {
                wchar_t text[1024]{};GetWindowTextW(child,text,1024);
                if(wcsstr(text,L"PMM v3"))*reinterpret_cast<bool*>(data)=true;
                return TRUE;
            },reinterpret_cast<LPARAM>(&matched));
            Check(matched,"expected invalid-data save warning");
            invalidSaveDialogHandled=true;++invalidSaveDialogs;
            PostMessageW(window,WM_CLOSE,0,0);return FALSE;
        },0);
    });
    Check(timer!=0,"install hidden save-warning dismissal timer");
    for(unsigned lang=0;lang<3;++lang) {
        app.EnglishUI()=lang;app.SceneModified()=1;
        invalidSaveDialogHandled=false;
        expectedInvalidSave=true;SaveSceneFile(&app);expectedInvalidSave=false;
        Check(invalidSaveDialogs==lang+1&&app.SceneModified()==1,"normal save reports invalid data in each language and retains modified state");
        fd=Open(existing,_O_RDONLY);char actual[sizeof marker]{};
        Check(_read(fd,actual,sizeof actual)==sizeof marker&&std::memcmp(actual,marker,sizeof marker)==0&&_filelengthi64(fd)==sizeof marker,
              "normal save never replaces an existing project with invalid data");_close(fd);
    }
    KillTimer(nullptr,timer);
    wcscpy_s(app.EnvFileName(),256,savedName.c_str());app.EnglishUI()=language;
    keys[0]=head;
    std::puts("PASS invalid links refuse backup and normal PMM saves, preserve existing files and report errors in all three languages");
}

void AutomaticCameraRoundtrip(MMDApp& app, const std::filesystem::path& directory) {
    const auto compact = directory / L"automatic-compact.pmm";
    const auto expanded = directory / L"automatic-expanded.pmm";
    Check(mdl::KeyPoolEnsure(app.CameraKeys(),10001), "reserve expanded camera track");
    app.CameraKeys()[0].next=10000;
    app.CameraKeys()[10000].frame=23;
    app.CameraKeys()[10000].distance=-83;
    Check(SaveSceneCopy(&app,compact.c_str()), "save sparse pool using legacy format");
    int fd=Open(compact,_O_RDONLY);char header[30]{};Check(_read(fd,header,30)==30,"read compact header");_close(fd);
    Check(std::memcmp(header,"Polygon Movie maker 0002",24)==0,"sparse high slots save as original PMM");
    Check(app.CameraKeys()[0].next==10000,"save compaction does not alter live indices");
    Load(app,compact);
    const auto slot=app.CameraKeys()[0].next;
    Check(slot>0&&slot<10000&&app.CameraKeys()[slot].frame==23&&app.CameraKeys()[slot].distance==-83,
          "compact PMM roundtrip retains links and values");
    Check(mdl::KeyPoolEnsure(app.CameraKeys(),10001),"reserve fully occupied camera track");
    for(unsigned i=0;i<10001;++i){auto& k=app.CameraKeys()[i];k.frame=i;k.previous=i?i-1:0;k.next=i<10000?i+1:0;k.distance=-float(i);}
    Check(SaveSceneCopy(&app,expanded.c_str()),"save actual expanded camera content");
    fd=Open(expanded,_O_RDONLY);Check(_read(fd,header,30)==30,"read expanded header");_close(fd);
    Check(std::memcmp(header,"Polygon Movie maker 0003",24)==0,"actual key overflow selects new PMM");
    Load(app,expanded);
    Check(mdl::KeyPoolCapacity(app.CameraKeys())>10000&&app.CameraKeys()[9999].next==10000&&
          app.CameraKeys()[10000].previous==9999&&app.CameraKeys()[10000].frame==10000&&app.CameraKeys()[10000].distance==-10000,
          "new PMM roundtrip preserves the key beyond the original camera limit");
    app.CameraKeys()[9999].next=0;app.CameraKeys()[10000]={};
    const auto reduced=directory/L"automatic-reduced.pmm";
    Check(SaveSceneCopy(&app,reduced.c_str()),"save formerly expanded track after deleting excess keys");
    fd=Open(reduced,_O_RDONLY);Check(_read(fd,header,30)==30,"read reduced header");_close(fd);
    Check(std::memcmp(header,"Polygon Movie maker 0002",24)==0,"v3 history does not prevent saving reduced content as v2");
    Load(app,reduced);
    Check(app.CameraKeys()[9998].next==9999&&app.CameraKeys()[9999].previous==9998&&app.CameraKeys()[9999].next==0,
          "reduced v2 roundtrip retains every remaining camera link");
    ResetAppState(&app);
    const auto longFrame = directory / L"automatic-large-frame.pmm";
    app.CurrentFrame()=frame_limits::maximum-20;
    SetDlgItemTextA(app.MainWindow(),panel::kPlayStartFrameEdit,"10000001");
    SetDlgItemTextA(app.MainWindow(),panel::kPlayStopFrameEdit,"2147483646");
    Check(SaveSceneCopy(&app,longFrame.c_str()),"save extended UI frame range");
    fd=Open(longFrame,_O_RDONLY);Check(_read(fd,header,30)==30,"read large-frame header");_close(fd);
    Check(std::memcmp(header,"Polygon Movie maker 0003",24)==0,"large UI frame range selects extended PMM");
    Load(app,longFrame);
    char range[32]{};GetDlgItemTextA(app.MainWindow(),panel::kPlayStopFrameEdit,range,sizeof range);
    Check(app.CurrentFrame()==frame_limits::maximum-20 && std::string(range)=="2147483646",
          "extended frame range roundtrips without digit truncation");
    std::wstring copy1,copy2;
    Check(save_compatibility::CopyPrefix(compact.c_str(),30,copy1) && save_compatibility::CopyPrefix(compact.c_str(),30,copy2) && copy1!=copy2,
          "compatible copies use distinct names without overwriting");
    Check(std::filesystem::file_size(copy1)==30 && std::filesystem::file_size(copy2)==30,"compatible copy contains only requested legacy prefix");
    ResetAppState(&app);
    SetDlgItemTextA(app.MainWindow(),panel::kPlayStartFrameEdit,"0");
    SetDlgItemTextA(app.MainWindow(),panel::kPlayStopFrameEdit,"0");
    std::puts("PASS real legacy-first and expanded PMM camera roundtrip");
}

int wmain(int argc, wchar_t** argv) {
    if(argc==2 && wcscmp(argv[1],L"--metadata-only")==0) {
        LegacyBindingMetadata();AutomaticMetadata();return 0;
    }
    if(argc==3 && wcscmp(argv[1],L"--pmx-boundary")==0) {
        const auto directory=std::filesystem::absolute(argv[2]);
        std::filesystem::create_directories(directory);
        std::setvbuf(stdout,nullptr,_IONBF,0);
        const auto aaPreference=directory/L"antialias-user.ini";
        SetEnvironmentVariableW(L"MMS_ANTIALIAS_CONFIG",aaPreference.c_str());
        SetEnvironmentVariableW(L"MMS_NVIDIA_AA_FIX",L"0");
        WritePrivateProfileStringW(L"Antialias",L"Algorithm",L"4",aaPreference.c_str());
        WritePrivateProfileStringW(L"Antialias",L"Enabled",L"0",aaPreference.c_str());
        WritePrivateProfileStringW(L"Antialias",L"LastSmaaAlgorithm",L"6",aaPreference.c_str());
        auto* app=new MMDApp();g_Block=app;
        app->state=MMDAppState{};app->InitDefaults();
        Check(InitMainWindowAndD3D(app,GetModuleHandleW(nullptr),SW_HIDE),"initialize hidden production app");
        ShowWindow(app->MainWindow(),SW_HIDE);
        MeasuredNameBoundariesRoundtrip(*app,directory);
        PmxNameBoundaries(*app,directory);
        ShutdownCleanup(app);delete app;g_Block=nullptr;
        return 0;
    }
    if (argc < 2 || argc > 4) return 2;
    const bool capacityOnly = argc == 4 && wcscmp(argv[3], L"--key-capacity") == 0;
    if (argc == 4 && !capacityOnly) return 2;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetWindowsHookExW(WH_CBT,[](int code,WPARAM wp,LPARAM lp)->LRESULT {
        if(code==HCBT_ACTIVATE){
            auto window=reinterpret_cast<HWND>(wp);wchar_t cls[32]{};GetClassNameW(window,cls,32);
            if(!wcscmp(cls,L"#32770") && GetDlgItem(window,IDOK)){
                if(expectedInvalidSave) {
                    return CallNextHookEx(nullptr,code,wp,lp);
                }
                EnumChildWindows(window,[](HWND child,LPARAM)->BOOL {
                    wchar_t text[1024]{};GetWindowTextW(child,text,1024);
                    if(*text)std::fwprintf(stderr,L"PMM unexpected dialog: %ls\n",text);
                    return TRUE;
                },0);
                std::exit(3);
            }
        }
        return CallNextHookEx(nullptr,code,wp,lp);
    },nullptr,GetCurrentThreadId());
    const auto directory = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(directory);
    const auto aaPreference = directory / L"antialias-user.ini";
    SetEnvironmentVariableW(L"MMS_ANTIALIAS_CONFIG", aaPreference.c_str());
    SetEnvironmentVariableW(L"MMS_NVIDIA_AA_FIX", L"0");
    WritePrivateProfileStringW(L"Antialias", L"Algorithm", L"4", aaPreference.c_str());
    WritePrivateProfileStringW(L"Antialias", L"Enabled", L"0", aaPreference.c_str());
    WritePrivateProfileStringW(L"Antialias", L"LastSmaaAlgorithm", L"6", aaPreference.c_str());
    LegacyBindingMetadata();AutomaticMetadata();
    MatchTypes();
    auto* app = new MMDApp(); g_Block = app;
    app->state = MMDAppState{}; app->InitDefaults();
    Check(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "initialize hidden production app");
    ShowWindow(app->MainWindow(), SW_HIDE);
    FormatTriggerBoundaries(*app);
    InvalidFormatEscalation(*app,directory);
    AutomaticCameraRoundtrip(*app,directory);
    const auto a = directory / L"settings-a.pmm";
    const auto b = directory / L"settings-b.pmm";
    const auto legacy = directory / L"legacy.pmm";
    const auto offFile = directory / L"off-overwrite.pmm";
    const auto corrupt = directory / L"corrupt.pmm";
    const auto precisionFile = directory / L"precision.pmm";
    const auto fixture = directory / L"fixture.pmm";
    if (argc >= 3) {
        LoadModelFile(app, argv[2], false);
        Check(app->ModelSlot(0) != nullptr, "load optional real model fixture");
    }


    if (auto* raw=app->ModelSlot(0); raw && mdl::DisplayGroupCount(*mdl::Mdl(raw))>255) {
        const auto groups=mdl::DisplayGroupCount(*mdl::Mdl(raw));
        const auto faces=mdl::FacialFrameCount(*mdl::Mdl(raw));
        const auto name=text_encoding::BoneName(mdl::Mdl(raw)->boneTable[0],false);
        mdl::DisplayGroups(raw)[groups-1].flags=1;
        const auto expanded=directory/L"expanded-display.pmm";
        Check(SaveSceneCopy(app,expanded.c_str()),"save complete expanded display groups");
        Load(*app,expanded);raw=app->ModelSlot(0);
        Check(raw && mdl::DisplayGroupCount(*mdl::Mdl(raw))==groups && mdl::FacialFrameCount(*mdl::Mdl(raw))==faces,
              "real PMM roundtrip retains groups and facial rows beyond byte counts");
        Check(text_encoding::BoneName(mdl::Mdl(raw)->boneTable[0],false)==name && mdl::DisplayGroups(raw)[groups-1].flags==1,
              "real PMM roundtrip retains complete Unicode name and last expanded group state");
        ShutdownCleanup(app);delete app;g_Block=nullptr;
        std::puts("PASS expanded model PMM display and name roundtrip");return 0;
    }

    if (capacityOnly) {
        auto* raw = app->ModelSlot(0);
        auto& model = *mdl::Mdl(raw);
        Check(model.morphCount > 1, "fixture supplies morphs");
        Check(mdl::MorphKeyPoolEnsure(model, 25002), "grow real model morph pool");
        const auto first = std::size_t(model.morphCount) + 20000;
        Check(mdl::MorphKeyPoolEnsure(model, first + 2), "reserve sparse morph slots");
        model.morphKeys[1].allocated = 1;
        model.morphKeys[1].next = static_cast<std::uint32_t>(first);
        model.morphKeys[first] = {};
        model.morphKeys[first].frame = 101;
        model.morphKeys[first].value = .75f;
        model.morphKeys[first].previous = 1;
        model.morphKeys[first].next = static_cast<std::uint32_t>(first + 1);
        model.morphKeys[first].allocated = 1;
        model.morphKeys[first+1] = {};
        model.morphKeys[first+1].frame = 202;
        model.morphKeys[first+1].value = .25f;
        model.morphKeys[first+1].previous = static_cast<std::uint32_t>(first);
        model.morphKeys[first+1].allocated = 1;
        model.maxFrame = 202;
        Save(*app, a);
        Load(*app, a);
        raw = app->ModelSlot(0);
        Check(raw != nullptr, "reload model with grown sparse morphs");
        auto& loaded = *mdl::Mdl(raw);
        const auto savedFirst = loaded.morphKeys[1].next;
        const auto savedSecond = loaded.morphKeys[savedFirst].next;
        Check(savedFirst < mdl::kMorphKeyCapacity && savedSecond < mdl::kMorphKeyCapacity,
              "sparse high indices compact into the original PMM capacity");
        Check(loaded.morphKeys[savedFirst].frame == 101 &&
                  loaded.morphKeys[savedFirst].value == .75f && loaded.morphKeys[savedSecond].frame == 202 &&
                  loaded.morphKeys[savedSecond].value == .25f && loaded.morphKeys[savedSecond].previous == savedFirst,
              "PMM roundtrip preserves every sparse morph link, frame and value");
        app->SetSelectedModelSlot(0); app->state.optflag[0] = 0;
        const auto vmd = directory / L"grown.vmd";
        SaveVmdFile(vmd.c_str());
        int fd = Open(vmd, _O_RDONLY);
        Check(_lseek(fd, 50, SEEK_SET) == 50, "seek VMD motion records");
        std::uint32_t count = 0; Check(_read(fd, &count, 4) == 4, "read VMD bone count");
        Check(_lseek(fd, static_cast<long>(count * 111), SEEK_CUR) >= 0, "skip VMD bones");
        Check(_read(fd, &count, 4) == 4, "read VMD morph count");
        bool found101 = false, found202 = false;
        for (std::uint32_t i = 0; i < count; ++i) {
            char name[15]; std::uint32_t frame; float value;
            Check(_read(fd, name, 15) == 15 && _read(fd, &frame, 4) == 4 && _read(fd, &value, 4) == 4,
                  "read complete standard VMD morph key");
            if (std::memcmp(name, loaded.morphs[1].name, 15) == 0) {
                found101 |= frame == 101 && value == .75f;
                found202 |= frame == 202 && value == .25f;
            }
        }
        _close(fd);
        Check(found101 && found202, "standard VMD exports grown sparse morph keys");
        Save(*app, b); Load(*app, b);
        Check(mdl::Mdl(app->ModelSlot(0))->morphKeys[savedSecond].frame == 202,
              "grown morph keys survive a second PMM save/load");
        std::puts("PASS real PMM and standard VMD grown-morph roundtrip");
        return 0;
    }

    // ---- the five tiers through the real command, writer and loader --------
    settings::Reset(*app);
    Check(app->TransparentMaterialSortPrecision() == TransparentSortPrecision::Off,
          "default sort precision is off");
    Check(!app->AutoTransparentMaterialSortingEnabled(),
          "the default disables transparent sorting");
    for (int value = 0; value < kTierCount; ++value) {
        app->SceneModified() = 0;
        SendMessageW(app->MainWindow(), WM_COMMAND, kTransparentSortPrecisionMenuFirst + value, 0);
        Check(static_cast<int>(app->TransparentMaterialSortPrecision()) == value && app->SceneModified(),
              "tier command changes the value and marks the project dirty");
        Check(app->AutoTransparentMaterialSortingEnabled() ==
                  (value != static_cast<int>(TransparentSortPrecision::Off)),
              "only the off entry reports transparent sorting as disabled");
        app->SceneModified() = 0;
        SendMessageW(app->MainWindow(), WM_COMMAND, kTransparentSortPrecisionMenuFirst + value, 0);
        Check(!app->SceneModified(), "selecting the same tier again is not an edit");
        Save(*app, precisionFile);
        const auto saved = Read(precisionFile);
        const auto* stored = ext::Find(saved, settings::kSortingPrecisionKey);
        Check(stored != nullptr &&
              stored->value == std::vector<std::uint8_t>{static_cast<std::uint8_t>(value)},
              "the real writer stores exactly the selected tier byte");
        Check(ext::Find(saved, kRetiredToggleKey) == nullptr,
              "the real writer stores no retired toggle record");
        Load(*app, precisionFile);
        Check(static_cast<int>(app->TransparentMaterialSortPrecision()) == value,
              "the real loader restores the stored tier");
        Check(app->AutoTransparentMaterialSortingEnabled() ==
                  (value != static_cast<int>(TransparentSortPrecision::Off)),
              "the restored tier drives the sorting state");
        for (int tier = 0; tier < kTierCount; ++tier)
            Check(TierChecked(app->MainWindow(), tier) == (tier == value),
                  "the stored tier marks exactly one radio entry");
    }
    std::puts("PASS five stored tiers through the real command, writer, loader and radio marks");

    // ---- one popup, five entries, one label per language, all radio --------
    const auto language = app->EnglishUI();
    for (int lang : {2, 1, 0, 2}) {
        app->EnglishUI() = lang; RefreshMenuLanguage(app);
        HMENU view = GetSubMenu(GetMenu(app->MainWindow()), 2);
        Check(view != nullptr, "the view menu exists");
        int count = 0;
        for (int pos = 0; pos < GetMenuItemCount(view); ++pos) {
            HMENU sub = GetSubMenu(view, pos);
            if (sub == nullptr ||
                GetMenuState(sub, kTransparentSortPrecisionMenuFirst, MF_BYCOMMAND) ==
                    static_cast<UINT>(-1))
                continue;
            ++count;
            Check(GetMenuItemCount(sub) == kTierCount, "five limit entries after a language switch");
            Check(GetMenuState(sub, kAutoTransparentMaterialSortingMenuId, MF_BYCOMMAND) ==
                      static_cast<UINT>(-1),
                  "the retired 303 toggle has no entry inside the limit popup");
            Check(GetMenuState(sub, kTransparentSortPrecisionMenuOff + 1, MF_BYCOMMAND) ==
                      static_cast<UINT>(-1),
                  "no sixth entry survives above the five tiers");
            for (int tier = 0; tier < kTierCount; ++tier) {
                MENUITEMINFOW item{};
                item.cbSize = sizeof(item);
                item.fMask = MIIM_FTYPE;
                Check(GetMenuItemInfoW(sub, kTransparentSortPrecisionMenuFirst + tier, FALSE, &item) &&
                      (item.fType & MFT_RADIOCHECK) != 0,
                      "every tier entry is drawn as a radio item");
                const wchar_t* expected = lang == 2 ? kTierLabels[tier].zh
                                                      : (lang == 1 ? kTierLabels[tier].en
                                                                   : kTierLabels[tier].jp);
                Check(ItemLabel(sub, kTransparentSortPrecisionMenuFirst + tier) == expected,
                      "localized tier entry label");
                const bool checked =
                    (GetMenuState(sub, kTransparentSortPrecisionMenuFirst + tier, MF_BYCOMMAND) &
                     MF_CHECKED) != 0;
                Check(checked == (tier == static_cast<int>(app->TransparentMaterialSortPrecision())),
                      "exactly one tier entry carries the mark of the running tier");
            }
            // The low tier is a bare label: no bracketed hint, unlike Standard.
            Check(ItemLabel(sub, kTransparentSortPrecisionMenuFirst).find(L'（') == std::wstring::npos &&
                  ItemLabel(sub, kTransparentSortPrecisionMenuFirst).find(L'(') == std::wstring::npos,
                  "the low entry carries no bracketed hint");
            wchar_t title[128]{}; GetMenuStringW(view, pos, title, 128, MF_BYPOSITION);
            Check(wcscmp(title, lang == 2 ? L"透明排序上限"
                                          : lang == 1 ? L"Transparent sort limit"
                                                      : L"透明ソート上限") == 0,
                  "localized transparent sort limit menu title");
        }
        Check(count == 1, "a language switch does not duplicate the limit menu");
        Check(GetMenuState(view, kAutoTransparentMaterialSortingMenuId, MF_BYCOMMAND) ==
                  static_cast<UINT>(-1),
              "the retired 303 toggle has no separate view-menu entry");
    }
    app->EnglishUI() = language; RefreshMenuLanguage(app);
    std::puts("PASS one five-entry radio popup with a title per language and no 303 entry");

    // ---- processor and unknown records stay independent of the tier --------
    settings::Reset(*app);
    const bone_scale::Matrix savedScale{{1.4f, .2f, 0, 0, .8f, .1f, 0, 0, 1.2f}};
    if (argc == 3) {
        auto* model = app->ModelSlot(0);
        Check(mdl::Mdl(model)->boneCount > 0, "fixture has a bone");
        mdl::Mdl(model)->boneSelection[0] = 1;
        Check(bone_scale::SetPose(model, 0, savedScale), "set real model scale");
        RegisterSelectedBoneKeys(model, 0, 0);
    }
    skinning::SetPreference({skinning::Mode::Cpu}); settings::ProcessorChanged(*app);
    SendMessageW(app->MainWindow(), WM_COMMAND, kTransparentSortPrecisionMenuFirst + 2, 0);
    Check(app->TransparentMaterialSortPrecision() == TransparentSortPrecision::High,
          "the tier does not depend on the processor class");
    ext::Set(app->PmmProjectSettings().document, "example.thirdparty.opaque", {0, 255, 3, 0, 17});
    Save(*app, a);
    std::uint64_t boundary = 0; auto doc = Read(a, &boundary);
    Check(ext::Find(doc, settings::kProcessorKey) == nullptr,
          "PMM stores no processor class any more (per-user processor.ini owns it)");
    Check(ext::Find(doc, settings::kSortingPrecisionKey)->value == std::vector<std::uint8_t>{2},
          "PMM stores the selected tier byte");
    Check(ext::Find(doc, kRetiredToggleKey) == nullptr, "PMM stores no retired toggle record");
    skinning::SetPreference({}); // a different running choice before opening
    settings::Reset(*app); Load(*app, a);
    if (argc == 3) {
        auto* model = app->ModelSlot(0);
        Check(model != nullptr, "standard model data survives PMM extension round-trip");
        Check(bone_scale::Pose(model, 0) == savedScale && bone_scale::Key(model, 0) == savedScale,
              "real PMM loader restores the affine pose and registered scale key");
    }
    Check(skinning::GetPreference().mode == skinning::Mode::Automatic,
          "opening a project never changes the running processor settings");
    Check(app->TransparentMaterialSortPrecision() == TransparentSortPrecision::High,
          "the real loader restores the tier");
    Check(TierChecked(app->MainWindow(), 2) && !TierChecked(app->MainWindow(), 1),
          "the restored tier moves the radio mark");
    Save(*app, b);
    const auto reloaded = Read(b);
    Check(ext::Find(reloaded, "example.thirdparty.opaque")->value == std::vector<std::uint8_t>({0, 255, 3, 0, 17}),
          "unknown record survives real load and save-as");
    Check(doc.records.size() == reloaded.records.size(), "round-trip does not duplicate extension blocks");
    for (std::size_t i=0; i<doc.records.size(); ++i)
        Check(doc.records[i].key == reloaded.records[i].key && doc.records[i].value == reloaded.records[i].value,
              "all extension records retain byte values and order");
    SendMessageW(app->MainWindow(), WM_COMMAND, kTransparentSortPrecisionMenuFirst + 3, 0);
    Save(*app, b);
    const auto afterEdit = Read(b);
    Check(ext::Find(afterEdit, settings::kProcessorKey) == nullptr &&
          ext::Find(afterEdit, "example.thirdparty.opaque")->value == std::vector<std::uint8_t>({0, 255, 3, 0, 17}),
          "a tier edit leaves the processor record absent and unknown keys alone");
    std::puts("PASS real PMM save/load/save-as with no processor data and unknown records preserved");

    // The retired processor record is removed on save and never rewrites the
    // per-user processor settings.
    ext::Set(app->PmmProjectSettings().document, settings::kProcessorKey, {2});
    skinning::SetPreference({skinning::Mode::Cpu}); // the running choice stays
    Check(ext::Find(settings::Capture(*app), settings::kProcessorKey) == nullptr,
          "saving removes the retired processor record");
    Check(skinning::GetPreference().mode == skinning::Mode::Cpu,
          "saving a project never changes the running processor settings");
    // An old-style record inside a project file is ignored on load.
    {
        ext::Document document;
        ext::Set(document, settings::kProcessorKey, {2});
        ext::Set(document, settings::kSortingPrecisionKey, {1});
        WriteFixture(a, fixture, boundary, document);
        settings::Reset(*app);
        Load(*app, fixture);
        Check(skinning::GetPreference().mode == skinning::Mode::Cpu,
              "an old processor record in a project is ignored on load");
    }

    // ---- a stored byte takes the five-tier meaning, not the earlier one ----
    // The earlier six-tier experiment used 3 for its extra tier and 4 for its
    // maximum.  This reader never translates those bytes: 3 is Maximum now and 4
    // is Off, and the earlier off byte 5 is simply unknown.
    {
        const std::pair<std::uint8_t, TransparentSortPrecision> cases[] = {
            {0, TransparentSortPrecision::Low},
            {1, TransparentSortPrecision::Standard},
            {2, TransparentSortPrecision::High},
            {3, TransparentSortPrecision::Maximum},
            {4, TransparentSortPrecision::Off},
        };
        for (const auto& plainCase : cases) {
            ext::Document document;
            ext::Set(document, settings::kSortingPrecisionKey, {plainCase.first});
            WriteFixture(a, fixture, boundary, document);
            settings::Reset(*app);
            Load(*app, fixture);
            Check(app->TransparentMaterialSortPrecision() == plainCase.second,
                  "a stored byte takes the five-tier meaning of that value");
        }
        ext::Document earlierOff;
        ext::Set(earlierOff, settings::kSortingPrecisionKey, {5});
        WriteFixture(a, fixture, boundary, earlierOff);
        settings::Reset(*app);
        Load(*app, fixture);
        Check(app->TransparentMaterialSortPrecision() == TransparentSortPrecision::Off,
              "the earlier off byte is unknown now and runs the off default");
    }
    std::puts("PASS stored bytes take the five-tier meaning with no earlier-format translation");

    // ---- re-selecting the running tier is not an edit ----------------------
    settings::Reset(*app);
    app->SceneModified() = 0;
    SendMessageW(app->MainWindow(), WM_COMMAND, kTransparentSortPrecisionMenuOff, 0);
    Check(!app->SceneModified() && app->TransparentMaterialSortPrecision() == TransparentSortPrecision::Off,
          "selecting the running default on a new project is not an edit");
    SendMessageW(app->MainWindow(), WM_COMMAND, kTransparentSortPrecisionMenuFirst + 2, 0);
    Check(app->SceneModified(), "selecting another tier is an edit");
    app->SceneModified() = 0;
    SendMessageW(app->MainWindow(), WM_COMMAND, kTransparentSortPrecisionMenuFirst + 2, 0);
    Check(!app->SceneModified(), "re-selecting the same unsaved tier is not an edit");
    settings::Reset(*app); Load(*app, a);
    Check(app->TransparentMaterialSortPrecision() == TransparentSortPrecision::High,
          "the saved project stores the high tier");
    app->SceneModified() = 0;
    SendMessageW(app->MainWindow(), WM_COMMAND, kTransparentSortPrecisionMenuFirst + 2, 0);
    Check(!app->SceneModified(), "re-selecting the tier the file already stores is not an edit");
    Save(*app, b);
    const auto rewritten = Read(b);
    Check(ext::Find(rewritten, settings::kSortingPrecisionKey)->value == std::vector<std::uint8_t>{2},
          "re-selecting the stored tier rewrites the same byte");
    settings::Reset(*app); Load(*app, b);
    Check(app->TransparentMaterialSortPrecision() == TransparentSortPrecision::High,
          "the untouched tier survives save-as and reopen");
    app->SceneModified() = 0;
    SendMessageW(app->MainWindow(), WM_COMMAND, kTransparentSortPrecisionMenuFirst + 3, 0);
    Check(app->SceneModified(), "selecting a different stored tier is an edit");
    std::puts("PASS re-selecting the running tier never marks the project dirty");

    // ---- a file without the tier field runs off -----------------------
    {
        ext::Document empty;
        WriteFixture(a, fixture, boundary, empty);
        settings::Reset(*app);
        Load(*app, fixture);
        Check(app->TransparentMaterialSortPrecision() == TransparentSortPrecision::Off,
              "a project without the tier field runs off");
        app->SceneModified() = 0;
        SendMessageW(app->MainWindow(), WM_COMMAND, kTransparentSortPrecisionMenuOff, 0);
        Check(!app->SceneModified(),
              "the missing field normalizes itself on save, so off is not an edit");
        Save(*app, b);
        const auto started = Read(b);
        Check(ext::Find(started, settings::kSortingPrecisionKey)->value == std::vector<std::uint8_t>{4},
              "the save starts the tier field at the off byte");
    }
    std::puts("PASS a project without the tier field runs and stores off");

    // ---- off overwrites the tier stored before it --------------------------
    settings::Reset(*app);
    app->SceneModified() = 0;
    SendMessageW(app->MainWindow(), WM_COMMAND, kTransparentSortPrecisionMenuFirst + 3, 0);
    Check(app->TransparentMaterialSortPrecision() == TransparentSortPrecision::Maximum && app->SceneModified(),
          "the maximum tier is reachable by its command id");
    Check(TierChecked(app->MainWindow(), 3), "the maximum tier carries the radio mark");
    app->SceneModified() = 0;
    SendMessageW(app->MainWindow(), WM_COMMAND, kTransparentSortPrecisionMenuOff, 0);
    Check(app->TransparentMaterialSortPrecision() == TransparentSortPrecision::Off && app->SceneModified(),
          "the off entry is reachable by its command id");
    Check(!app->AutoTransparentMaterialSortingEnabled(), "the off entry disables transparent sorting");
    Check(TierChecked(app->MainWindow(), 4) && !TierChecked(app->MainWindow(), 3),
          "off moves the radio mark off the previously selected tier");
    Save(*app, offFile);
    doc = Read(offFile);
    Check(ext::Find(doc, settings::kSortingPrecisionKey)->value == std::vector<std::uint8_t>{4},
          "off overwrites the tier that was stored before it");
    Check(ext::Find(doc, kRetiredToggleKey) == nullptr, "off stores no second flag");
    settings::Reset(*app); Load(*app, offFile);
    Check(app->TransparentMaterialSortPrecision() == TransparentSortPrecision::Off &&
          !app->AutoTransparentMaterialSortingEnabled(),
          "off survives a reopen");
    Check(TierChecked(app->MainWindow(), 4), "the reopened radio mark sits on off");
    SendMessageW(app->MainWindow(), WM_COMMAND, kTransparentSortPrecisionMenuFirst + 2, 0);
    Save(*app, b);
    doc = Read(b);
    Check(ext::Find(doc, settings::kSortingPrecisionKey)->value == std::vector<std::uint8_t>{2},
          "the off byte leaves no earlier tier behind when another tier is picked");
    settings::Reset(*app); Load(*app, offFile);
    Save(*app, b);
    doc = Read(b);
    Check(ext::Find(doc, settings::kSortingPrecisionKey)->value == std::vector<std::uint8_t>{4},
          "save-as keeps the single off byte and remembers no earlier tier");
    std::puts("PASS maximum then off stores a single off byte through save, reopen and save-as");

    // ---- the 303 compatibility command keeps its off/on swap ---------------
    settings::Reset(*app);
    SendMessageW(app->MainWindow(), WM_COMMAND, kTransparentSortPrecisionMenuFirst + 1, 0);
    Check(app->TransparentMaterialSortPrecision() == TransparentSortPrecision::Standard,
          "303 starts from the standard tier");
    app->SceneModified() = 0;
    SendMessageW(app->MainWindow(), WM_COMMAND, kAutoTransparentMaterialSortingMenuId, 0);
    Check(app->TransparentMaterialSortPrecision() == TransparentSortPrecision::Off && app->SceneModified(),
          "303 turns transparent sorting off through the tier field");
    Check(TierChecked(app->MainWindow(), 4), "303 moves the radio mark to off");
    app->SceneModified() = 0;
    SendMessageW(app->MainWindow(), WM_COMMAND, kTransparentSortPrecisionMenuOff, 0);
    Check(!app->SceneModified(), "re-selecting the unsaved off entry is not a second edit");
    SendMessageW(app->MainWindow(), WM_COMMAND, kAutoTransparentMaterialSortingMenuId, 0);
    Check(app->TransparentMaterialSortPrecision() == TransparentSortPrecision::Standard,
          "303 turns transparent sorting back on at the standard tier");
    SendMessageW(app->MainWindow(), WM_COMMAND, kTransparentSortPrecisionMenuFirst + 2, 0);
    Check(app->TransparentMaterialSortPrecision() == TransparentSortPrecision::High &&
          app->AutoTransparentMaterialSortingEnabled(),
          "a tier picked after off re-enables sorting at that tier");
    std::puts("PASS the 303 compatibility command swaps off and standard through the tier field");

    // ---- the retired toggle key is an ordinary unknown record --------------
    const auto loadFixture = [&](const std::vector<std::uint8_t>* toggle,
                                 const std::vector<std::uint8_t>* precision) {
        WriteFixture(a, fixture, boundary, FixtureRecords(toggle, precision));
        settings::Reset(*app);
        Load(*app, fixture);
    };
    // Neither value the old toggle ever held selects a tier any more: the loader
    // does not look the retired key up at all.
    for (int value = 0; value <= 1; ++value) {
        const std::vector<std::uint8_t> toggle{static_cast<std::uint8_t>(value)};
        loadFixture(&toggle, nullptr);
        Check(app->TransparentMaterialSortPrecision() == TransparentSortPrecision::Off,
              "the retired toggle no longer selects a tier");
        Save(*app, b);
        const auto saved = Read(b);
        Check(ext::Find(saved, kRetiredToggleKey)->value == toggle,
              "the retired toggle is preserved byte for byte");
        Check(ext::Find(saved, settings::kSortingPrecisionKey)->value == std::vector<std::uint8_t>{4},
              "a file with only the retired toggle starts the tier field at off");
    }
    // The retired record sits next to a real tier field without changing it.
    for (int value = 0; value < kTierCount; ++value) {
        for (int toggleValue = 0; toggleValue <= 1; ++toggleValue) {
            const std::vector<std::uint8_t> toggle{static_cast<std::uint8_t>(toggleValue)};
            const std::vector<std::uint8_t> precision{static_cast<std::uint8_t>(value)};
            loadFixture(&toggle, &precision);
            Check(static_cast<int>(app->TransparentMaterialSortPrecision()) == value,
                  "a retired toggle does not override the stored tier");
            Save(*app, b);
            const auto saved = Read(b);
            Check(ext::Find(saved, settings::kSortingPrecisionKey)->value == precision &&
                  ext::Find(saved, kRetiredToggleKey)->value == toggle,
                  "both the tier field and the retired toggle survive save-as unchanged");
        }
    }
    // A malformed retired toggle is unknown data like any other.
    for (const std::vector<std::uint8_t>& raw : {std::vector<std::uint8_t>{255}, {2, 99}, {}}) {
        loadFixture(&raw, nullptr);
        Check(app->TransparentMaterialSortPrecision() == TransparentSortPrecision::Off,
              "a malformed retired toggle has no effect on the running tier");
        Save(*app, b);
        const auto saved = Read(b);
        Check(ext::Find(saved, kRetiredToggleKey)->value == raw &&
              ext::Find(saved, settings::kSortingPrecisionKey)->value == std::vector<std::uint8_t>{4},
              "a malformed retired toggle is carried over next to the off byte");
    }
    // An explicit pick writes the tier field only; the retired record stays.
    {
        const std::vector<std::uint8_t> toggle{0};
        loadFixture(&toggle, nullptr);
        app->SceneModified() = 0;
        SendMessageW(app->MainWindow(), WM_COMMAND, kTransparentSortPrecisionMenuFirst + 3, 0);
        Check(app->SceneModified() &&
              app->TransparentMaterialSortPrecision() == TransparentSortPrecision::Maximum,
              "an explicit tier on a toggle-only file is an edit");
        Save(*app, b);
        const auto saved = Read(b);
        Check(ext::Find(saved, settings::kSortingPrecisionKey)->value == std::vector<std::uint8_t>{3} &&
              ext::Find(saved, kRetiredToggleKey)->value == toggle,
              "the explicit tier is written and the retired record is left alone");
    }
    std::puts("PASS the retired toggle key is carried over as unknown data and never selects a tier");

    // ---- an unknown tier byte stays raw until the user picks one -----------
    for (const std::vector<std::uint8_t>& raw : {std::vector<std::uint8_t>{5}, {255}, {2, 99}, {}}) {
        ext::Document document;
        ext::Set(document, settings::kSortingPrecisionKey, raw);
        ext::Set(document, settings::kProcessorKey, {255, 9});
        ext::Set(document, kRetiredToggleKey, {0});
        WriteFixture(a, fixture, boundary, document);
        settings::Reset(*app);
        Load(*app, fixture);
        Check(app->TransparentMaterialSortPrecision() == TransparentSortPrecision::Off,
              "an unknown tier byte runs the off default");
        Check(TierChecked(app->MainWindow(), 4), "the off default carries the radio mark");
        Save(*app, b);
        const auto unchanged = Read(b);
        Check(ext::Find(unchanged, settings::kSortingPrecisionKey)->value == raw,
              "an unknown tier byte survives an unchanged save");
        Check(ext::Find(unchanged, kRetiredToggleKey)->value == std::vector<std::uint8_t>{0},
              "the retired toggle beside an unknown tier byte is carried over too");
        Check(ext::Find(unchanged, settings::kProcessorKey) == nullptr,
              "the retired processor record is removed on save, unlike unknown data");
        app->SceneModified() = 0;
        SendMessageW(app->MainWindow(), WM_COMMAND, kTransparentSortPrecisionMenuFirst + 1, 0);
        Check(app->SceneModified() && app->TransparentMaterialSortPrecision() == TransparentSortPrecision::Standard,
              "selecting standard edits an unknown tier byte");
        Save(*app, b);
        const auto replaced = Read(b);
        Check(ext::Find(replaced, settings::kSortingPrecisionKey)->value == std::vector<std::uint8_t>{1},
              "an explicit standard replaces the unknown tier byte");
        Check(ext::Find(replaced, kRetiredToggleKey)->value == std::vector<std::uint8_t>{0},
              "the replacement leaves the retired toggle alone");
        Check(ext::Find(replaced, settings::kProcessorKey) == nullptr,
              "the replacement still drops the retired processor record");
    }
    std::puts("PASS unknown tier bytes stay raw until an explicit selection replaces them");

    // ---- a legacy file without an extension and a brand new project --------
    std::filesystem::copy_file(a, legacy, std::filesystem::copy_options::overwrite_existing);
    int fd = Open(legacy, _O_RDWR);
    Check(_chsize_s(fd, static_cast<__int64>(boundary)) == 0, "strip extension for legacy fixture");
    _close(fd);
    Load(*app, legacy);
    Check(!app->AutoTransparentMaterialSortingEnabled() &&
          app->TransparentMaterialSortPrecision() == TransparentSortPrecision::Off &&
          app->PmmProjectSettings().document.records.empty(),
          "legacy project without an extension resets defaults and unknown data");
    Check(skinning::GetPreference().mode == skinning::Mode::Cpu,
          "a legacy project does not change the per-user processor settings");
    Save(*app, b);
    doc = Read(b);
    Check(ext::Find(doc, settings::kSortingPrecisionKey)->value == std::vector<std::uint8_t>{4},
          "a plain project starts the tier field at the off byte");
    Check(ext::Find(doc, kRetiredToggleKey) == nullptr, "a plain project writes no retired toggle record");
    Load(*app, a);
    ResetAppState(app);
    Check(!app->AutoTransparentMaterialSortingEnabled() &&
          app->TransparentMaterialSortPrecision() == TransparentSortPrecision::Off &&
          app->PmmProjectSettings().document.records.empty(),
          "new project resets both settings and extensions");
    Check(skinning::GetPreference().mode == skinning::Mode::Cpu,
          "a new project does not change the per-user processor settings");
    std::puts("PASS legacy PMM without an extension and new-project isolation");

    // ---- a damaged trailer never leaks the last project's settings ---------
    settings::Reset(*app);
    SendMessageW(app->MainWindow(), WM_COMMAND, kTransparentSortPrecisionMenuOff, 0);
    Check(app->TransparentMaterialSortPrecision() == TransparentSortPrecision::Off,
          "the app runs off before the corrupt load");
    std::filesystem::copy_file(a, corrupt, std::filesystem::copy_options::overwrite_existing);
    fd = Open(corrupt, _O_RDWR); _lseeki64(fd, -8, SEEK_END); std::uint32_t crc = 0;
    Check(_read(fd, &crc, 4) == 4, "read CRC"); crc ^= 1;
    _lseeki64(fd, -8, SEEK_END); Check(_write(fd, &crc, 4) == 4, "damage CRC only"); _close(fd);
    Load(*app, a); Load(*app, corrupt);
    Check(!app->AutoTransparentMaterialSortingEnabled() &&
          app->TransparentMaterialSortPrecision() == TransparentSortPrecision::Off &&
          app->PmmProjectSettings().document.records.empty(),
          "corrupt extension never leaks previous settings");
    Check(skinning::GetPreference().mode == skinning::Mode::Cpu,
          "a corrupt extension does not change the per-user processor settings");
    std::puts("PASS corrupt extension keeps the standard project readable");
    AntialiasRoundtrip(*app, directory);
    ShutdownCleanup(app); delete app; g_Block = nullptr;
    std::puts("PASS PMM v2 project settings integration");
    return 0;
}
