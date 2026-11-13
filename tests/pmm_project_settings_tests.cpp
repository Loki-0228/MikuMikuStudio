#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "../src/plugins/project_storage.hpp"
#include <MinHook.h>
#endif
#include "mikumikustudio/extended_names.hpp"
#include "mikumikustudio/owned_text.hpp"
#include "mikumikustudio/display_capacity.hpp"
#include "mikumikustudio/save_compatibility.hpp"
#include "mikumikustudio/pmm_key_map.hpp"
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
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <clocale>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/pmm_project_settings.hpp"
#include "mikumikustudio/view_options.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/model_order_projection.hpp"
#include "mikumikustudio/ui_language.hpp"

using namespace mikumikustudio;
namespace ext = mikumikustudio::pmm_extension;
namespace settings = mikumikustudio::pmm_settings;
namespace {
// Five tiers, stored as their own enum values.  The five menu ids are
// contiguous from kTransparentSortPrecisionMenuFirst: Low = 304 .. Maximum = 307
// and Off = kTransparentSortPrecisionMenuOff = 308.
constexpr int kTierCount = 5;

// The retired toggle key of the earlier six-tier test format.  The settings
// module no longer declares it, so this file spells the key out to place it in a
// fixture as an ordinary unknown record: the loader never looks it up, and a
// real writer carries it over byte for byte.
constexpr char kRetiredToggleKey[] = "org.mikumikustudio.view.transparent_sort";

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
#if MMD_ORIGINAL_PROFILE
// A successful PMX import warns about the 15-byte VMD name limit even when
// its PMM identity is writable. Accept only this exact warning during one
// explicitly scoped fixture load; every other dialog remains a test failure.
struct OriginalNameWarningExpectation {
    HWND owner=nullptr, window=nullptr;
    DWORD thread=GetCurrentThreadId();
    ULONGLONG deadline=GetTickCount64()+5000;
    bool required=false;
    unsigned dialogs=0;
};
OriginalNameWarningExpectation* originalNameWarning=nullptr;
bool OriginalNameWarningActivation(HWND window) {
    auto* expected=originalNameWarning;
    if(!expected || !expected->required || expected->dialogs ||
       GetWindow(window,GW_OWNER)!=expected->owner ||
       GetWindowThreadProcessId(window,nullptr)!=expected->thread)return false;
    wchar_t title[32]{};GetWindowTextW(window,title,std::size(title));
    if(std::wcscmp(title,L"MMD"))return false;
    struct TextProbe { bool matched=false; unsigned buttons=0; } probe;
    EnumChildWindows(window,[](HWND child,LPARAM parameter)->BOOL {
        auto& result=*reinterpret_cast<TextProbe*>(parameter);
        wchar_t cls[32]{},body[1024]{};
        GetClassNameW(child,cls,std::size(cls));GetWindowTextW(child,body,std::size(body));
        constexpr wchar_t expectedBody[]=
            L"The model was loaded. Some original MMD output formats cannot preserve these fields completely:\n"
            L"- Name characters or lengths (VMD bone/morph names allow 15 CP932 bytes)\n"
            L"\nThe loaded data has been retained. Saving checks the selected format and warns if it cannot preserve the data completely.";
        if(std::wcscmp(cls,L"Static")==0 && std::wcscmp(body,expectedBody)==0)result.matched=true;
        if(std::wcscmp(cls,L"Button")==0 && IsWindowEnabled(child))++result.buttons;
        return TRUE;
    },reinterpret_cast<LPARAM>(&probe));
    if(!probe.matched || probe.buttons!=1)return false;
    expected->window=window;++expected->dialogs;
    Check(PostMessageW(window,WM_CLOSE,0,0)!=FALSE,"close only the exact successful-import name warning");
    return true;
}
template<class LoadOperation>
void WithOriginalNameWarning(MMDApp& app,bool required,LoadOperation&& operation) {
    Check(!originalNameWarning,"one scoped original-name import warning expectation");
    OriginalNameWarningExpectation expected;expected.owner=app.MainWindow();expected.required=required;
    const auto language=app.EnglishUI();app.EnglishUI()=1;originalNameWarning=&expected;
    const UINT_PTR timer=SetTimer(nullptr,0,20,[](HWND,UINT,UINT_PTR,DWORD){
        if(originalNameWarning)Check(GetTickCount64()<originalNameWarning->deadline,
            "scoped model import and its exact warning complete within five seconds");
    });
    Check(timer!=0,"install bounded original-name warning guard");
    operation();
    KillTimer(nullptr,timer);originalNameWarning=nullptr;app.EnglishUI()=language;
    Check(expected.dialogs==unsigned(required) && (!expected.window || !IsWindow(expected.window)),
        "known model-name case emits exactly its expected warning count and closes the native dialog");
}
#endif
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

void TransparentSortMenuLanguage(MMDApp& app) {
    const HWND window = app.MainWindow();
    const auto language = app.EnglishUI();
    if (build_profile::original) {
        for (unsigned char lang : {kUiChinese, kUiEnglish, kUiJapanese, kUiChinese}) {
            app.EnglishUI() = lang;
            RefreshMenuLanguage(&app);
            const HMENU view = GetSubMenu(GetMenu(window), 2);
            Check(view && !PrecisionPopup(window) &&
                  GetMenuState(view, kAutoTransparentMaterialSortingMenuId, MF_BYCOMMAND) == UINT(-1) &&
                  GetMenuState(view, kTransparentSortCrossObjectsMenuId, MF_BYCOMMAND) == UINT(-1),
                  "original profile has no enhanced sorting menu in any language");
        }
        app.EnglishUI() = language;
        RefreshMenuLanguage(&app);
        std::puts("PASS original profile keeps enhanced sorting hidden through language switches");
        return;
    }

    RefreshTransparentSortPrecisionMenu(&app);
    const HMENU popup = PrecisionPopup(window);
    Check(popup != nullptr, "transparent sorting popup exists");
    const auto initialTier = app.TransparentMaterialSortPrecision();
    const bool initialScope = app.TransparentSortCrossObjects();
    const auto initialSettings = app.PmmProjectSettings();
    const auto initialDirty = app.SceneModified();
    constexpr UINT strengthHeading = 32005;
    constexpr UINT order[] = {kTransparentSortPrecisionMenuOff, 0, strengthHeading,
        kTransparentSortPrecisionMenuFirst, kTransparentSortPrecisionMenuFirst + 1,
        kTransparentSortPrecisionMenuFirst + 2, kTransparentSortPrecisionMenuLast,
        0, kTransparentSortCrossObjectsMenuId};
    constexpr const wchar_t* titles[] = {L"透明ソート", L"Transparent sorting", L"透明排序"};
    constexpr const wchar_t* headings[] = {L"ソート精度", L"Sorting strength", L"排序程度"};
    constexpr const wchar_t* scopeLabels[] = {L"オブジェクト間もソート", L"Sort across objects", L"开启跨物体排序"};

    const auto verify = [&](unsigned char lang) {
        const HMENU view = GetSubMenu(GetMenu(window), 2);
        Check(view && PrecisionPopup(window) == popup, "the same sorting popup survives refresh");
        int popups = 0;
        for (int pos = 0; pos < GetMenuItemCount(view); ++pos) {
            const HMENU sub = GetSubMenu(view, pos);
            if (!sub || GetMenuState(sub, kTransparentSortPrecisionMenuFirst, MF_BYCOMMAND) == UINT(-1))
                continue;
            ++popups;
            wchar_t title[128]{};
            GetMenuStringW(view, pos, title, std::size(title), MF_BYPOSITION);
            Check(std::wstring_view(title) == titles[lang], "localized transparent sorting title");
        }
        Check(popups == 1 && GetMenuItemCount(popup) == std::size(order),
              "one sorting popup with the complete ordered layout");
        Check(GetMenuState(view, kAutoTransparentMaterialSortingMenuId, MF_BYCOMMAND) == UINT(-1),
              "the retired toggle has no separate entry");
        for (UINT i = 0; i < std::size(order); ++i) {
            MENUITEMINFOW item{};
            item.cbSize = sizeof(item);
            item.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STATE | MIIM_SUBMENU;
            Check(GetMenuItemInfoW(popup, i, TRUE, &item) && !item.hSubMenu,
                  "each sorting item is a leaf");
            const UINT id = order[i];
            if (!id) {
                Check((item.fType & MFT_SEPARATOR) != 0, "separator at the intended position");
            } else if (id == strengthHeading) {
                Check(item.wID == id && !(item.fType & (MFT_RADIOCHECK | MFT_SEPARATOR)) &&
                      (item.fState & MFS_DISABLED) && ItemLabel(popup, id) == headings[lang],
                      "the localized strength heading cannot be invoked");
            } else if (id == kTransparentSortCrossObjectsMenuId) {
                Check(item.wID == id && !(item.fType & (MFT_RADIOCHECK | MFT_SEPARATOR)) &&
                      !(item.fState & MFS_DISABLED) && ItemLabel(popup, id) == scopeLabels[lang] &&
                      bool(item.fState & MFS_CHECKED) == app.TransparentSortCrossObjects(),
                      "cross-object scope has an independent localized checkbox");
            } else {
                const auto tier = id - kTransparentSortPrecisionMenuFirst;
                const auto& labels = kTierLabels[tier];
                const auto* label = lang == kUiChinese ? labels.zh : lang == kUiEnglish ? labels.en : labels.jp;
                Check(item.wID == id && (item.fType & MFT_RADIOCHECK) &&
                      !(item.fState & MFS_DISABLED) && ItemLabel(popup, id) == label &&
                      bool(item.fState & MFS_CHECKED) == (tier == unsigned(app.TransparentMaterialSortPrecision())),
                      "all five enabled radio items keep their localized labels and exclusive selection");
            }
        }
    };

    for (unsigned char lang : {kUiChinese, kUiEnglish, kUiJapanese, kUiChinese}) {
        app.EnglishUI() = lang;
        RefreshMenuLanguage(&app);
        Check(app.TransparentMaterialSortPrecision() == initialTier &&
              app.TransparentSortCrossObjects() == initialScope && app.SceneModified() == initialDirty,
              "changing language leaves sorting options and project dirty state intact");
        verify(lang);
    }
    const auto invoke = [&](UINT id) { SendMessageW(window, WM_COMMAND, id, 0); };
    invoke(kTransparentSortPrecisionMenuFirst + 1);
    Check(app.TransparentMaterialSortPrecision() == TransparentSortPrecision::Standard, "select Standard");
    invoke(kTransparentSortCrossObjectsMenuId);
    Check(app.TransparentSortCrossObjects() != initialScope &&
          app.TransparentMaterialSortPrecision() == TransparentSortPrecision::Standard, "toggle scope without changing Standard");
    verify(kUiChinese);
    invoke(kTransparentSortPrecisionMenuOff);
    Check(!app.AutoTransparentMaterialSortingEnabled() && app.TransparentSortCrossObjects() != initialScope,
          "Off disables sorting and retains the independent scope");
    verify(kUiChinese);
    invoke(kTransparentSortCrossObjectsMenuId);
    Check(app.TransparentSortCrossObjects() == initialScope && !app.AutoTransparentMaterialSortingEnabled(),
          "toggle the scope back while remaining Off");
    verify(kUiChinese);
    app.TransparentMaterialSortPrecision() = initialTier;
    app.PmmProjectSettings() = initialSettings;
    app.SceneModified() = initialDirty;
    app.EnglishUI() = language;
    RefreshMenuLanguage(&app);
    std::puts("PASS transparent sorting menu order, three languages, exclusive precision and independent scope");
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
        if(extended) {
            Check(!report.details.empty(),"name warnings include an exact field");
            const auto notification=save_compatibility::Describe(report,L"",false);
            Check(notification.find(L"Ronova")!=std::wstring::npos &&
                notification.find(report.nameIssue)!=std::wstring::npos,"notification identifies the model and rejected binding name");
        }
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
    assign(morphs[0],L"abcdefghijklmnopqrあ");expect(false,"a split 19-byte CP932 morph mirror still binds to the same model bytes");
    assign(morphs[0],L"face");
    assign(bones[0],L"abcdefghijklmnopqrst");
    std::memcpy(bones[0].name,"abcdefghijklmnopqrst",20);
    expect(false,"an exact full-width twenty-byte PMD identity is accepted without manufacturing a NUL byte");
    Check(save_compatibility::InspectMotion(*model).namesNeedExtension,
        "full-width PMM identity acceptance still cannot relax the VMD fifteen-byte limit");
    assign(bones[0],L"+FlycloakRootB CB A01");
    std::strcpy(bones[0].name,"+FlycloakRootB");expect(true,"an arbitrary shorter alias still requires an extension");
    assign(bones[0],L"+FlycloakRootB CB A01");
    std::strcpy(bones[0].name,"unrelated-alias-123");expect(true,"a nineteen-byte non-prefix alias still requires an extension");
    assign(bones[0],L"+FlycloakRootB CB A01");
    assign(morphs[0],L"+FlycloakRootB CB A01");std::strcpy(morphs[0].name,"unrelated-alias-123");
    expect(true,"a nineteen-byte non-prefix morph alias still requires an extension");assign(morphs[0],L"face");
    Check(extended_names::Assign(bones[0].jpText,L"骨骼\U0001f31f"),"allocate non-CP932 binding name");
    expect(true,"Unicode replacement aliases still require an extension");
    assign(bones[0],L"+FlycloakRootB CB A01");assign(bones[1],L"+FlycloakRootB CB A02");model->boneCount=2;
    expect(true,"distinct bones with colliding canonical short names still require an extension");model->boneCount=1;
    assign(morphs[0],L"+FlycloakRootB CB A01");assign(morphs[1],L"+FlycloakRootB CB A02");model->morphCount=2;
    expect(true,"distinct morphs with colliding canonical short names still require an extension");
    model->morphKeys=mdl::MorphKeyPoolCreate(2);
    expect(false,"default zero-weight morph collisions do not require an extension");
    model->morphKeys[0].value=0.01f;
    expect(true,"a frame-zero morph weight makes its collision group significant");
    model->morphKeys[0].value=0;model->morphKeys[1].next=1;
    expect(true,"subsequent morph keys prevent the default-only exemption");
    mdl::MorphKeyPoolDestroy(model->morphKeys);model->morphKeys=nullptr;
    model->boneCount=1;model->morphCount=1;
    const auto actualMirror=[](auto& record,const std::wstring& name){
        Check(extended_names::Assign(record.jpText,name),"allocate a name for the canonical PMX mirror producer");
        text_encoding::EncodePmxName(record.name,sizeof record.name,name.c_str());
    };
    for(const wchar_t* name:{L"single\U0001f31f",L"\u6c49"}){
        actualMirror(bones[0],name);actualMirror(morphs[0],name);
        expect(false,"unique actual PMX Unicode mirrors are restorable, independent of the Windows ACP");
        char oldToken[20]{};text_encoding::EncodePmxMirror(oldToken,sizeof oldToken,name);
        if(std::strcmp(oldToken,bones[0].name)!=0)
            Check(!save_compatibility::InspectPmm(*app).canCopyLegacy,
                "a new nonempty token cannot claim original-MMD compatibility when its old loader identity differs");
        std::strcpy(bones[0].name,"fabricated-token");expect(true,"a fabricated Unicode bone alias cannot be accepted as a real loader mirror");
        actualMirror(bones[0],name);
        std::strcpy(morphs[0].name,"fabricated-token");expect(true,"a fabricated Unicode morph alias cannot be accepted as a real loader mirror");
    }
    // Distinct full strings, rather than bytes selected from different code
    // pages, determine whether one raw binding identity is ambiguous.
    bool mixedFound=false;
    for(unsigned lead=0x81;lead<=0xfe&&!mixedFound;++lead)for(unsigned trail=0x40;trail<=0xfe&&!mixedFound;++trail){
        if(trail==0x7f)continue;
        const char raw[]={static_cast<char>(lead),static_cast<char>(trail),0};
        std::wstring jp,alternate;std::string firstBytes,secondBytes,not932;
        if(!text_encoding::Decode(raw,932,jp)||!text_encoding::EncodeExact(jp.c_str(),932,firstBytes)||firstBytes!=std::string(raw)||
            !text_encoding::Decode(raw,CP_ACP,alternate)||jp==alternate||text_encoding::EncodeExact(alternate.c_str(),932,not932)||
            !text_encoding::EncodeExact(alternate.c_str(),CP_ACP,secondBytes)||firstBytes!=secondBytes)continue;
        model->boneCount=2;model->morphCount=0;
        Check(extended_names::Assign(bones[0].jpText,jp)&&extended_names::Assign(bones[1].jpText,alternate),"allocate distinct mixed-code-page full names");
        std::memcpy(bones[0].name,raw,sizeof raw);std::memcpy(bones[1].name,raw,sizeof raw);
        expect(true,"identical encoded bytes under two different code pages do not make distinct Unicode names identical");
        mixedFound=true;
    }
    if(!mixedFound)std::printf("LIMIT current ACP=%u has no tested two-byte mixed-code-page collision; no such coverage claimed\n",GetACP());
    app->ModelSlot(0)=nullptr;
    for(auto* p:model->pmxTextBuffers)owned_text::Free(p);
    for(auto& bone:bones){owned_text::Free(bone.jpText);owned_text::Free(bone.enText);}
    for(auto& morph:morphs){owned_text::Free(morph.jpText);owned_text::Free(morph.enText);}
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
                               L"CollisionPrefixABCDEFG1", L"CollisionPrefixABCDEFG2"},
                           const std::vector<std::wstring>& morphNames = {},
                           const std::wstring& nativeTitle = L"Boundary",
                           const std::wstring& englishTitle = L"Boundary",
                           // PMX 2.0 bone flag word. The zero default keeps every
                           // earlier fixture byte for byte identical; only the
                           // duplicate-paste fixture passes real rotate/translate
                           // flags so the loader produces movable bones.
                           std::uint16_t boneFlags = 0) {
    std::ofstream file(path, std::ios::binary);
    file.write("PMX ", 4);
    PutPmx(file, 2.0f);
    const unsigned char header[8] = {0, 0, 1, 1, 1, 1, 1, 1};  // UTF-16, 1-byte indices
    PutPmx(file, static_cast<unsigned char>(sizeof header));
    file.write(reinterpret_cast<const char*>(header), sizeof header);
    PutPmxText(file, nativeTitle); PutPmxText(file, englishTitle);
    PutPmxText(file, L""); PutPmxText(file, L"");
    PutPmx(file, std::int32_t(0));  // vertices
    PutPmx(file, std::int32_t(0));  // surface indices
    PutPmx(file, std::int32_t(0));  // texture table
    PutPmx(file, std::int32_t(0));  // materials
    PutPmx(file, static_cast<std::int32_t>(names.size()));  // bones
    for (std::size_t i = 0; i < names.size(); ++i) {
        PutPmxText(file, names[i]);
        PutPmxText(file, L"en");
        PutPmx(file, float(i)); PutPmx(file, 0.0f); PutPmx(file, 0.0f);
        PutPmx(file, static_cast<signed char>(-1));  // parent
        PutPmx(file, std::int32_t(0));               // layer
        PutPmx(file, boneFlags);                     // flags
        PutPmx(file, 0.0f); PutPmx(file, 1.0f); PutPmx(file, 0.0f);  // tail offset
    }
    PutPmx(file, static_cast<std::int32_t>(morphNames.size()));
    for (const auto& name : morphNames) {
        PutPmxText(file, name); PutPmxText(file, L"en");
        PutPmx(file, static_cast<unsigned char>(1)); // eyebrow panel
        PutPmx(file, static_cast<unsigned char>(1)); // vertex morph
        PutPmx(file, std::int32_t(0));               // no geometry entries
    }
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
    // Retain main b559a8b's ten measured cases and add ACP-only names. The
    // VMD width must not force PMM 0003 or lose bone/morph key data.
    const std::vector<std::wstring> names{
        std::wstring(15, L'B'), std::wstring(16, L'B'), std::wstring(19, L'B'),
        std::wstring(20, L'B'), std::wstring(21, L'B'),
        std::wstring(7, L'あ') + L"A", std::wstring(8, L'あ'),
        std::wstring(9, L'あ') + L"A", std::wstring(10, L'あ'), L"骨\U0001f31f",
        L"\u6c49",L"bone\u6c49"};
    for (std::size_t i = 0; i < names.size(); ++i) {
        ResetAppState(&app);
        const auto stem = L"measured-name-" + std::to_wstring(i);
        const auto pmx = directory / (stem + L".pmx");
        const auto pmm = directory / (stem + L".pmm");
        WritePmmNameBoundaryPmx(pmx, {names[i]}, {names[i]});
#if MMD_ORIGINAL_PROFILE
        std::string vmdName;
        const bool warning=!text_encoding::EncodeExact(names[i].c_str(),932,vmdName) || vmdName.size()>15;
        WithOriginalNameWarning(app,warning,[&]{LoadModelFile(&app,pmx.c_str(),false);});
#else
        LoadModelFile(&app, pmx.c_str(), false);
#endif
        Check(app.ModelSlot(0) != nullptr, "load a measured name through the production PMX loader");
        auto& model = *mdl::Mdl(app.ModelSlot(0));
        Check(model.boneCount == 1 && model.morphCount == 1, "measured-name fixture has one bone and one morph");
        const auto report = save_compatibility::InspectPmm(app);
        Check(!report.invalidData && !report.needsNewPmm, "measured names alone never upgrade PMM to v3");
        Check(!report.namesNeedExtension,
            "actual canonical CP932, ACP-only and emoji mirrors need no warning");
        const std::string mirror(model.boneTable[0].name);
        const std::string morphMirror(model.morphs[0].name);
        model.boneKeys[0].position[0] = float(i + 1);
        const float morphValue = .125f * float(i % 7 + 1);
        model.morphKeys[0].value = morphValue;
#if MMD_ORIGINAL_PROFILE
        std::string exact;
        const bool originalIdentity=text_encoding::EncodeExact(names[i].c_str(),932,exact) &&
            exact.size()<=20 && exact==mirror && exact==morphMirror;
        if(!originalIdentity){
            Check(!SaveSceneCopy(&app,pmm.c_str()) && !std::filesystem::exists(pmm),
                "original profile refuses non-CP932 or differing full identities without publishing a file");
            continue;
        }
#endif
        Check(SaveSceneCopy(&app, pmm.c_str()), "save the measured-name project");
        const auto document=Read(pmm); // Backups retain an extension container even in the original profile.
#if MMD_ORIGINAL_PROFILE
        Check(ext::Find(document,"org.mikumikustudio.model.0.unicode.v1")==nullptr,
            "original profile backup has no model Unicode extension payload");
        // CaptureScene skips project metadata in this profile, but every
        // backup still records its naming stem, filename and ownership marker.
        const auto* base=ext::Find(document,save_naming::BaseKey);
        const auto* file=ext::Find(document,save_naming::FileKey);
        const auto* owner=ext::Find(document,autosave::kOwnershipKey);
        const auto filename=pmm.filename().u8string();
        Check(document.records.size()==3 && base && !base->value.empty() && file && owner &&
            std::string(file->value.begin(),file->value.end())==filename &&
            std::string(owner->value.begin(),owner->value.end())==filename,
            "original backup retains exactly its naming and ownership records with the correct filename");
#else
        Check(ext::Find(document,"org.mikumikustudio.model.0.unicode.v1")!=nullptr,
            "a no-warning decision never suppresses the real Unicode extension");
#endif
#if MMD_ORIGINAL_PROFILE
        WithOriginalNameWarning(app,warning,[&]{Load(app,pmm);});
#else
        Load(app, pmm);
#endif
        Check(app.ModelSlot(0) != nullptr, "reload the measured-name PMM");
        const auto& restored = *mdl::Mdl(app.ModelSlot(0));
        Check(restored.boneCount == 1 && restored.morphCount == 1 &&
            text_encoding::BoneName(restored.boneTable[0], false) == names[i] &&
            text_encoding::MorphName(restored.morphs[0],false)==names[i] &&
            mirror == restored.boneTable[0].name && morphMirror==restored.morphs[0].name &&
            restored.boneKeys[0].position[0] == float(i + 1) && restored.morphKeys[0].value==morphValue,
            "PMM v2 roundtrip retains complete bone/morph names, original mirrors and distinct keyed values");
    }
    ResetAppState(&app);
#if MMD_ORIGINAL_PROFILE
    std::puts("PASS twelve measured names: original CP932 identities roundtrip and unsupported identities refuse without publishing");
#else
    std::puts("PASS twelve measured bone/morph cases keep PMM v2 and survive actual MMS save/load");
#endif
}
void PmxNameBoundaries(MMDApp& app, const std::filesystem::path& directory) {
    const auto pmx = directory / L"boundary-bones.pmx";
    WritePmmNameBoundaryPmx(pmx);
#if MMD_ORIGINAL_PROFILE
    WithOriginalNameWarning(app,true,[&]{LoadModelFile(&app,pmx.c_str(),false);});
#else
    LoadModelFile(&app, pmx.c_str(), false);
#endif
    auto* raw = app.ModelSlot(0);
    Check(raw != nullptr, "load the boundary PMX through the real PMX loader");
    auto& model = *mdl::Mdl(raw);
    Check(model.boneCount == 6, "boundary PMX supplies six bones");
    const auto originalBoneCount = model.boneCount;
    const auto originalGroups = mdl::DisplayGroupCount(model), originalFaces = mdl::FacialFrameCount(model);
    const auto originalMaxFrame = model.maxFrame;
    const std::wstring originalPath = model.path;
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
    Check(!report.namesNeedExtension && !report.needsNewPmm,
          "a unique actual Unicode source mirror is restored without warning and never selects v3");
    model.boneCount = 6;
    report = inspect();
    Check(!report.namesNeedExtension,
          "unused colliding bones with default initial keys do not block ordinary PMM");
    const auto initial=model.boneKeys[4];
    model.boneKeys[4].position[0]=0.001f;
    Check(inspect().namesNeedExtension,"even a small frame-zero position edit protects the entire collision group");
    model.boneKeys[4]=initial;model.boneKeys[5].physicsDisabled=1;
    Check(inspect().namesNeedExtension,"physics disable on a colliding bone must be preserved");
    model.boneKeys[5].physicsDisabled=0;model.boneKeys[5].rotation[0]=0.1f;
    Check(inspect().namesNeedExtension,"frame-zero rotation on a colliding bone must be preserved");
    model.boneKeys[5]=initial;model.boneKeys[5].interpolation[0]=21;
    Check(inspect().namesNeedExtension,"edited interpolation is not treated as an untouched initial key");
    model.boneKeys[5]=initial;model.boneKeys[5].next=4;
    Check(inspect().namesNeedExtension,"a subsequent key link prevents the default-only exemption");
    model.boneKeys[5]=initial;model.boneKeys[4].position[0]=1;
    report = inspect();
    Check(report.namesNeedExtension && !report.needsNewPmm && !report.canCopyLegacy,
          "distinct full names sharing one mirror need the extension and cannot be copied");
    model.boneCount = 2;
    const auto canonical = directory / L"boundary-canonical.pmm";
#if MMD_ORIGINAL_PROFILE
    Check(!SaveSceneCopy(&app,canonical.c_str()) && !std::filesystem::exists(canonical),
        "original profile refuses a full name different from its truncated token without publishing");
#else
    Check(SaveSceneCopy(&app, canonical.c_str()), "save the canonical boundary project");
    int fd = Open(canonical, _O_RDONLY);
    char header30[30]{};
    Check(_read(fd, header30, 30) == 30, "read the canonical PMM header");
    _close(fd);
    Check(std::memcmp(header30, "Polygon Movie maker 0002", 24) == 0,
          "canonical model names keep the PMM 0002 header");
#endif

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
    Check(!inspect().needsNewPmm, "a reversible non-CP932 model path stays PMM v2");
    // Restore the actual allocation counts before normal scene ownership
    // cleanup; the ABI destructor must release every loaded bone's text.
    model.boneCount = originalBoneCount;
    model.maxFrame = originalMaxFrame;
    mdl::SetDisplayCapacity(model, originalGroups, originalFaces);
    wcscpy_s(model.path, originalPath.c_str());
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
    Check(extended_names::Assign(morphs[0].enText,L"Expression caption \U0001f31f"),"set a Unicode morph display caption");
    text_encoding::SetDisplayGroupName(*model,groups[0],L"分组 \U0001f31f",L"Display group \U0001f31f");
    Check(extended_names::Assign(model->pmxTextBuffers[0],L"A model caption longer than twenty bytes"),"set a length-prefixed PMM model caption");
    const auto ordinary=save_compatibility::InspectPmm(*app);
    Check(!ordinary.needsNewPmm&&!ordinary.namesNeedExtension,"English captions and display-group metadata do not trigger a PMM compatibility warning");
    Check(extended_names::Assign(model->pmxTextBuffers[0],std::wstring(255,L'a')),"set the largest PMM model caption");
    Check(!save_compatibility::InspectPmm(*app).namesNeedExtension,"255 encoded caption bytes fit the PMM length field");
    Check(extended_names::Assign(model->pmxTextBuffers[0],std::wstring(256,L'a')),"set an overflowing PMM model caption");
    Check(save_compatibility::InspectPmm(*app).namesNeedExtension,"256 encoded caption bytes still require extension data");
    Check(extended_names::Assign(model->pmxTextBuffers[0],L"model"),"restore a compatible model caption");
    Check(extended_names::Assign(model->pmxTextBuffers[1],L"Model \U0001f31f"),"set an unencodable model caption");
    Check(!save_compatibility::InspectPmm(*app).namesNeedExtension,"a fitting marked UTF-8 model caption needs no compatibility warning");
    Check(extended_names::Assign(model->pmxTextBuffers[1],L"model"),"restore a compatible English model caption");
    Check(extended_names::Assign(morphs[0].jpText,L"different"),"set a real morph binding alias");
    Check(save_compatibility::InspectPmm(*app).namesNeedExtension,"changed morph binding identity still requires extension data");
    Check(extended_names::Assign(morphs[0].jpText,L"face"),"restore exact morph binding identity");
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
    for(auto* p:model->pmxTextBuffers)owned_text::Free(p);
    for(auto& bone:bones){owned_text::Free(bone.jpText);owned_text::Free(bone.enText);}
    for(auto& morph:morphs){owned_text::Free(morph.jpText);owned_text::Free(morph.enText);}
    std::puts("PASS automatic Unicode metadata, display counts and legacy-first format selection");
}

