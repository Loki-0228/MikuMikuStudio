// MAINTENANCE: regression-keep; real menu 262, resources and edit subclasses.
// Default: private MMS_REPAIR_ desktop. Explicit --hidden-windows-only uses
// in-process visibility/message-box hooks without claiming desktop isolation.
// A zero-device wrapper satisfies the PMD loader; no D3D device is created.
// --expect-acp checks the actual process ACP selected by an external manifest;
// this test never mocks GetACP or changes a machine/user language setting.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>
#include <MinHook.h>
#include <btBulletDynamicsCommon.h>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/path_workspace.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include "mikumikustudio/d3d_wrapper.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/dynamics_world.hpp"
#include "mikumikustudio/history_physics.hpp"
#endif
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/owned_text.hpp"
#include "mikumikustudio/original_import.hpp"
#include "mikumikustudio/ui_language.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fcntl.h>
#include <io.h>
#include <memory>
#include <string>
#include <vector>

using namespace mikumikustudio;
namespace {
using Bytes = std::vector<unsigned char>;
int failures = 0, checks = 0, messages = 0, opened = 0;
const char* context = "startup";
bool quiet = false;
decltype(&ShowWindow) realShowWindow = nullptr;

bool Check(bool ok, const char* label) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL context=%s acp=%u %s\n", context, GetACP(), label); }
    return ok;
}
std::string Utf8(const std::wstring& text) {
    std::string encoded;
    text_encoding::EncodeExact(text.c_str(), CP_UTF8, encoded);
    return encoded;
}
BOOL WINAPI HideTopLevel(HWND window, int show) {
    if (!(GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD)) show = SW_HIDE;
    return realShowWindow(window, show);
}
LRESULT CALLBACK HideCreated(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        auto* create = reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;
        if (!(create->style & WS_CHILD)) {
            create->style &= ~WS_VISIBLE;
            create->x = create->y = -32000;
        }
    }
    if (code == HCBT_ACTIVATE) return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}
int WINAPI ObserveMessageW(HWND, LPCWSTR text, LPCWSTR title, UINT) {
    ++messages;
    if (!quiet) std::printf("MESSAGE context=%s title=%s body=%s\n", context,
        Utf8(title ? title : L"").c_str(), Utf8(text ? text : L"").c_str());
    return IDOK;
}
int WINAPI ObserveMessageA(HWND, LPCSTR text, LPCSTR title, UINT) {
    ++messages;
    if (!quiet) std::printf("MESSAGE_A context=%s title=%s body=%s\n", context,
        title ? title : "", text ? text : "");
    return IDOK;
}

std::wstring WindowText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::vector<wchar_t> text(static_cast<size_t>(length) + 1);
    GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
    return text.data();
}
std::wstring ItemText(HWND dialog, int id, int item) {
    const HWND combo = GetDlgItem(dialog, id);
    const LRESULT length = SendMessageW(combo, CB_GETLBTEXTLEN, item, 0);
    if (!Check(length >= 0 && length < 10000, "read actual combo item length")) return {};
    std::vector<wchar_t> text(static_cast<size_t>(length) + 1);
    if (!Check(SendMessageW(combo, CB_GETLBTEXT, item,
        reinterpret_cast<LPARAM>(text.data())) != CB_ERR, "read actual combo item")) return {};
    return text.data();
}
void ExpectText(const std::wstring& actual, const std::wstring& expected, const char* label) {
    if (!Check(actual == expected, label))
        std::printf("TEXT expected=%s actual=%s\n", Utf8(expected).c_str(), Utf8(actual).c_str());
}
void Command(HWND dialog, int id) { SendMessageW(dialog, WM_COMMAND, id, 0); }
void Select(HWND dialog, int id, int index) {
    const HWND control = GetDlgItem(dialog, id);
    Check(SendMessageW(control, CB_SETCURSEL, index, 0) == index, "select actual combo row");
    SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), reinterpret_cast<LPARAM>(control));
}
void Enter(HWND dialog, int id) { SendMessageW(GetDlgItem(dialog, id), WM_KEYDOWN, VK_RETURN, 0); }
void Type(HWND dialog, int id, const std::wstring& value) {
    Check(SetWindowTextW(GetDlgItem(dialog, id), value.c_str()) != FALSE, "type into production name edit");
}

