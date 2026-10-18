// MAINTENANCE: regression-keep; real menu 262, resources and edit subclasses.
// Run on a private MMS_REPAIR_ desktop. No D3D device or renderer is created.
// --expect-acp checks the actual process ACP selected by an external manifest;
// this test never mocks GetACP or changes a machine/user language setting.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>
#include <MinHook.h>
#include <btBulletDynamicsCommon.h>
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/path_workspace.hpp"
#include "mikudancestudio/physics_scene.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/text_encoding.hpp"
#include "mikudancestudio/ui_language.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fcntl.h>
#include <io.h>
#include <memory>
#include <string>
#include <vector>

using namespace mikudancestudio;
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
    return {{{std::wstring(18, L'A') + L"塔", L"右腕"}},
            {{std::wstring(18, L'J') + L"塔", L"左腕接点"}},
            {{L"センター", L"右腕"}}};
}
Names AsciiNames() { return {{{L"body-A", L"body-B"}}, {{L"joint-A", L"joint-B"}}, {{L"bone-A", L"bone-B"}}}; }
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
        Text(bytes, names.bodies[i]); Text(bytes, i ? L"body-English-B" : L"body-English-A");
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
        Text(bytes, names.joints[i]); Text(bytes, i ? L"joint-English-B" : L"joint-English-A");
        Put(bytes, static_cast<unsigned char>(0));
        Put(bytes, static_cast<signed char>(i)); Put(bytes, static_cast<signed char>(1 - i));
        for (int field = 0; field < 24; ++field) Put(bytes, 0.f);
    }
    return bytes;
}
struct World {
    btDefaultCollisionConfiguration config;
    btCollisionDispatcher dispatcher{&config};
    btDbvtBroadphase broadphase;
    btSequentialImpulseConstraintSolver solver;
    btDiscreteDynamicsWorld world{&dispatcher, &broadphase, &solver, &config};
    PhysicsScene scene{};
    World() { scene.world = &world; world.setGravity(btVector3(0,0,0)); }
};
template<class Record> void SetLegacyName(Record& record, const std::wstring& name) {
    std::free(record.jpText); record.jpText = nullptr;
    std::free(record.enText); record.enText = nullptr;
    std::string bytes;
    if (!Check(text_encoding::EncodeExact(name.c_str(), 932, bytes) && bytes.size() <= sizeof record.name,
               "fixture legacy name fits exact CP932 field")) return;
    std::memset(record.name, 0, sizeof record.name);
    std::memcpy(record.name, bytes.data(), bytes.size());
}
struct NameState {
    std::array<char, 20> raw{};
    wchar_t* jp = nullptr;
    wchar_t* en = nullptr;
    std::wstring jpValue, enValue;
    template<class Record> explicit NameState(const Record& record) : jp(record.jpText), en(record.enText),
        jpValue(jp ? jp : L""), enValue(en ? en : L"") { std::memcpy(raw.data(), record.name, raw.size()); }
    template<class Record> void ExpectUnchanged(const Record& record, const char* label) const {
        Check(!std::memcmp(raw.data(), record.name, raw.size()), label);
        Check(record.jpText == jp && record.enText == en, "unchanged name transfers original wide ownership");
        ExpectText(record.jpText ? record.jpText : L"", jpValue, "original full Japanese text survives");
        ExpectText(record.enText ? record.enText : L"", enValue, "original full English text survives");
    }
};
struct Fixture {
    World world;
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
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
        const Bytes bytes = Pmx(names);
        wchar_t temp[MAX_PATH]{}, path[MAX_PATH]{};
        if (!Check(GetTempPathW(MAX_PATH, temp) && GetTempFileNameW(temp, L"pec", 0, path), "create fixture file")) return;
        int fd = _wopen(path, _O_BINARY | _O_WRONLY | _O_TRUNC);
        if (!Check(fd >= 0, "open fixture for writing")) { DeleteFileW(path); return; }
        const int written = _write(fd, bytes.data(), static_cast<unsigned>(bytes.size())); _close(fd);
        if (!Check(written == static_cast<int>(bytes.size()), "write full fixture")) { DeleteFileW(path); return; }
        fd = _wopen(path, _O_BINARY | _O_RDONLY);
        if (!Check(fd >= 0, "open fixture for production loader")) { DeleteFileW(path); return; }
        _lseek(fd, 3, SEEK_SET);
        PathResolutionWorkspace paths{};
        ready = LoadPMX(reinterpret_cast<unsigned char*>(model.get()), nullptr, 0, 0, paths, fd) != 0;
        Check(ready, "real PMX loader completes without a renderer");
        Check(DeleteFileW(path) != FALSE, "loader closes fixture file");
        if (!ready) return;
        ready = Check(model->boneCount == 2 && model->rigidCount == 2 && model->jointCount == 2 &&
            world.world.getNumCollisionObjects() == 2 && world.world.getNumConstraints() == 2,
            "fixture has complete real bone/body/joint/keyframe state");
        if (legacy) {
            // A complete loaded Record fixture with PMD's fixed CP932 name
            // representation. This does not claim to exercise the PMD loader.
            for (int i = 0; i < 2; ++i) {
                SetLegacyName(model->rigidTable[i], names.bodies[i]);
                SetLegacyName(model->jointTable[i], names.joints[i]);
                SetLegacyName(model->boneTable[i], names.bones[i]);
            }
        }
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
        Check(app->Renderer() == nullptr, "physics editor test never creates a renderer");
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
            body[i].ExpectUnchanged(fixture.Bodies()[i], "unchanged Enter/Collect preserves every body-name byte");
            joint[i].ExpectUnchanged(fixture.Joints()[i], "unchanged Enter/Collect preserves every joint-name byte");
        }
        // Production copy/paste buttons copy physics parameters, not names.
        Command(dialog,724); Select(dialog,704,0); Command(dialog,725);
        ExpectText(WindowText(GetDlgItem(dialog,705)), fixture.names.bodies[0], "body parameter paste keeps destination name");
        Check(fixture.Bodies()[0].mass == fixture.Bodies()[1].mass, "body parameter paste actually ran");
        Command(dialog,738); Select(dialog,736,0); Command(dialog,739);
        ExpectText(WindowText(GetDlgItem(dialog,740)), fixture.names.joints[0], "joint parameter paste keeps destination name");
        fixture.Close(true);
        for (int i=0;i<2;++i) {
            body[i].ExpectUnchanged(fixture.model->rigidTable[i], "OK preserves unchanged body raw name");
            joint[i].ExpectUnchanged(fixture.model->jointTable[i], "OK preserves unchanged joint raw name");
        }
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
    const std::wstring full = std::wstring(18,L'R') + L"塔";
    std::string fullBytes; text_encoding::EncodeExact(full.c_str(),932,fullBytes);
    Check(fullBytes.size()==20 && static_cast<unsigned char>(fullBytes.back())==0x83,
        "boundary fixture has a valid DBCS trail byte in lead-byte range at byte 20");
    Type(dialog,705,full); Enter(dialog,705);
    Check(RawName(fixture.Bodies()[0].name)==fullBytes, "Enter stores full 20-byte name without splitting valid DBCS pair");
    Check(!fixture.Bodies()[0].jpText && !fixture.Bodies()[0].enText, "rename clears stale wide aliases only in scratch");
    for (int id:{704,741,742}) ExpectText(ItemText(dialog,id,0),full,"Enter updates rigid list and joint A/B names");
    const int beforeBodyWarning=messages;
    Type(dialog,705,L"无法保存_😀"); Enter(dialog,705);
    Check(messages>beforeBodyWarning,"unrepresentable body rename reports rejection");
    Check(RawName(fixture.Bodies()[0].name)==fullBytes,"rejected body rename retains old bytes");
    ExpectText(WindowText(GetDlgItem(dialog,705)),full,"rejected body input restores previous displayed name");

    Type(dialog,740,L"関節_塔"); Enter(dialog,740);
    std::string jointBytes; text_encoding::EncodeExact(L"関節_塔",932,jointBytes);
    const int beforeJointWarning=messages;
    Type(dialog,740,L"拒否_🦴"); Command(dialog,800); // CollectJointEdits, not only Enter
    Check(messages>beforeJointWarning,"unrepresentable joint Collect reports rejection");
    Check(RawName(fixture.Joints()[0].name)==jointBytes,"rejected joint Collect retains old bytes");
    ExpectText(WindowText(GetDlgItem(dialog,740)),L"関節_塔","rejected joint input restores previous displayed name");
    Select(dialog,704,1);
    Type(dialog,705,std::wstring(19,L'T')+L"塔"); Enter(dialog,705);
    Check(RawName(fixture.Bodies()[1].name)==std::string(19,'T'),"overlong CP932 rename truncates at a complete character boundary");
    ExpectText(WindowText(GetDlgItem(dialog,705)),std::wstring(19,L'T'),"truncated rename shows exactly stored name");
    Select(dialog,704,0); Select(dialog,736,1); Select(dialog,736,0);
    ExpectText(WindowText(GetDlgItem(dialog,705)),full,"selection round trip keeps full 20-byte renamed body");
    ExpectText(WindowText(GetDlgItem(dialog,740)),L"関節_塔","selection round trip keeps renamed joint");
    Type(dialog,740,L"確定_塔"); // final name is collected by actual OK
    fixture.Close(true);
    std::string finalJoint; text_encoding::EncodeExact(L"確定_塔",932,finalJoint);
    Check(RawName(fixture.model->rigidTable[0].name)==fullBytes,"OK commits 20-byte body name");
    Check(RawName(fixture.model->rigidTable[1].name)==std::string(19,'T'),"OK commits truncated name at a complete character boundary");
    Check(RawName(fixture.model->jointTable[0].name)==finalJoint,"OK directly collects and commits renamed joint");
    Check(!fixture.model->rigidTable[0].jpText && !fixture.model->rigidTable[0].enText &&
          !fixture.model->jointTable[0].jpText && !fixture.model->jointTable[0].enText,
          "OK removes stale wide aliases for renamed records");
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
        fixture.Close(true);
        Check(fixture.model->rigidCount == 0 && fixture.model->jointCount == 0 &&
              fixture.world.world.getNumCollisionObjects() == 0 && fixture.world.world.getNumConstraints() == 0,
              "empty physics edit commits without resurrecting deleted scratch slots");
    }
    {
        Fixture fixture(legacy, true); if (!fixture.ready) return;
        HWND dialog = fixture.Open(kUiEnglish); if (!dialog) return;
        auto* oldBodies = fixture.model->rigidTable;
        auto* oldJoints = fixture.model->jointTable;
        void* oldBody = oldBodies[0].body;
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
        Command(dialog,685);
        const int added = fixture.app->state.selectedRigidIndex;
        Check(fixture.Bodies()[added].mode == 0 && fixture.Bodies()[added].staticFlag == 1,
              "new bone-follow rigid body starts with matching authoring flags");
        CheckDlgButton(dialog,731,BST_UNCHECKED); Command(dialog,729);
        Check(fixture.Bodies()[added].mode == 1 && fixture.Bodies()[added].staticFlag == 0 &&
              fixture.Bodies()[added].kinematicFlag == 0,
              "ordinary physical selection clears static and alignment flags");
        fixture.Close(true);
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
    bool benchmark=false; unsigned iterations=3,expectedAcp=0;
    for(int i=1;i<argc;++i) {
        if(!std::strcmp(argv[i],"--benchmark"))benchmark=true;
        else if(!std::strcmp(argv[i],"--expect-acp")&&i+1<argc)expectedAcp=static_cast<unsigned>(std::strtoul(argv[++i],nullptr,10));
        else if(!std::strcmp(argv[i],"--iterations")&&i+1<argc)iterations=static_cast<unsigned>(std::strtoul(argv[++i],nullptr,10));
        else {Check(false,"usage: [--expect-acp 932|936|1252|65001] [--benchmark] [--iterations 1..10]");return 2;}
    }
    if(!Check(iterations>=1&&iterations<=10,"bounded benchmark iteration count"))return 2;
    std::printf("ENV actual_acp=%u expected_acp=%u gpu=0 entry=CommandDispatch-262\n",GetACP(),expectedAcp);
    if(expectedAcp&&!Check(GetACP()==expectedAcp,"actual process ACP matches external manifest"))return 2;
    wchar_t desktop[256]{};DWORD needed=0;
    if(!Check(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,desktop,sizeof desktop,&needed)&&
        std::wcsncmp(desktop,L"MMS_REPAIR_",11)==0,"require a real private MMS_REPAIR_ desktop"))return 2;
    std::printf("PRIVATE_DESKTOP name=%s\n",Utf8(desktop).c_str());
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
    else for(bool legacy:{false,true}) {
        context=legacy?"PMD-CP932-records":"PMX-Unicode";
        { Fixture fixture(legacy); if(fixture.ready){Unchanged(fixture);Renames(fixture);} }
        TopologyAndModes(legacy);
    }
    UnhookWindowsHookEx(guard);MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();
    std::printf("RESULT checks=%d failures=%d dialogs=%d messages=%d actual_acp=%u gpu=0\n",checks,failures,opened,messages,GetACP());
    return failures?1:0;
}