std::vector<unsigned char> EncodingFileBytes(const std::filesystem::path& path) {
    const int fd=Open(path,_O_RDONLY);const auto length=_filelengthi64(fd);
    Check(length>=0&&length<128*1024*1024,"owned encoding fixture has a bounded readable size");
    std::vector<unsigned char> bytes(static_cast<size_t>(length));
    Check(bytes.empty()||_read(fd,bytes.data(),static_cast<unsigned>(bytes.size()))==static_cast<int>(bytes.size()),"read every byte of the owned encoding fixture");
    _close(fd);return bytes;
}
struct EncodingPrefix {
    std::array<std::string,2> captions;
    std::array<char,256> path{};
    std::vector<std::string> bones,morphs;
};
EncodingPrefix ReadEncodingPrefix(const std::filesystem::path& path) {
    const auto bytes=EncodingFileBytes(path);
    Check(bytes.size()>56&&std::memcmp(bytes.data(),"Polygon Movie maker 0002",24)==0,
        "real reversible-encoding output retains the PMM 0002 standard header");
    // Independent standard layout: 30-byte header, four int32s, seven option
    // bytes, selected model, model count; then slot byte and both captions.
    Check(bytes[54]==1&&bytes[55]==0,"parse only the one-model slot-zero fixture's actual standard block");
    size_t at=56;
    const auto text=[&]() {
        Check(at<bytes.size(),"standard length-prefixed name has a length byte");const auto count=bytes[at++];
        Check(count<=bytes.size()-at,"standard name bytes remain inside the real PMM");
        std::string result(reinterpret_cast<const char*>(bytes.data()+at),count);at+=count;return result;
    };
    EncodingPrefix prefix;for(auto& name:prefix.captions)name=text();
    Check(prefix.path.size()<=bytes.size()-at,"standard model path retains its entire fixed 256-byte field");
    std::memcpy(prefix.path.data(),bytes.data()+at,prefix.path.size());at+=prefix.path.size();
    Check(at<bytes.size(),"standard model group count exists");++at;
    const auto u32=[&]() {
        Check(bytes.size()-at>=4,"standard name table has a complete count");uint32_t value=0;
        for(unsigned i=0;i<4;++i)value|=uint32_t(bytes[at++])<<(i*8);return value;
    };
    for(auto* names:{&prefix.bones,&prefix.morphs}){
        const auto count=u32();Check(count<256&&count<=bytes.size()-at,"owned standard name table has a bounded count");
        for(uint32_t i=0;i<count;++i)names->push_back(text());
    }
    return prefix;
}

void PmmCaptionReadBoundaries(MMDApp& app,const std::filesystem::path& directory) {
    for(unsigned width:{21u,255u}){
        ResetAppState(&app);
        const std::wstring native(width,L'A'),english(width,L'B');
        const auto stem=L"pmm-caption-"+std::to_wstring(width);
        const auto source=directory/(stem+L".pmx"),project=directory/(stem+L".pmm");
        WritePmmNameBoundaryPmx(source,{L"bone"},{L"face"},native,english);
        const auto sourceBefore=EncodingFileBytes(source);
#if MMD_ORIGINAL_PROFILE
        // Model captions exceed the PMD/VMD header width, but fit PMM's
        // independent uint8-length fields. The warning does not reject import.
        WithOriginalNameWarning(app,true,[&]{LoadModelFile(&app,source.c_str(),false);});
#else
        LoadModelFile(&app,source.c_str(),false);
#endif
        Check(app.ModelSlot(0)!=nullptr,"load owned source with two complete PMM caption boundary strings");
        auto& model=*mdl::Mdl(app.ModelSlot(0));
        Check(text_encoding::ModelName(model,false)==native && text_encoding::ModelName(model,true)==english,
            "source retains both full captions without truncating to native model-name mirrors");
        model.boneKeys[0].position[0]=12.25f;model.morphKeys[0].value=.375f;
        app.CameraKeys()[0].distance=-81.f;
        Check(SaveSceneCopy(&app,project.c_str()),"save both model captions at the PMM field boundary");
        const auto prefix=ReadEncodingPrefix(project);
        Check(prefix.captions[0]==std::string(width,'A') && prefix.captions[1]==std::string(width,'B'),
            "real PMM bytes contain both exact 21/255-byte captions, not truncated substitutes");
        std::uint64_t standard=0;Read(project,&standard);
        const auto saved=EncodingFileBytes(project);
#if MMD_ORIGINAL_PROFILE
        for(unsigned language=0;language<2;++language){
            for(const auto& unsupported:{std::wstring(256,L'X'),std::wstring(L"caption\U0001f31f")}){
                Check(extended_names::Assign(model.pmxTextBuffers[language],unsupported),"prepare an unsupported original PMM caption");
                Check(!SaveSceneCopy(&app,project.c_str()) && EncodingFileBytes(project)==saved,
                    "original writer rejects a 256-byte or non-CP932 caption without replacing the valid project");
            }
            Check(extended_names::Assign(model.pmxTextBuffers[language],language?english:native),"restore exact supported caption before reload");
        }
        WithOriginalNameWarning(app,true,[&]{Load(app,project);});
#else
        Load(app,project);
#endif
        Check(app.ModelSlot(0)!=nullptr,"reload a PMM containing both caption boundary fields");
        const auto& restored=*mdl::Mdl(app.ModelSlot(0));
        Check(text_encoding::ModelName(restored,false)==native && text_encoding::ModelName(restored,true)==english &&
            restored.boneKeys[0].position[0]==12.25f && restored.morphKeys[0].value==.375f &&
            app.CameraKeys()[0].distance==-81.f,
            "caption boundary PMM reload preserves complete source titles and bone/morph/camera key data");
        Check(EncodingFileBytes(source)==sourceBefore,"PMM caption tests never modify their source PMX");

        // Exercise the independent invalid-slot skip branch using the actual
        // standard body. Drop extensions deliberately; the file under test is
        // a standard PMM with one out-of-range slot, followed by camera data.
        auto skipped=saved;
        Check(standard>56 && standard<=skipped.size() && skipped[54]==1 && skipped[55]==0,
            "owned skip fixture starts with exactly one model in slot zero");
        skipped.resize(static_cast<std::size_t>(standard));skipped[55]=255;
        const auto skip=directory/(stem+L"-skip.pmm");
        Check(!std::filesystem::exists(skip),"caption skip fixture never overwrites a file");
        std::ofstream output(skip,std::ios::binary);output.write(reinterpret_cast<const char*>(skipped.data()),static_cast<std::streamsize>(skipped.size()));
        output.close();Check(bool(output),"write isolated standard caption-skip fixture");
#if MMD_ORIGINAL_PROFILE
        WithOriginalNameWarning(app,false,[&]{Load(app,skip);});
#else
        Load(app,skip);
#endif
        for(int slot=0;slot<kModelSlotCount;++slot)Check(app.ModelSlot(slot)==nullptr,
            "invalid-slot caption path leaves no partially loaded model");
        Check(app.CameraKeys()[0].distance==-81.f,"caption skip consumes all model bytes and preserves following camera data");
        std::printf("PASS PMM captions native/English %u bytes: exact wire fields, roundtrip and invalid-slot skip\n",width);
    }
    ResetAppState(&app);
}

#if !MMD_ORIGINAL_PROFILE
void RealUnicodeBindingRoundtrip(MMDApp& app,const std::filesystem::path& directory) {
    const std::array<std::wstring,2> originals{L"source\U0001f31f",L"\u6c49"};
    const std::array<std::wstring,2> edited{L"persisted\U0001f680",L"\u6c49-edited"};
    std::printf("ENCODING_ENV ACP=%u CRT=%s\n",GetACP(),std::setlocale(LC_CTYPE,nullptr));
    for(size_t which=0;which<originals.size();++which){
        ResetAppState(&app);
        if(which==1){std::string legacy,acp;
            Check(!text_encoding::EncodeExact(originals[which].c_str(),932,legacy),"the Han source fixture is genuinely outside exact CP932");
            if(text_encoding::EncodeExact(originals[which].c_str(),CP_ACP,acp))std::printf("PROOF current ACP=%u exactly encodes the Han fixture while CP932 cannot\n",GetACP());
            else std::printf("LIMIT current ACP=%u cannot encode Han either; this run covers its Unicode path, not an ACP-only positive\n",GetACP());}
        const auto pmx=directory/(L"unicode-binding-source-"+std::to_wstring(which)+L".pmx");
        const auto pmm=directory/(L"unicode-binding-saved-"+std::to_wstring(which)+L".pmm");
        WritePmmNameBoundaryPmx(pmx,{originals[which]},{originals[which]});
        const auto sourceBytes=EncodingFileBytes(pmx);
        LoadModelFile(&app,pmx.c_str(),false);Check(app.ModelSlot(0)!=nullptr,"load actual Unicode source PMX for binding roundtrip");
        auto& model=*mdl::Mdl(app.ModelSlot(0));Check(model.boneCount==1&&model.morphCount==1,"actual Unicode fixture exposes both identity domains");
        const std::string boneMirror=model.boneTable[0].name,morphMirror=model.morphs[0].name;
        const auto sourceReport=save_compatibility::InspectPmm(app);
        Check(!sourceReport.namesNeedExtension,"actual single emoji or ACP-only source needs no compatibility warning");
        char legacyMirror[20]{};text_encoding::EncodePmxMirror(legacyMirror,sizeof legacyMirror,originals[which].c_str());
        if(boneMirror!=legacyMirror)Check(!sourceReport.canCopyLegacy,
            "complete Unicode PMM remains valid while a divergent original-loader token forbids a misleading compatibility copy");
        std::strcpy(model.boneTable[0].name,"forged-token");
        Check(save_compatibility::InspectPmm(app).namesNeedExtension,"replacing an actual loaded Unicode bone mirror with an arbitrary token still warns");
        std::memset(model.boneTable[0].name,0,sizeof model.boneTable[0].name);std::memcpy(model.boneTable[0].name,boneMirror.data(),boneMirror.size());
        std::strcpy(model.morphs[0].name,"forged-token");
        Check(save_compatibility::InspectPmm(app).namesNeedExtension,"replacing an actual loaded Unicode morph mirror with an arbitrary token still warns");
        std::memset(model.morphs[0].name,0,sizeof model.morphs[0].name);std::memcpy(model.morphs[0].name,morphMirror.data(),morphMirror.size());
        char boneExpected[20]{},morphExpected[20]{};
        text_encoding::EncodePmxName(boneExpected,sizeof boneExpected,originals[which].c_str());
        text_encoding::EncodePmxName(morphExpected,sizeof morphExpected,originals[which].c_str());
        Check(boneMirror==boneExpected&&morphMirror==morphExpected,
            "current PMX producer emits the exact nonempty source binding mirror");
        Check(extended_names::Assign(model.boneTable[0].jpText,edited[which])&&extended_names::Assign(model.morphs[0].jpText,edited[which]),
            "edit complete Unicode metadata without editing the original binding token or source file");
        model.boneKeys[0].position[0]=float(81+which);model.morphKeys[0].value=.375f+.125f*float(which);
        const auto report=save_compatibility::InspectPmm(app);
        Check(!report.invalidData&&!report.needsNewPmm&&report.namesNeedExtension,"edited Unicode captions retain v2 but explicitly require extension metadata");
        Check(SaveSceneCopy(&app,pmm.c_str()),"save actual edited Unicode metadata and native bone/morph key data");
        const auto prefix=ReadEncodingPrefix(pmm);
        Check(prefix.bones==std::vector<std::string>{boneMirror}&&prefix.morphs==std::vector<std::string>{morphMirror},
            "standard PMM writes exactly the unchanged real source bone and morph identity bytes");
        const auto document=Read(pmm);
        Check(ext::Find(document,"org.mikumikustudio.model.0.unicode.v1")!=nullptr,"complete Unicode metadata is present for the edited captions");
        Load(app,pmm);Check(app.ModelSlot(0)!=nullptr,"reload actual Unicode PMM from its original source file");
        const auto& restored=*mdl::Mdl(app.ModelSlot(0));
        Check(text_encoding::BoneName(restored.boneTable[0],false)==edited[which]&&text_encoding::MorphName(restored.morphs[0],false)==edited[which]&&
            boneMirror==restored.boneTable[0].name&&morphMirror==restored.morphs[0].name&&
            restored.boneKeys[0].position[0]==float(81+which)&&restored.morphKeys[0].value==.375f+.125f*float(which),
            "MMS extension actually restores edited names, original mirrors and keyed values after source reload");
        Check(EncodingFileBytes(pmx)==sourceBytes,"the original PMX remains byte-for-byte unchanged by PMM save/load");
        if(which==0){
            auto oldMarker=EncodingFileBytes(pmm);
            Check(oldMarker.size()>=32&&std::memcmp(oldMarker.data()+oldMarker.size()-32,"MMSXPMM2",8)==0,"positive control actually has the MMS trailer");
            oldMarker[oldMarker.size()-31]='D';
            const auto mds=directory/L"unicode-binding-unknown-mds.pmm";
            Check(!std::filesystem::exists(mds),"MDS negative fixture does not overwrite an existing file");
            std::ofstream out(mds,std::ios::binary|std::ios::out);out.write(reinterpret_cast<const char*>(oldMarker.data()),static_cast<std::streamsize>(oldMarker.size()));out.close();Check(bool(out),"write owned unknown-marker negative fixture");
            int fd=Open(mds,_O_RDONLY);ext::Document ignored;uint64_t boundary=0xdeadbeef;
            Check(ext::Read(fd,ignored,boundary)==ext::ReadResult::Absent&&ignored.records.empty()&&boundary==0xdeadbeef,"otherwise valid MDS framing is never accepted as MMS Unicode metadata");_close(fd);
            Load(app,mds);Check(app.ModelSlot(0)!=nullptr,"unknown MDS trailer still permits ordinary standard source model loading");
            const auto& withoutExtension=*mdl::Mdl(app.ModelSlot(0));
            Check(text_encoding::BoneName(withoutExtension.boneTable[0],false)==originals[which]&&
                text_encoding::MorphName(withoutExtension.morphs[0],false)==originals[which]&&
                boneMirror==withoutExtension.boneTable[0].name&&morphMirror==withoutExtension.morphs[0].name&&
                withoutExtension.boneKeys[0].position[0]==81.f&&withoutExtension.morphKeys[0].value==.375f,
                "unknown MDS tail cannot restore extension-only edits while standard binding and keys still load");
        }
    }
    ResetAppState(&app);
    const auto collisionPmx=directory/L"unicode-colliding-source.pmx",collisionPmm=directory/L"unicode-colliding-saved.pmm";
    const std::vector<std::wstring> names{L"first\U0001f31f",L"second\U0001f680"};
    WritePmmNameBoundaryPmx(collisionPmx,names,names);LoadModelFile(&app,collisionPmx.c_str(),false);
    Check(app.ModelSlot(0)!=nullptr,"load real PMX with two different Unicode names");auto& collision=*mdl::Mdl(app.ModelSlot(0));
    Check(collision.boneCount==2&&collision.morphCount==2&&collision.boneTable[0].name[0]!=0&&collision.boneTable[1].name[0]!=0&&
        collision.morphs[0].name[0]!=0&&collision.morphs[1].name[0]!=0,"Unicode identities remain nonempty independently of the CRT locale");
    Check(std::strcmp(collision.boneTable[0].name,collision.boneTable[1].name)!=0 &&
        std::strcmp(collision.morphs[0].name,collision.morphs[1].name)!=0,"distinct short Unicode names no longer collapse into empty identities");
    auto report=save_compatibility::InspectPmm(app);
    Check(!report.invalidData&&!report.needsNewPmm&&!report.namesNeedExtension,"unchanged unique Unicode tokens need no name extension warning");
    collision.boneKeys[0].position[0]=31;collision.boneKeys[1].position[0]=47;collision.morphKeys[0].value=.25f;collision.morphKeys[1].value=.75f;
    Check(SaveSceneCopy(&app,collisionPmm.c_str()),"complete MMS project can preserve colliding names without pretending they are legacy-safe");Read(collisionPmm);Load(app,collisionPmm);
    Check(app.ModelSlot(0)!=nullptr,"reload colliding-name PMM against the same unchanged source model");const auto& restored=*mdl::Mdl(app.ModelSlot(0));
    Check(text_encoding::BoneName(restored.boneTable[0],false)==names[0]&&text_encoding::BoneName(restored.boneTable[1],false)==names[1]&&
        text_encoding::MorphName(restored.morphs[0],false)==names[0]&&text_encoding::MorphName(restored.morphs[1],false)==names[1]&&
        restored.boneKeys[0].position[0]==31&&restored.boneKeys[1].position[0]==47&&restored.morphKeys[0].value==.25f&&restored.morphKeys[1].value==.75f,
        "same-source MMS reload retains two complete Unicode names and independently keyed bone/morph values");
    auto& oldModel=*mdl::Mdl(app.ModelSlot(0));
    for(unsigned i=0;i<2;++i){
        text_encoding::EncodePmxMirror(oldModel.boneTable[i].name,20,names[i].c_str());
        text_encoding::EncodePmxMirror(oldModel.morphs[i].name,20,names[i].c_str());
    }
    Check(oldModel.boneTable[0].name[0]==0&&oldModel.boneTable[1].name[0]==0,
        "legacy source fixture actually reproduces duplicate empty tokens under the test locale");
    const auto oldPmm=directory/L"unicode-old-empty-identities.pmm";
    Check(SaveSceneCopy(&app,oldPmm.c_str()),"save old-token migration fixture without editing the source PMX");
    Load(app,oldPmm);const auto& migrated=*mdl::Mdl(app.ModelSlot(0));
    Check(migrated.boneKeys[0].position[0]==31&&migrated.boneKeys[1].position[0]==47&&
        migrated.morphKeys[0].value==.25f&&migrated.morphKeys[1].value==.75f,
        "same-size same-index legacy matching preserves distinct keys sharing old empty identities");
    ResetAppState(&app);std::puts("PASS actual Unicode binding, ACP-only metadata, collisions and MMS-only restore");
}