template<class T> void Put(Bytes& bytes, T value) {
    const auto* data = reinterpret_cast<const unsigned char*>(&value);
    bytes.insert(bytes.end(), data, data + sizeof value);
}
void Text(Bytes& bytes, const std::wstring& value) {
    Put(bytes, static_cast<unsigned>(value.size() * sizeof(wchar_t)));
    const auto* data = reinterpret_cast<const unsigned char*>(value.data());
    bytes.insert(bytes.end(), data, data + value.size() * sizeof(wchar_t));
}
struct Names {
    std::array<std::wstring, 2> bodies, joints, bones;
};
Names UnicodeNames() {
    return {{{L"剛体_中文_😀", std::wstring(300, L'長') + L"_髪_中文_😀"}},
            {{L"関節_中文_🦴", std::wstring(310, L'接') + L"_続_中文_😀"}},
            {{L"骨_中文_😀", L"関連骨_髪_中文_😀"}}};
}
Names LegacyNames() {
    // The minimal profile's PMD editor reserves a terminating byte. The full
    // profile also exercises a valid character ending at the 20-byte boundary.
    constexpr unsigned prefix = MMD_ORIGINAL_PROFILE ? 17 : 18;
    return {{{std::wstring(prefix, L'A') + L"塔", L"右腕"}},
            {{std::wstring(prefix, L'J') + L"塔", L"左腕接点"}},
            {{L"センター", L"右腕"}}};
}
Names AsciiNames() { return {{{L"body-A", L"body-B"}}, {{L"joint-A", L"joint-B"}}, {{L"bone-A", L"bone-B"}}}; }
std::wstring WithTail(const std::wstring& name) {
    auto value = name; value.push_back(L'\0'); value += L"尾部"; return value;
}
Bytes Pmx(const Names& names) {
    Bytes bytes{'P','M','X',' '}; Put(bytes, 2.f);
    // UTF-16, no extra UV, one-byte vertex/texture/material/bone/morph/rigid indices.
    for (unsigned char value : {8,0,0,1,1,1,1,1,1}) Put(bytes, value);
    Text(bytes, L"physics editor fixture"); Text(bytes, L"physics editor fixture");
    Text(bytes, L""); Text(bytes, L"");
    for (int i = 0; i < 4; ++i) Put(bytes, 0u); // no GPU mesh resources
    Put(bytes, 2u);
    for (int i = 0; i < 2; ++i) {
        Text(bytes, names.bones[i]); Text(bytes, i ? L"bone-English-B" : L"bone-English-A");
        for (int axis = 0; axis < 3; ++axis) Put(bytes, 0.f);
        Put(bytes, static_cast<signed char>(-1)); Put(bytes, 0u); Put(bytes, static_cast<unsigned short>(0));
        for (int axis = 0; axis < 3; ++axis) Put(bytes, 0.f);
    }
    Put(bytes, 0u); Put(bytes, 0u); // morphs/display groups
    Put(bytes, 2u);
    for (int i = 0; i < 2; ++i) {
        Text(bytes, WithTail(names.bodies[i])); Text(bytes, WithTail(i ? L"body-English-B" : L"body-English-A"));
        Put(bytes, static_cast<signed char>(i)); Put(bytes, static_cast<unsigned char>(0));
        Put(bytes, static_cast<unsigned short>(0)); Put(bytes, static_cast<unsigned char>(0));
        for (int axis = 0; axis < 3; ++axis) Put(bytes, 1.f);
        for (int axis = 0; axis < 3; ++axis) Put(bytes, axis == 0 ? float(i) : 0.f);
        for (int axis = 0; axis < 3; ++axis) Put(bytes, 0.f);
        for (float value : {float(i + 1), .5f, .5f, 0.f, .5f}) Put(bytes, value);
        Put(bytes, static_cast<unsigned char>(1));
    }
    Put(bytes, 2u);
    for (int i = 0; i < 2; ++i) {
        Text(bytes, WithTail(names.joints[i])); Text(bytes, WithTail(i ? L"joint-English-B" : L"joint-English-A"));
        Put(bytes, static_cast<unsigned char>(0));
        Put(bytes, static_cast<signed char>(i)); Put(bytes, static_cast<signed char>(1 - i));
        for (int field = 0; field < 24; ++field) Put(bytes, 0.f);
    }
    return bytes;
}
void Fixed(Bytes& bytes, const std::wstring& text, std::size_t capacity) {
    std::string encoded;
    Check(text_encoding::EncodeExact(text.c_str(),932,encoded)&&encoded.size()<=capacity,
        "real PMD fixture field fits complete CP932 bytes");
    const auto start=bytes.size();bytes.resize(start+capacity,0);
    if(encoded.size()<=capacity)std::memcpy(bytes.data()+start,encoded.data(),encoded.size());
}
Bytes Pmd(const Names& names) {
    Bytes bytes{'P','m','d'};Put(bytes,1.f);
    Fixed(bytes,L"physics-test",20);Fixed(bytes,L"",256);
    Put(bytes,0u);Put(bytes,0u);Put(bytes,0u); // no GPU vertices, indices or materials
    Put(bytes,static_cast<unsigned short>(2));
    for(int i=0;i<2;++i) {
        Fixed(bytes,names.bones[i],20);
        Put(bytes,static_cast<short>(-1));Put(bytes,static_cast<short>(-1));
        Put(bytes,static_cast<unsigned char>(1));Put(bytes,static_cast<short>(0));
        for(int axis=0;axis<3;++axis)Put(bytes,0.f);
    }
    Put(bytes,static_cast<unsigned short>(0)); // IK
    Put(bytes,static_cast<unsigned short>(0)); // morphs
    Put(bytes,static_cast<unsigned char>(0)); // facial display entries
    Put(bytes,static_cast<unsigned char>(0)); // custom display groups
    Put(bytes,0u); // bone display entries
    Put(bytes,static_cast<unsigned char>(1)); // English section
    Fixed(bytes,L"physics-test",20);Fixed(bytes,L"",256);
    Fixed(bytes,L"bone-English-A",20);Fixed(bytes,L"bone-English-B",20);
    for(unsigned i=0;i<10;++i) {
        wchar_t toon[24]{};swprintf_s(toon,L"toon%02u.bmp",i+1);Fixed(bytes,toon,100);
    }
    Put(bytes,2u);
    for(int i=0;i<2;++i) {
        Fixed(bytes,names.bodies[i],20);Put(bytes,static_cast<short>(i));
        Put(bytes,static_cast<unsigned char>(0));Put(bytes,static_cast<unsigned short>(0));
        Put(bytes,static_cast<unsigned char>(0));
        for(int axis=0;axis<3;++axis)Put(bytes,1.f);
        for(int axis=0;axis<3;++axis)Put(bytes,axis==0?float(i):0.f);
        for(int axis=0;axis<3;++axis)Put(bytes,0.f);
        for(float value:{float(i+1),.5f,.5f,0.f,.5f})Put(bytes,value);
        Put(bytes,static_cast<unsigned char>(1));
    }
    Put(bytes,2u);
    for(int i=0;i<2;++i) {
        Fixed(bytes,names.joints[i],20);Put(bytes,i);Put(bytes,1-i);
        for(int field=0;field<24;++field)Put(bytes,0.f);
    }
    return bytes;
}
struct World {
    btDefaultCollisionConfiguration config;
    btCollisionDispatcher dispatcher{&config};
#if MMD_ORIGINAL_PROFILE
    btDbvtBroadphase broadphase;
#else
    history_physics::Broadphase broadphase{btVector3(-100,-100,-100),btVector3(100,100,100),256};
#endif
    btSequentialImpulseConstraintSolver solver;
#if MMD_ORIGINAL_PROFILE
    btDiscreteDynamicsWorld world{&dispatcher, &broadphase, &solver, &config};
#else
    DynamicsWorld world{&dispatcher, &broadphase, &solver, &config};
#endif
    PhysicsScene scene{};
    World() { scene.world = &world; world.setGravity(btVector3(0,0,0));
#if !MMD_ORIGINAL_PROFILE
        scene.broadphase=&broadphase;scene.dispatcher=&dispatcher;scene.solver=&solver;scene.collisionConfig=&config;
#endif
    }
};
template<class Record> void SetLegacyName(Record& record, const std::wstring& name) {
    owned_text::Free(record.jpText); record.jpText = nullptr;
    owned_text::Free(record.enText); record.enText = nullptr;
    std::string bytes;
    if (!Check(text_encoding::EncodeExact(name.c_str(), 932, bytes) && bytes.size() <= sizeof record.name,
               "fixture legacy name fits exact CP932 field")) return;
    std::memset(record.name, 0, sizeof record.name);
    std::memcpy(record.name, bytes.data(), bytes.size());
}
struct NameState {
    enum class Storage { FormalRetained, PrivateScratch, FormalVersion };
    std::array<char, 20> raw{};
    wchar_t* jp = nullptr;
    wchar_t* en = nullptr;
    std::wstring jpValue, enValue;
    std::vector<unsigned char> jpAllocation, enAllocation;
    template<class Record> explicit NameState(const Record& record) : jp(record.jpText), en(record.enText),
        jpValue(jp ? jp : L""), enValue(en ? en : L"") {
        std::memcpy(raw.data(), record.name, raw.size());
#if !MMD_ORIGINAL_PROFILE
        for(auto pair : {std::pair{jp,&jpAllocation},std::pair{en,&enAllocation}})if(pair.first) {
            owned_text::Allocation info;
            if(Check(owned_text::Find(pair.first,info),"formal source name has registered complete allocation")) {
                const auto* first=reinterpret_cast<const unsigned char*>(pair.first);pair.second->assign(first,first+info.bytes);
            }
        }
#endif
    }
    template<class Record> void ExpectUnchanged(const Record& record, const char* label,
                                               Storage storage=Storage::FormalRetained) const {
        Check(!std::memcmp(raw.data(), record.name, raw.size()), label);
#if MMD_ORIGINAL_PROFILE
        // This profile has no owned_text capacity registry. Its legacy editor
        // borrows/transfers the same pointers; only the terminated text is
        // observable here. Do not infer allocation capacity from its length.
        Check(storage==Storage::FormalRetained,"original editor retains legacy name storage rather than claiming registered version ownership");
        Check(record.jpText==jp&&record.enText==en,"original editor preserves the exact borrowed or transferred wide pointers");
#else
        const wchar_t* actual[]{record.jpText,record.enText};
        const wchar_t* source[]{jp,en};
        const std::vector<unsigned char>* allocation[]{&jpAllocation,&enAllocation};
        for(unsigned language=0;language<2;++language) {
            owned_text::Allocation info;
            if(allocation[language]->empty()) {
                Check(!source[language]&&!actual[language],"absent wide name remains absent in this storage class");
                continue;
            }
            if(storage==Storage::FormalRetained)
                Check(actual[language]==source[language],"unchanged formal name retains its exact original owned pointer");
            else
                Check(actual[language]&&actual[language]!=source[language],"private scratch and new versions never alias the source wide allocation");
            if(storage==Storage::PrivateScratch) {
                Check(actual[language]&&!owned_text::Find(actual[language],info),"private scratch name uses vector storage rather than model-owned text");
                Check(actual[language]&&!std::memcmp(actual[language],allocation[language]->data(),allocation[language]->size()),
                    "private scratch copies every source allocation byte including text after NUL");
            } else {
                Check(actual[language]&&owned_text::Find(actual[language],info)&&info.bytes==allocation[language]->size()&&
                    !std::memcmp(actual[language],allocation[language]->data(),info.bytes),
                    "formal name retains registered exact allocation capacity and all tail bytes");
            }
        }
#endif
        ExpectText(record.jpText ? record.jpText : L"", jpValue, "original full Japanese text survives");
        ExpectText(record.enText ? record.enText : L"", enValue, "original full English text survives");
    }
};
struct Fixture {
    World world;
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    // ModelLoadPMD requires the native wrapper even for a zero-mesh file, but
    // this value-initialized wrapper owns no device or other GPU resource.
    std::unique_ptr<D3DRenderer> loaderRenderer = std::make_unique<D3DRenderer>();
    Names names;
    bool ready = false;
    explicit Fixture(bool legacy, bool ascii = false) : names(ascii ? AsciiNames() : legacy ? LegacyNames() : UnicodeNames()) {
        app->state = MMDAppState{};
        app->HInstance() = GetModuleHandleW(nullptr);
        app->Hwnd() = CreateWindowExW(0, L"STATIC", L"physics editor test owner", WS_OVERLAPPED,
            -32000, -32000, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        app->Physics() = &world.scene;
        app->ModelSlot(0) = reinterpret_cast<unsigned char*>(model.get());
        app->SelectedModelSlot() = 0;
        app->state.optflag[0] = 0;
        g_Block = app.get();
        model->scenePtr = &world.scene;
        const Bytes bytes = legacy ? Pmd(names) : Pmx(names);
        wchar_t temp[MAX_PATH]{}, path[MAX_PATH]{};
        if (!Check(GetTempPathW(MAX_PATH, temp) && GetTempFileNameW(temp, L"pec", 0, path), "create fixture file")) return;
        int fd = _wopen(path, _O_BINARY | _O_WRONLY | _O_TRUNC);
        if (!Check(fd >= 0, "open fixture for writing")) { DeleteFileW(path); return; }
        const int written = _write(fd, bytes.data(), static_cast<unsigned>(bytes.size())); _close(fd);
        if (!Check(written == static_cast<int>(bytes.size()), "write full fixture")) { DeleteFileW(path); return; }
        PathResolutionWorkspace paths{};
        if(legacy) {
            wchar_t previousDirectory[32768]{};
            const auto length=GetCurrentDirectoryW(static_cast<DWORD>(std::size(previousDirectory)),previousDirectory);
            if(!Check(length&&length<std::size(previousDirectory),"capture working directory before actual PMD loader")) { DeleteFileW(path);return; }
            Check(!loaderRenderer->device&&!loaderRenderer->d3d9,"PMD fixture uses an empty loader wrapper without creating a GPU device");
            ready=ModelLoadPMD(reinterpret_cast<unsigned char*>(model.get()),nullptr,path,loaderRenderer.get(),0,0,0,&world.scene,paths);
            Check(SetCurrentDirectoryW(previousDirectory)!=FALSE,"restore working directory after actual PMD loader");
            Check(ready,"real PMD loader completes without a GPU device");
        } else {
            fd = _wopen(path, _O_BINARY | _O_RDONLY);
            if (!Check(fd >= 0, "open fixture for production loader")) { DeleteFileW(path); return; }
            _lseek(fd, 3, SEEK_SET);
            ready = LoadPMX(reinterpret_cast<unsigned char*>(model.get()), nullptr, 0, 0, paths, fd) != 0;
            Check(ready, "real PMX loader completes without a renderer");
        }
        Check(DeleteFileW(path) != FALSE, "loader closes fixture file");
        if (!ready) return;
        ready = Check(model->boneCount == 2 && model->rigidCount == 2 && model->jointCount == 2 &&
            world.world.getNumCollisionObjects() == 2 && world.world.getNumConstraints() == 2,
            "fixture has complete real bone/body/joint/keyframe state");
        if (legacy) Check(model->physicsMode!=2&&!model->pmxVertices&&original_import::CanEdit(*model),
            "actual PMD fixture is structurally saveable and admitted by the production editor gate");
    }
    ~Fixture() {
        if (app->state.frameCopyDialog) SendMessageW(app->state.frameCopyDialog, WM_COMMAND, IDCANCEL, 0);
        app->ModelSlot(0) = nullptr;
        ModelDispose(reinterpret_cast<unsigned char*>(model.get()));
        DestroyWindow(static_cast<HWND>(app->Hwnd()));
        app->Physics() = nullptr;
        g_Block = nullptr;
    }
    HWND Open(unsigned char language) {
        app->EnglishUI() = language;
        CommandDispatch(nullptr, MAKEWPARAM(262, 0));
        HWND dialog = app->state.frameCopyDialog;
        if (!Check(dialog && IsWindow(dialog), "menu 262 creates actual physics resource dialog")) return nullptr;
        ++opened;
        Check(!IsWindowVisible(dialog), "real modeless dialog stays hidden");
        Check(app->Renderer()==nullptr&&!loaderRenderer->device&&!loaderRenderer->d3d9,
            "physics editor test attaches no renderer and creates no GPU device");
        if (!quiet) std::printf("DIALOG language=%u unicode=%d body_edit_unicode=%d\n", language,
            IsWindowUnicode(dialog), IsWindowUnicode(GetDlgItem(dialog, 705)));
        return dialog;
    }
    void Close(bool commit) {
        HWND dialog = app->state.frameCopyDialog;
        Command(dialog, commit ? IDOK : IDCANCEL);
        Check(!IsWindow(dialog) && !app->state.frameCopyDialog, "real OK/Cancel closes dialog");
        Check(!app->state.rigidScratchArray && !app->state.jointScratchArray, "close releases both production scratch arrays");
    }
    mdl::RigidRecord* Bodies() { return static_cast<mdl::RigidRecord*>(app->state.rigidScratchArray); }
    mdl::JointRecord* Joints() { return static_cast<mdl::JointRecord*>(app->state.jointScratchArray); }
};

#if MMD_ORIGINAL_PROFILE
void ExpectEditorRefused(Fixture& fixture,const char* label) {
    Check(!original_import::CanEdit(*fixture.model),label);
    auto* const bodies=fixture.model->rigidTable;auto* const joints=fixture.model->jointTable;
    std::array<unsigned char,sizeof(mdl::RigidRecord)*2> bodyBytes{};
    std::array<unsigned char,sizeof(mdl::JointRecord)*2> jointBytes{};
    std::array<unsigned char,sizeof(mdl::BoneRecord)*2> boneBytes{};
    std::memcpy(bodyBytes.data(),bodies,bodyBytes.size());std::memcpy(jointBytes.data(),joints,jointBytes.size());
    std::memcpy(boneBytes.data(),fixture.model->boneTable,boneBytes.size());
    const std::array<NameState,2> bodyNames{{NameState(bodies[0]),NameState(bodies[1])}};
    const std::array<NameState,2> jointNames{{NameState(joints[0]),NameState(joints[1])}};
    const std::array<btCollisionObject*,2> objects{{fixture.world.world.getCollisionObjectArray()[0],fixture.world.world.getCollisionObjectArray()[1]}};
    const std::array<btTypedConstraint*,2> constraints{{fixture.world.world.getConstraint(0),fixture.world.world.getConstraint(1)}};
    const auto texts=owned_text::Count();const auto time=fixture.app->state.timeNowHigh;
    const std::uint8_t dirty=fixture.app->SceneModified(),enhanced=fixture.app->EnhancedModelDirty(),reset=fixture.app->PhysicsResetPending();
    for(unsigned char language:{kUiJapanese,kUiEnglish,kUiChinese}) {
        const auto warnings=messages;fixture.app->EnglishUI()=language;
        CommandDispatch(nullptr,MAKEWPARAM(262,0));
        Check(messages==warnings+1,"unsupported original-profile editor request emits one explicit compatibility refusal");
        Check(!fixture.app->state.frameCopyDialog&&!fixture.app->state.rigidScratchArray&&!fixture.app->state.jointScratchArray&&
            fixture.model->rigidTable==bodies&&fixture.model->jointTable==joints&&
            !std::memcmp(bodyBytes.data(),bodies,sizeof(bodyBytes))&&!std::memcmp(jointBytes.data(),joints,sizeof(jointBytes))&&
            !std::memcmp(boneBytes.data(),fixture.model->boneTable,sizeof(boneBytes))&&owned_text::Count()==texts&&
            fixture.app->state.timeNowHigh==time&&fixture.app->SceneModified()==dirty&&
            fixture.app->EnhancedModelDirty()==enhanced&&fixture.app->PhysicsResetPending()==reset,
            "compatibility refusal creates no editor or scratch and leaves formal records, pose, ownership and flags intact");
        Check(fixture.world.world.getNumCollisionObjects()==2&&fixture.world.world.getNumConstraints()==2&&
            fixture.world.world.getCollisionObjectArray()[0]==objects[0]&&fixture.world.world.getCollisionObjectArray()[1]==objects[1]&&
            fixture.world.world.getConstraint(0)==constraints[0]&&fixture.world.world.getConstraint(1)==constraints[1],
            "refused editor request leaves exact live Bullet members and order intact");
        for(unsigned i=0;i<2;++i) {
            bodyNames[i].ExpectUnchanged(bodies[i],"refused edit retains every body-name byte and original allocation");
            jointNames[i].ExpectUnchanged(joints[i],"refused edit retains every joint-name byte and original allocation");
        }
    }
}
void OriginalRefusals() {
    context="original-reject-PMX";
    { Fixture fixture(false);if(fixture.ready)ExpectEditorRefused(fixture,"PMX source is outside the original-profile PMD-only editor contract"); }
    context="original-reject-PMD-full-width-name";
    {
        Fixture fixture(true);if(!fixture.ready)return;
        SetLegacyName(fixture.model->rigidTable[0],std::wstring(18,L'A')+L"塔");
        Check(strnlen_s(fixture.model->rigidTable[0].name,20)==20,
            "rejection boundary is a complete valid 20-byte CP932 raw name, not broken DBCS");
        ExpectEditorRefused(fixture,"minimal PMD editor refuses a full-width raw name that leaves no terminator for its standard writer");
    }
}
#endif

void Lists(Fixture& fixture, HWND dialog, const Names& names, unsigned char language) {
    const wchar_t* caption = language == kUiJapanese ? L"物理演算モデル" :
        language == kUiChinese ? L"物理模型编辑" : L"physics model";
    ExpectText(WindowText(dialog), caption, "dialog title uses intact current UI language");
    for (int id : {704,705,707,736,740,741,742,743})
        Check(IsWindowUnicode(GetDlgItem(dialog, id)) != FALSE, "actual resource name control is Unicode");
    for (int i = 0; i < 2; ++i) {
        for (int id : {704,741,742}) ExpectText(ItemText(dialog, id, i), names.bodies[i], "rigid list/A/B preserves complete original name");
        ExpectText(ItemText(dialog, 736, i), names.joints[i], "joint list preserves complete original name");
        const std::wstring bone = language == kUiEnglish ?
            (i ? L"bone-English-B" : L"bone-English-A") : names.bones[i];
        ExpectText(ItemText(dialog, 707, i + 1), bone, "related-bone list preserves correct model language");
        ExpectText(ItemText(dialog, 743, i), bone, "pivot-bone list preserves correct model language");
    }
    Check(SendDlgItemMessageW(dialog,741,CB_GETCURSEL,0,0) == fixture.model->jointTable[0].rigidA &&
        SendDlgItemMessageW(dialog,742,CB_GETCURSEL,0,0) == fixture.model->jointTable[0].rigidB,
        "joint's A/B selection identifies current model bodies");
    const wchar_t* radius=language==kUiEnglish?L"radius":L"半径";
    const wchar_t* width=language==kUiJapanese?L"　幅":language==kUiChinese?L"宽度":L"width";
    const wchar_t* height=language==kUiJapanese?L"高さ":language==kUiChinese?L"高度":L"height";
    const wchar_t* depth=language==kUiJapanese?L"奥行":language==kUiChinese?L"深度":L"depth";
    Command(dialog,727); // real box radio command refreshes dynamic captions
    ExpectText(WindowText(GetDlgItem(dialog,732)),width,"box width label uses current UI language");
    ExpectText(WindowText(GetDlgItem(dialog,733)),height,"box height label uses current UI language");
    ExpectText(WindowText(GetDlgItem(dialog,734)),depth,"box depth label uses current UI language");
    Command(dialog,728);
    ExpectText(WindowText(GetDlgItem(dialog,732)),radius,"capsule radius label uses current UI language");
    ExpectText(WindowText(GetDlgItem(dialog,733)),height,"capsule height label uses current UI language");
    Check(!(GetWindowLongPtrW(GetDlgItem(dialog,734),GWL_STYLE)&WS_VISIBLE),"capsule hides depth label");
    Command(dialog,726); // restore the fixture's original sphere shape
    ExpectText(WindowText(GetDlgItem(dialog,732)),radius,"sphere radius label uses current UI language");
    Check(!(GetWindowLongPtrW(GetDlgItem(dialog,733),GWL_STYLE)&WS_VISIBLE)&&
          !(GetWindowLongPtrW(GetDlgItem(dialog,734),GWL_STYLE)&WS_VISIBLE),"sphere hides height and depth labels");
}
void Unchanged(Fixture& fixture) {
#if !MMD_ORIGINAL_PROFILE
    {
        auto* const rigids=fixture.model->rigidTable;auto* const joints=fixture.model->jointTable;
        auto* const body=rigids[0].body;
        const NameState bodyName(rigids[0]),jointName(joints[0]);
        const auto dialog=fixture.Open(kUiEnglish);
        if(dialog) {
            fixture.Close(true);
            Check(fixture.model->rigidTable==rigids&&fixture.model->jointTable==joints&&rigids[0].body==body,
                "unchanged private OK retains the exact formal tables and native body rather than publishing a new version");
            bodyName.ExpectUnchanged(fixture.model->rigidTable[0],"no-op OK retains original rigid name storage");
            jointName.ExpectUnchanged(fixture.model->jointTable[0],"no-op OK retains original joint name storage");
        }
    }
#endif
    for (unsigned char language : {kUiJapanese, kUiEnglish, kUiChinese}) {
        const std::array<NameState,2> body{{NameState(fixture.model->rigidTable[0]),NameState(fixture.model->rigidTable[1])}};
        const std::array<NameState,2> joint{{NameState(fixture.model->jointTable[0]),NameState(fixture.model->jointTable[1])}};
        HWND dialog = fixture.Open(language); if (!dialog) continue;
        Lists(fixture, dialog, fixture.names, language);
        for (int i = 0; i < 2; ++i) {
            Select(dialog,704,i); Select(dialog,736,i);
            ExpectText(WindowText(GetDlgItem(dialog,705)), fixture.names.bodies[i], "body edit preserves full original name");
            ExpectText(WindowText(GetDlgItem(dialog,740)), fixture.names.joints[i], "joint edit preserves full original name");
            Enter(dialog,705); Enter(dialog,740);
            Command(dialog,801); Command(dialog,800); // both production Collect paths
            constexpr auto scratchStorage=MMD_ORIGINAL_PROFILE?NameState::Storage::FormalRetained:NameState::Storage::PrivateScratch;
            body[i].ExpectUnchanged(fixture.Bodies()[i], "unchanged Enter/Collect preserves every body-name byte",scratchStorage);
            joint[i].ExpectUnchanged(fixture.Joints()[i], "unchanged Enter/Collect preserves every joint-name byte",scratchStorage);
        }
        // Production copy/paste buttons copy physics parameters, not names.
        // Change the source parameter explicitly so every language iteration
        // requires a new formal version in the full profile.
        Type(dialog,718,std::to_wstring(3u+language));Enter(dialog,718);
        Check(fixture.Bodies()[1].mass==float(3u+language),"copy source has a real edited parameter before paste");
        Command(dialog,724); Select(dialog,704,0); Command(dialog,725);
        ExpectText(WindowText(GetDlgItem(dialog,705)), fixture.names.bodies[0], "body parameter paste keeps destination name");
        Check(fixture.Bodies()[0].mass == fixture.Bodies()[1].mass, "body parameter paste actually ran");
        Command(dialog,738); Select(dialog,736,0); Command(dialog,739);
        ExpectText(WindowText(GetDlgItem(dialog,740)), fixture.names.joints[0], "joint parameter paste keeps destination name");
        fixture.Close(true);
        for (int i=0;i<2;++i) {
            constexpr auto publishedStorage=MMD_ORIGINAL_PROFILE?NameState::Storage::FormalRetained:NameState::Storage::FormalVersion;
            body[i].ExpectUnchanged(fixture.model->rigidTable[i], "OK preserves unchanged body raw name",publishedStorage);
            joint[i].ExpectUnchanged(fixture.model->jointTable[i], "OK preserves unchanged joint raw name",publishedStorage);
        }
        Check(fixture.model->rigidTable[0].mass==float(3u+language)&&fixture.model->rigidTable[1].mass==float(3u+language),
            "OK really publishes the changed copy/paste parameters while retaining the unchanged names");
        Check(fixture.world.world.getNumCollisionObjects()==2 && fixture.world.world.getNumConstraints()==2,
            "real OK rebuild keeps complete physics state");
    }
}
std::string RawName(const char (&name)[20]) { return std::string(name, strnlen_s(name,20)); }
void Renames(Fixture& fixture) {
    const NameState oldBody(fixture.model->rigidTable[0]), oldJoint(fixture.model->jointTable[0]);
    HWND dialog = fixture.Open(kUiChinese); if (!dialog) return;
    Type(dialog,705,L"取消_塔"); Enter(dialog,705);
    Type(dialog,740,L"取消関節"); Enter(dialog,740);
    oldBody.ExpectUnchanged(fixture.model->rigidTable[0], "scratch body rename does not mutate model before OK");
    oldJoint.ExpectUnchanged(fixture.model->jointTable[0], "scratch joint rename does not mutate model before OK");
    fixture.Close(false);
    oldBody.ExpectUnchanged(fixture.model->rigidTable[0], "Cancel preserves body name and wide ownership");
    oldJoint.ExpectUnchanged(fixture.model->jointTable[0], "Cancel preserves joint name and wide ownership");

    dialog = fixture.Open(kUiJapanese); if (!dialog) return;
    const std::wstring full = std::wstring(MMD_ORIGINAL_PROFILE?17:18,L'R') + L"塔";
    std::string fullBytes; text_encoding::EncodeExact(full.c_str(),932,fullBytes);
    Check(fullBytes.size()==(MMD_ORIGINAL_PROFILE?19u:20u) && static_cast<unsigned char>(fullBytes.back())==0x83,
        "accepted boundary fixture ends with a complete DBCS character whose trail byte is in the lead-byte range");
    Type(dialog,705,full); Enter(dialog,705);
    Check(RawName(fixture.Bodies()[0].name)==fullBytes, "Enter stores the complete accepted CP932 name without splitting its DBCS pair");
#if MMD_ORIGINAL_PROFILE
    Check(!fixture.Bodies()[0].jpText && !fixture.Bodies()[0].enText, "rename clears stale wide aliases only in scratch");
#else
    ExpectText(fixture.Bodies()[0].jpText,full,"private rename retains the full Unicode name alongside its CP932 mirror");
    Check(!fixture.Bodies()[0].enText,"a real rename clears the old English alias in private scratch");
#endif
    for (int id:{704,741,742}) ExpectText(ItemText(dialog,id,0),full,"Enter updates rigid list and joint A/B names");
    const int beforeBodyWarning=messages;
    Type(dialog,705,L"无法保存_😀"); Enter(dialog,705);
#if MMD_ORIGINAL_PROFILE
    Check(messages>beforeBodyWarning,"unrepresentable body rename reports rejection");
    Check(RawName(fixture.Bodies()[0].name)==fullBytes,"rejected body rename retains old bytes");
    ExpectText(WindowText(GetDlgItem(dialog,705)),full,"rejected body input restores previous displayed name");
#else
    Check(messages==beforeBodyWarning&&RawName(fixture.Bodies()[0].name)==fullBytes,
          "Unicode body rename keeps the valid legacy mirror without claiming a lossy replacement");
    ExpectText(fixture.Bodies()[0].jpText,L"无法保存_😀","private Unicode body rename retains non-BMP characters");
    ExpectText(WindowText(GetDlgItem(dialog,705)),L"无法保存_😀","body input displays its actual Unicode draft");
    Type(dialog,705,full); Enter(dialog,705);
#endif

    Type(dialog,740,L"関節_塔"); Enter(dialog,740);
    std::string jointBytes; text_encoding::EncodeExact(L"関節_塔",932,jointBytes);
    const int beforeJointWarning=messages;
    Type(dialog,740,L"拒否_🦴"); Command(dialog,800); // CollectJointEdits, not only Enter
#if MMD_ORIGINAL_PROFILE
    Check(messages>beforeJointWarning,"unrepresentable joint Collect reports rejection");
    Check(RawName(fixture.Joints()[0].name)==jointBytes,"rejected joint Collect retains old bytes");
    ExpectText(WindowText(GetDlgItem(dialog,740)),L"関節_塔","rejected joint input restores previous displayed name");
#else
    Check(messages==beforeJointWarning&&RawName(fixture.Joints()[0].name)==jointBytes,
          "Unicode joint collection keeps its original representable mirror");
    ExpectText(fixture.Joints()[0].jpText,L"拒否_🦴","joint Collect retains its complete Unicode draft");
    ExpectText(WindowText(GetDlgItem(dialog,740)),L"拒否_🦴","joint input displays the Unicode draft after page collection");
    Type(dialog,740,L"関節_塔"); Enter(dialog,740);
#endif
    Select(dialog,704,1);
    Type(dialog,705,std::wstring(19,L'T')+L"塔"); Enter(dialog,705);
    Check(RawName(fixture.Bodies()[1].name)==std::string(19,'T'),"overlong CP932 rename truncates at a complete character boundary");
    ExpectText(WindowText(GetDlgItem(dialog,705)),std::wstring(19,L'T')+
        (MMD_ORIGINAL_PROFILE?L"":L"塔"),"display follows the active profile's stored name representation");
    Select(dialog,704,0); Select(dialog,736,1); Select(dialog,736,0);
    ExpectText(WindowText(GetDlgItem(dialog,705)),full,"selection round trip keeps the complete accepted renamed body");
    ExpectText(WindowText(GetDlgItem(dialog,740)),L"関節_塔","selection round trip keeps renamed joint");
    Type(dialog,740,L"確定_塔"); // final name is collected by actual OK
    fixture.Close(true);
    std::string finalJoint; text_encoding::EncodeExact(L"確定_塔",932,finalJoint);
    Check(RawName(fixture.model->rigidTable[0].name)==fullBytes,"OK commits the complete accepted CP932 body name");
    Check(RawName(fixture.model->rigidTable[1].name)==std::string(19,'T'),"OK commits truncated name at a complete character boundary");
    Check(RawName(fixture.model->jointTable[0].name)==finalJoint,"OK directly collects and commits renamed joint");
#if MMD_ORIGINAL_PROFILE
    Check(!fixture.model->rigidTable[0].jpText && !fixture.model->rigidTable[0].enText &&
          !fixture.model->jointTable[0].jpText && !fixture.model->jointTable[0].enText,
          "OK removes stale wide aliases for renamed records");
#else
    ExpectText(fixture.model->rigidTable[0].jpText,full,"OK commits the renamed body's full Unicode allocation");
    ExpectText(fixture.model->rigidTable[1].jpText,std::wstring(19,L'T')+L"塔","OK keeps the Unicode suffix beyond its legacy mirror");
    ExpectText(fixture.model->jointTable[0].jpText,L"確定_塔","OK collects the final Unicode joint name");
    Check(!fixture.model->rigidTable[0].enText&&!fixture.model->jointTable[0].enText,
          "OK never restores obsolete English aliases after real renames");
#endif
    dialog=fixture.Open(kUiChinese); if (!dialog) return;
    ExpectText(WindowText(GetDlgItem(dialog,705)),full,"reopening uses committed body name");
    ExpectText(WindowText(GetDlgItem(dialog,740)),L"確定_塔","reopening uses committed joint name");
    for(int id:{704,741,742}) ExpectText(ItemText(dialog,id,0),full,"reopened list/A/B agree on committed body name");
    fixture.Close(false);
}
void TopologyAndModes(bool legacy) {
    {
        Fixture fixture(legacy, true); if (!fixture.ready) return;
        HWND dialog = fixture.Open(kUiEnglish); if (!dialog) return;
        Command(dialog, 738); // copy a joint whose endpoints will be deleted
        Command(dialog, 706); // delete body scratch slot 0
        Check(fixture.app->state.selectedRigidIndex == 1 &&
              SendDlgItemMessageW(dialog,704,CB_GETCURSEL,0,0) == 0,
              "deleting first body selects the surviving scratch slot");
        Check(fixture.app->state.selectedJointIndex == -1,
              "deleting a body clears selection when all attached joints disappear");
        Type(dialog,718,L"3.25"); Enter(dialog,718);
        Check(fixture.Bodies()[1].mass == 3.25f,
              "Enter updates surviving body even with no selected joint");
        Command(dialog,735);
        Check(fixture.Joints()[0].rigidA == 1 && fixture.Joints()[0].rigidB == 1 &&
              SendDlgItemMessageW(dialog,741,CB_GETCURSEL,0,0) == 0 &&
              SendDlgItemMessageW(dialog,742,CB_GETCURSEL,0,0) == 0,
              "new joint uses the first surviving body rather than deleted slot zero");
        Command(dialog,739);
        Check(fixture.Joints()[0].rigidA == 1 && fixture.Joints()[0].rigidB == 1,
              "joint paste does not restore an endpoint deleted after copy");
        fixture.Close(true);
        Check(fixture.model->rigidCount == 1 && fixture.model->jointCount == 1 &&
              fixture.model->rigidTable[0].mass == 3.25f &&
              fixture.model->jointTable[0].rigidA == 0 && fixture.model->jointTable[0].rigidB == 0 &&
              fixture.world.world.getNumCollisionObjects() == 1 && fixture.world.world.getNumConstraints() == 1,
              "OK safely compacts surviving body and newly added joint into live physics");
    }
    {
        Fixture fixture(legacy, true); if (!fixture.ready) return;
        HWND dialog = fixture.Open(kUiEnglish); if (!dialog) return;
        Command(dialog,737);
        Check(fixture.app->state.selectedJointIndex == 1,
              "deleting first joint selects surviving scratch slot");
        Type(dialog,744,L"2.5"); Enter(dialog,744);
        Check(fixture.Joints()[1].position[0] == 2.5f,
              "joint edits target the visible surviving joint after deleting the first");
        Command(dialog,737);
        Check(fixture.app->state.selectedJointIndex == -1 &&
              SendDlgItemMessageW(dialog,736,CB_GETCOUNT,0,0) == 0,
              "consecutive joint deletes clear both actual records and list");
        Command(dialog,706); Command(dialog,706);
        Check(fixture.app->state.selectedRigidIndex == -1 &&
              SendDlgItemMessageW(dialog,704,CB_GETCOUNT,0,0) == 0,
              "consecutive body deletes clear both actual records and list");
        // Enter still reaches a subclass even after the native edit has been
        // disabled. Neither family may form a reference to selected slot -1.
        Type(dialog,718,L"8.5"); Enter(dialog,718);
        Type(dialog,744,L"7.5"); Enter(dialog,744);
        const auto deletedBody = fixture.Bodies()[0];
        const auto deletedJoint = fixture.Joints()[0];
        for (int id : {724,725,726,727,728,729,730,731,738,739,706,737}) Command(dialog,id);
        for (int id : {704,736,707,708,741,742,743}) {
            const HWND combo = GetDlgItem(dialog,id);
            SendMessageW(combo,CB_SETCURSEL,WPARAM(-1),0);
            SendMessageW(dialog,WM_COMMAND,MAKEWPARAM(id,CBN_SELCHANGE),reinterpret_cast<LPARAM>(combo));
        }
        Check(fixture.app->state.selectedRigidIndex == -1 && fixture.app->state.selectedJointIndex == -1 &&
              !std::memcmp(&deletedBody,&fixture.Bodies()[0],sizeof deletedBody) &&
              !std::memcmp(&deletedJoint,&fixture.Joints()[0],sizeof deletedJoint),
              "empty selection ignores queued edits, copy/paste, mode and combo notifications without touching deleted slots");
        fixture.Close(true);
        Check(fixture.model->rigidCount == 0 && fixture.model->jointCount == 0 &&
              fixture.world.world.getNumCollisionObjects() == 0 && fixture.world.world.getNumConstraints() == 0,
              "empty physics edit commits without resurrecting deleted scratch slots");
        dialog = fixture.Open(kUiEnglish); if (!dialog) return;
        Check(fixture.app->state.selectedRigidIndex == -1 && fixture.app->state.selectedJointIndex == -1 &&
              !IsWindowEnabled(GetDlgItem(dialog,718)) && !IsWindowEnabled(GetDlgItem(dialog,744)),
              "reopened empty tables have no selection and disable both editor families");
        fixture.Close(false);
    }
    {
        Fixture fixture(legacy); if (!fixture.ready) return;
        HWND dialog = fixture.Open(kUiEnglish); if (!dialog) return;
        auto* oldBodies = fixture.model->rigidTable;
        auto* oldJoints = fixture.model->jointTable;
        void* oldBody = oldBodies[0].body;
        const std::array<NameState,2> bodyNames{{NameState(oldBodies[0]),NameState(oldBodies[1])}};
        const std::array<NameState,2> jointNames{{NameState(oldJoints[0]),NameState(oldJoints[1])}};
        const int warnings = messages;
        fixture.Joints()[0].rigidA = -1;
        Command(dialog,IDOK);
        Check(messages == warnings + 1 && IsWindow(dialog) &&
              fixture.model->rigidTable == oldBodies && fixture.model->jointTable == oldJoints &&
              oldBodies[0].body == oldBody && fixture.world.world.getNumConstraints() == 2 &&
              fixture.world.world.getNumCollisionObjects() == 2,
              "invalid joint refuses OK before releasing any old table or Bullet object");
        fixture.Joints()[0].rigidA = 100000;
        Command(dialog,IDOK);
        Check(IsWindow(dialog) && fixture.model->rigidTable == oldBodies &&
              fixture.world.world.getNumCollisionObjects() == 2,
              "out-of-range joint endpoint also leaves live physics intact");
        for (unsigned i = 0; i < 2; ++i) {
            bodyNames[i].ExpectUnchanged(oldBodies[i],"invalid references preserve exact rigid names and ownership");
            jointNames[i].ExpectUnchanged(oldJoints[i],"invalid references preserve exact joint names and ownership");
        }
        fixture.Close(false);
    }
    {
        Fixture fixture(legacy); if (!fixture.ready) return;
        HWND dialog = fixture.Open(kUiEnglish); if (!dialog) return;
        Select(dialog,736,1); Select(dialog,741,1); Select(dialog,742,1);
        Command(dialog,706);
        Check(fixture.app->state.selectedJointIndex == 1 && fixture.Joints()[1].constraint >= 0 &&
              reinterpret_cast<std::intptr_t>(fixture.Bodies()[0].keyData) == -1 &&
              fixture.Joints()[1].rigidA == 1 && fixture.Joints()[1].rigidB == 1,
              "deleted-body rejection fixture retains an independent live joint and an in-range deleted body slot");
        auto* oldBodies = fixture.model->rigidTable;
        auto* oldJoints = fixture.model->jointTable;
        std::array<unsigned char,sizeof(mdl::RigidRecord)*2> bodyBytes{};
        std::array<unsigned char,sizeof(mdl::JointRecord)*2> jointBytes{};
        std::memcpy(bodyBytes.data(),oldBodies,bodyBytes.size());
        std::memcpy(jointBytes.data(),oldJoints,jointBytes.size());
        const std::array<NameState,2> bodyNames{{NameState(oldBodies[0]),NameState(oldBodies[1])}};
        const std::array<NameState,2> jointNames{{NameState(oldJoints[0]),NameState(oldJoints[1])}};
        const std::array<btCollisionObject*,2> bodyObjects{{fixture.world.world.getCollisionObjectArray()[0],
            fixture.world.world.getCollisionObjectArray()[1]}};
        const std::array<btTypedConstraint*,2> constraints{{fixture.world.world.getConstraint(0),
            fixture.world.world.getConstraint(1)}};
        const auto textCount = owned_text::Count();
        const int warnings = messages;
        fixture.Joints()[1].rigidB = 0;
        Command(dialog,IDOK);
        const bool unchangedTables = fixture.model->rigidTable == oldBodies &&
            fixture.model->jointTable == oldJoints && fixture.model->rigidCount == 2 && fixture.model->jointCount == 2;
        Check(messages == warnings + 1 && IsWindow(dialog) && fixture.app->state.frameCopyDialog == dialog &&
              unchangedTables && !std::memcmp(bodyBytes.data(),oldBodies,bodyBytes.size()) &&
              !std::memcmp(jointBytes.data(),oldJoints,jointBytes.size()) && owned_text::Count() == textCount &&
              fixture.world.world.getNumCollisionObjects() == 2 && fixture.world.world.getNumConstraints() == 2 &&
              fixture.world.world.getCollisionObjectArray()[0] == bodyObjects[0] &&
              fixture.world.world.getCollisionObjectArray()[1] == bodyObjects[1] &&
              fixture.world.world.getConstraint(0) == constraints[0] && fixture.world.world.getConstraint(1) == constraints[1],
              "live joint B referencing an in-range deleted body refuses OK before altering window, tables, Bullet objects or text ownership");
        if (unchangedTables) for (unsigned i = 0; i < 2; ++i) {
            bodyNames[i].ExpectUnchanged(oldBodies[i],"deleted B endpoint refusal preserves rigid names and ownership");
            jointNames[i].ExpectUnchanged(oldJoints[i],"deleted B endpoint refusal preserves joint names and ownership");
        }
        fixture.Close(false);
    }
    {
        Fixture fixture(legacy, true); if (!fixture.ready) return;
        HWND dialog = fixture.Open(kUiEnglish); if (!dialog) return;
        Command(dialog,730);
        Check(fixture.Bodies()[0].mode == 0 && fixture.Bodies()[0].staticFlag == 1 &&
              fixture.Bodies()[0].kinematicFlag == 0,
              "bone-follow selection synchronizes static and alignment flags in scratch");
        fixture.Close(true);
        dialog = fixture.Open(kUiEnglish); if (!dialog) return;
        Check(fixture.Bodies()[0].mode == 0 && IsDlgButtonChecked(dialog,730) == BST_CHECKED,
              "bone-follow authoring mode survives commit and reopen");
        CheckDlgButton(dialog,731,BST_CHECKED); Command(dialog,729);
        Check(fixture.Bodies()[0].mode == 2 && fixture.Bodies()[0].staticFlag == 0 &&
              fixture.Bodies()[0].kinematicFlag == 1,
              "physical bone-alignment selection clears the old static flag");
        Command(dialog,724); Select(dialog,704,1); Command(dialog,725);
        Check(fixture.Bodies()[1].mode == 2 && fixture.Bodies()[1].staticFlag == 0 &&
              fixture.Bodies()[1].kinematicFlag == 1,
              "body parameter paste synchronizes authoring flags");
        fixture.Close(true);
        dialog = fixture.Open(kUiEnglish); if (!dialog) return;
        Check(fixture.Bodies()[0].mode == 2 && fixture.Bodies()[1].mode == 2,
              "physical alignment mode survives commit and reopen on both bodies");
        Command(dialog,731);
        Check(fixture.Bodies()[0].mode == 1 && fixture.Bodies()[0].staticFlag == 0 &&
              fixture.Bodies()[0].kinematicFlag == 0,
              "turning alignment off keeps the physical author type consistent");
        fixture.Close(true);
        dialog = fixture.Open(kUiEnglish); if (!dialog) return;
        Check(fixture.Bodies()[0].mode == 1 && IsDlgButtonChecked(dialog,729) == BST_CHECKED &&
              IsDlgButtonChecked(dialog,731) == BST_UNCHECKED,
              "ordinary physical mode and checkbox survive commit and reopen");
        Command(dialog,731);
        Check(fixture.Bodies()[0].mode == 2 && fixture.Bodies()[0].staticFlag == 0 &&
              fixture.Bodies()[0].kinematicFlag == 1,
              "turning alignment back on updates all physical author flags");
        Command(dialog,685);
        const int added = fixture.app->state.selectedRigidIndex;
        Check(fixture.Bodies()[added].mode == 0 && fixture.Bodies()[added].staticFlag == 1,
              "new bone-follow rigid body starts with matching authoring flags");
        fixture.Close(true);
        dialog = fixture.Open(kUiEnglish); if (!dialog) return;
        Select(dialog,704,added);
        Check(fixture.Bodies()[added].mode == 0 && fixture.Bodies()[added].staticFlag == 1 &&
              IsDlgButtonChecked(dialog,730) == BST_CHECKED,
              "new bone-follow body survives its own commit and reopen");
        CheckDlgButton(dialog,731,BST_UNCHECKED); Command(dialog,729);
        Check(fixture.Bodies()[added].mode == 1 && fixture.Bodies()[added].staticFlag == 0 &&
              fixture.Bodies()[added].kinematicFlag == 0,
              "ordinary physical selection clears static and alignment flags");
        fixture.Close(true);
        dialog = fixture.Open(kUiEnglish); if (!dialog) return;
        Select(dialog,704,added);
        Check(fixture.Bodies()[added].mode == 1 && fixture.Bodies()[added].staticFlag == 0 &&
              fixture.Bodies()[added].kinematicFlag == 0,
              "newly created body's ordinary physical mode survives commit and reopen");
        fixture.Close(false);
    }
    {
        Fixture fixture(legacy); if (!fixture.ready) return;
        HWND dialog = fixture.Open(kUiEnglish); if (!dialog) return;
        // Keep joint 1 attached only to body 1, then delete body 0 through the
        // normal dialog. Its associated joint 0 must disappear independently.
        Select(dialog,736,1); Select(dialog,741,1); Select(dialog,742,1);
        fixture.Close(true);
        const NameState survivingBody(fixture.model->rigidTable[1]);
        const NameState survivingJoint(fixture.model->jointTable[1]);
        dialog = fixture.Open(kUiEnglish); if (!dialog) return;
        Command(dialog,706);
        Check(fixture.app->state.selectedRigidIndex == 1 && fixture.app->state.selectedJointIndex == 1 &&
              SendDlgItemMessageW(dialog,704,CB_GETCURSEL,0,0) == 0 &&
              SendDlgItemMessageW(dialog,736,CB_GETCURSEL,0,0) == 0,
              "body deletion selects surviving body and independent joint scratch slots together");
        Type(dialog,744,L"4.75"); Enter(dialog,744);
        Check(fixture.Joints()[1].position[0] == 4.75f,
              "joint Enter targets the displayed survivor after related body deletion");
        fixture.Close(true);
        Check(fixture.model->rigidCount == 1 && fixture.model->jointCount == 1 &&
              fixture.model->jointTable[0].rigidA == 0 && fixture.model->jointTable[0].rigidB == 0 &&
              fixture.model->jointTable[0].position[0] == 4.75f,
              "surviving independent joint compacts and retains its actual edit");
        constexpr auto publishedStorage=MMD_ORIGINAL_PROFILE?NameState::Storage::FormalRetained:NameState::Storage::FormalVersion;
        survivingBody.ExpectUnchanged(fixture.model->rigidTable[0],"body compaction retains surviving wide text",publishedStorage);
        survivingJoint.ExpectUnchanged(fixture.model->jointTable[0],"joint compaction retains surviving wide text",publishedStorage);
    }
    {
        Fixture fixture(legacy,true); if (!fixture.ready) return;
        HWND dialog = fixture.Open(kUiEnglish); if (!dialog) return;
        Command(dialog,738); Command(dialog,706);
        Command(dialog,685); // the replacement body reuses scratch slot 0
        Check(fixture.app->state.selectedRigidIndex == 0,"new body reuses the deleted first scratch slot");
        Command(dialog,735);
        Select(dialog,741,0); Select(dialog,742,0); // combo row 0 is original body 1
        Command(dialog,739);
        const int joint = fixture.app->state.selectedJointIndex;
        Check(fixture.Joints()[joint].rigidA == 1 && fixture.Joints()[joint].rigidB == 1,
              "copied joint cannot acquire a different body after its old endpoint slot is reused");
        fixture.Close(true);
        Check(fixture.model->jointCount == 1 && fixture.model->jointTable[0].rigidA == 1 &&
              fixture.model->jointTable[0].rigidB == 1,
              "slot-reuse paste keeps the chosen surviving endpoint after native compaction");
    }
}
unsigned long long FileTicks(const FILETIME& value) {
    return (static_cast<unsigned long long>(value.dwHighDateTime)<<32)|value.dwLowDateTime;
}
unsigned long long CpuTicks() {
    FILETIME create{},exit{},kernel{},user{};
    Check(GetProcessTimes(GetCurrentProcess(),&create,&exit,&kernel,&user)!=FALSE,"read process CPU time");
    return FileTicks(kernel)+FileTicks(user);
}
void Benchmark(unsigned iterations) {
    context="benchmark-ascii"; Fixture fixture(true,true); if(!fixture.ready)return;
    quiet=true;
    if(fixture.Open(kUiEnglish))fixture.Close(false); // warmup outside timing
    unsigned long long checksum=0;
    LARGE_INTEGER frequency{},start{},end{}; QueryPerformanceFrequency(&frequency);
    const auto cpuStart=CpuTicks(); QueryPerformanceCounter(&start);
    for(unsigned i=0;i<iterations;++i) {
        HWND dialog=fixture.Open(kUiEnglish); if(!dialog)break;
        checksum+=WindowText(GetDlgItem(dialog,705)).size()+WindowText(GetDlgItem(dialog,740)).size();
        fixture.Close(false);
    }
    QueryPerformanceCounter(&end); const auto cpuEnd=CpuTicks(); quiet=false;
    const double elapsed=1000.0*double(end.QuadPart-start.QuadPart)/double(frequency.QuadPart);
    std::printf("BENCHMARK acp=%u warmup=1 iterations=%u bodies=2 joints=2 elapsed_ms=%.6f mean_ms=%.6f process_cpu_ms=%.6f checksum=%llu scope=menu262-create-fill-read-cancel fixture_load_excluded=1 gpu=0 fps_claim=0\n",
        GetACP(),iterations,elapsed,elapsed/iterations,double(cpuEnd-cpuStart)/10000.0,checksum);
    Check(checksum==iterations*13ull,"all timed dialogs completed the same ASCII workload");
}
}