void RealCaptionEncodingRoundtrip(MMDApp& app,const std::filesystem::path& directory) {
    ResetAppState(&app);const auto pmx=directory/L"caption-encoding-source.pmx";
    WritePmmNameBoundaryPmx(pmx,{L"bone"},{L"face"},L"Source native",L"Source English");LoadModelFile(&app,pmx.c_str(),false);
    Check(app.ModelSlot(0)!=nullptr,"load real PMX for standard caption byte checks");
    const size_t tagBytes=GetACP()==CP_UTF8?0:3;
    const std::wstring largestUtf8=L"\U0001f31f"+std::wstring(255-tagBytes-4,L'A');
    const std::vector<std::wstring> names{std::wstring(255,L'A'),std::wstring(256,L'A'),L"Caption \u6c49",L"Caption \U0001f31f",largestUtf8,largestUtf8+L"A"};
    for(size_t i=0;i<names.size();++i){
        auto& model=*mdl::Mdl(app.ModelSlot(0));const std::array<std::string,2> raw{model.name,model.nameEn};
        auto english=names[i];if(!english.empty()&&english.back()==L'A')english.back()=L'B';
        Check(extended_names::Assign(model.pmxTextBuffers[0],names[i])&&extended_names::Assign(model.pmxTextBuffers[1],english),"set distinct complete native/English caption metadata");
        std::array<char,256> nativeBytes{},englishBytes{};
        const bool nativeFits=text_encoding::EncodeDisplay(nativeBytes.data(),nativeBytes.size(),names[i].c_str());
        const bool englishFits=text_encoding::EncodeDisplay(englishBytes.data(),englishBytes.size(),english.c_str());
        if(i==0)Check(nativeFits&&std::strlen(nativeBytes.data())==255,"exactly 255 ASCII caption bytes fit before the NUL");
        if(i==1)Check(!nativeFits,"256 ASCII caption bytes cannot wrap the one-byte standard length");
        if(i==4)Check(nativeFits&&std::strlen(nativeBytes.data())==255,"the marked UTF-8 payload plus its actual BOM budget fits exactly 255 bytes");
        if(i==5)Check(!nativeFits,"one byte beyond the UTF-8/BOM caption budget requires extension-only storage");
        const auto report=save_compatibility::InspectPmm(app);
        Check(!report.invalidData&&!report.needsNewPmm&&report.namesNeedExtension==(!nativeFits||!englishFits),"caption decision matches actual encoded field size while retaining PMM v2");
        model.boneKeys[0].position[0]=float(101+i);
        const auto pmm=directory/(L"caption-encoding-"+std::to_wstring(i)+L".pmm");
        Check(SaveSceneCopy(&app,pmm.c_str()),"save actual caption metadata at and beyond standard field limits");
        const auto prefix=ReadEncodingPrefix(pmm);
        Check(prefix.captions[0]==(nativeFits?std::string(nativeBytes.data()):raw[0])&&prefix.captions[1]==(englishFits?std::string(englishBytes.data()):raw[1]),
            "both actual standard caption lengths and payload bytes match the selected encoding or original raw fallback");
        if(i==3||i==4){std::string utf8;Check(text_encoding::EncodeExact(names[i].c_str(),CP_UTF8,utf8),"independently encode the Unicode caption payload");
            const std::string marked=(tagBytes?std::string("\xef\xbb\xbf"):std::string{})+utf8;
            Check(prefix.captions[0]==marked,"standard Unicode caption carries exactly its UTF-8 payload and required marker bytes");}
        const auto document=Read(pmm);Check(ext::Find(document,"org.mikumikustudio.model.0.unicode.v1")!=nullptr,"caption overflow never suppresses complete Unicode extension data");
        Load(app,pmm);Check(app.ModelSlot(0)!=nullptr,"reload caption PMM against its original PMX");const auto& loaded=*mdl::Mdl(app.ModelSlot(0));
        Check(text_encoding::ModelName(loaded,false)==names[i]&&text_encoding::ModelName(loaded,true)==english&&loaded.boneKeys[0].position[0]==float(101+i),
            "actual extension reload restores complete captions including standard-overflow text and native key data");
    }
    ResetAppState(&app);std::puts("PASS standard caption bytes, 255/256 limits and actual Unicode/BOM overflow restore");
}

void RealResourcePathEncodingRoundtrip(MMDApp& app,const std::filesystem::path& directory) {
    ResetAppState(&app);const auto assets=directory/L"encoding-assets-\u6c49-\U0001f31f";
    Check(std::filesystem::create_directory(assets),"create a new exclusively owned Unicode resource directory");
    const auto source=assets/L"model-\u6c49-\U0001f680.pmx";
    WritePmmNameBoundaryPmx(source,{L"bone"},{L"face"});LoadModelFile(&app,source.c_str(),false);
    Check(app.ModelSlot(0)!=nullptr,"load an existing source PMX from its actual Unicode filesystem path");
    auto& model=*mdl::Mdl(app.ModelSlot(0));model.boneKeys[0].position[0]=59;model.morphKeys[0].value=.625f;
    std::array<char,256> expected{};Check(text_encoding::EncodePath(expected.data(),expected.size(),model.path),"owned Unicode resource path fits the real fixed path encoder");
    const auto report=save_compatibility::InspectPmm(app);Check(!report.invalidData&&!report.needsNewPmm&&!report.canCopyLegacy,"existing reversible Unicode resource path stays PMM v2 but cannot make an original-MMD copy");
    const auto pmm=directory/L"unicode-resource-roundtrip.pmm";
    Check(SaveSceneCopy(&app,pmm.c_str()),"save a PMM referring to the actual Unicode model resource");
    const auto prefix=ReadEncodingPrefix(pmm);Check(prefix.path==expected,"actual fixed 256-byte standard path is written without substitution or truncation");Read(pmm);Load(app,pmm);
    Check(app.ModelSlot(0)!=nullptr,"actual path resolver reopens the Unicode source resource without a missing-model dialog");
    const auto& loaded=*mdl::Mdl(app.ModelSlot(0));
    Check(std::filesystem::equivalent(std::filesystem::path(loaded.path),source)&&loaded.boneKeys[0].position[0]==59&&loaded.morphKeys[0].value==.625f,
        "reloaded source is the original existing Unicode file and its keyed values are retained");
    ResetAppState(&app);
    const std::wstring base=assets.wstring()+L"\\";std::string utf8Base;
    Check(text_encoding::EncodeExact(base.c_str(),CP_UTF8,utf8Base),"encode the owned Unicode directory for exact boundary construction");
    const size_t tagBytes=GetACP()==CP_UTF8?0:3;
    Check(utf8Base.size()+tagBytes+4<255,"choose a shorter fresh evidence directory so the boundary fixture has a usable filename");
    const auto padding=255-tagBytes-utf8Base.size()-4;
    const std::filesystem::path exactPath(base+std::wstring(padding,L'p')+L".pmx");
    Check(exactPath.wstring().size()<256&&!std::filesystem::exists(exactPath),"the exactly fitting path is a new resource inside both native path and filename limits");
    WritePmmNameBoundaryPmx(exactPath,{L"bone"},{L"face"});LoadModelFile(&app,exactPath.c_str(),false);
    Check(app.ModelSlot(0)!=nullptr,"load an existing PMX whose encoded complete path is exactly 255 bytes");
    auto& exact=*mdl::Mdl(app.ModelSlot(0));exact.boneKeys[0].position[0]=73;
    Check(text_encoding::EncodePath(expected.data(),expected.size(),exact.path)&&std::strlen(expected.data())==255&&expected[255]==0,
        "the fixed 256-byte path field admits 255 data bytes and its terminating NUL");
    const auto exactPmm=directory/L"path-capacity-255.pmm";
    Check(SaveSceneCopy(&app,exactPmm.c_str())&&ReadEncodingPrefix(exactPmm).path==expected,"real save accepts and exactly writes the largest encoded resource path");Read(exactPmm);Load(app,exactPmm);
    Check(app.ModelSlot(0)!=nullptr&&std::filesystem::equivalent(std::filesystem::path(mdl::Mdl(app.ModelSlot(0))->path),exactPath)&&
        mdl::Mdl(app.ModelSlot(0))->boneKeys[0].position[0]==73,"real load resolves the exactly fitting 255-byte reference and restores its key");
    const auto preserved=directory/L"path-capacity-overflow-preserve.pmm";
    Check(SaveSceneCopy(&app,preserved.c_str()),"seed an actual complete PMM/sidecar pair before the rejection check");
    const auto before=EncodingFileBytes(preserved);const auto side=plugins::PluginSidecar(preserved);
    Check(std::filesystem::is_regular_file(side),"valid positive control publishes its actual owned plugin sidecar");const auto sideBefore=EncodingFileBytes(side);
    auto& overflow=*mdl::Mdl(app.ModelSlot(0));const std::wstring original=overflow.path,tooLong=original+L"x";
    Check(tooLong.size()<256,"overflow is in encoded byte capacity rather than the input wchar buffer");
    wcscpy_s(overflow.path,tooLong.c_str());
    Check(!text_encoding::EncodePath(expected.data(),expected.size(),overflow.path),"a 256-data-byte Unicode reference cannot fit the terminating byte");
    const auto refused=save_compatibility::InspectPmm(app);
    Check(refused.needsNewPmm&&!refused.canCopyLegacy,"inspection retains the unrepresentable-path escalation rather than globally cancelling v3 checks");
    Check(!SaveSceneCopy(&app,preserved.c_str())&&EncodingFileBytes(preserved)==before&&EncodingFileBytes(side)==sideBefore,
        "actual save rejects encoded path overflow before replacing either existing project or sidecar");
    wcscpy_s(overflow.path,original.c_str());ResetAppState(&app);
    std::puts("PASS actual Unicode resource resolution, exact 256-field boundary and atomic overflow refusal");
}

void RealResourcePathCollisionRoundtrip(MMDApp& app, const std::filesystem::path& directory) {
    const UINT acp = GetACP();
    if (acp == 932) {
        std::puts("SKIP resource path collision: ACP and the old writer both prefer CP932");
        return;
    }

    // The ASCII prefix and extension avoid reserved device names and trailing dots.
    const auto safeLeaf = [](const std::wstring& leaf) {
        if (leaf.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos) return false;
        for (wchar_t c : leaf)
            if (c < L' ' || (c >= 0x7f && c <= 0x9f)) return false;
        return true;
    };

    std::wstring sourceLeaf, decoyLeaf;
    // Search two-byte payloads, including pairs that are UTF-8 or two
    // single-byte characters. The old writer prefers CP932; the reader
    // probes ACP first. Derive the collision directly from the code pages.
    for (unsigned first = 0x80; first <= 0xfe && sourceLeaf.empty(); ++first) {
        for (unsigned second = 0x40; second <= 0xfe; ++second) {
            std::string bytes = "model-";
            bytes += static_cast<char>(first);
            bytes += static_cast<char>(second);
            bytes += ".pmx";
            std::wstring cp932Leaf, acpLeaf;
            std::string cp932Bytes, acpBytes;
            if (!text_encoding::Decode(bytes.c_str(), 932, cp932Leaf) ||
                !text_encoding::Decode(bytes.c_str(), acp, acpLeaf) ||
                !safeLeaf(cp932Leaf) || !safeLeaf(acpLeaf) ||
                CompareStringOrdinal(cp932Leaf.c_str(), -1, acpLeaf.c_str(), -1, TRUE) == CSTR_EQUAL ||
                !text_encoding::EncodeExact(cp932Leaf.c_str(), 932, cp932Bytes) || cp932Bytes != bytes ||
                !text_encoding::EncodeExact(acpLeaf.c_str(), acp, acpBytes) || acpBytes != bytes)
                continue;
            sourceLeaf = cp932Leaf;
            decoyLeaf = acpLeaf;
            break;
        }
    }
    if (sourceLeaf.empty()) {
        std::printf("SKIP resource path collision: no safe tested CP932/ACP %u leaf pair\n", acp);
        return;
    }

    const auto assets = directory / L"path-collision";
    const auto source = assets / sourceLeaf;
    const auto decoy = assets / decoyLeaf;
    const auto pmm = assets / L"roundtrip.pmm";
    std::string legacySource, legacyDecoy, utf8Source;
    Check(text_encoding::EncodeExact(source.c_str(), 932, legacySource) &&
          text_encoding::EncodeExact(decoy.c_str(), acp, legacyDecoy) &&
          legacySource == legacyDecoy && !text_encoding::HasUtf8Bom(legacySource.c_str()) &&
          legacySource.size() < 256 && source.native().size() < 256 &&
          decoy.native().size() < 256 && pmm.native().size() < 256 &&
          text_encoding::EncodeExact(source.c_str(), CP_UTF8, utf8Source) && utf8Source.size() + 3 < 256,
          "use a short ASCII evidence directory so the complete paths collide and marked UTF-8 fits");

    Check(std::filesystem::create_directory(assets), "create a fresh directory for the colliding resources");
    // Matching bindings let the old reader attach the saved keys to the decoy.
    WritePmmNameBoundaryPmx(source, {L"bone"}, {L"face"}, L"Source", L"Source");
    WritePmmNameBoundaryPmx(decoy, {L"bone"}, {L"face"}, L"Decoy", L"Decoy");
    Check(std::filesystem::is_regular_file(source) && std::filesystem::is_regular_file(decoy) &&
          !std::filesystem::equivalent(source, decoy), "both spellings name distinct existing PMX files");

    Check(ResetAppState(&app), "start the collision fixture in an empty scene");
    LoadModelFile(&app, source.c_str(), false);
    auto* raw = app.ModelSlot(0);
    Check(raw != nullptr, "load the original collision PMX directly");
    auto& model = *mdl::Mdl(raw);
    Check(std::filesystem::equivalent(model.path, source) && model.boneCount == 1 &&
          model.morphCount == 1 && model.boneKeys && model.morphKeys,
          "the collision fixture starts on the source file with both tracks");
    model.boneKeys[0].position[0] = 67.f;
    constexpr std::uint32_t morphFrame = 17;
    constexpr float morphWeight = .625f;
    Check(RegisterMorphKeyAtIndex(raw, 0, morphFrame, morphWeight), "key the source morph at frame 17");

    Check(SaveSceneCopy(&app, pmm.c_str()), "save the real PMM while both colliding PMX files exist");
    Read(pmm);
    Check(ResetAppState(&app) && app.ModelSlot(0) == nullptr, "discard the live source before opening the PMM");
    Load(app, pmm);
    Check(app.ModelSlot(0) != nullptr, "reopen the saved collision PMM");
    const auto& restored = *mdl::Mdl(app.ModelSlot(0));
    Check(std::filesystem::equivalent(restored.path, source),
          "reopened PMM resolves the source PMX even with the ACP-decoded decoy present");
    const auto savedPath = ReadEncodingPrefix(pmm).path;
    Check(text_encoding::HasUtf8Bom(savedPath.data()), "a fitting path collision is written with an explicit UTF-8 marker");
    const auto report = save_compatibility::InspectPmm(app);
    Check(!report.invalidData && !report.needsNewPmm && !report.canCopyLegacy &&
          std::any_of(report.details.begin(), report.details.end(), [&](const std::wstring& detail) {
              return detail.find(sourceLeaf) != std::wstring::npos;
          }), "the compatibility report identifies the actual marked model path and refuses a legacy copy");
    Check(restored.boneCount == 1 && restored.morphCount == 1 && restored.boneKeys && restored.morphKeys,
          "reopened collision PMM has both source tracks");
    const auto morphKey = restored.morphKeys[0].next;
    Check(restored.boneKeys[0].position[0] == 67.f &&
          morphKey != 0 && morphKey < mdl::MorphKeyCapacity(restored.morphKeys) &&
          restored.morphKeys[morphKey].frame == morphFrame && restored.morphKeys[morphKey].value == morphWeight,
          "reopened collision PMM preserves the bone position and frame-17 morph key");
    ResetAppState(&app);
    std::printf("PASS actual PMM resource path collision roundtrip (CP932 / ACP %u)\n", acp);
}

#endif

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
    for(const auto previous:{1u,UINT32_MAX}) {
        track[0].previous=previous;
        expect(false,"initial key deletion sentinel does not invalidate a PMM track");
        const auto initial=track[0];
        const PmmKeyMap<K> compact(track,1,original+1,original);
        const auto written=compact.Remap(track[0]);
        Check(written.previous==0 && written.next==1 && compact.Remap(track[original]).previous==0,
            "PMM compaction ignores the initial sentinel and preserves actual links");
        const PmmKeyMap<K> unchanged(track,1,original+1,original+1);
        Check(unchanged.Remap(track[0]).previous==0 && unchanged.Remap(track[0]).next==original &&
            !std::memcmp(&initial,&track[0],sizeof(K)),
            "ordinary PMM writing clears only the saved sentinel without changing live keys");
    }
    track[0].next=1;
    Check(save_compatibility::InspectPmm(app).invalidData,"initial next still cannot reference an empty slot");
    track[0].next=static_cast<std::uint32_t>(original);track[0].previous=0;
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
    const auto expectPath=[&](bool canCopy,const char* message) {
        const auto report=save_compatibility::InspectPmm(app);
        Check(!report.invalidData&&!report.needsNewPmm&&report.canCopyLegacy==canCopy,message);
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
    expectPath(true,"ASCII model reference stays v2 and permits an original-MMD copy");
    wcscpy_s(model->path,L"D:\\model-\U0001f31f.pmx");expect(false,"reversible non-CP932 model reference stays v2");
    expectPath(false,"Unicode model reference stays v2 but blocks an original-MMD copy");
    app.ModelSlot(254)=nullptr;
    mdl::ForgetDisplayCapacity(*model);
    mdl::BoneKeyPoolDestroy(model->boneKeys);mdl::MorphKeyPoolDestroy(model->morphKeys);mdl::KeyPoolDestroy(model->displayKeys);
    auto accessory=std::make_unique<mdl::AccessoryRecord>();
    Check(!app.AccessorySlot(254),"format audit accessory slot is free");
    auto* savedAccessoryKeys=app.AccessoryKeys(254);
    app.AccessorySlot(254)=accessory.get();app.AccessoryKeys(254)=mdl::KeyPoolCreate<mdl::AccessoryKey>(1);
    TrackFormatBoundaries(app,app.AccessoryKeys(254),10000);
    wcscpy_s(accessory->sourcePath,L"D:\\accessory.x");
    expectPath(true,"ASCII accessory reference stays v2 and permits an original-MMD copy");
    wcscpy_s(accessory->sourcePath,L"D:\\accessory-\U0001f31f.x");expect(false,"reversible non-CP932 accessory reference stays v2");
    expectPath(false,"Unicode accessory reference stays v2 but blocks an original-MMD copy");
    mdl::KeyPoolDestroy(app.AccessoryKeys(254));app.AccessorySlot(254)=nullptr;app.AccessoryKeys(254)=savedAccessoryKeys;
    for(auto* path:{app.WavePath(),app.AviBackgroundPath(),app.PictureBackgroundPath()}) {
        const std::wstring saved=path;
        wcscpy_s(path,256,L"D:\\media.png");
        expectPath(true,"each ASCII Wave/AVI/picture reference stays v2 and permits an original-MMD copy");
        wcscpy_s(path,256,L"D:\\media-\U0001f31f.png");expect(false,"each reversible non-CP932 media reference stays v2");
        expectPath(false,"each Unicode Wave/AVI/picture reference stays v2 but blocks an original-MMD copy");
        wcscpy_s(path,256,saved.c_str());
    }
    for(auto* frame:{&app.CurrentFrame(),&app.TimelineStartFrame()}) {
        const auto saved=*frame;
        *frame=frame_limits::legacyMaximum;expect(false,"exact legacy UI frame selects v2");
        ++*frame;expect(true,"first extended UI frame selects v3");
        *frame=-1;{const auto negative=save_compatibility::InspectPmm(app);Check(!negative.invalidData&&!negative.canCopyLegacy,"negative UI frame is retained by complete MMS projects, not original-MMD copies");}
        *frame=saved;
    }
    const auto last=app.LastRegisteredFrame();
    app.LastRegisteredFrame()=frame_limits::legacyMaximum;expect(false,"exact legacy last frame selects v2");
    ++app.LastRegisteredFrame();expect(true,"extended last frame selects v3");app.LastRegisteredFrame()=last;
    {
        char start[256]{},stop[256]{};
        GetDlgItemTextA(app.MainWindow(),panel::kPlayStartFrameEdit,start,sizeof start);
        GetDlgItemTextA(app.MainWindow(),panel::kPlayStopFrameEdit,stop,sizeof stop);
        app.LastRegisteredFrame()=600;
        for(const auto& range:std::array<std::array<const char*,2>,3>{{{{"0","-1"}},{{"100","-5"}},{{"100","5"}}}}) {
            SetDlgItemTextA(app.MainWindow(),panel::kPlayStartFrameEdit,range[0]);
            SetDlgItemTextA(app.MainWindow(),panel::kPlayStopFrameEdit,range[1]);
            const auto r=save_compatibility::InspectPmm(app);
            Check(!r.invalidData && !r.needsNewPmm && r.canCopyLegacy,
                  "end-relative playback range is not a negative-frame compatibility restriction");
        }
        SetDlgItemTextA(app.MainWindow(),panel::kPlayStartFrameEdit,"-10");
        SetDlgItemTextA(app.MainWindow(),panel::kPlayStopFrameEdit,"-1");
        Check(!save_compatibility::InspectPmm(app).canCopyLegacy,
              "ordered negative playback bounds retain literal negative-frame semantics");
        SetDlgItemTextA(app.MainWindow(),panel::kPlayStartFrameEdit,start);
        SetDlgItemTextA(app.MainWindow(),panel::kPlayStopFrameEdit,stop);
        app.LastRegisteredFrame()=last;
    }
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

#include "pmm_invalid_save_checks.hpp"

void InvalidFormatEscalation(MMDApp& app, const std::filesystem::path& directory) {
    auto* keys = app.CameraKeys();
    const auto head = keys[0];
    const auto dirty = app.SceneModified();
    const auto language = app.EnglishUI();
    const std::wstring savedName = app.EnvFileName();
#if !MMD_ORIGINAL_PROFILE
    InvalidSaveCaptureProbe captureProbe;
    const auto valid = directory / L"invalid-save-serializer-positive-control.pmm";
    Check(SaveSceneCopy(&app, valid.c_str()) && captureProbe.Calls() == 1,
        "valid project save reaches the observed production serializer capture boundary once");
    Check(std::filesystem::is_regular_file(plugins::PluginSidecar(valid)),
        "valid serializer control publishes the actual plugin sidecar");
    captureProbe.Reset();
#endif
    keys[0].next = static_cast<std::uint32_t>(mdl::KeyPoolCapacity(keys));
    const auto path = directory / L"invalid-links-must-not-become-v3.pmm";
    Check(!SaveSceneCopy(&app, path.c_str()), "corrupt links are refused rather than saved as PMM v3");
    Check(!std::filesystem::exists(path), "invalid data creates no PMM file");
#if !MMD_ORIGINAL_PROFILE
    Check(!std::filesystem::exists(plugins::PluginSidecar(path)), "invalid backup creates no plugin sidecar");
#endif
    const auto existing = directory / L"invalid-links-preserve-existing.pmm";
    const char marker[] = "existing PMM must remain untouched";
    int fd = Open(existing, _O_CREAT | _O_EXCL | _O_WRONLY);
    Check(_write(fd, marker, sizeof marker) == sizeof marker, "create existing-file preservation fixture");
    _close(fd);
#if !MMD_ORIGINAL_PROFILE
    const auto sidecar = plugins::PluginSidecar(existing);
    const char pluginMarker[] = "existing plugin data must remain untouched";
    fd = Open(sidecar, _O_CREAT | _O_EXCL | _O_WRONLY);
    Check(_write(fd, pluginMarker, sizeof pluginMarker) == sizeof pluginMarker,
        "create existing plugin sidecar preservation fixture");
    _close(fd);
#endif
    const auto preservedFiles = [&] {
        fd = Open(existing, _O_RDONLY);
        char actual[sizeof marker]{};
        Check(_read(fd, actual, sizeof actual) == sizeof marker &&
            std::memcmp(actual, marker, sizeof marker) == 0 && _filelengthi64(fd) == sizeof marker,
            "invalid save never replaces an existing project");
        _close(fd);
#if !MMD_ORIGINAL_PROFILE
        fd = Open(sidecar, _O_RDONLY);
        char pluginActual[sizeof pluginMarker]{};
        Check(_read(fd, pluginActual, sizeof pluginActual) == sizeof pluginMarker &&
            std::memcmp(pluginActual, pluginMarker, sizeof pluginMarker) == 0 &&
            _filelengthi64(fd) == sizeof pluginMarker,
            "invalid save preserves the existing plugin sidecar byte for byte");
        _close(fd);
#endif
    };
    // Backup saves remain silent; normal saves exercise the actual localized
    // MessageBox. Both routes must refuse before any serializer is captured.
    for (unsigned lang = 0; lang < 3; ++lang) {
        app.EnglishUI() = lang;
        Check(!SaveSceneCopy(&app, existing.c_str()), "invalid backup refuses existing PMM and sidecar publication");
        Check(app.SceneModified() == dirty, "invalid backup preserves the original dirty state");
        preservedFiles();
#if !MMD_ORIGINAL_PROFILE
        Check(captureProbe.Calls() == 0, "invalid backup is rejected before plugin serializers are captured");
#endif
    }
    wcscpy_s(app.EnvFileName(), 256, existing.c_str());
    for (unsigned lang = 0; lang < 3; ++lang) {
        app.EnglishUI() = lang;
        app.SceneModified() = 1;
        {
            ExpectedInvalidSaveDialog dialog(app.MainWindow(), lang);
            SaveSceneFile(&app);
            Check(dialog.Handled(), "normal save reports exactly one actual invalid-data dialog in the requested language");
        }
        Check(app.SceneModified() == 1 && std::wstring(app.EnvFileName()) == existing.native() &&
            keys[0].next == static_cast<std::uint32_t>(mdl::KeyPoolCapacity(keys)),
            "invalid normal save preserves its filename, modified state and original invalid data");
        preservedFiles();
#if !MMD_ORIGINAL_PROFILE
        Check(captureProbe.Calls() == 0, "invalid normal save is rejected before plugin serializers are captured");
#endif
        std::printf("PASS normal invalid-save language=%u localized dialog and unchanged project pair\n", lang);
    }
    wcscpy_s(app.EnvFileName(), 256, savedName.c_str());
    app.EnglishUI() = language;
    app.SceneModified() = dirty;
    keys[0] = head;
    std::puts("PASS invalid backup and three-language normal saves refuse serializer capture and preserve project and plugin sidecar bytes");
}

void SignedCursorRoundtrip(MMDApp& app, const std::filesystem::path& directory) {
    const std::array<std::array<int,2>,5> cases{{{{-17,-63}},{{-17,0}},{{42,-63}},{{42,12}},
        {{frame_limits::minimum,frame_limits::minimum}}}};
    for(std::size_t i=0;i<cases.size();++i) {
        ResetAppState(&app);
        SetDlgItemTextA(app.MainWindow(),panel::kPlayStartFrameEdit,"0");
        SetDlgItemTextA(app.MainWindow(),panel::kPlayStopFrameEdit,"-1");
        const int frame=cases[i][0],scroll=cases[i][1];
        app.CurrentFrame()=frame;app.TimelineStartFrame()=scroll;
        const auto project=directory/(L"signed-cursor-"+std::to_wstring(i)+L".pmm");
        const auto prefix=directory/(L"signed-cursor-body-"+std::to_wstring(i)+L".pmm");
        Check(SaveSceneCopy(&app,project.c_str()),"production PMM writer saves signed cursor using standard body plus supported extensions");
        Check(app.CurrentFrame()==frame&&app.TimelineStartFrame()==scroll,"saving the clamped PMM body never moves the live signed cursor or timeline");
        std::uint64_t boundary=0;const auto document=Read(project,&boundary);
        const auto* cursor=ext::Find(document,"org.mikudancestudio.timeline.signed_cursor.v1");
#if MMD_ORIGINAL_PROFILE
        Check(cursor==nullptr,"original profile does not promise the full-profile signed cursor extension");
#else
        Check(cursor&&cursor->value.size()==8,"full PMM contains an independent signed current-frame and timeline-left extension");
        std::array<std::int32_t,2> saved{};std::memcpy(saved.data(),cursor->value.data(),sizeof(saved));
        Check(saved[0]==frame&&saved[1]==scroll,"signed cursor extension retains exact negative values, including the minimum frame");
#endif
        // Strip only the extension from an actual saved project. The real PMM
        // loader then reads the two body fields without a signed-cursor override.
        std::filesystem::copy_file(project,prefix,std::filesystem::copy_options::overwrite_existing);
        const int fd=Open(prefix,_O_RDWR);
        Check(boundary>30&&_chsize_s(fd,boundary)==0,"extract the exact standard body from the real saved PMM");
        _close(fd);
        app.CurrentFrame()=91;app.TimelineStartFrame()=27;
        Load(app,prefix);
        Check(app.CurrentFrame()==(std::max)(0,frame)&&app.TimelineStartFrame()==(std::max)(0,scroll),
              "actual PMM standard body reloads nonnegative current-frame and timeline-left fields independently");
        app.CurrentFrame()=91;app.TimelineStartFrame()=27;
        Load(app,project);
#if MMD_ORIGINAL_PROFILE
        Check(app.CurrentFrame()==(std::max)(0,frame)&&app.TimelineStartFrame()==(std::max)(0,scroll),
              "original-profile reload follows only the nonnegative standard PMM fields");
#else
        Check(app.CurrentFrame()==frame&&app.TimelineStartFrame()==scroll,
              "full production PMM reload restores the exact signed editing cursor over its safe standard body");
#endif
    }
    ResetAppState(&app);
    SetDlgItemTextA(app.MainWindow(),panel::kPlayStartFrameEdit,"0");
    SetDlgItemTextA(app.MainWindow(),panel::kPlayStopFrameEdit,"0");
    std::puts("PASS real PMM nonnegative body and profile-specific signed cursor roundtrip");
}

void AutomaticCameraRoundtrip(MMDApp& app, const std::filesystem::path& directory) {
    const auto compact = directory / L"automatic-compact.pmm";
    const auto expanded = directory / L"automatic-expanded.pmm";
    Check(mdl::KeyPoolEnsure(app.CameraKeys(),10001), "reserve expanded camera track");
    app.CameraKeys()[0].next=10000;
    app.CameraKeys()[0].previous=1;
    app.CameraKeys()[10000].frame=23;
    app.CameraKeys()[10000].distance=-83;
    Check(SaveSceneCopy(&app,compact.c_str()), "save sparse pool using legacy format");
    int fd=Open(compact,_O_RDONLY);char header[30]{};Check(_read(fd,header,30)==30,"read compact header");_close(fd);
    Check(std::memcmp(header,"Polygon Movie maker 0002",24)==0,"sparse high slots save as original PMM");
    Check(app.CameraKeys()[0].next==10000 && app.CameraKeys()[0].previous==1,
          "save compaction does not alter live indices or the stale initial sentinel");
    Load(app,compact);
    const auto slot=app.CameraKeys()[0].next;
    Check(slot>0&&slot<10000&&app.CameraKeys()[0].previous==0&&app.CameraKeys()[slot].frame==23&&app.CameraKeys()[slot].distance==-83,
          "compact PMM roundtrip retains links and values");
    const auto sentinel=directory/L"automatic-initial-sentinel.pmm";
    app.CameraKeys()[0].previous=UINT32_MAX;
    Check(SaveSceneCopy(&app,sentinel.c_str()) && app.CameraKeys()[0].previous==UINT32_MAX,
          "ordinary PMM save accepts an unused initial sentinel without changing live state");
    Load(app,sentinel);
    Check(app.CameraKeys()[0].previous==0 && app.CameraKeys()[0].next==slot &&
          app.CameraKeys()[slot].previous==0 && app.CameraKeys()[slot].frame==23 && app.CameraKeys()[slot].distance==-83,
          "ordinary PMM roundtrip canonicalizes only the unused initial sentinel");
    std::puts("PASS real PMM stale initial sentinels through compact and ordinary save/load");
    Check(mdl::KeyPoolEnsure(app.CameraKeys(),10001),"reserve fully occupied camera track");
    for(unsigned i=0;i<10001;++i){auto& k=app.CameraKeys()[i];k.frame=i;k.previous=i?i-1:0;k.next=i<10000?i+1:0;k.distance=-float(i);}
#if MMD_ORIGINAL_PROFILE
    Check(!SaveSceneCopy(&app,expanded.c_str()) && !std::filesystem::exists(expanded),
        "original profile refuses camera content requiring PMM v3 without publishing");
#else
    Check(SaveSceneCopy(&app,expanded.c_str()),"save actual expanded camera content");
    fd=Open(expanded,_O_RDONLY);Check(_read(fd,header,30)==30,"read expanded header");_close(fd);
    Check(std::memcmp(header,"Polygon Movie maker 0003",24)==0,"actual key overflow selects new PMM");
    Load(app,expanded);
    Check(mdl::KeyPoolCapacity(app.CameraKeys())>10000&&app.CameraKeys()[9999].next==10000&&
          app.CameraKeys()[10000].previous==9999&&app.CameraKeys()[10000].frame==10000&&app.CameraKeys()[10000].distance==-10000,
          "new PMM roundtrip preserves the key beyond the original camera limit");
#endif
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
#if MMD_ORIGINAL_PROFILE
    Check(!SaveSceneCopy(&app,longFrame.c_str()) && !std::filesystem::exists(longFrame),
        "original profile refuses a UI frame range requiring PMM v3 without publishing");
#else
    Check(SaveSceneCopy(&app,longFrame.c_str()),"save extended UI frame range");
    fd=Open(longFrame,_O_RDONLY);Check(_read(fd,header,30)==30,"read large-frame header");_close(fd);
    Check(std::memcmp(header,"Polygon Movie maker 0003",24)==0,"large UI frame range selects extended PMM");
    Load(app,longFrame);
    char range[32]{};GetDlgItemTextA(app.MainWindow(),panel::kPlayStopFrameEdit,range,sizeof range);
    Check(app.CurrentFrame()==frame_limits::maximum-20 && std::string(range)=="2147483646",
          "extended frame range roundtrips without digit truncation");
#endif
    std::wstring copy1,copy2;
    Check(save_compatibility::CopyPrefix(compact.c_str(),30,copy1) && save_compatibility::CopyPrefix(compact.c_str(),30,copy2) && copy1!=copy2,
          "compatible copies use distinct names without overwriting");
    Check(std::filesystem::file_size(copy1)==30 && std::filesystem::file_size(copy2)==30,"compatible copy contains only requested legacy prefix");
    ResetAppState(&app);
    SetDlgItemTextA(app.MainWindow(),panel::kPlayStartFrameEdit,"0");
    SetDlgItemTextA(app.MainWindow(),panel::kPlayStopFrameEdit,"0");
#if MMD_ORIGINAL_PROFILE
    std::puts("PASS original PMM camera roundtrip and refusal of expanded content/frame ranges");
#else
    std::puts("PASS real legacy-first and expanded PMM camera roundtrip");
#endif
}

void ModelCommentRoundtrip(MMDApp& app, const std::filesystem::path& directory) {
    Check(app.ModelSlot(0) != nullptr, "model comment roundtrip requires an actually loaded model");
    const std::wstring local = L"完整本地备注 😀\n" + std::wstring(600, L'文');
    const std::wstring english = L"Complete English comment 𝄞\n" + std::wstring(700, L'e');
    auto assign = [&](const std::wstring& a, const std::wstring& b) {
        auto& model = *mdl::Mdl(app.ModelSlot(0));
        Check(extended_names::Assign(model.pmxTextBuffers[2], a) &&
              extended_names::Assign(model.pmxTextBuffers[3], b), "set complete canonical comments on the loaded model");
    };
    assign(local, english);
    const auto first = directory / L"model-comments.pmm", second = directory / L"model-comments-save-as.pmm";
    for (const auto& path : {first, second}) {
        Check(SaveSceneCopy(&app, path.c_str()), "production PMM backup/save-as captures both complete comments");
        int fd = Open(path, _O_RDONLY); ext::Document document; std::uint64_t boundary = 0;
        Check(ext::Read(fd, document, boundary) == ext::ReadResult::Loaded && boundary > 64,
              "actual PMM body has a readable MMS extension");
        _close(fd);
        const auto* record = ext::Find(document, "org.mikumikustudio.model.0.comments.v1");
        Check(record && record->value.size() > 1000 && record->value[0] == 1 && record->value[4] == 3,
              "actual project writer emits the versioned two-language comments record");
        assign(L"transient local value", L"transient English value");
        Load(app, path);
        Check(app.ModelSlot(0) != nullptr, "actual PMM reload recreates its model from the source file");
        auto& model = *mdl::Mdl(app.ModelSlot(0));
        Check(model.pmxTextBuffers[2] && model.pmxTextBuffers[3] &&
              std::wstring(model.pmxTextBuffers[2]) == local && std::wstring(model.pmxTextBuffers[3]) == english,
              "actual PMM reload restores both long Unicode comments instead of transient or source-model text");
    }
    std::puts("PASS real model PMM Unicode comments, source reload and save-as");
}

// ---- real duplicate-name copy/paste/undo and PMM reopen -------------------
// Two real PMX files are generated and loaded through the production loader.
// The source has two bones and two morphs that share one short ASCII name each;
// the target keeps the same pair but places unrelated entries before and
// between them, so its second duplicate sits at a different absolute index.
// Only the second source bone and morph track is marked. The ordinary command
// path then copies (420), pastes (421), undoes (400) and redoes (401) the
// batch, and the resulting project is saved with SaveSceneCopy and reopened
// with LoadSceneFile. No test-only pose binding takes part.
constexpr std::uint16_t kPmxRotatableBoneFlag = 0x0002;  // PMX 2.0 rotate flag

// Expected fixture values. Registration and every later check use these same
// constants, so a wrong track mapping cannot hide behind a rewritten key.
constexpr float kDuplicateSourcePositionA[3] = {1.25f, -2.5f, 3.75f};
constexpr float kDuplicateSourcePositionB[3] = {-4.5f, 5.25f, -6.75f};
constexpr float kDuplicateSourceSentinelPosition[3] = {9.5f, -8.25f, 7.75f};
constexpr std::uint8_t kDuplicateSourceInterpolationA[16] = {
    20, 40, 60, 88, 18, 36, 64, 92, 24, 48, 72, 96, 14, 34, 58, 86};
constexpr std::uint8_t kDuplicateSourceInterpolationB[16] = {
    28, 44, 68, 84, 16, 38, 62, 90, 22, 46, 74, 98, 12, 32, 56, 82};
constexpr std::uint8_t kDuplicateSentinelInterpolation[16] = {
    9, 27, 45, 63, 11, 29, 47, 65, 13, 31, 49, 67, 15, 33, 51, 69};
constexpr float kDuplicateSourceMorphA = 0.125f;
constexpr float kDuplicateSourceMorphB = 0.625f;
constexpr float kDuplicateSourceSentinelMorph = 0.875f;
constexpr float kDuplicateTargetSentinelPosition0[3] = {4.0f, 0.0f, 0.0f};
constexpr float kDuplicateTargetFirstPosition[3] = {0.5f, 0.25f, 0.75f};
constexpr float kDuplicateTargetSentinelPosition2[3] = {0.0f, 0.0f, -4.0f};
constexpr float kDuplicateTargetSecondPosition[3] = {-1.5f, -2.5f, -3.5f};
constexpr float kDuplicateTargetSentinelMorph0 = 0.0625f;
constexpr float kDuplicateTargetSentinelMorph1 = 0.25f;
constexpr float kDuplicateTargetSentinelMorph2 = 0.3125f;
constexpr float kDuplicateTargetSentinelMorph3 = 0.5f;
constexpr float kDuplicateIdentityRotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};

// Frame-zero root payload for every fixture track. Roots are persisted records;
// they get valid unit quaternions and finite values so the PMM wire format
// carries them back exactly (the loader repairs only a zero/invalid quaternion).
constexpr std::uint8_t kDuplicateRootInterpolation[16] = {
    31, 62, 93, 112, 33, 64, 95, 114, 35, 66, 97, 116, 37, 68, 99, 118};
constexpr float kDuplicateSourceBoneRoot0Position[3] = {0.125f, 0.25f, 0.5f};
constexpr float kDuplicateSourceBoneRoot1Position[3] = {-0.125f, -0.25f, -0.5f};
constexpr float kDuplicateSourceBoneRoot0Rotation[4] = {0.6f, 0.0f, 0.0f, 0.8f};
constexpr float kDuplicateSourceBoneRoot1Rotation[4] = {0.0f, 0.6f, 0.0f, 0.8f};
constexpr float kDuplicateTargetBoneRoot0Position[3] = {1.5f, 0.0f, 0.0f};
constexpr float kDuplicateTargetBoneRoot1Position[3] = {0.0f, 1.5f, 0.0f};
constexpr float kDuplicateTargetBoneRoot2Position[3] = {0.0f, 0.0f, 1.5f};
constexpr float kDuplicateTargetBoneRoot3Position[3] = {-1.5f, 0.0f, 0.0f};
constexpr float kDuplicateTargetBoneRoot0Rotation[4] = {0.0f, 0.0f, 0.6f, 0.8f};
constexpr float kDuplicateTargetBoneRoot1Rotation[4] = {0.6f, 0.0f, 0.0f, 0.8f};
constexpr float kDuplicateTargetBoneRoot2Rotation[4] = {0.0f, 0.6f, 0.0f, 0.8f};
constexpr float kDuplicateTargetBoneRoot3Rotation[4] = {-0.6f, 0.0f, 0.0f, 0.8f};
constexpr float kDuplicateSourceMorphRoot0 = 0.375f;
constexpr float kDuplicateSourceMorphRoot1 = 0.75f;
constexpr float kDuplicateTargetMorphRoot0 = 0.1875f;
constexpr float kDuplicateTargetMorphRoot1 = 0.4375f;
constexpr float kDuplicateTargetMorphRoot2 = 0.6875f;
constexpr float kDuplicateTargetMorphRoot3 = 0.9375f;

// A persisted bone/morph track starts in its frame-zero root slot: one root per
// bone or morph table entry. The root is saved together with the keyframes, so
// the helpers below validate the root and every link before trusting a result:
// links stay inside the pool, never point at another root, carry a matching
// previous index, and strictly increase the frame. Only next == 0 ends a chain;
// exhausting the pool capacity is a failure, never a silent "not found".
void CheckDuplicateBoneTrackRoot(unsigned char* model, std::uint32_t bone,
                                 const char* description) {
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    Check(mdl::BoneKeys(model) != nullptr && bone < record.boneCount, description);
    Check(record.boneKeys[bone].frame == 0, description);
}

void CheckDuplicateMorphTrackRoot(unsigned char* model, std::uint32_t morph,
                                  const char* description) {
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    Check(mdl::MorphKeys(model) != nullptr && morph < record.morphCount, description);
    Check(record.morphKeys[morph].frame == 0, description);
}

mdl::BoneKey* FindDuplicateBoneKey(unsigned char* model, std::uint32_t bone,
                                   std::uint32_t frame) {
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    mdl::BoneKey* const keys = mdl::BoneKeys(model);
    const std::size_t capacity = mdl::BoneKeyCapacity(record);
    const std::size_t roots = record.boneCount;
    CheckDuplicateBoneTrackRoot(model, bone, "bone lookup starts at a valid frame-zero root");
    mdl::BoneKey* found = nullptr;
    std::size_t index = bone;
    for (std::size_t visited = 0; visited < capacity; ++visited) {
        const mdl::BoneKey& key = keys[index];
        if (key.frame == frame) found = &keys[index];
        // The remaining links are validated even after the target frame, so a
        // damaged chain is never reported as a plain missing frame.
        const std::uint32_t next = key.next;
        if (next == 0) return found;            // normal end of the track
        Check(next < capacity && next >= roots,
              "bone track link stays inside the pool and outside the root slots");
        Check(keys[next].previous == index,
              "bone track reverse link points at its predecessor");
        Check(keys[next].frame > key.frame,
              "bone track frames strictly increase along next links");
        index = next;
    }
    Check(false, "bone track link chain reaches next zero before the pool capacity");
    return nullptr;
}

mdl::MorphKey* FindDuplicateMorphKey(unsigned char* model, std::uint32_t morph,
                                     std::uint32_t frame) {
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    mdl::MorphKey* const keys = mdl::MorphKeys(model);
    const std::size_t capacity = mdl::MorphKeyCapacity(keys);
    const std::size_t roots = record.morphCount;
    CheckDuplicateMorphTrackRoot(model, morph, "morph lookup starts at a valid frame-zero root");
    mdl::MorphKey* found = nullptr;
    std::size_t index = morph;
    for (std::size_t visited = 0; visited < capacity; ++visited) {
        const mdl::MorphKey& key = keys[index];
        if (key.frame == frame) found = &keys[index];
        // Validate the remaining links as well, so a damaged chain is never
        // reported as a plain missing frame.
        const std::uint32_t next = key.next;
        if (next == 0) return found;            // normal end of the track
        Check(next < capacity && next >= roots,
              "morph track link stays inside the pool and outside the root slots");
        Check(keys[next].previous == index,
              "morph track reverse link points at its predecessor");
        Check(keys[next].frame > key.frame,
              "morph track frames strictly increase along next links");
        index = next;
    }
    Check(false, "morph track link chain reaches next zero before the pool capacity");
    return nullptr;
}