int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    bool benchmark=false,hiddenWindowsOnly=false; unsigned iterations=3,expectedAcp=0;
    std::wstring expectedDesktop;
    for(int i=1;i<argc;++i) {
        if(!std::strcmp(argv[i],"--benchmark"))benchmark=true;
        else if(!std::strcmp(argv[i],"--hidden-windows-only"))hiddenWindowsOnly=true;
        else if(!std::strcmp(argv[i],"--expect-acp")&&i+1<argc)expectedAcp=static_cast<unsigned>(std::strtoul(argv[++i],nullptr,10));
        else if(!std::strcmp(argv[i],"--expect-desktop")&&i+1<argc) {
            const std::string name=argv[++i];expectedDesktop.assign(name.begin(),name.end());
        }
        else if(!std::strcmp(argv[i],"--iterations")&&i+1<argc)iterations=static_cast<unsigned>(std::strtoul(argv[++i],nullptr,10));
        else {Check(false,"usage: [--expect-acp 932|936|1252|65001] [--expect-desktop PRIVATE_NAME | --hidden-windows-only] [--benchmark] [--iterations 1..10]");return 2;}
    }
    if(!Check(iterations>=1&&iterations<=10,"bounded benchmark iteration count"))return 2;
    std::printf("ENV actual_acp=%u expected_acp=%u gpu=0 entry=CommandDispatch-262\n",GetACP(),expectedAcp);
    if(expectedAcp&&!Check(GetACP()==expectedAcp,"actual process ACP matches external manifest"))return 2;
    if(!Check(!hiddenWindowsOnly||(!benchmark&&expectedDesktop.empty()),
        "explicit hidden-window mode is separate from private-desktop tests and benchmarks"))return 2;
    wchar_t desktop[256]{};DWORD needed=0;
    const bool namedPrivate=expectedDesktop.empty()||expectedDesktop.rfind(L"MMS_REPAIR_",0)==0||
        expectedDesktop.rfind(L"CodexSandboxDesktop-",0)==0;
    if(!Check(namedPrivate&&GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,desktop,sizeof desktop,&needed)&&
        (hiddenWindowsOnly||(expectedDesktop.empty()?std::wcsncmp(desktop,L"MMS_REPAIR_",11)==0:expectedDesktop==desktop)),
        "require the actual named private test desktop, never Default"))return 2;
    std::printf("TEST_DESKTOP name=%s private_guard=%d hidden_windows_only=%d\n",Utf8(desktop).c_str(),
        int(!hiddenWindowsOnly),int(hiddenWindowsOnly));
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    if(!Check(MH_Initialize()==MH_OK,"initialize hidden-window hooks"))return 2;
    if(!Check(MH_CreateHook(&ShowWindow,&HideTopLevel,reinterpret_cast<void**>(&realShowWindow))==MH_OK&&
        MH_CreateHook(&MessageBoxW,&ObserveMessageW,nullptr)==MH_OK&&
        MH_CreateHook(&MessageBoxA,&ObserveMessageA,nullptr)==MH_OK&&MH_EnableHook(MH_ALL_HOOKS)==MH_OK,
        "hide top-level windows and observe production warnings"))return 2;
    const HHOOK guard=SetWindowsHookExW(WH_CBT,HideCreated,nullptr,GetCurrentThreadId());
    if(!Check(guard!=nullptr,"prevent test-window activation"))return 2;
    INITCOMMONCONTROLSEX controls{sizeof controls,ICC_WIN95_CLASSES};InitCommonControlsEx(&controls);
    SetModelInfoLanguagePreference(kModelInfoLanguageAuto);
    SetModelNameAutoTranslation(false);
    if(benchmark)Benchmark(iterations);
    else {
#if MMD_ORIGINAL_PROFILE
        OriginalRefusals();
        constexpr bool formats[]{true};
#else
        constexpr bool formats[]{false,true};
#endif
        for(bool legacy:formats) {
            context=legacy?"PMD-CP932-loader":"PMX-Unicode";
            { Fixture fixture(legacy); if(fixture.ready){Unchanged(fixture);Renames(fixture);} }
            TopologyAndModes(legacy);
        }
    }
    UnhookWindowsHookEx(guard);MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();
    std::printf("RESULT checks=%d failures=%d dialogs=%d messages=%d actual_acp=%u gpu=0\n",checks,failures,opened,messages,GetACP());
    return failures?1:0;
}