// Nonzero keyframes of one persisted track, in track order. The frame-zero root
// is itself a persisted record, so it is checked and compared separately through
// DuplicateBoneRootPayload / DuplicateMorphRootPayload instead of being listed.
std::vector<std::uint32_t> DuplicateBoneFrames(unsigned char* model, std::uint32_t bone) {
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    mdl::BoneKey* const keys = mdl::BoneKeys(model);
    const std::size_t capacity = mdl::BoneKeyCapacity(record);
    const std::size_t roots = record.boneCount;
    CheckDuplicateBoneTrackRoot(model, bone, "bone enumeration starts at a valid frame-zero root");
    std::vector<std::uint32_t> frames;
    std::size_t index = bone;
    for (std::size_t visited = 0; visited < capacity; ++visited) {
        const mdl::BoneKey& key = keys[index];
        const std::uint32_t next = key.next;
        if (next == 0) return frames;           // normal end of the track
        Check(next < capacity && next >= roots,
              "bone track link stays inside the pool and outside the root slots");
        Check(keys[next].previous == index,
              "bone track reverse link points at its predecessor");
        Check(keys[next].frame > key.frame,
              "bone track frames strictly increase along next links");
        index = next;
        frames.push_back(keys[index].frame);
    }
    Check(false, "bone track link chain reaches next zero before the pool capacity");
    return frames;
}

std::vector<std::uint32_t> DuplicateMorphFrames(unsigned char* model, std::uint32_t morph) {
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    mdl::MorphKey* const keys = mdl::MorphKeys(model);
    const std::size_t capacity = mdl::MorphKeyCapacity(keys);
    const std::size_t roots = record.morphCount;
    CheckDuplicateMorphTrackRoot(model, morph, "morph enumeration starts at a valid frame-zero root");
    std::vector<std::uint32_t> frames;
    std::size_t index = morph;
    for (std::size_t visited = 0; visited < capacity; ++visited) {
        const mdl::MorphKey& key = keys[index];
        const std::uint32_t next = key.next;
        if (next == 0) return frames;           // normal end of the track
        Check(next < capacity && next >= roots,
              "morph track link stays inside the pool and outside the root slots");
        Check(keys[next].previous == index,
              "morph track reverse link points at its predecessor");
        Check(keys[next].frame > key.frame,
              "morph track frames strictly increase along next links");
        index = next;
        frames.push_back(keys[index].frame);
    }
    Check(false, "morph track link chain reaches next zero before the pool capacity");
    return frames;
}

// Logical payload of one persisted frame-zero root. Slot numbers, links and the
// selection byte are deliberately excluded: they are bookkeeping that commands
// are allowed to change, while these fields are the persisted pose/value data.
struct DuplicateBoneRootPayload {
    std::uint32_t frame;
    float position[3];
    float rotation[4];
    std::uint8_t interpolation[16];
    std::uint8_t physicsDisabled;
};

struct DuplicateMorphRootPayload {
    std::uint32_t frame;
    float value;
};

DuplicateBoneRootPayload ReadDuplicateBoneRoot(unsigned char* model, std::uint32_t bone) {
    CheckDuplicateBoneTrackRoot(model, bone, "the frame-zero bone root stays a valid root");
    const mdl::BoneKey& key = mdl::BoneKeys(model)[bone];
    DuplicateBoneRootPayload payload{};
    payload.frame = key.frame;
    std::memcpy(payload.position, key.position, sizeof payload.position);
    std::memcpy(payload.rotation, key.rotation, sizeof payload.rotation);
    std::memcpy(payload.interpolation, key.interpolation, sizeof payload.interpolation);
    payload.physicsDisabled = key.physicsDisabled;
    return payload;
}

DuplicateMorphRootPayload ReadDuplicateMorphRoot(unsigned char* model, std::uint32_t morph) {
    CheckDuplicateMorphTrackRoot(model, morph, "the frame-zero morph root stays a valid root");
    const mdl::MorphKey& key = mdl::MorphKeys(model)[morph];
    DuplicateMorphRootPayload payload{};
    payload.frame = key.frame;
    payload.value = key.value;
    return payload;
}

bool SameDuplicateBoneRoot(const DuplicateBoneRootPayload& left,
                           const DuplicateBoneRootPayload& right) {
    return left.frame == right.frame &&
        std::memcmp(left.position, right.position, sizeof left.position) == 0 &&
        std::memcmp(left.rotation, right.rotation, sizeof left.rotation) == 0 &&
        std::memcmp(left.interpolation, right.interpolation, sizeof left.interpolation) == 0 &&
        left.physicsDisabled == right.physicsDisabled;
}

bool SameDuplicateMorphRoot(const DuplicateMorphRootPayload& left,
                            const DuplicateMorphRootPayload& right) {
    return left.frame == right.frame && left.value == right.value;
}

void CaptureDuplicateRoots(unsigned char* source, unsigned char* target,
                           DuplicateBoneRootPayload* sourceBones,
                           DuplicateBoneRootPayload* targetBones,
                           DuplicateMorphRootPayload* sourceMorphs,
                           DuplicateMorphRootPayload* targetMorphs) {
    for (unsigned i = 0; i < 2; ++i) sourceBones[i] = ReadDuplicateBoneRoot(source, i);
    for (unsigned i = 0; i < 4; ++i) targetBones[i] = ReadDuplicateBoneRoot(target, i);
    for (unsigned i = 0; i < 2; ++i) sourceMorphs[i] = ReadDuplicateMorphRoot(source, i);
    for (unsigned i = 0; i < 4; ++i) targetMorphs[i] = ReadDuplicateMorphRoot(target, i);
}

void VerifyDuplicateRoots(unsigned char* source, unsigned char* target,
                          const DuplicateBoneRootPayload* sourceBones,
                          const DuplicateBoneRootPayload* targetBones,
                          const DuplicateMorphRootPayload* sourceMorphs,
                          const DuplicateMorphRootPayload* targetMorphs,
                          const char* description) {
    for (unsigned i = 0; i < 2; ++i)
        Check(SameDuplicateBoneRoot(ReadDuplicateBoneRoot(source, i), sourceBones[i]), description);
    for (unsigned i = 0; i < 4; ++i)
        Check(SameDuplicateBoneRoot(ReadDuplicateBoneRoot(target, i), targetBones[i]), description);
    for (unsigned i = 0; i < 2; ++i)
        Check(SameDuplicateMorphRoot(ReadDuplicateMorphRoot(source, i), sourceMorphs[i]), description);
    for (unsigned i = 0; i < 4; ++i)
        Check(SameDuplicateMorphRoot(ReadDuplicateMorphRoot(target, i), targetMorphs[i]), description);
}

// The copy command reads bone/morph selection from the allocated byte, and a
// real registration also sets it. Clear every pool before marking only the
// second-track frames that this regression wants to copy.
void ClearDuplicateKeyMarks(unsigned char* model) {
    mdl::BoneKey* const bones = mdl::BoneKeys(model);
    const std::size_t boneCapacity = mdl::BoneKeyCapacity(*mdl::Mdl(model));
    for (std::size_t i = 0; i < boneCapacity; ++i) bones[i].allocated = 0;
    mdl::MorphKey* const morphs = mdl::MorphKeys(model);
    const std::size_t morphCapacity = mdl::MorphKeyCapacity(morphs);
    for (std::size_t i = 0; i < morphCapacity; ++i) morphs[i].allocated = 0;
    mdl::DisplayKey* const displays = mdl::DisplayKeys(model);
    const std::size_t displayCapacity = mdl::KeyPoolCapacity(displays);
    for (std::size_t i = 0; i < displayCapacity; ++i) displays[i].allocated = 0;
}

// Seed the persisted frame-zero root of one bone track through the same
// production registrar. The root keeps its valid payload across PMM save/load
// and must not be damaged by the copy/paste/undo/replay path.
void RegisterDuplicateBoneRoot(unsigned char* model, int bone,
                               const float position[3], const float rotation[4],
                               bool physicsDisabled) {
    mdl::BoneClipboardRecord record{};
    record.frame = 0;
    std::memcpy(record.position, position, sizeof record.position);
    std::memcpy(record.rotation, rotation, sizeof record.rotation);
    record.physicsDisabled = physicsDisabled ? 1 : 0;
    std::memcpy(record.interpolation, kDuplicateRootInterpolation,
                sizeof record.interpolation);
    Check(RegisterBoneKeyAtIndex(model, reinterpret_cast<unsigned char*>(&record), 0, bone),
          "register a persisted frame-zero bone root through the production registrar");
}

// Seed one ordinary bone key through the registrar the paste command uses. The
// caller opens the production before-image slot first, exactly as command 421
// does before it hands its records to the same registrar.
void RegisterDuplicateBoneKey(unsigned char* model, int bone, std::uint32_t frame,
                              const float position[3],
                              const std::uint8_t interpolation[16],
                              bool physicsDisabled) {
    mdl::BoneClipboardRecord record{};
    record.frame = frame;
    std::memcpy(record.position, position, sizeof record.position);
    record.rotation[3] = 1.0f;
    record.physicsDisabled = physicsDisabled ? 1 : 0;
    std::memcpy(record.interpolation, interpolation, sizeof record.interpolation);
    Check(RegisterBoneKeyAtIndex(model, reinterpret_cast<unsigned char*>(&record), 0, bone),
          "register a real positive bone key through the production registrar");
}

void CheckDuplicateBoneKey(unsigned char* model, std::uint32_t bone, std::uint32_t frame,
                           const float position[3],
                           const std::uint8_t interpolation[16],
                           bool physicsDisabled, const char* description) {
    const mdl::BoneKey* const key = FindDuplicateBoneKey(model, bone, frame);
    Check(key != nullptr, description);
    Check(std::memcmp(key->position, position, sizeof key->position) == 0 &&
          std::memcmp(key->rotation, kDuplicateIdentityRotation, sizeof key->rotation) == 0 &&
          std::memcmp(key->interpolation, interpolation, sizeof key->interpolation) == 0 &&
          key->physicsDisabled == (physicsDisabled ? 1 : 0),
          description);
}

void CheckDuplicateMorphKey(unsigned char* model, std::uint32_t morph, std::uint32_t frame,
                            float value, const char* description) {
    const mdl::MorphKey* const key = FindDuplicateMorphKey(model, morph, frame);
    Check(key != nullptr && key->value == value, description);
}

// Complete persisted evidence for the two loaded models: full name tables,
// real loader flags, every sentinel track, and the pasted 40/44 batch resolved
// through the link chain, never through remembered pool slots.
void VerifyDuplicatePastePair(unsigned char* source, unsigned char* target) {
    const mdl::ModelRecord& sourceModel = *mdl::Mdl(source);
    const mdl::ModelRecord& targetModel = *mdl::Mdl(target);
    Check(sourceModel.boneCount == 2 && sourceModel.morphCount == 2 &&
          targetModel.boneCount == 4 && targetModel.morphCount == 4,
          "both models keep the real PMX table sizes through the command roundtrip");
    Check(text_encoding::BoneName(sourceModel.boneTable[0], false) == L"DupBone" &&
          text_encoding::BoneName(sourceModel.boneTable[1], false) == L"DupBone" &&
          text_encoding::BoneName(targetModel.boneTable[0], false) == L"OtherA" &&
          text_encoding::BoneName(targetModel.boneTable[1], false) == L"DupBone" &&
          text_encoding::BoneName(targetModel.boneTable[2], false) == L"OtherB" &&
          text_encoding::BoneName(targetModel.boneTable[3], false) == L"DupBone",
          "complete bone name tables keep both duplicate names at their absolute indices");
    Check(text_encoding::MorphName(sourceModel.morphs[0], false) == L"DupFace" &&
          text_encoding::MorphName(sourceModel.morphs[1], false) == L"DupFace" &&
          text_encoding::MorphName(targetModel.morphs[0], false) == L"OtherFaceA" &&
          text_encoding::MorphName(targetModel.morphs[1], false) == L"DupFace" &&
          text_encoding::MorphName(targetModel.morphs[2], false) == L"OtherFaceB" &&
          text_encoding::MorphName(targetModel.morphs[3], false) == L"DupFace",
          "complete morph name tables keep both duplicate names at their absolute indices");
    const std::uint16_t expectedFlags =
        std::uint16_t(kPmxRotatableBoneFlag | mdl::kBoneFlagMovable | mdl::kBoneFlagVisible);
    for (unsigned i = 0; i < sourceModel.boneCount; ++i)
        Check(sourceModel.boneTable[i].flags == expectedFlags &&
              sourceModel.boneTable[i].type == mdl::BoneType::Move,
              "source bones keep the real PMX rotate/translate flags through the loader");
    for (unsigned i = 0; i < targetModel.boneCount; ++i)
        Check(targetModel.boneTable[i].flags == expectedFlags &&
              targetModel.boneTable[i].type == mdl::BoneType::Move,
              "target bones keep the real PMX rotate/translate flags through the loader");

    Check(DuplicateBoneFrames(source, 0) == std::vector<std::uint32_t>{11} &&
          DuplicateBoneFrames(source, 1) == std::vector<std::uint32_t>{10, 14},
          "source first track keeps its sentinel and the second track keeps frames 10 and 14");
    Check(DuplicateBoneFrames(target, 0) == std::vector<std::uint32_t>{13} &&
          DuplicateBoneFrames(target, 1) == std::vector<std::uint32_t>{11} &&
          DuplicateBoneFrames(target, 2) == std::vector<std::uint32_t>{17} &&
          DuplicateBoneFrames(target, 3) == std::vector<std::uint32_t>{20, 40, 44},
          "target keeps three sentinel tracks and the second duplicate gains frames 20, 40 and 44");
    Check(DuplicateMorphFrames(source, 0) == std::vector<std::uint32_t>{12} &&
          DuplicateMorphFrames(source, 1) == std::vector<std::uint32_t>{10, 14} &&
          DuplicateMorphFrames(target, 0) == std::vector<std::uint32_t>{13} &&
          DuplicateMorphFrames(target, 1) == std::vector<std::uint32_t>{11} &&
          DuplicateMorphFrames(target, 2) == std::vector<std::uint32_t>{17} &&
          DuplicateMorphFrames(target, 3) == std::vector<std::uint32_t>{20, 40, 44},
          "both morph tables keep their sentinel frames and the pasted 40/44 batch");
    Check(FindDuplicateBoneKey(target, 1, 40) == nullptr &&
          FindDuplicateBoneKey(target, 1, 44) == nullptr &&
          FindDuplicateBoneKey(target, 0, 40) == nullptr &&
          FindDuplicateBoneKey(target, 2, 40) == nullptr &&
          FindDuplicateMorphKey(target, 1, 40) == nullptr &&
          FindDuplicateMorphKey(target, 0, 40) == nullptr &&
          FindDuplicateMorphKey(target, 2, 44) == nullptr,
          "the first duplicate and the unrelated target tracks never receive the pasted frames");

    CheckDuplicateBoneKey(source, 0, 11, kDuplicateSourceSentinelPosition,
                          kDuplicateSentinelInterpolation, false,
                          "source first duplicate keeps its sentinel pose");
    CheckDuplicateBoneKey(source, 1, 10, kDuplicateSourcePositionA,
                          kDuplicateSourceInterpolationA, false,
                          "source second duplicate keeps the first standard frame");
    CheckDuplicateBoneKey(source, 1, 14, kDuplicateSourcePositionB,
                          kDuplicateSourceInterpolationB, true,
                          "source second duplicate keeps the second standard frame");
    CheckDuplicateMorphKey(source, 0, 12, kDuplicateSourceSentinelMorph,
                           "source first duplicate keeps its sentinel morph value");
    CheckDuplicateMorphKey(source, 1, 10, kDuplicateSourceMorphA,
                           "source second duplicate keeps its first morph value");
    CheckDuplicateMorphKey(source, 1, 14, kDuplicateSourceMorphB,
                           "source second duplicate keeps its second morph value");

    CheckDuplicateBoneKey(target, 0, 13, kDuplicateTargetSentinelPosition0,
                          kDuplicateSentinelInterpolation, false,
                          "target leading unrelated track keeps its sentinel pose");
    CheckDuplicateBoneKey(target, 1, 11, kDuplicateTargetFirstPosition,
                          kDuplicateSentinelInterpolation, true,
                          "target first duplicate keeps its sentinel pose");
    CheckDuplicateBoneKey(target, 2, 17, kDuplicateTargetSentinelPosition2,
                          kDuplicateSentinelInterpolation, false,
                          "target middle unrelated track keeps its sentinel pose");
    CheckDuplicateBoneKey(target, 3, 20, kDuplicateTargetSecondPosition,
                          kDuplicateSentinelInterpolation, true,
                          "target second duplicate keeps its own sentinel pose");
    CheckDuplicateBoneKey(target, 3, 40, kDuplicateSourcePositionA,
                          kDuplicateSourceInterpolationA, false,
                          "paste writes the first copied pose to the second target duplicate at frame 40");
    CheckDuplicateBoneKey(target, 3, 44, kDuplicateSourcePositionB,
                          kDuplicateSourceInterpolationB, true,
                          "paste writes the second copied pose to the second target duplicate at frame 44");
    CheckDuplicateMorphKey(target, 0, 13, kDuplicateTargetSentinelMorph0,
                           "target leading unrelated morph track keeps its sentinel value");
    CheckDuplicateMorphKey(target, 1, 11, kDuplicateTargetSentinelMorph1,
                           "target first duplicate keeps its sentinel morph value");
    CheckDuplicateMorphKey(target, 2, 17, kDuplicateTargetSentinelMorph2,
                           "target middle unrelated morph track keeps its sentinel value");
    CheckDuplicateMorphKey(target, 3, 20, kDuplicateTargetSentinelMorph3,
                           "target second duplicate keeps its own sentinel morph value");
    CheckDuplicateMorphKey(target, 3, 40, kDuplicateSourceMorphA,
                           "paste writes the first copied morph value to frame 40");
    CheckDuplicateMorphKey(target, 3, 44, kDuplicateSourceMorphB,
                           "paste writes the second copied morph value to frame 44");
}

void FindDuplicatePasteModels(MMDApp& app, const std::filesystem::path& sourcePath,
                              const std::filesystem::path& targetPath,
                              unsigned char** source, unsigned char** target) {
    *source = nullptr;
    *target = nullptr;
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        unsigned char* const raw = app.ModelSlot(slot);
        if (raw == nullptr) continue;
        std::error_code error;
        const std::filesystem::path stored(mdl::Mdl(raw)->path);
        if (std::filesystem::equivalent(stored, sourcePath, error) && !error) {
            Check(*source == nullptr, "the reopened project holds the source model once");
            *source = raw;
        } else {
            error.clear();
            if (std::filesystem::equivalent(stored, targetPath, error) && !error) {
                Check(*target == nullptr, "the reopened project holds the target model once");
                *target = raw;
            }
        }
    }
    Check(*source != nullptr && *target != nullptr,
          "the reopened project resolves both original PMX model paths");
}

// A real SaveSceneCopy / LoadSceneFile roundtrip of the whole scene.
struct MissingPasteWarning {
    HWND owner;
    unsigned char language;
    HWND window = nullptr;

    bool Handle(HWND dialog) {
        if (GetWindow(dialog, GW_OWNER) != owner) return false;
        if (dialog == window) return true;
        wchar_t title[64]{}, body[512]{};
        GetWindowTextW(dialog, title, std::size(title));
        GetDlgItemTextW(dialog, 0xFFFF, body, std::size(body));
        constexpr const wchar_t* titles[] = {L"キーフレームの貼り付け", L"Paste keyframes", L"粘贴关键帧"};
        constexpr const wchar_t* details[] = {L"対応するボーンまたは表情トラックがありません",
            L"missing a matching bone or morph track", L"缺少对应的骨骼或表情轨道"};
        Check(!window && GetDlgItem(dialog, IDOK) && std::wstring_view(title) == titles[language] &&
              std::wstring_view(body).find(details[language]) != std::wstring_view::npos,
              "show one localized missing-track warning in the real command");
        window = dialog;
        std::printf("STAGE missing-track warning language=%u\n", unsigned(language));
        Check(PostMessageW(dialog, WM_CLOSE, 0, 0) != FALSE, "dismiss the owned warning");
        return true;
    }
};
MissingPasteWarning* missingPasteWarning = nullptr;

void MirrorPasteRoundtrip(MMDApp& app, const std::filesystem::path& directory) {
    namespace fs = std::filesystem;
    const auto pmx = directory / L"mirror-rig.pmx";
    const auto pmm = directory / L"mirrored.pmm";
    Check(!fs::exists(pmx) && !fs::exists(pmm), "use only new mirror paste fixtures");
    WritePmmNameBoundaryPmx(pmx, {L"右腕", L"センター", L"左腕"}, {}, L"MirrorRig", L"MirrorRig",
        std::uint16_t(kPmxRotatableBoneFlag | mdl::kBoneFlagMovable | mdl::kBoneFlagVisible));
    const auto modelBytes = EncodingFileBytes(pmx);
    LoadModelFile(&app, pmx.c_str(), false);
    auto* model = app.ModelSlot(0);
    Check(model && app.CameraMode() == 0 && app.SelectedModelSlot() == 0 &&
          mdl::Mdl(model)->boneCount == 3, "load the asymmetric PMX rig through the ordinary entry");
    Check(text_encoding::BoneName(mdl::Mdl(model)->boneTable[0], false) == L"右腕" &&
          text_encoding::BoneName(mdl::Mdl(model)->boneTable[2], false) == L"左腕",
          "the loaded left and right tracks are separated by an unrelated track");
    constexpr int right = 0, center = 1, left = 2;
    constexpr float oldPosition[]{1.f, 2.f, 3.f};
    constexpr float leftPosition[]{4.f, 5.f, 6.f}, rightPosition[]{-7.f, 8.f, 9.f};
    constexpr float leftMirrored[]{-4.f, 5.f, 6.f}, rightMirrored[]{7.f, 8.f, 9.f};
    constexpr float rotation[]{.5f, .5f, -.5f, .5f}, mirrored[]{.5f, -.5f, .5f, .5f};
    const auto seed = [&](int bone, unsigned frame, const float (&position)[3], bool disabled) {
        mdl::BoneClipboardRecord record{};
        record.frame = frame;
        std::memcpy(record.position, position, sizeof position);
        std::memcpy(record.rotation, rotation, sizeof rotation);
        std::memcpy(record.interpolation, kDuplicateSourceInterpolationA, sizeof record.interpolation);
        record.physicsDisabled = disabled;
        Check(RegisterBoneKeyAtIndex(model, reinterpret_cast<unsigned char*>(&record), 0, bone),
              "seed the ordinary positive key");
    };
    Check(BeginModelFramePasteUndo(model, 0, 8, false, false), "open the real fixture registration batch");
    ResetBoneKeyCursor(model);
    seed(left, 10, leftPosition, true);
    seed(right, 14, rightPosition, false);
    seed(right, 40, oldPosition, false);
    seed(center, 40, oldPosition, true);
    const auto require = [&](int bone, unsigned frame, const float (&position)[3],
                             const float (&quaternion)[4], bool disabled) {
        const auto* key = FindDuplicateBoneKey(model, bone, frame);
        Check(key && std::memcmp(key->position, position, sizeof position) == 0 &&
              std::memcmp(key->rotation, quaternion, sizeof quaternion) == 0 &&
              std::memcmp(key->interpolation, kDuplicateSourceInterpolationA, sizeof key->interpolation) == 0 &&
              key->physicsDisabled == disabled, "read the expected pose, native interpolation and physics from the correct track");
    };
    const auto unchanged = [&] {
        require(left, 10, leftPosition, rotation, true);
        require(right, 14, rightPosition, rotation, false);
        require(center, 40, oldPosition, rotation, true);
        Check(!FindDuplicateBoneKey(model, left, 40) && !FindDuplicateBoneKey(model, right, 44),
              "the mirror never writes the same-side tracks");
    };
    const auto pasted = [&] {
        require(right, 40, leftMirrored, mirrored, true);
        require(left, 44, rightMirrored, mirrored, false);
        unchanged();
    };
    const auto send = [&](int command) { SendMessageW(app.MainWindow(), WM_COMMAND, command, 0); };
    ClearDuplicateKeyMarks(model);
    FindDuplicateBoneKey(model, left, 10)->allocated = 1;
    FindDuplicateBoneKey(model, right, 14)->allocated = 1;
    app.CurrentFrame() = 40;
    send(420);
    Check(app.ClipboardCounts().bones == 2 &&
          IsWindowEnabled(GetDlgItem(app.MainWindow(), panel::kReversePasteButton)),
          "copy both selected bones through the actual frame command and enable reverse paste");
    send(panel::kReversePasteButton);
    pasted();
    ClearDuplicateKeyMarks(model);
    FindDuplicateBoneKey(model, center, 40)->allocated = 1;
    send(420);
    Check(app.ClipboardCounts().bones == 1, "replace the clipboard before replay");
    send(panel::kUndoButton);
    require(right, 40, oldPosition, rotation, false);
    Check(!FindDuplicateBoneKey(model, left, 44), "one undo removes the inserted opposite-side key");
    unchanged();
    send(panel::kRedoButton);
    pasted();
    Check(app.ClipboardCounts().bones == 1, "redo uses the committed mirror edit and preserves the new clipboard");
    Check(SaveSceneCopy(&app, pmm.c_str()) && LoadSceneFile(pmm.c_str()),
          "save and reopen the real mirrored scene");
    model = app.ModelSlot(0);
    Check(model && EncodingFileBytes(pmx) == modelBytes, "the original model bytes survive the roundtrip");
    pasted();
    std::puts("PASS timeline mirror paste overwrites, inserts, undoes, redoes and reopens");
}

void DuplicatePasteRoundtrip(MMDApp& app, const std::filesystem::path& directory) {
    std::printf("PROFILE original=%d DUPLICATE_PASTE directory=%ls\n",
                build_profile::original ? 1 : 0, directory.c_str());
    Check(app.ModelSlot(0) == nullptr && app.ModelSlot(1) == nullptr,
          "the duplicate-paste fixture starts on an empty scene");

    const auto sourcePmx = directory / L"duplicate-paste-source.pmx";
    const auto targetPmx = directory / L"duplicate-paste-target.pmx";
    const auto savedPmm = directory / L"duplicate-paste-roundtrip.pmm";
    const auto resavedPmm = directory / L"duplicate-paste-roundtrip-resaved.pmm";
    Check(!std::filesystem::exists(sourcePmx) && !std::filesystem::exists(targetPmx) &&
          !std::filesystem::exists(savedPmm) && !std::filesystem::exists(resavedPmm),
          "the duplicate-paste fixture writes only brand-new files");

    const std::vector<std::wstring> sourceBones{L"DupBone", L"DupBone"};
    const std::vector<std::wstring> sourceMorphs{L"DupFace", L"DupFace"};
    const std::vector<std::wstring> targetBones{L"OtherA", L"DupBone", L"OtherB", L"DupBone"};
    const std::vector<std::wstring> targetMorphs{
        L"OtherFaceA", L"DupFace", L"OtherFaceB", L"DupFace"};
    const std::uint16_t boneFlags =
        std::uint16_t(kPmxRotatableBoneFlag | mdl::kBoneFlagMovable | mdl::kBoneFlagVisible);
    WritePmmNameBoundaryPmx(sourcePmx, sourceBones, sourceMorphs,
                            L"DupSource", L"DupSource", boneFlags);
    WritePmmNameBoundaryPmx(targetPmx, targetBones, targetMorphs,
                            L"DupTarget", L"DupTarget", boneFlags);
    const auto sourceBytes = EncodingFileBytes(sourcePmx);
    const auto targetBytes = EncodingFileBytes(targetPmx);

    std::puts("STAGE load: two real PMX files through LoadModelFile");
    LoadModelFile(&app, sourcePmx.c_str(), false);
    Check(app.ModelSlot(0) != nullptr && app.ModelSlot(1) == nullptr,
          "the source PMX occupies the first model slot");
    LoadModelFile(&app, targetPmx.c_str(), false);
    Check(app.ModelSlot(1) != nullptr, "the target PMX occupies the second model slot");
    // The production model-loading entry leaves camera mode on the empty scene;
    // loading a real model is the ordinary UI switch back to model editing.
    Check(app.state.optflag[0] == 0 && app.CameraMode() == 0,
          "loading both models through the ordinary entry leaves model editing mode active");

    unsigned char* const sourceRaw = app.ModelSlot(0);
    unsigned char* const targetRaw = app.ModelSlot(1);
    const mdl::ModelRecord& loadedSource = *mdl::Mdl(sourceRaw);
    const mdl::ModelRecord& loadedTarget = *mdl::Mdl(targetRaw);
    Check(loadedSource.boneCount == 2 && loadedSource.morphCount == 2 &&
          loadedTarget.boneCount == 4 && loadedTarget.morphCount == 4,
          "the loaded PMX tables expose two source and four target bone/morph entries");
    Check(text_encoding::BoneName(loadedSource.boneTable[0], false) == L"DupBone" &&
          text_encoding::BoneName(loadedSource.boneTable[1], false) == L"DupBone" &&
          text_encoding::MorphName(loadedSource.morphs[0], false) == L"DupFace" &&
          text_encoding::MorphName(loadedSource.morphs[1], false) == L"DupFace" &&
          text_encoding::BoneName(loadedTarget.boneTable[1], false) == L"DupBone" &&
          text_encoding::BoneName(loadedTarget.boneTable[3], false) == L"DupBone" &&
          text_encoding::MorphName(loadedTarget.morphs[1], false) == L"DupFace" &&
          text_encoding::MorphName(loadedTarget.morphs[3], false) == L"DupFace",
          "the complete loaded name tables hold the duplicate pairs");
    constexpr int kSourceSecond = 1;
    constexpr int kTargetSecond = 3;
    Check(kSourceSecond != kTargetSecond,
          "the source and target second duplicates sit at different absolute indices");
    Check(loadedSource.boneTable[kSourceSecond].type == mdl::BoneType::Move &&
          loadedTarget.boneTable[kTargetSecond].type == mdl::BoneType::Move &&
          (loadedSource.boneTable[kSourceSecond].flags & mdl::kBoneFlagMovable) != 0 &&
          (loadedTarget.boneTable[kTargetSecond].flags & mdl::kBoneFlagMovable) != 0,
          "the fixture enables real PMX rotate/translate flags instead of editing loaded records");
    std::printf("STAGE tables: source_second=%d target_second=%d source_bones=2 "
                "source_morphs=2 target_bones=4 target_morphs=4\n",
                kSourceSecond, kTargetSecond);

    std::puts("STAGE register: ordinary bone/morph registrars set the standard frames");
    // The registrars append their before-image to the current undo entry, so
    // open that entry first, exactly as command 421 does before registering.
    Check(BeginModelFramePasteUndo(sourceRaw, 0, 8, false, false),
          "open the before-image batch for the source fixture keys");
    ResetBoneKeyCursor(sourceRaw);
    RegisterDuplicateBoneRoot(sourceRaw, 0, kDuplicateSourceBoneRoot0Position,
                              kDuplicateSourceBoneRoot0Rotation, false);
    RegisterDuplicateBoneRoot(sourceRaw, 1, kDuplicateSourceBoneRoot1Position,
                              kDuplicateSourceBoneRoot1Rotation, true);
    Check(RegisterMorphKeyAtIndex(sourceRaw, 0, 0, kDuplicateSourceMorphRoot0),
          "register the source first frame-zero morph root");
    Check(RegisterMorphKeyAtIndex(sourceRaw, 1, 0, kDuplicateSourceMorphRoot1),
          "register the source second frame-zero morph root");
    RegisterDuplicateBoneKey(sourceRaw, 0, 11, kDuplicateSourceSentinelPosition,
                             kDuplicateSentinelInterpolation, false);
    RegisterDuplicateBoneKey(sourceRaw, kSourceSecond, 10, kDuplicateSourcePositionA,
                             kDuplicateSourceInterpolationA, false);
    RegisterDuplicateBoneKey(sourceRaw, kSourceSecond, 14, kDuplicateSourcePositionB,
                             kDuplicateSourceInterpolationB, true);
    Check(RegisterMorphKeyAtIndex(sourceRaw, 0, 12, kDuplicateSourceSentinelMorph),
          "register the source first-duplicate morph sentinel");
    Check(RegisterMorphKeyAtIndex(sourceRaw, kSourceSecond, 10, kDuplicateSourceMorphA),
          "register the source second-duplicate first morph frame");
    Check(RegisterMorphKeyAtIndex(sourceRaw, kSourceSecond, 14, kDuplicateSourceMorphB),
          "register the source second-duplicate second morph frame");

    Check(BeginModelFramePasteUndo(targetRaw, 0, 16, false, false),
          "open the before-image batch for the target fixture keys");
    ResetBoneKeyCursor(targetRaw);
    RegisterDuplicateBoneRoot(targetRaw, 0, kDuplicateTargetBoneRoot0Position,
                              kDuplicateTargetBoneRoot0Rotation, false);
    RegisterDuplicateBoneRoot(targetRaw, 1, kDuplicateTargetBoneRoot1Position,
                              kDuplicateTargetBoneRoot1Rotation, true);
    RegisterDuplicateBoneRoot(targetRaw, 2, kDuplicateTargetBoneRoot2Position,
                              kDuplicateTargetBoneRoot2Rotation, false);
    RegisterDuplicateBoneRoot(targetRaw, 3, kDuplicateTargetBoneRoot3Position,
                              kDuplicateTargetBoneRoot3Rotation, true);
    Check(RegisterMorphKeyAtIndex(targetRaw, 0, 0, kDuplicateTargetMorphRoot0),
          "register the target leading unrelated frame-zero morph root");
    Check(RegisterMorphKeyAtIndex(targetRaw, 1, 0, kDuplicateTargetMorphRoot1),
          "register the target first-duplicate frame-zero morph root");
    Check(RegisterMorphKeyAtIndex(targetRaw, 2, 0, kDuplicateTargetMorphRoot2),
          "register the target middle unrelated frame-zero morph root");
    Check(RegisterMorphKeyAtIndex(targetRaw, 3, 0, kDuplicateTargetMorphRoot3),
          "register the target second-duplicate frame-zero morph root");
    RegisterDuplicateBoneKey(targetRaw, 0, 13, kDuplicateTargetSentinelPosition0,
                             kDuplicateSentinelInterpolation, false);
    RegisterDuplicateBoneKey(targetRaw, 1, 11, kDuplicateTargetFirstPosition,
                             kDuplicateSentinelInterpolation, true);
    RegisterDuplicateBoneKey(targetRaw, 2, 17, kDuplicateTargetSentinelPosition2,
                             kDuplicateSentinelInterpolation, false);
    RegisterDuplicateBoneKey(targetRaw, kTargetSecond, 20, kDuplicateTargetSecondPosition,
                             kDuplicateSentinelInterpolation, true);
    Check(RegisterMorphKeyAtIndex(targetRaw, 0, 13, kDuplicateTargetSentinelMorph0),
          "register the target leading unrelated morph sentinel");
    Check(RegisterMorphKeyAtIndex(targetRaw, 1, 11, kDuplicateTargetSentinelMorph1),
          "register the target first-duplicate morph sentinel");
    Check(RegisterMorphKeyAtIndex(targetRaw, 2, 17, kDuplicateTargetSentinelMorph2),
          "register the target middle unrelated morph sentinel");
    Check(RegisterMorphKeyAtIndex(targetRaw, kTargetSecond, 20, kDuplicateTargetSentinelMorph3),
          "register the target second-duplicate morph sentinel");

    // Capture the persisted frame-zero root payloads before any command runs.
    // Links and selection bytes are excluded; only the persisted pose/value data
    // is compared, so replay may move slots without weakening this baseline.
    DuplicateBoneRootPayload sourceBoneRoots[2]{};
    DuplicateBoneRootPayload targetBoneRoots[4]{};
    DuplicateMorphRootPayload sourceMorphRoots[2]{};
    DuplicateMorphRootPayload targetMorphRoots[4]{};
    CaptureDuplicateRoots(sourceRaw, targetRaw, sourceBoneRoots, targetBoneRoots,
                          sourceMorphRoots, targetMorphRoots);

    // Mark only the source second-track frames: two bone keys and two morph keys.
    ClearDuplicateKeyMarks(sourceRaw);
    mdl::BoneKey* const sourceBoneFirst = FindDuplicateBoneKey(sourceRaw, kSourceSecond, 10);
    mdl::BoneKey* const sourceBoneSecond = FindDuplicateBoneKey(sourceRaw, kSourceSecond, 14);
    mdl::MorphKey* const sourceMorphFirst = FindDuplicateMorphKey(sourceRaw, kSourceSecond, 10);
    mdl::MorphKey* const sourceMorphSecond = FindDuplicateMorphKey(sourceRaw, kSourceSecond, 14);
    Check(sourceBoneFirst != nullptr && sourceBoneSecond != nullptr &&
          sourceMorphFirst != nullptr && sourceMorphSecond != nullptr,
          "the marked source frames are reachable through the real track links");
    sourceBoneFirst->allocated = 1;
    sourceBoneSecond->allocated = 1;
    sourceMorphFirst->allocated = 1;
    sourceMorphSecond->allocated = 1;

    app.SetSelectedModelSlot(0);
    app.CurrentFrame() = 40;
    std::puts("STAGE command 420: copy the marked second duplicate bone/morph frames");
    SendMessageW(app.MainWindow(), WM_COMMAND, 420, 0);
    const mdl::ClipboardSelectionCounts& clipboardCounts = app.ClipboardCounts();
    Check(clipboardCounts.bones == 2 && clipboardCounts.morphs == 2 &&
          clipboardCounts.displays == 0,
          "command 420 copies exactly the four marked second-track frames");
    std::printf("STAGE copy: bones=%u morphs=%u displays=%u\n",
                clipboardCounts.bones, clipboardCounts.morphs, clipboardCounts.displays);

    const mdl::BoneClipboardRecord* clipboardBoneZero = nullptr;
    const mdl::BoneClipboardRecord* clipboardBoneFour = nullptr;
    for (std::uint32_t i = 0; i < clipboardCounts.bones; ++i) {
        const mdl::BoneClipboardRecord& record = app.BoneClipboard()[i];
        Check(std::strncmp(record.name, "DupBone", sizeof record.name) == 0,
              "every clipboard bone record carries the complete duplicate name");
        if (record.frame == 0) clipboardBoneZero = &record;
        else if (record.frame == 4) clipboardBoneFour = &record;
    }
    Check(clipboardBoneZero != nullptr && clipboardBoneFour != nullptr,
          "the copied bone records keep the relative frames zero and four");
    Check(std::memcmp(clipboardBoneZero->position, kDuplicateSourcePositionA,
                      sizeof clipboardBoneZero->position) == 0 &&
          std::memcmp(clipboardBoneZero->rotation, kDuplicateIdentityRotation,
                      sizeof clipboardBoneZero->rotation) == 0 &&
          std::memcmp(clipboardBoneZero->interpolation, kDuplicateSourceInterpolationA,
                      sizeof clipboardBoneZero->interpolation) == 0 &&
          clipboardBoneZero->physicsDisabled == 0,
          "the first clipboard bone record keeps position, unit rotation, native interpolation and physics flag");
    Check(std::memcmp(clipboardBoneFour->position, kDuplicateSourcePositionB,
                      sizeof clipboardBoneFour->position) == 0 &&
          std::memcmp(clipboardBoneFour->rotation, kDuplicateIdentityRotation,
                      sizeof clipboardBoneFour->rotation) == 0 &&
          std::memcmp(clipboardBoneFour->interpolation, kDuplicateSourceInterpolationB,
                      sizeof clipboardBoneFour->interpolation) == 0 &&
          clipboardBoneFour->physicsDisabled == 1,
          "the second clipboard bone record keeps position, unit rotation, native interpolation and physics flag");
    const mdl::MorphClipboardRecord* clipboardMorphZero = nullptr;
    const mdl::MorphClipboardRecord* clipboardMorphFour = nullptr;
    for (std::uint32_t i = 0; i < clipboardCounts.morphs; ++i) {
        const mdl::MorphClipboardRecord& record = app.MorphClipboard()[i];
        Check(std::strncmp(record.name, "DupFace", sizeof record.name) == 0,
              "every clipboard morph record carries the complete duplicate name");
        if (record.frame == 0) clipboardMorphZero = &record;
        else if (record.frame == 4) clipboardMorphFour = &record;
    }
    Check(clipboardMorphZero != nullptr && clipboardMorphZero->value == kDuplicateSourceMorphA &&
          clipboardMorphFour != nullptr && clipboardMorphFour->value == kDuplicateSourceMorphB,
          "the copied morph records keep their relative frames and distinct values");

    app.SetSelectedModelSlot(1);
    app.CurrentFrame() = 40;
    std::puts("STAGE command 421: paste at frame 40 onto the target model");
    SendMessageW(app.MainWindow(), WM_COMMAND, 421, 0);
    VerifyDuplicatePastePair(sourceRaw, targetRaw);
    VerifyDuplicateRoots(sourceRaw, targetRaw, sourceBoneRoots, targetBoneRoots,
                         sourceMorphRoots, targetMorphRoots,
                         "frame-zero root payloads survive the ordinary copy and paste");
    std::printf("STAGE paste: source_second=%d target_second=%d frames=40,44\n",
                kSourceSecond, kTargetSecond);

    // Remove one matching occurrence at a time. The other key family still
    // resolves, so this catches partial edits as well as first-match fallback.
    auto& target = *mdl::Mdl(targetRaw);
    auto& boneName = target.boneTable[kTargetSecond].name;
    auto& morphName = target.morphs[kTargetSecond].name;
    const auto expectRejection = [&](auto& name, unsigned char language) {
        app.EnglishUI() = language;
        const char firstByte = name[0];
        name[0] = '_';
        const auto savedUndo = target.undoState[0];
        const auto savedRedo = target.undoState[1];
        const auto savedDirty = app.SceneModified();
        const auto* savedBones = app.BoneClipboard();
        const auto* savedMorphs = app.MorphClipboard();
        const auto boneCapacity = mdl::BoneKeyCapacity(target);
        const auto morphCapacity = mdl::MorphKeyCapacity(target.morphKeys);
        const std::vector<mdl::BoneKey> bones(target.boneKeys, target.boneKeys + boneCapacity);
        const std::vector<mdl::MorphKey> morphs(target.morphKeys, target.morphKeys + morphCapacity);
        app.CurrentFrame() = 60;
        MissingPasteWarning warning{app.MainWindow(), language};
        missingPasteWarning = &warning;
        SendMessageW(app.MainWindow(), WM_COMMAND, 421, 0);
        missingPasteWarning = nullptr;
        Check(warning.window && !IsWindow(warning.window), "the warning closes after one dismissal");
        Check(app.CurrentFrame() == 60 && target.undoState[0] == savedUndo &&
              target.undoState[1] == savedRedo && app.SceneModified() == savedDirty &&
              app.BoneClipboard() == savedBones && app.MorphClipboard() == savedMorphs &&
              app.ClipboardCounts().bones == 2 && app.ClipboardCounts().morphs == 2,
              "refused paste preserves history cursor, frame, dirty flag and clipboard");
        Check(mdl::BoneKeyCapacity(target) == boneCapacity && mdl::MorphKeyCapacity(target.morphKeys) == morphCapacity &&
              !std::memcmp(target.boneKeys, bones.data(), bones.size() * sizeof(mdl::BoneKey)) &&
              !std::memcmp(target.morphKeys, morphs.data(), morphs.size() * sizeof(mdl::MorphKey)),
              "refused paste preserves every target key and selection mark");
        name[0] = firstByte;
    };
    const auto previousLanguage = app.EnglishUI();
    expectRejection(boneName, kUiJapanese);
    expectRejection(morphName, kUiEnglish);
    expectRejection(boneName, kUiChinese);
    app.EnglishUI() = previousLanguage;
    std::puts("PASS missing duplicate track: localized warning, complete rejection and prior undo preserved");

    // Replace the clipboard with different content, then undo and redo the
    // still-pending paste. Neither replay step may consult this new clipboard.
    ClearDuplicateKeyMarks(sourceRaw);
    mdl::BoneKey* const replacement = FindDuplicateBoneKey(sourceRaw, 0, 11);
    Check(replacement != nullptr, "the unrelated source sentinel is reachable for a replacement copy");
    replacement->allocated = 1;
    app.SetSelectedModelSlot(0);
    std::puts("STAGE command 420: replace the clipboard with unrelated content");
    SendMessageW(app.MainWindow(), WM_COMMAND, 420, 0);
    Check(app.ClipboardCounts().bones == 1 && app.ClipboardCounts().morphs == 0 &&
          app.ClipboardCounts().displays == 0,
          "the replacement copy holds only the unrelated sentinel bone frame");
    Check(app.BoneClipboard()[0].frame == 0 &&
          app.BoneClipboard()[0].position[0] == kDuplicateSourceSentinelPosition[0],
          "the replacement clipboard content is the sentinel, not the earlier paste");

    app.SetSelectedModelSlot(1);
    app.CurrentFrame() = 40;
    std::puts("STAGE command 400: one undo removes the whole bone and morph paste");
    SendMessageW(app.MainWindow(), WM_COMMAND, 400, 0);
    Check(DuplicateBoneFrames(targetRaw, kTargetSecond) ==
              std::vector<std::uint32_t>{20},
          "one undo removes every pasted bone frame from the second target track");
    Check(DuplicateMorphFrames(targetRaw, kTargetSecond) ==
              std::vector<std::uint32_t>{20},
          "the same undo removes every pasted morph frame from the second target track");
    Check(FindDuplicateBoneKey(targetRaw, kTargetSecond, 40) == nullptr &&
          FindDuplicateBoneKey(targetRaw, kTargetSecond, 44) == nullptr &&
          FindDuplicateMorphKey(targetRaw, kTargetSecond, 40) == nullptr &&
          FindDuplicateMorphKey(targetRaw, kTargetSecond, 44) == nullptr,
          "undo leaves no pasted bone or morph frame on the second target track");
    CheckDuplicateBoneKey(targetRaw, kTargetSecond, 20, kDuplicateTargetSecondPosition,
                          kDuplicateSentinelInterpolation, true,
                          "undo restores the second target duplicate sentinel pose");
    CheckDuplicateBoneKey(targetRaw, 1, 11, kDuplicateTargetFirstPosition,
                          kDuplicateSentinelInterpolation, true,
                          "undo keeps the first target duplicate sentinel pose");
    CheckDuplicateBoneKey(targetRaw, 0, 13, kDuplicateTargetSentinelPosition0,
                          kDuplicateSentinelInterpolation, false,
                          "undo keeps the leading unrelated target sentinel pose");
    CheckDuplicateBoneKey(targetRaw, 2, 17, kDuplicateTargetSentinelPosition2,
                          kDuplicateSentinelInterpolation, false,
                          "undo keeps the middle unrelated target sentinel pose");
    CheckDuplicateMorphKey(targetRaw, kTargetSecond, 20, kDuplicateTargetSentinelMorph3,
                           "undo restores the second target duplicate sentinel morph value");
    CheckDuplicateMorphKey(targetRaw, 1, 11, kDuplicateTargetSentinelMorph1,
                           "undo keeps the first target duplicate sentinel morph value");
    Check(app.ClipboardCounts().bones == 1 && app.ClipboardCounts().morphs == 0,
          "undo does not consume the replacement clipboard contents");
    // The source is untouched by the target's undo; verify its key payloads.
    CheckDuplicateBoneKey(sourceRaw, kSourceSecond, 10, kDuplicateSourcePositionA,
                          kDuplicateSourceInterpolationA, false,
                          "undo on the target leaves the source first frame unchanged");
    CheckDuplicateBoneKey(sourceRaw, kSourceSecond, 14, kDuplicateSourcePositionB,
                          kDuplicateSourceInterpolationB, true,
                          "undo on the target leaves the source second frame unchanged");
    VerifyDuplicateRoots(sourceRaw, targetRaw, sourceBoneRoots, targetBoneRoots,
                         sourceMorphRoots, targetMorphRoots,
                         "frame-zero root payloads survive one undo of the pasted batch");

    std::puts("STAGE command 401: one redo restores the original paste");
    SendMessageW(app.MainWindow(), WM_COMMAND, 401, 0);
    VerifyDuplicatePastePair(sourceRaw, targetRaw);
    VerifyDuplicateRoots(sourceRaw, targetRaw, sourceBoneRoots, targetBoneRoots,
                         sourceMorphRoots, targetMorphRoots,
                         "frame-zero root payloads survive one redo of the original paste");
    Check(app.ClipboardCounts().bones == 1 && app.ClipboardCounts().morphs == 0,
          "redo restores the original paste without reading the replacement clipboard");
    std::printf("STAGE redo: source_second=%d target_second=%d frames=20,40,44\n",
                kSourceSecond, kTargetSecond);

    std::puts("STAGE save: real SaveSceneCopy of both loaded models");
    Check(SaveSceneCopy(&app, savedPmm.c_str()),
          "production SaveSceneCopy saves the duplicate-name project");
    Check(std::filesystem::file_size(savedPmm) > 32,
          "the saved project contains a complete standard PMM body");
    Read(savedPmm);  // asserts the standard PMM 0002 header and extension framing
    std::printf("STAGE save path=%ls\n", savedPmm.c_str());

    std::puts("STAGE reopen: real LoadSceneFile of the saved project");
    Load(app, savedPmm);
    Check(app.ModelSlot(0) != nullptr && app.ModelSlot(1) != nullptr,
          "reopening the saved project recreates both models from their PMX files");
    unsigned char* reopenedSource = nullptr;
    unsigned char* reopenedTarget = nullptr;
    FindDuplicatePasteModels(app, sourcePmx, targetPmx, &reopenedSource, &reopenedTarget);
    VerifyDuplicatePastePair(reopenedSource, reopenedTarget);
    VerifyDuplicateRoots(reopenedSource, reopenedTarget, sourceBoneRoots, targetBoneRoots,
                         sourceMorphRoots, targetMorphRoots,
                         "frame-zero root payloads survive the real PMM save and reopen");
    Check(EncodingFileBytes(sourcePmx) == sourceBytes &&
          EncodingFileBytes(targetPmx) == targetBytes,
          "neither original PMX file changes across copy, paste, undo, save and reopen");
    std::printf("STAGE reopen result: project=%ls models=2 target_frames=20,40,44\n",
                savedPmm.c_str());

    std::puts("STAGE save-as and reopen: one more real PMM roundtrip");
    Check(SaveSceneCopy(&app, resavedPmm.c_str()),
          "production SaveSceneCopy saves the reopened duplicate-name project again");
    Load(app, resavedPmm);
    Check(app.ModelSlot(0) != nullptr && app.ModelSlot(1) != nullptr,
          "the second PMM roundtrip recreates both models again");
    unsigned char* stableSource = nullptr;
    unsigned char* stableTarget = nullptr;
    FindDuplicatePasteModels(app, sourcePmx, targetPmx, &stableSource, &stableTarget);
    VerifyDuplicatePastePair(stableSource, stableTarget);
    VerifyDuplicateRoots(stableSource, stableTarget, sourceBoneRoots, targetBoneRoots,
                         sourceMorphRoots, targetMorphRoots,
                         "frame-zero root payloads survive the second PMM roundtrip");
    Check(EncodingFileBytes(sourcePmx) == sourceBytes &&
          EncodingFileBytes(targetPmx) == targetBytes,
          "the second PMM roundtrip leaves both original PMX files byte for byte unchanged");
    std::printf("STAGE second reopen result: project=%ls models=2 target_frames=20,40,44\n",
                resavedPmm.c_str());
    std::printf("PASS duplicate-paste roundtrip: source_second=%d target_second=%d "
                "saved=%ls reopened=2 PMX=unchanged\n",
                kSourceSecond, kTargetSecond, savedPmm.c_str());
}


// ---- camera-mode PMM model target and draw-order persistence --------------
// One ordinary production path: three real PMX files through LoadModelFile, the
// real 436 model selector, the real 536 model/camera button, production
// SaveSceneCopy / LoadSceneFile, and the real menu 288 display-order dialog.
// Nothing here repairs the selector, edits order bytes directly, or calls a
// private dialog apply helper.
constexpr int kCameraTargetCount = 3;

struct CameraModelTargetDialogDriver {
    bool active = false;
    HWND window = nullptr;
    int targetSlot = 0;
    unsigned activations = 0;
    std::array<std::wstring, kCameraTargetCount> initialLabels;
    std::array<int, kCameraTargetCount> initialSlots{};
    bool Handle(HWND dialog);
};

CameraModelTargetDialogDriver* cameraModelTargetDialog = nullptr;

struct CameraTargetDeleteConfirmation {
    HWND owner;
    int answer;
    HWND window = nullptr;

    bool Handle(HWND dialog) {
        if (dialog == window) return true;
        if (window || GetWindow(dialog, GW_OWNER) != owner ||
            !GetDlgItem(dialog, IDOK) || !GetDlgItem(dialog, IDCANCEL)) return false;
        bool namesTarget = false;
        EnumChildWindows(dialog, [](HWND child, LPARAM parameter) -> BOOL {
            wchar_t cls[32]{}, body[512]{};
            GetClassNameW(child, cls, std::size(cls));
            GetWindowTextW(child, body, std::size(body));
            constexpr std::wstring_view suffix = L"\n\nBravo";
            const std::wstring_view text = body;
            auto& matched = *reinterpret_cast<bool*>(parameter);
            matched = std::wcscmp(cls, L"Static") == 0 && text.size() >= suffix.size() &&
                      text.substr(text.size() - suffix.size()) == suffix;
            return !matched;
        }, reinterpret_cast<LPARAM>(&namesTarget));
        if (!namesTarget) return false;
        window = dialog;
        Check(PostMessageW(dialog, WM_COMMAND, answer, 0) != FALSE,
              "answer the real delete confirmation for Bravo");
        return true;
    }
};

CameraTargetDeleteConfirmation* cameraTargetDeleteConfirmation = nullptr;

std::wstring CameraTargetListText(HWND list, int row) {
    const auto length = SendMessageW(list, LB_GETTEXTLEN, static_cast<WPARAM>(row), 0);
    Check(length >= 0, "model-order row exists in the actual 628 list");
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    Check(SendMessageW(list, LB_GETTEXT, static_cast<WPARAM>(row),
                       reinterpret_cast<LPARAM>(text.data())) == length,
          "read the complete model-order row from the actual 628 list");
    text.resize(static_cast<std::size_t>(length));
    return text;
}

bool CameraModelTargetDialogDriver::Handle(HWND dialog) {
    // A repeated HCBT_ACTIVATE for the window already driven by this step is not
    // a second dialog; a different dialog while the step is armed stays fatal.
    if (window == dialog) return true;
    if (window != nullptr || !active) return false;
    // Mark the window before any validation or posted command so a repeated
    // activation cannot enqueue the 630/632 sequence twice.
    window = dialog;
    ++activations;
    Check(g_Block != nullptr, "the camera target driver runs inside a real host");
    HWND const list = GetDlgItem(dialog, panel::kOrderListBox);
    Check(list != nullptr && GetDlgItem(dialog, panel::kOrderUpButton) != nullptr &&
          GetDlgItem(dialog, panel::kOrderDownButton) != nullptr &&
          GetDlgItem(dialog, panel::kDialogOkButton) != nullptr,
          "menu command 288 opens the actual 628/630/631/632 draw-order dialog");
    Check(SendMessageW(list, LB_GETCOUNT, 0, 0) == kCameraTargetCount,
          "the actual draw-order list holds all three loaded models");
    auto* const scratch = static_cast<const std::int32_t*>(g_Block->AccessoryOrderArray());
    Check(scratch != nullptr, "the actual draw-order dialog publishes its bounded scratch identities");
    const bool english = UiUsesEnglishNames(g_Block->EnglishUI());
    int targetRow = -1;
    for (int row = 1; row <= kCameraTargetCount; ++row) {
        const std::int32_t stored = scratch[row];
        Check(stored > 0 && stored <= kModelSlotCount,
              "every actual draw-order row carries a bounded model slot identity");
        const int slot = static_cast<int>(stored) - 1;
        unsigned char* const model = g_Block->ModelSlot(slot);
        Check(model != nullptr && mdl::Mdl(model)->comboSelIndex == row,
              "each actual draw-order row is its object's current display rank");
        const std::wstring label = text_encoding::ModelLabel(*mdl::Mdl(model), english);
        Check(CameraTargetListText(list, row - 1) == label,
              "each actual draw-order caption belongs to the object its identity resolves");
        initialLabels[row - 1] = label;
        initialSlots[row - 1] = slot;
        if (slot == targetSlot) targetRow = row - 1;
    }
    Check(targetRow > 0, "the regression starts with the target below the first draw-order row");
    Check(SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(targetRow), 0) != LB_ERR,
          "select the actual target row in the 628 list");
    // Only enqueue the real 630 moves and the real 632 confirmation. Running the
    // dialog procedure synchronously here would reenter HCBT_ACTIVATE handling.
    for (int row = targetRow; row > 0; --row)
        Check(PostMessageW(dialog, WM_COMMAND, panel::kOrderUpButton, 0) != FALSE,
              "queue one actual 630 move for every row above the target");
    Check(PostMessageW(dialog, WM_COMMAND, panel::kDialogOkButton, 0) != FALSE,
          "queue the actual 632 confirmation after moving the target to the first row");
    return true;
}

void CameraTargetFindModels(MMDApp& app, const std::array<std::filesystem::path, kCameraTargetCount>& paths,
                            std::array<unsigned char*, kCameraTargetCount>& models) {
    models = {};
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        unsigned char* const raw = app.ModelSlot(slot);
        if (raw == nullptr) continue;
        const std::filesystem::path stored(mdl::Mdl(raw)->path);
        for (int i = 0; i < kCameraTargetCount; ++i) {
            if (models[i] != nullptr) continue;
            std::error_code error;
            if (std::filesystem::equivalent(stored, paths[i], error) && !error) {
                models[i] = raw;
                break;
            }
        }
    }
    Check(models[0] != nullptr && models[1] != nullptr && models[2] != nullptr,
          "the project resolves all three original PMX model paths");
}

int CameraTargetSlotOf(MMDApp& app, unsigned char* model) {
    for (int slot = 0; slot < kModelSlotCount; ++slot)
        if (app.ModelSlot(slot) == model) return slot;
    return -1;
}

std::wstring CameraTargetLabel(MMDApp& app, unsigned char* model) {
    return text_encoding::ModelLabel(*mdl::Mdl(model), UiUsesEnglishNames(app.EnglishUI()));
}

std::wstring CameraTargetComboText(HWND combo, int row) {
    const auto length = SendMessageW(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(row), 0);
    Check(length >= 0, "model selector row exists");
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    Check(SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(row),
                       reinterpret_cast<LPARAM>(text.data())) == length,
          "read the complete model selector caption");
    text.resize(static_cast<std::size_t>(length));
    return text;
}

int CameraTargetComboRowSlot(MMDApp& app, HWND combo, int row) {
    const auto order = model_order::Project<kModelSlotCount>([&](std::size_t slot) noexcept {
        const auto* model = app.ModelSlot(static_cast<int>(slot));
        return model ? int(mdl::Mdl(model)->comboSelIndex) : -1;
    });
    return model_order::Resolve(order, row, static_cast<std::int64_t>(SendMessageW(combo, CB_GETCOUNT, 0, 0)),
        [&](std::size_t index) noexcept { return SendMessageW(combo, CB_GETITEMDATA, index, 0); });
}

int CameraTargetRememberedSlot(MMDApp& app) {
    HWND const combo = GetDlgItem(app.MainWindow(), panel::kMainComboModel);
    return CameraTargetComboRowSlot(app, combo, static_cast<int>(app.MainModelComboSelection()));
}

void CameraTargetVerifySelectors(MMDApp& app, const std::array<unsigned char*, kCameraTargetCount>& rows,
                                 bool managed, const char* message) {
    for (const int id : {panel::kMainComboModel, panel::kMainComboNormal, panel::kMainComboGround}) {
        HWND const combo = GetDlgItem(app.MainWindow(), id);
        Check(combo != nullptr && SendMessageW(combo, CB_GETCOUNT, 0, 0) == kCameraTargetCount + 1, message);
        if (managed) Check(SendMessageW(combo, CB_GETITEMDATA, 0, 0) == -1, message);
        for (int row = 1; row <= kCameraTargetCount; ++row) {
            unsigned char* const model = rows[row - 1];
            const int slot = CameraTargetSlotOf(app, model);
            Check(model != nullptr && slot >= 0, message);
            if (managed) Check(SendMessageW(combo, CB_GETITEMDATA, row, 0) == slot, message);
            Check(CameraTargetComboText(combo, row) == CameraTargetLabel(app, model), message);
        }
    }
}

void CameraTargetVerifySources(MMDApp& app, const std::array<unsigned char*, kCameraTargetCount>& models,
                               const std::array<std::filesystem::path, kCameraTargetCount>& paths,
                               const std::array<std::wstring, kCameraTargetCount>& titles,
                               const std::array<unsigned char, kCameraTargetCount>& draw,
                               const std::array<unsigned char, kCameraTargetCount>& calculate,
                               const char* message) {
    for (int i = 0; i < kCameraTargetCount; ++i) {
        Check(models[i] != nullptr && CameraTargetSlotOf(app, models[i]) >= 0, message);
        std::error_code error;
        const std::filesystem::path stored(mdl::Mdl(models[i])->path);
        Check(std::filesystem::equivalent(stored, paths[i], error) && !error, message);
        Check(mdl::Mdl(models[i])->comboSelIndex == draw[i] &&
              mdl::Mdl(models[i])->comboSelIndex2 == calculate[i], message);
        Check(CameraTargetLabel(app, models[i]) == titles[i], message);
    }
}

void CameraTargetClickModelToggle(MMDApp& app, const char* message) {
    HWND const viewport = app.FloatingWindow() != nullptr ? app.FloatingWindow() : app.MainWindow();
    HWND const toggle = GetDlgItem(viewport, panel::kModelEditToggle);
    wchar_t cls[32]{};
    Check(toggle != nullptr && GetDlgCtrlID(toggle) == panel::kModelEditToggle, message);
    GetClassNameW(toggle, cls, 32);
    Check(std::wcscmp(cls, L"Button") == 0, message);
    Check(GetParent(toggle) == viewport && IsWindowEnabled(toggle) != FALSE, message);
    SendMessageW(toggle, BM_CLICK, 0, 0);
}

void CameraTargetDeleteSelected(MMDApp& app, int answer) {
    constexpr int kDeleteModelButton = 437;
    const auto button = GetDlgItem(app.MainWindow(), kDeleteModelButton);
    Check(button && IsWindowEnabled(button), "the ordinary model delete button is available");
    CameraTargetDeleteConfirmation confirmation{app.MainWindow(), answer};
    cameraTargetDeleteConfirmation = &confirmation;
    SendMessageW(button, BM_CLICK, 0, 0);
    cameraTargetDeleteConfirmation = nullptr;
    Check(confirmation.window && !IsWindow(confirmation.window),
          "the actual delete confirmation appeared for Bravo and closed");
}

void CameraTargetVerifyFallback(MMDApp& app, int alphaSlot, int bravoSlot, int charlieSlot,
                                const std::filesystem::path& alphaPath,
                                const std::filesystem::path& charliePath) {
    auto* const alpha = app.ModelSlot(alphaSlot);
    auto* const charlie = app.ModelSlot(charlieSlot);
    Check(alpha && charlie && !app.ModelSlot(bravoSlot), "only Alpha and Charlie remain in their original slots");
    Check(std::filesystem::equivalent(mdl::Mdl(alpha)->path, alphaPath) &&
          std::filesystem::equivalent(mdl::Mdl(charlie)->path, charliePath),
          "both remaining objects resolve to the correct PMX sources");
    const auto combo = GetDlgItem(app.MainWindow(), panel::kMainComboModel);
    Check(app.CameraMode() != 0 && SendMessageW(combo, CB_GETCURSEL, 0, 0) == 0,
          "deleting the active model and reopening the saved project both leave camera mode visible");
    Check(SendMessageW(combo, CB_GETCOUNT, 0, 0) == 3 &&
          SendMessageW(combo, CB_GETITEMDATA, 1, 0) == alphaSlot &&
          SendMessageW(combo, CB_GETITEMDATA, 2, 0) == charlieSlot &&
          CameraTargetComboText(combo, 1) == L"Alpha" && CameraTargetComboText(combo, 2) == L"Charlie",
          "the remaining visible rows carry the correct names and model slots");
    CameraTargetClickModelToggle(app, "To model is available with models remaining after deletion");
    Check(app.CameraMode() == 0 && app.SelectedModelSlot() == alphaSlot && app.SelectedModel() == alpha &&
          SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1 && app.MainModelComboSelection() == 1,
          "To model selects the first remaining object without an intervening model-list click");
}

void CameraModelTargetRoundtrip(MMDApp& app, const std::filesystem::path& directory) {
    std::printf("PROFILE original=%d CAMERA_MODEL_TARGET directory=%ls\n",
                build_profile::original ? 1 : 0, directory.c_str());
    Check(app.ModelSlot(0) == nullptr && app.ModelSlot(1) == nullptr,
          "the camera-model target fixture starts on an empty scene");

    const std::array<std::filesystem::path, kCameraTargetCount> pmx{
        directory / L"camera-model-target-alpha.pmx",
        directory / L"camera-model-target-bravo.pmx",
        directory / L"camera-model-target-charlie.pmx"};
    const auto firstPmm = directory / L"camera-model-target-first.pmm";
    const auto secondPmm = directory / L"camera-model-target-second.pmm";
    const std::array<std::wstring, kCameraTargetCount> titles{L"Alpha", L"Bravo", L"Charlie"};
    const std::array<std::wstring, kCameraTargetCount> boneNames{L"AlphaBone", L"BravoBone", L"CharlieBone"};
    for (const auto& path : pmx)
        Check(!std::filesystem::exists(path), "the camera-model target fixture writes only brand-new PMX files");
    Check(!std::filesystem::exists(firstPmm) && !std::filesystem::exists(secondPmm),
          "the camera-model target fixture writes only brand-new PMM files");

    const std::uint16_t boneFlags =
        std::uint16_t(kPmxRotatableBoneFlag | mdl::kBoneFlagMovable | mdl::kBoneFlagVisible);
    std::array<std::vector<unsigned char>, kCameraTargetCount> pmxBytes;
    for (int i = 0; i < kCameraTargetCount; ++i) {
        WritePmmNameBoundaryPmx(pmx[i], {boneNames[i]}, {}, titles[i], titles[i], boneFlags);
        pmxBytes[i] = EncodingFileBytes(pmx[i]);
    }

    std::puts("STAGE load: three real PMX files through LoadModelFile");
    for (int i = 0; i < kCameraTargetCount; ++i) {
        LoadModelFile(&app, pmx[i].c_str(), false);
        Check(app.ModelSlot(i) != nullptr, "each real PMX load occupies the next model slot");
    }
    Check(app.ModelSlot(kCameraTargetCount) == nullptr,
          "the fixture loads exactly three models without moving slots");
    Check(app.CameraMode() == 0, "loading real PMX files leaves ordinary model editing active");
    std::array<unsigned char*, kCameraTargetCount> models{};
    CameraTargetFindModels(app, pmx, models);
    const std::array<unsigned char, kCameraTargetCount> initialDraw{1, 2, 3};
    const std::array<unsigned char, kCameraTargetCount> initialCalculate{1, 2, 3};
    CameraTargetVerifySources(app, models, pmx, titles, initialDraw, initialCalculate,
        "the loaded PMX models expose their real paths, names, display order and calculation order");
    CameraTargetVerifySelectors(app, models, false,
        "the ordinary loaded model selectors carry every real model caption in draw order");

    std::puts("STAGE model selection: actual 436 combo picks the middle model Bravo");
    HWND const main = app.MainWindow();
    HWND const modelCombo = GetDlgItem(main, panel::kMainComboModel);
    Check(modelCombo != nullptr, "the main window exposes the real 436 model selector");
    const int bravoSlot = CameraTargetSlotOf(app, models[1]);
    // The fixture has independently verified the load order Alpha/Bravo/Charlie
    // and stored draw ranks 1/2/3 before this point, so the ordinary visible
    // middle row is fixed at 2. The production mapping result is logged only:
    // it must not gate the real selector command, whose own strict postcondition
    // below still has to prove the selection. No model, selection or selector
    // state is written here before the ordinary control messages.
    constexpr int bravoRow = 2;
    const LRESULT selectorCount = SendMessageW(modelCombo, CB_GETCOUNT, 0, 0);
    std::printf("DIAG select before: combo_count=%lld\n", static_cast<long long>(selectorCount));
    for (LRESULT row = 0; row < selectorCount; ++row)
        std::printf("DIAG select before: row=%lld itemdata=%lld text=%ls\n",
                    static_cast<long long>(row),
                    static_cast<long long>(SendMessageW(modelCombo, CB_GETITEMDATA, row, 0)),
                    CameraTargetComboText(modelCombo, static_cast<int>(row)).c_str());
    for (int i = 0; i < kCameraTargetCount; ++i)
        std::printf("DIAG select before: model=%d slot=%d draw_rank=%u calculate_rank=%u label=%ls\n",
                    i, CameraTargetSlotOf(app, models[i]),
                    static_cast<unsigned>(mdl::Mdl(models[i])->comboSelIndex),
                    static_cast<unsigned>(mdl::Mdl(models[i])->comboSelIndex2),
                    CameraTargetLabel(app, models[i]).c_str());
    for (int row = 1; row <= kCameraTargetCount; ++row)
        std::printf("DIAG select before: production_resolve row=%d slot=%d\n",
                    row, CameraTargetComboRowSlot(app, modelCombo, row));
    Check(selectorCount > bravoRow &&
          CameraTargetComboText(modelCombo, bravoRow) == CameraTargetLabel(app, models[1]),
          "the visible second model row carries the independently verified Bravo caption");
    Check(SendMessageW(modelCombo, CB_SETCURSEL, static_cast<WPARAM>(bravoRow), 0) != CB_ERR,
          "select the actual middle model row");
    SendMessageW(main, WM_COMMAND, MAKEWPARAM(panel::kMainComboModel, CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(modelCombo));
    std::printf("DIAG select after: camera=%u cursel=%lld selected_slot=%u "
                "selected_is_bravo=%d remembered_row=%u visible_text=%ls\n",
                static_cast<unsigned>(app.CameraMode()),
                static_cast<long long>(SendMessageW(modelCombo, CB_GETCURSEL, 0, 0)),
                static_cast<unsigned>(app.SelectedModelSlot()),
                app.SelectedModel() == models[1] ? 1 : 0, app.MainModelComboSelection(),
                CameraTargetComboText(modelCombo,
                    static_cast<int>(SendMessageW(modelCombo, CB_GETCURSEL, 0, 0))).c_str());
    Check(app.CameraMode() == 0 && app.SelectedModelSlot() == bravoSlot &&
          app.SelectedModel() == models[1] &&
          app.MainModelComboSelection() == static_cast<std::uint32_t>(bravoRow),
          "the real model selector selects the Bravo object without changing modes");

    std::puts("STAGE camera: actual button 536 switches to the camera panel");
    CameraTargetClickModelToggle(app,
        "control 536 is an enabled Button child of the real viewport window");
    Check(app.CameraMode() == 1, "the actual 536 button enters camera mode");
    Check(SendMessageW(modelCombo, CB_GETCURSEL, 0, 0) == 0,
          "camera mode leaves the visible model selector on row zero");
    Check(app.MainModelComboSelection() == static_cast<std::uint32_t>(bravoRow) &&
          CameraTargetRememberedSlot(app) == bravoSlot,
          "camera mode remembers Bravo as the real To-model target");
    Check(app.SelectedModelSlot() == bravoSlot && app.SelectedModel() == models[1],
          "camera mode retains Bravo as the selected model object");
    std::printf("STAGE camera state: camera=%u visible_row=%lld remembered_row=%u "
                "remembered_slot=%d selected_slot=%d target_pmx=%ls\n",
                static_cast<unsigned>(app.CameraMode()),
                static_cast<long long>(SendMessageW(modelCombo, CB_GETCURSEL, 0, 0)),
                app.MainModelComboSelection(), CameraTargetRememberedSlot(app),
                static_cast<int>(app.SelectedModelSlot()), pmx[1].c_str());

    std::puts("STAGE save: production SaveSceneCopy of the camera-mode project");
    Check(SaveSceneCopy(&app, firstPmm.c_str()), "production SaveSceneCopy saves the camera-mode project");
    Check(std::filesystem::file_size(firstPmm) > 32,
          "the camera-mode project contains a complete standard PMM body");
    Read(firstPmm);
    std::puts("STAGE reopen: real LoadSceneFile of the camera-mode project");
    Load(app, firstPmm);
    CameraTargetFindModels(app, pmx, models);
    Check(app.CameraMode() == 1, "the reopened camera-mode project opens in camera mode");
    Check(SendMessageW(modelCombo, CB_GETCURSEL, 0, 0) == 0,
          "the reopened camera-mode selector still shows row zero");
    Check(SendMessageW(modelCombo, CB_GETITEMDATA, 0, 0) == -1,
          "the reopened selector zero row is the explicit non-model row");
    Check(app.MainModelComboSelection() > 0 &&
          app.MainModelComboSelection() < static_cast<std::uint32_t>(kCameraTargetCount + 1) &&
          CameraTargetRememberedSlot(app) == bravoSlot,
          "the reopened project remembers Bravo as the To-model target without a selector click");
    Check(app.SelectedModelSlot() == bravoSlot && app.SelectedModel() == models[1],
          "the reopened project keeps Bravo as the selected model object");
    CameraTargetVerifySelectors(app, models, true,
        "the reopened selectors map every row identity and caption to the real model objects");
    std::printf("STAGE reopen state: project=%ls camera=%u visible_row=%lld remembered_row=%u "
                "remembered_slot=%d selected_slot=%d target_pmx=%ls\n",
                firstPmm.c_str(), static_cast<unsigned>(app.CameraMode()),
                static_cast<long long>(SendMessageW(modelCombo, CB_GETCURSEL, 0, 0)),
                app.MainModelComboSelection(), CameraTargetRememberedSlot(app),
                static_cast<int>(app.SelectedModelSlot()), pmx[1].c_str());

    std::puts("STAGE to model: actual button 536 with no selector or model-list repair");
    CameraTargetClickModelToggle(app,
        "the same real 536 button toggles a reopened camera-mode project");
    Check(app.CameraMode() == 0, "the actual 536 button returns the reopened project to model editing");
    const auto mainRow = SendMessageW(modelCombo, CB_GETCURSEL, 0, 0);
    Check(mainRow > 0 && CameraTargetComboRowSlot(app, modelCombo, static_cast<int>(mainRow)) == bravoSlot,
          "the real 536 button lands on the remembered Bravo object");
    Check(app.MainModelComboSelection() == static_cast<std::uint32_t>(mainRow),
          "the visible row and the remembered target agree after To model");
    Check(app.SelectedModelSlot() == bravoSlot && app.SelectedModel() == models[1],
          "To model selects the Bravo object itself");
    Check(SendMessageW(modelCombo, CB_GETITEMDATA, mainRow, 0) == bravoSlot,
          "the visible row's item data is the Bravo model slot");
    Check(CameraTargetComboText(modelCombo, static_cast<int>(mainRow)) == CameraTargetLabel(app, models[1]),
          "the visible row's caption is the Bravo object name");
    CameraTargetVerifySelectors(app, models, true,
        "To model keeps every selector identity and caption attached to its real object");
    CameraTargetVerifySources(app, models, pmx, titles, initialDraw, initialCalculate,
        "To model leaves every model source, name and independent order intact");
    std::printf("STAGE to model result: camera=%u visible_row=%lld target_slot=%d selected_slot=%d\n",
                static_cast<unsigned>(app.CameraMode()), static_cast<long long>(mainRow),
                CameraTargetComboRowSlot(app, modelCombo, static_cast<int>(mainRow)),
                static_cast<int>(app.SelectedModelSlot()));

    std::puts("STAGE draw order: real menu command 288 moves Bravo to the first row");
    CameraModelTargetDialogDriver driver;
    driver.active = true;
    driver.targetSlot = bravoSlot;
    cameraModelTargetDialog = &driver;
    SendMessageW(main, WM_COMMAND, 288, 0);
    cameraModelTargetDialog = nullptr;
    driver.active = false;
    Check(driver.activations == 1, "the actual 288 dialog is driven exactly once");
    Check(driver.window != nullptr && IsWindow(driver.window) == FALSE,
          "the actual 288 dialog completes and closes through its real 632 button");
    Check(app.AccessoryOrderArray() == nullptr,
          "the actual 288 dialog releases only its bounded scratch order");
    Check(driver.initialSlots[0] == CameraTargetSlotOf(app, models[0]) &&
          driver.initialSlots[1] == bravoSlot &&
          driver.initialSlots[2] == CameraTargetSlotOf(app, models[2]),
          "the 288 list starts from the three real draw-order identities");
    Check(driver.initialLabels[0] == titles[0] && driver.initialLabels[1] == titles[1] &&
          driver.initialLabels[2] == titles[2],
          "the 288 list starts from the three real model captions");
    const std::array<unsigned char, kCameraTargetCount> movedDraw{2, 1, 3};
    const std::array<unsigned char*, kCameraTargetCount> movedRows{models[1], models[0], models[2]};
    CameraTargetVerifySources(app, models, pmx, titles, movedDraw, initialCalculate,
        "the real 288 confirmation publishes the requested draw order and preserves calculation order");
    CameraTargetVerifySelectors(app, movedRows, true,
        "every selector row follows the new draw order with its real object identity");
    Check(SendMessageW(modelCombo, CB_GETCURSEL, 0, 0) == 1,
          "the reordered selector keeps Bravo on its new first model row");
    Check(app.MainModelComboSelection() == 1 && app.SelectedModelSlot() == bravoSlot &&
          app.SelectedModel() == models[1],
          "the draw-order confirmation keeps the selected Bravo identity and remembered row");
    std::printf("STAGE draw order result: draw=(%u,%u,%u) calculate=(%u,%u,%u) "
                "visible_row=%lld selected_slot=%d\n",
                static_cast<unsigned>(mdl::Mdl(models[0])->comboSelIndex),
                static_cast<unsigned>(mdl::Mdl(models[1])->comboSelIndex),
                static_cast<unsigned>(mdl::Mdl(models[2])->comboSelIndex),
                static_cast<unsigned>(mdl::Mdl(models[0])->comboSelIndex2),
                static_cast<unsigned>(mdl::Mdl(models[1])->comboSelIndex2),
                static_cast<unsigned>(mdl::Mdl(models[2])->comboSelIndex2),
                static_cast<long long>(SendMessageW(modelCombo, CB_GETCURSEL, 0, 0)),
                static_cast<int>(app.SelectedModelSlot()));

    std::puts("STAGE second camera: actual button 536 before the second save");
    CameraTargetClickModelToggle(app, "the real 536 button is available after a draw-order change");
    Check(app.CameraMode() == 1, "the actual 536 button enters camera mode again");
    Check(SendMessageW(modelCombo, CB_GETCURSEL, 0, 0) == 0,
          "the second camera switch leaves the visible model selector on row zero");
    Check(app.MainModelComboSelection() == 1 && CameraTargetRememberedSlot(app) == bravoSlot,
          "the second camera switch still remembers Bravo as the To-model target");
    Check(app.SelectedModelSlot() == bravoSlot && app.SelectedModel() == models[1],
          "the second camera switch retains the Bravo object");
    Check(SaveSceneCopy(&app, secondPmm.c_str()),
          "production SaveSceneCopy saves the reordered camera-mode project");
    Check(std::filesystem::file_size(secondPmm) > 32,
          "the reordered camera-mode project contains a complete standard PMM body");
    Read(secondPmm);
    std::puts("STAGE second reopen: real LoadSceneFile keeps the new draw order");
    Load(app, secondPmm);
    CameraTargetFindModels(app, pmx, models);
    const std::array<unsigned char*, kCameraTargetCount> persistedRows{models[1], models[0], models[2]};
    Check(app.CameraMode() == 1, "the reordered project reopens in camera mode");
    Check(SendMessageW(modelCombo, CB_GETCURSEL, 0, 0) == 0,
          "the reordered project reopens with the visible model selector on row zero");
    Check(SendMessageW(modelCombo, CB_GETITEMDATA, 0, 0) == -1,
          "the reordered selector zero row is the explicit non-model row");
    Check(app.MainModelComboSelection() == 1 && CameraTargetRememberedSlot(app) == bravoSlot,
          "the reordered project remembers Bravo directly after reopening");
    Check(app.SelectedModelSlot() == bravoSlot && app.SelectedModel() == models[1],
          "the reordered project keeps the Bravo object selected");
    CameraTargetVerifySources(app, models, pmx, titles, movedDraw, initialCalculate,
        "the second PMM roundtrip persists the new draw order, the old calculation order and all sources");
    CameraTargetVerifySelectors(app, persistedRows, true,
        "the reordered selectors keep every row identity and caption after the second reopen");
    std::printf("STAGE second reopen order: project=%ls draw=(%u,%u,%u) calculate=(%u,%u,%u) "
                "remembered_row=%u remembered_slot=%d\n",
                secondPmm.c_str(),
                static_cast<unsigned>(mdl::Mdl(models[0])->comboSelIndex),
                static_cast<unsigned>(mdl::Mdl(models[1])->comboSelIndex),
                static_cast<unsigned>(mdl::Mdl(models[2])->comboSelIndex),
                static_cast<unsigned>(mdl::Mdl(models[0])->comboSelIndex2),
                static_cast<unsigned>(mdl::Mdl(models[1])->comboSelIndex2),
                static_cast<unsigned>(mdl::Mdl(models[2])->comboSelIndex2),
                app.MainModelComboSelection(), CameraTargetRememberedSlot(app));

    std::puts("STAGE second to model: actual button 536 returns to Bravo directly");
    CameraTargetClickModelToggle(app, "the real 536 button is available after the second reopen");
    Check(app.CameraMode() == 0, "the second reopen switches back to model editing through button 536");
    Check(app.SelectedModelSlot() == bravoSlot && app.SelectedModel() == models[1],
          "the second To model lands on the same Bravo object");
    Check(SendMessageW(modelCombo, CB_GETCURSEL, 0, 0) == 1,
          "the second To model lands on the moved Bravo row");
    Check(SendMessageW(modelCombo, CB_GETITEMDATA, 1, 0) == bravoSlot,
          "the moved row's item data is still the Bravo model slot");
    Check(CameraTargetComboText(modelCombo, 1) == CameraTargetLabel(app, models[1]),
          "the moved row's caption is still the Bravo object name");
    Check(app.MainModelComboSelection() == 1,
          "the remembered To-model row stays on Bravo's new row");
    CameraTargetVerifySelectors(app, persistedRows, true,
        "the final selectors keep every model row identity and caption after To model");
    CameraTargetVerifySources(app, models, pmx, titles, movedDraw, initialCalculate,
        "the final model sources, names and independent orders survived both PMM roundtrips");
    for (int i = 0; i < kCameraTargetCount; ++i)
        Check(EncodingFileBytes(pmx[i]) == pmxBytes[i],
              "all three original PMX files stay byte for byte unchanged");
    std::printf("PASS camera-model target roundtrip: saved=%ls reopened=%ls "
                "target_slot=%d draw=(2,1,3) calculate=(1,2,3) PMX=unchanged\n",
                firstPmm.c_str(), secondPmm.c_str(), bravoSlot);

    std::puts("STAGE cancel model deletion through the ordinary model delete button");
    CameraTargetDeleteSelected(app, IDCANCEL);
    Check(app.CameraMode() == 0 && app.SelectedModelSlot() == bravoSlot && app.SelectedModel() == models[1] &&
          SendMessageW(modelCombo, CB_GETCURSEL, 0, 0) == 1 && app.MainModelComboSelection() == 1,
          "cancelling deletion keeps Bravo selected in the same row");
    CameraTargetVerifySelectors(app, persistedRows, true, "cancelling deletion preserves all three selector identities");

    std::puts("STAGE confirm model deletion and save with camera mode still visible");
    const int alphaSlot = CameraTargetSlotOf(app, models[0]);
    const int charlieSlot = CameraTargetSlotOf(app, models[2]);
    CameraTargetDeleteSelected(app, IDOK);
    Check(app.CameraMode() != 0 && app.ModelSlot(bravoSlot) == nullptr,
          "confirming deletion removes Bravo from the scene and switches to the camera");
    const auto fallbackPmm = directory / L"camera-model-target-deleted.pmm";
    Check(SaveSceneCopy(&app, fallbackPmm.c_str()), "save the camera-mode project immediately after deletion");
    const auto fallbackBytes = EncodingFileBytes(fallbackPmm);
    CameraTargetVerifyFallback(app, alphaSlot, bravoSlot, charlieSlot, pmx[0], pmx[2]);

    std::puts("STAGE reopen the deleted-target project and click To model again");
    Load(app, fallbackPmm);
    CameraTargetVerifyFallback(app, alphaSlot, bravoSlot, charlieSlot, pmx[0], pmx[2]);
    Check(EncodingFileBytes(fallbackPmm) == fallbackBytes, "opening the project leaves the saved PMM bytes intact");
    for (int i = 0; i < kCameraTargetCount; ++i)
        Check(EncodingFileBytes(pmx[i]) == pmxBytes[i], "deleting a scene model never changes any source PMX");
    std::printf("PASS camera-model deletion fallback: cancelled=Bravo deleted_slot=%d fallback=Alpha slot=%d reopened=Alpha\n",
                bravoSlot, alphaSlot);
}



#include "model_source_save_host_checks.inc"

#include "pmm_identity_host_checks.inc"

int wmain(int argc, wchar_t** argv) {
    if(argc>=2 && wcscmp(argv[1],L"--metadata-only")==0) {
        if(argc!=2)return 2;
        LegacyBindingMetadata();AutomaticMetadata();return 0;
    }
    const bool signedCursor=argc>=2 && wcscmp(argv[1],L"--signed-cursor")==0;
    const bool saveBoundaries=argc>=2 && wcscmp(argv[1],L"--save-boundaries")==0;
    const bool pmxBoundary=argc>=2 && wcscmp(argv[1],L"--pmx-boundary")==0;
    const bool measuredNames=argc>=2 && wcscmp(argv[1],L"--measured-names")==0;
    const bool reversibleEncoding=argc>=2 && wcscmp(argv[1],L"--reversible-encoding")==0;
    const bool pmdIdentity=argc>=2 && wcscmp(argv[1],L"--pmd-identity")==0;
    const bool duplicatePaste=argc>=2 && wcscmp(argv[1],L"--duplicate-paste-roundtrip")==0;
    const bool mirrorPaste=argc>=2 && wcscmp(argv[1],L"--mirror-paste-roundtrip")==0;
    const bool transparentSortMenu=argc>=2 && wcscmp(argv[1],L"--transparent-sort-menu")==0;
    const bool cameraModelTarget=argc>=2 && wcscmp(argv[1],L"--camera-model-target")==0;
    if((signedCursor || saveBoundaries || pmxBoundary || measuredNames || reversibleEncoding || pmdIdentity || duplicatePaste || mirrorPaste || transparentSortMenu || cameraModelTarget) && argc!=3)return 2;
    if (argc < 2 || argc > 4) return 2;
    const bool capacityOnly = argc == 4 && wcscmp(argv[3], L"--key-capacity") == 0;
    const bool commentsOnly = argc == 4 && wcscmp(argv[3], L"--model-comments") == 0;
    const bool sourceAll = argc == 4 && wcscmp(argv[3], L"--model-source-all") == 0;
    const bool sourceExplicit = argc == 4 && wcscmp(argv[3], L"--model-source-explicit") == 0;
    const bool sourceOrdinary = argc == 4 && wcscmp(argv[3], L"--model-source-ordinary") == 0;
    if (argc == 4 && !capacityOnly && !commentsOnly && !sourceAll && !sourceExplicit && !sourceOrdinary) return 2;
#if MMD_ORIGINAL_PROFILE
    if(reversibleEncoding){std::puts("SKIP reversible-encoding host: original profile retains strict CP932 resource fields");return 0;}
    if(pmdIdentity){std::puts("SKIP pmd-identity host: full-profile PMD identity regression only");return 0;}
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetWindowsHookExW(WH_CBT,[](int code,WPARAM wp,LPARAM lp)->LRESULT {
        if(code==HCBT_CREATEWND && ((cameraModelTargetDialog && cameraModelTargetDialog->active) ||
                                  cameraTargetDeleteConfirmation)){
            auto* created=reinterpret_cast<CBT_CREATEWNDW*>(lp);
            if(created && created->lpcs && !(created->lpcs->style & WS_CHILD)){
                created->lpcs->x=-32000;created->lpcs->y=-32000;
            }
        }
        if(code==HCBT_ACTIVATE){
            auto window=reinterpret_cast<HWND>(wp);wchar_t cls[32]{};GetClassNameW(window,cls,32);
            if(!wcscmp(cls,L"#32770")){
                if(missingPasteWarning && missingPasteWarning->Handle(window))
                    return CallNextHookEx(nullptr,code,wp,lp);
                if(cameraModelTargetDialog && cameraModelTargetDialog->active &&
                   cameraModelTargetDialog->Handle(window)) return 1;
                if(cameraTargetDeleteConfirmation && cameraTargetDeleteConfirmation->Handle(window)) return 1;
                if (InvalidSaveDialogActivation(window)) return 1;
#if MMD_ORIGINAL_PROFILE
                if (OriginalNameWarningActivation(window)) return 1;
#endif
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
    // A fresh absolute fixture directory is the only input these evidence modes
    // accept; a non-absolute or already existing directory is rejected before
    // the directory is created and before the production app is initialized.
    if((pmdIdentity || duplicatePaste || mirrorPaste || transparentSortMenu || cameraModelTarget) && (!std::filesystem::path(argv[2]).is_absolute() || std::filesystem::exists(argv[2])))return 2;
    const auto directory = std::filesystem::absolute(signedCursor || saveBoundaries || pmxBoundary || measuredNames || reversibleEncoding || pmdIdentity || duplicatePaste || mirrorPaste || transparentSortMenu || cameraModelTarget ? argv[2] : argv[1]);
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
    if (transparentSortMenu) {
        TransparentSortMenuLanguage(*app);
        ShutdownCleanup(app); delete app; g_Block = nullptr;
        return 0;
    }
    if(duplicatePaste || mirrorPaste) {
        if (mirrorPaste) MirrorPasteRoundtrip(*app,directory);
        else DuplicatePasteRoundtrip(*app,directory);
        ShutdownCleanup(app);delete app;g_Block=nullptr;return 0;
    }
    if(cameraModelTarget) {
        CameraModelTargetRoundtrip(*app,directory);
        ShutdownCleanup(app);delete app;g_Block=nullptr;return 0;
    }
    if(signedCursor) {
        SignedCursorRoundtrip(*app,directory);
        ShutdownCleanup(app);delete app;g_Block=nullptr;return 0;
    }
#if !MMD_ORIGINAL_PROFILE
    if (pmdIdentity) {
        pmm_identity_host::Run(*app,directory);
        ShutdownCleanup(app); delete app; g_Block = nullptr;
        return 0;
    }
    if (sourceAll || sourceExplicit || sourceOrdinary) {
        LoadModelFile(app, argv[2], false);
        Check(app->ModelSlot(0) != nullptr, "source-save cohort loads its real standard PMD fixture");
        model_source_save_host::Run(*app, directory, sourceOrdinary ? -1 : sourceExplicit ? 1 : 0);
        ShutdownCleanup(app); delete app; g_Block = nullptr;
        return 0;
    }
#endif
    if(pmxBoundary || measuredNames || reversibleEncoding) {
        MeasuredNameBoundariesRoundtrip(*app,directory);
#if !MMD_ORIGINAL_PROFILE
        if(reversibleEncoding){
            RealUnicodeBindingRoundtrip(*app,directory);
            RealCaptionEncodingRoundtrip(*app,directory);
            RealResourcePathEncodingRoundtrip(*app,directory);
            RealResourcePathCollisionRoundtrip(*app,directory);
        }
#endif
        if(pmxBoundary) {
            PmmCaptionReadBoundaries(*app,directory);
            FormatTriggerBoundaries(*app);
            AutomaticCameraRoundtrip(*app,directory);
            PmxNameBoundaries(*app,directory);
        }
        ShutdownCleanup(app);delete app;g_Block=nullptr;
        return 0;
    }
    FormatTriggerBoundaries(*app);
    InvalidFormatEscalation(*app,directory);
    AutomaticCameraRoundtrip(*app,directory);
    SignedCursorRoundtrip(*app,directory);
    if(saveBoundaries) {
        ShutdownCleanup(app);delete app;g_Block=nullptr;
        std::puts("PASS targeted PMM save boundaries and roundtrips");return 0;
    }
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
    if (commentsOnly) {
        ModelCommentRoundtrip(*app, directory);
        ShutdownCleanup(app); delete app; g_Block = nullptr;
        return 0;
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

    TransparentSortMenuLanguage(*app);

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
