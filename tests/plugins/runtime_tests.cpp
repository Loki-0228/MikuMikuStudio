#include "manager.hpp"
#include "scene_identity.hpp"
#include "project_storage.hpp"
#include "edit_store.hpp"
#include "shared_history.hpp"
#include "native_ui.hpp"
#include <native_ui/layout.hpp>
#include <native_ui/update.hpp>
#include <native_ui/win32_host.hpp>
#include <commctrl.h>
#include <fstream>
#include <iostream>
#include <thread>
#include <limits>

using namespace mikudancestudio::plugins;
static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #x); } while (false)
template<class F> static bool Throws(F f) { try { f(); return false; } catch (...) { return true; } }
static MmsString S(const char* s) { return {s, std::strlen(s)}; }
#include "surface_checks.inc"
#include "layout_checks.inc"
#include "update_checks.inc"
#include "win32_checks.inc"
#include "scene_checks.inc"
#include "cpp_sdk_checks.inc"
#include "refresh_checks.inc"
#include "state_refresh_checks.inc"
#include "visibility_refresh_checks.inc"
#include "cpp_refresh_checks.inc"
#include "data_refresh_checks.inc"
#include "cpp_data_refresh_checks.inc"
static void Write(const fs::path& path, const std::string& bytes) { fs::create_directories(path.parent_path()); std::ofstream f(path, std::ios::binary); f << bytes; if (!f) throw std::runtime_error("fixture write failed"); }
#include "ui_preferences_file_checks.inc"
#include "storage_checks.inc"
#include "edit_checks.inc"
#include "shared_history_checks.inc"
static std::string ManifestText(std::string id = "org.example.hello", std::string entry = "hello_menu.dll") {
    return "{\"schemaVersion\":1,\"id\":\"" + id + "\",\"name\":\"测试\",\"version\":\"1.0.0\",\"kind\":\"native\",\"abi\":{\"major\":1,\"minMinor\":0},\"entry\":\"" + entry + "\",\"architecture\":\"x64\",\"minWindows\":\"6.1.7601\",\"capabilities\":[\"ui.command\"],\"requiresServices\":[{\"id\":\"mms.ui\",\"major\":1,\"minMinor\":0}],\"dependencies\":[],\"nativeDependencies\":[],\"reloadPolicy\":\"restart\"}";
}
#include "manager_edit_checks.inc"
#include "manager_refresh_checks.inc"
#include "layout_refresh_checks.inc"
#include "state_content_checks.inc"
#include "manager_data_refresh_checks.inc"
#include "manager_layout_refresh_checks.inc"
#include "manager_data_refresh_concurrency_checks.inc"
#include "task_dispatch_checks.inc"
static void JsonChecks() {
    CHECK(Json::Parse("{\"a\":1}").At("a").UInt() == 1);
    CHECK(Throws([] { Json::Parse("{\"a\":1,\"a\":2}"); }));
    CHECK(Throws([] { Json::Parse("[1,]"); }));
    CHECK(Throws([] { Json::Parse("{\"a\":1}x"); }));
    CHECK(Throws([] { Json::Parse("01"); }));
    CHECK(Throws([] { Json::Parse("1e"); }));
    CHECK(Throws([] { Json::Parse("1.0").UInt(); }));
    CHECK(Throws([] { Json::Parse("4294967296").UInt(); }));
    CHECK(Throws([] { Json::Parse("\"\\ud800\""); }));
    CHECK(Json::Parse("\"\\ud83d\\ude00\"").String().size() == 4);
    CHECK(Throws([] { ParseVersion("1.2.3.4"); }));
    CHECK(Throws([] { ParseVersion("1.2.-1"); }));
    CHECK(ParseVersion("1.10.0") > ParseVersion("1.2.99"));
}
static void DiscoveryChecks(const fs::path& root) {
    fs::path p = root / L"发现测试"; fs::create_directories(p);
    Write(p / "root.dll", "not a PE"); Write(p / "root.plugin.json", ManifestText("example.root", "root.dll"));
    fs::path nested = p;
    for (int i = 1; i <= 5; ++i) {
        nested /= L"分类";
        fs::path package = nested / L"插件"; Write(package / "hello_menu.dll", "not a PE"); Write(package / "plugin.json", ManifestText("example.depth" + std::to_string(i)));
        Write(package / "ignored" / "hello_menu.dll", "not a PE"); Write(package / "ignored" / "plugin.json", ManifestText("example.ignored"));
    }
    for (int depth : {0, 1, 2, 5}) {
        Write(p / "config.json", "{\"schemaVersion\":1,\"maxDepth\":" + std::to_string(depth) + ",\"disabledPlugins\":[]}");
        auto d = Discover(p); CHECK(d.config.maxDepth == depth); CHECK(d.packages.size() == size_t(depth ? depth : 1));
    }
    Write(p / "config.json", "{broken"); auto d = Discover(p); CHECK(d.config.maxDepth == 2); CHECK(!d.diagnostics.empty());
    CHECK(Throws([&] { PackageFile(p, "../escape.dll"); }));
    CHECK(Throws([&] { PackageFile(p, "root.dll:evil"); }));
    CHECK(Throws([&] { PackageFile(p, "C:\\escape.dll"); }));
    CHECK(Throws([&] { PackageFile(p, "root.dll."); }));
    CHECK(SamePath(PackageFile(p, "root.dll"), p / "root.dll"));
    Discovery graph; Manifest a, b, c; a.id = "example.a"; b.id = "example.b"; c.id = "example.c"; a.version = b.version = c.version = {1, 0, 0};
    b.dependencies.push_back({a.id, {1,0,0}, {2,0,0}}); graph.packages = {b, c, a};
    auto order = Resolve(graph, {}); CHECK(order.size() == 3); CHECK(order[0] == 2 && order[1] == 0 && order[2] == 1);
    graph.packages.push_back(a); order = Resolve(graph, {}); CHECK(order.size() == 1 && order[0] == 1);
    graph.packages.pop_back(); graph.packages[2].dependencies.push_back({b.id, {1,0,0}, {2,0,0}}); order = Resolve(graph, {}); CHECK(order.size() == 1 && order[0] == 1);
}
struct Counter { int invokes = 0, releases = 0; };
static MmsResult MMS_CALL State(void*, MmsObjectRef, MmsCommandStateV1* out) { *out = {Header<MmsCommandStateV1>(), 1, 0, 1, 0, {}}; return MMS_OK; }
static MmsResult MMS_CALL Invoke(void* p, MmsObjectRef, MmsSpan, MmsHandle tx) { if (tx) return MMS_E_ARGUMENT; ++static_cast<Counter*>(p)->invokes; return MMS_OK; }
static MmsValue Number(double n) { MmsValue v{}; v.type = MMS_VALUE_NUMBER; v.value.number = n; return v; }
static void UiChecks() {
    UiRegistry registry; UiClient c{&registry, "example.test"}, other{&registry, "example.other"}; auto api = registry.Api(c); Counter counter, otherCounter;
    MmsArgumentV1 arg{}; arg.h = Header<MmsArgumentV1>(); arg.name = S("amount"); arg.valueType = MMS_VALUE_NUMBER; arg.flags = MMS_ARGUMENT_REQUIRED; arg.minimum = Number(1); arg.maximum = Number(3);
    MmsCommandDescV1 command{}; command.info.h = Header<MmsCommandDescV1>(); command.info.id = S("example.test.command"); command.info.ownerPlugin = S("example.test"); command.info.label = S("命令"); command.context = &counter; command.getState = State; command.invoke = Invoke;
    command.info.arguments = {&arg, 1, sizeof(arg), 1}; MmsHandle h = 0;
    CHECK(api.registerCommand(api.context, &command, &h) == MMS_OK);
    MmsNamedValue value{S("amount"), Number(4)}; MmsSpan args{&value, 1, sizeof(value), 1};
    CHECK(api.invoke(api.context, command.info.id, {}, args) == MMS_E_ARGUMENT); CHECK(counter.invokes == 0);
    value.value = Number(2); CHECK(api.invoke(api.context, command.info.id, {}, args) == MMS_OK); CHECK(counter.invokes == 1);
    command.context = &otherCounter; CHECK(api.updateCommand(api.context, h, &command) == MMS_E_ARGUMENT); command.context = &counter;
    CHECK(api.invoke(api.context, command.info.id, {}, args) == MMS_OK); CHECK(counter.invokes == 2 && otherCounter.invokes == 0);
    arg.valueType = MMS_VALUE_INT; CHECK(api.updateCommand(api.context, h, &command) == MMS_E_ARGUMENT); arg.valueType = MMS_VALUE_NUMBER;
    MmsUiSnapshotV1 snapshot{}; snapshot.h = Header<MmsUiSnapshotV1>(); CHECK(api.acquireSnapshot(api.context, 0, &snapshot) == MMS_OK);
    const auto* infos = static_cast<const MmsCommandInfoV1*>(snapshot.commands.data); CHECK(snapshot.commands.stride == sizeof(MmsCommandInfoV1) && infos[0].h.size == sizeof(MmsCommandInfoV1));
    auto oldRevision = snapshot.revision;
    auto foreign = registry.Api(other); CHECK(foreign.unregisterItem(foreign.context, h) == MMS_E_ARGUMENT);
    c.stopping = true;
    CHECK(foreign.invoke(foreign.context, command.info.id, {}, args) == MMS_E_NOT_READY);
    MmsCommandStateV1 stoppedState{}; stoppedState.h = Header<MmsCommandStateV1>(); MmsHandle stoppedLease = 0;
    CHECK(foreign.getCommandState(foreign.context, command.info.id, {}, &stoppedState, &stoppedLease) == MMS_E_NOT_READY);
    CHECK(counter.invokes == 2); c.stopping = false;
    MmsResult threadResult = MMS_OK; std::thread t([&] { MmsHandle x; threadResult = api.registerCommand(api.context, &command, &x); }); t.join(); CHECK(threadResult == MMS_E_THREAD);
    MmsUiElementV1 e{}; e.h = Header<MmsUiElementV1>(); e.id = S("example.test.button"); e.ownerPlugin = command.info.ownerPlugin; e.label = S("测试按钮"); e.parentId = S("mms.toolbar.main.extensions"); e.commandId = command.info.id; e.kind = MMS_UI_BUTTON; e.capabilities = MMS_UI_MOVE | MMS_UI_HIDE | MMS_UI_RESIZE | MMS_UI_SHORTCUT;
    MmsHandle button; CHECK(api.registerElement(api.context, &e, &button) == MMS_OK);
    MmsHandle tx; CHECK(api.beginLayout(api.context, oldRevision, &tx) == MMS_E_STALE);
    CHECK(api.unregisterItem(api.context, h) == MMS_E_BUSY);
    CHECK(String(infos[0].label) == "命令"); api.releaseSnapshot(api.context, snapshot.handle);
    for (int i = 0; i < 8; ++i) {
        CHECK(api.acquireSnapshot(api.context, 0, &snapshot) == MMS_OK); auto rev = snapshot.revision;
        auto generation = registry.elements.at("example.test.button").desc.generation;
        api.releaseSnapshot(api.context, snapshot.handle);
        CHECK(api.beginLayout(api.context, rev, &tx) == MMS_OK);
        MmsLayoutOpV1 op{}; op.h = Header<MmsLayoutOpV1>(); op.operation = MMS_LAYOUT_RESTORE; op.elementId = e.id; op.expectedGeneration = generation;
        CHECK(api.applyLayout(api.context, tx, &op) == MMS_OK); CHECK(api.commitLayout(api.context, tx) == MMS_OK);
        CHECK(String(registry.elements.at("example.test.button").desc.label) == "测试按钮");
    }
    CHECK(api.beginLayout(api.context, registry.Revision(), &tx) == MMS_OK);
    MmsLayoutOpV1 op{}; op.h = Header<MmsLayoutOpV1>(); op.operation = MMS_LAYOUT_VISIBLE; op.elementId = e.id; op.expectedGeneration = registry.elements.at("example.test.button").desc.generation; op.value.visible = 0;
    CHECK(api.applyLayout(api.context, tx, &op) == MMS_OK); op.expectedGeneration++; CHECK(api.applyLayout(api.context, tx, &op) == MMS_OK);
    CHECK(api.commitLayout(api.context, tx) == MMS_E_STALE); CHECK(registry.elements.at("example.test.button").visible);
    CHECK(api.unregisterItem(api.context, button) == MMS_OK); CHECK(api.unregisterItem(api.context, h) == MMS_OK);
    CHECK(!registry.HasLeases(c));
}
static void UiPreferenceChecks() {
    struct Fixture {
        unsigned changed = 0, callbacks = 0;
        UiRegistry registry{[this] { ++changed; }};
        UiClient plugin{&registry, "example.preferences"}, editor{&registry, "example.editor"};
        MmsUiApiV1 api = registry.Api(plugin), edit = registry.Api(editor);
        MmsUiLayoutApiV1 layout = registry.LayoutApi(plugin);
        void Register(bool reverse = false, float preferredWidth = 100, float preferredHeight = 24) {
            MmsCommandDescV1 command{}; command.info.h = Header<MmsCommandDescV1>();
            command.info.id = S("example.preferences.run"); command.info.ownerPlugin = S("example.preferences"); command.context = this;
            command.getState = [](void* p, MmsObjectRef, MmsCommandStateV1* state) -> MmsResult {
                ++static_cast<Fixture*>(p)->callbacks; *state = {Header<MmsCommandStateV1>(), 1, 0, 1, 0, {}}; return MMS_OK;
            };
            command.invoke = [](void* p, MmsObjectRef, MmsSpan, MmsHandle) -> MmsResult { ++static_cast<Fixture*>(p)->callbacks; return MMS_OK; };
            MmsHandle handle; CHECK(api.registerCommand(api.context, &command, &handle) == MMS_OK);
            const char* ids[] = {"example.preferences.a", "example.preferences.b"};
            if (reverse) std::swap(ids[0], ids[1]);
            for (const char* id : ids) {
                MmsUiElementV1 e{}; e.h = Header<MmsUiElementV1>(); e.id = S(id); e.ownerPlugin = command.info.ownerPlugin;
                e.label = S("原生标签"); e.kind = MMS_UI_BUTTON; e.parentId = S("mms.toolbar.main.extensions"); e.commandId = command.info.id;
                e.capabilities = MMS_UI_MOVE | MMS_UI_HIDE | MMS_UI_RESIZE | MMS_UI_SHORTCUT | MMS_UI_LAYOUT;
                CHECK(api.registerElement(api.context, &e, &handle) == MMS_OK);
                MmsUiLayoutHintsV1 hints{}; hints.h = Header<MmsUiLayoutHintsV1>();
                hints.minimum[0] = 60; hints.preferred[0] = preferredWidth; hints.preferred[1] = preferredHeight;
                hints.order = std::string(id).back() == 'a' ? 10 : 20;
                CHECK(layout.setDefaultHints(layout.context, e.id, &hints) == MMS_OK);
            }
        }
        void Commit(MmsLayoutOpV1 op) {
            op.h = Header<MmsLayoutOpV1>();
            if (op.operation != MMS_LAYOUT_CREATE_GROUP && op.elementId.bytes)
                op.expectedGeneration = registry.elements.at(String(op.elementId)).desc.generation;
            MmsHandle tx; CHECK(edit.beginLayout(edit.context, registry.Revision(), &tx) == MMS_OK);
            CHECK(edit.applyLayout(edit.context, tx, &op) == MMS_OK); CHECK(edit.commitLayout(edit.context, tx) == MMS_OK);
        }
    } source;
    const auto empty = source.registry.ExportPreferences(); const auto pristine = source.registry.PreferencesRevision();
    source.Register(); CHECK(source.registry.ExportPreferences() == empty && source.registry.PreferencesRevision() == pristine);
    MmsLayoutOpV1 op{}; op.operation = MMS_LAYOUT_CREATE_GROUP; op.elementId = S("example.editor.group");
    op.parentId = S("mms.toolbar.main.extensions"); op.value.groupLabel = S("分组\"\\\n"); source.Commit(op);
    op = {}; op.operation = MMS_LAYOUT_MOVE; op.elementId = S("example.preferences.a"); op.parentId = S("example.editor.group"); source.Commit(op);
    op = {}; op.operation = MMS_LAYOUT_MOVE; op.elementId = S("example.preferences.b"); op.parentId = S("mms.toolbar.main.extensions"); op.beforeId = S("example.editor.group"); source.Commit(op);
    op = {}; op.operation = MMS_LAYOUT_VISIBLE; op.elementId = S("example.preferences.b"); op.value.visible = 0; source.Commit(op);
    op = {}; op.operation = MMS_LAYOUT_SIZE; op.elementId = S("example.preferences.a"); op.value.size = {210, 32}; source.Commit(op);
    op = {}; op.operation = MMS_LAYOUT_SHORTCUT; op.elementId = S("example.preferences.a"); op.value.shortcut = {75, 2, 1, 0}; source.Commit(op);
    MmsHandle tx; CHECK(source.edit.beginLayout(source.edit.context, source.registry.Revision(), &tx) == MMS_OK);
    auto hints = source.registry.elements.at("example.preferences.a").effectiveHints; hints.preferred[0] = 180;
    auto editorLayout = source.registry.LayoutApi(source.editor);
    CHECK(editorLayout.stageHints(editorLayout.context, tx, S("example.preferences.a"), source.registry.elements.at("example.preferences.a").desc.generation, &hints) == MMS_OK);
    CHECK(source.edit.commitLayout(source.edit.context, tx) == MMS_OK);
    const auto saved = source.registry.ExportPreferences(); CHECK(saved != empty && source.registry.PreferencesRevision() > pristine);
    const auto json = Json::Parse(saved); CHECK(json.At("schemaVersion").UInt() == 1 && json.At("elements").Items().size() == 3);
    for (const char* forbidden : {"handle", "generation", "HWND", "callback", "context", "defaultHints"}) CHECK(saved.find(forbidden) == std::string::npos);
    CHECK(saved.find("preferredHeight") == std::string::npos && saved.find("minimumWidth") == std::string::npos);
    Fixture loaded; loaded.Register(true, 120, 30);
    MmsUiSnapshotV1 snapshot{}; snapshot.h = Header<MmsUiSnapshotV1>(); CHECK(loaded.api.acquireSnapshot(loaded.api.context, 0, &snapshot) == MMS_OK);
    const auto beforeChanged = loaded.changed;
    CHECK(loaded.registry.ImportPreferences(saved) == MMS_OK && loaded.changed == beforeChanged + 1 && loaded.callbacks == 0);
    CHECK(loaded.registry.ExportPreferences() == saved);
    const auto& a = loaded.registry.elements.at("example.preferences.a");
    CHECK(String(a.desc.parentId) == "example.editor.group" && a.width == 210 && a.height == 32 && a.key == 75 && a.modifiers == 2 && a.scope == 1);
    CHECK(a.effectiveHints.preferred[0] == 180 && a.effectiveHints.preferred[1] == 30 && a.effectiveHints.minimum[0] == 60);
    CHECK(!loaded.registry.elements.at("example.preferences.b").visible);
    CHECK(loaded.registry.elements.at("example.preferences.b").effectiveHints.order < loaded.registry.elements.at("example.editor.group").effectiveHints.order);
    CHECK(String(loaded.registry.elements.at("example.editor.group").desc.label) == "分组\"\\\n");
    const auto* previousElements = static_cast<const MmsUiElementV1*>(snapshot.elements.data);
    bool oldParentPreserved = false;
    for (size_t i = 0; i < snapshot.elements.count; ++i) if (String(previousElements[i].id) == "example.preferences.a") oldParentPreserved = String(previousElements[i].parentId) == "mms.toolbar.main.extensions";
    CHECK(oldParentPreserved); loaded.api.releaseSnapshot(loaded.api.context, snapshot.handle);
    const auto savedRevision = loaded.registry.PreferencesRevision(); hints = a.defaultHints; hints.preferred[0] = 140; hints.preferred[1] = 42;
    CHECK(loaded.layout.setDefaultHints(loaded.layout.context, S("example.preferences.a"), &hints) == MMS_OK);
    CHECK(loaded.registry.elements.at("example.preferences.a").effectiveHints.preferred[0] == 180 && loaded.registry.elements.at("example.preferences.a").effectiveHints.preferred[1] == 42);
    CHECK(loaded.registry.ExportPreferences() == saved && loaded.registry.PreferencesRevision() == savedRevision);
    const auto reject = [&](const std::string& value, MmsResult expected) {
        const auto revision = loaded.registry.Revision(), preferenceRevision = loaded.registry.PreferencesRevision(); const auto notifications = loaded.changed;
        CHECK(loaded.registry.ImportPreferences(value) == expected);
        CHECK(loaded.registry.Revision() == revision && loaded.registry.PreferencesRevision() == preferenceRevision && loaded.changed == notifications && loaded.callbacks == 0);
        CHECK(loaded.registry.ExportPreferences() == saved && String(loaded.registry.elements.at("example.preferences.a").desc.parentId) == "example.editor.group");
    };
    reject("{broken", MMS_E_ARGUMENT);
    reject("{\"schemaVersion\":2,\"elements\":[]}", MMS_E_VERSION);
    reject("{\"schemaVersion\":1,\"elements\":[],\"generation\":123}", MMS_E_ARGUMENT);
    reject("{\"schemaVersion\":1,\"elements\":[{\"id\":\"example.preferences.a\",\"owner\":\"forged\",\"kind\":4,\"visible\":false}]}", MMS_E_ARGUMENT);
    const auto record = [](const std::string& fields) { return "{\"schemaVersion\":1,\"elements\":[{\"id\":\"example.preferences.a\",\"owner\":\"example.preferences\",\"kind\":" + std::to_string(MMS_UI_BUTTON) + fields + "}]}"; };
    reject(record(",\"size\":[32769,20]"), MMS_E_ARGUMENT);
    reject(record(",\"size\":[1e999,20]"), MMS_E_ARGUMENT);
    reject(record(",\"shortcut\":[256,0,0]"), MMS_E_ARGUMENT);
    reject(record(",\"hints\":{\"overflow\":3}"), MMS_E_UNSUPPORTED);
    reject(record(",\"parent\":\"mms.menu.plugins\""), MMS_E_ARGUMENT);
    reject("{\"schemaVersion\":1,\"elements\":[{\"id\":\"missing.a\",\"owner\":\"missing\",\"kind\":7,\"parent\":\"missing.b\"},{\"id\":\"missing.b\",\"owner\":\"missing\",\"kind\":7,\"parent\":\"missing.a\"}]}", MMS_E_ARGUMENT);
    MmsResult threadResult = MMS_OK; std::thread wrongThread([&] { threadResult = loaded.registry.ImportPreferences(saved); }); wrongThread.join(); CHECK(threadResult == MMS_E_THREAD);
    Fixture late; CHECK(late.registry.ImportPreferences(saved) == MMS_OK && late.registry.ExportPreferences() == saved);
    CHECK(late.registry.elements.count("example.preferences.a") == 0 && late.registry.elements.count("example.editor.group") == 1);
    late.Register(true, 130, 36); CHECK(String(late.registry.elements.at("example.preferences.a").desc.parentId) == "example.editor.group");
    CHECK(late.registry.elements.at("example.preferences.a").effectiveHints.preferred[0] == 180 && late.registry.elements.at("example.preferences.a").effectiveHints.preferred[1] == 36);
    const auto retiredRevision = late.registry.PreferencesRevision(); late.registry.Retire(late.plugin);
    CHECK(late.registry.ExportPreferences() == saved && late.registry.PreferencesRevision() == retiredRevision && late.registry.elements.count("example.preferences.a") == 0);
    late.Register(false, 150, 40); CHECK(!late.registry.elements.at("example.preferences.b").visible && late.registry.ExportPreferences() == saved);
    Fixture missingParent; missingParent.Register();
    const auto waiting = record(",\"parent\":\"example.missing.container\",\"visible\":false");
    CHECK(missingParent.registry.ImportPreferences(waiting) == MMS_OK);
    CHECK(String(missingParent.registry.elements.at("example.preferences.a").desc.parentId) == "mms.toolbar.main.extensions");
    op = {}; op.operation = MMS_LAYOUT_VISIBLE; op.elementId = S("example.preferences.b"); op.value.visible = 0; missingParent.Commit(op);
    CHECK(missingParent.registry.ExportPreferences().find("example.missing.container") != std::string::npos);
    UiClient missing{&missingParent.registry, "example.missing"}; auto missingApi = missingParent.registry.Api(missing);
    MmsUiElementV1 container{}; container.h = Header<MmsUiElementV1>(); container.id = S("example.missing.container");
    container.ownerPlugin = S("example.missing"); container.parentId = S("mms.toolbar.main.extensions"); container.kind = MMS_UI_GROUP;
    container.capabilities = MMS_UI_CONTAINER | MMS_UI_MOVE | MMS_UI_LAYOUT;
    MmsHandle containerHandle; CHECK(missingApi.registerElement(missingApi.context, &container, &containerHandle) == MMS_OK);
    CHECK(String(missingParent.registry.elements.at("example.preferences.a").desc.parentId) == "example.missing.container" && !missingParent.registry.elements.at("example.preferences.a").visible);
    op = {}; op.operation = MMS_LAYOUT_RESTORE; op.elementId = S("example.preferences.a"); loaded.Commit(op);
    const auto& reset = loaded.registry.elements.at("example.preferences.a");
    CHECK(String(reset.desc.parentId) == "mms.toolbar.main.extensions" && reset.width == 0 && reset.key == 0 && reset.effectiveHints.preferred[0] == 140 && reset.effectiveHints.preferred[1] == 42);
    CHECK(loaded.registry.ExportPreferences().find("example.preferences.a") == std::string::npos);
    op = {}; op.operation = MMS_LAYOUT_RESTORE; loaded.Commit(op); CHECK(loaded.registry.ExportPreferences() == empty && loaded.registry.elements.count("example.editor.group") == 0);
    Fixture dormant; CHECK(dormant.registry.ImportPreferences(saved) == MMS_OK); dormant.Commit(op);
    CHECK(dormant.registry.ExportPreferences() == empty && dormant.registry.elements.size() == 3);
    Fixture constrained; constrained.Register();
    op = {}; op.operation = MMS_LAYOUT_SIZE; op.elementId = S("example.preferences.a"); op.value.size = {100, 40}; constrained.Commit(op);
    const auto sizeSaved = constrained.registry.ExportPreferences();
    const auto sizeRevision = constrained.registry.Revision(), sizePreferenceRevision = constrained.registry.PreferencesRevision();
    const auto sizeNotifications = constrained.changed;
    auto tooLarge = constrained.registry.elements.at("example.preferences.a").defaultHints; tooLarge.minimum[0] = 200;
    CHECK(constrained.layout.setDefaultHints(constrained.layout.context, S("example.preferences.a"), &tooLarge) == MMS_E_ARGUMENT);
    CHECK(constrained.registry.Revision() == sizeRevision && constrained.registry.PreferencesRevision() == sizePreferenceRevision && constrained.changed == sizeNotifications);
    CHECK(constrained.registry.elements.at("example.preferences.a").defaultHints.minimum[0] == 60 && constrained.registry.elements.at("example.preferences.a").width == 100);
    CHECK(constrained.registry.ExportPreferences() == sizeSaved);
    auto constrainedEditor = constrained.registry.LayoutApi(constrained.editor);
    CHECK(constrained.edit.beginLayout(constrained.edit.context, constrained.registry.Revision(), &tx) == MMS_OK);
    CHECK(constrainedEditor.stageHints(constrainedEditor.context, tx, S("example.preferences.a"), constrained.registry.elements.at("example.preferences.a").desc.generation, &tooLarge) == MMS_OK);
    CHECK(constrained.edit.commitLayout(constrained.edit.context, tx) == MMS_E_ARGUMENT);
    CHECK(constrained.registry.Revision() == sizeRevision && constrained.registry.PreferencesRevision() == sizePreferenceRevision && constrained.changed == sizeNotifications);
    CHECK(constrained.registry.ExportPreferences() == sizeSaved && constrained.registry.elements.at("example.preferences.a").effectiveHints.minimum[0] == 60);
    auto valid = tooLarge; valid.minimum[0] = 80;
    CHECK(constrained.edit.beginLayout(constrained.edit.context, constrained.registry.Revision(), &tx) == MMS_OK);
    CHECK(constrainedEditor.stageHints(constrainedEditor.context, tx, S("example.preferences.a"), constrained.registry.elements.at("example.preferences.a").desc.generation, &valid) == MMS_OK);
    CHECK(constrained.edit.commitLayout(constrained.edit.context, tx) == MMS_OK);
    Fixture sizeReloaded; sizeReloaded.Register(); CHECK(sizeReloaded.registry.ImportPreferences(constrained.registry.ExportPreferences()) == MMS_OK);
    CHECK(sizeReloaded.registry.elements.at("example.preferences.a").width == 100 && sizeReloaded.registry.elements.at("example.preferences.a").effectiveHints.minimum[0] == 80);
}
static void LayoutChecks() {
    std::vector<native_ui::Item> items{{{60,24},{80,24}}, {{60,24},{100,24}}, {{60,24},{80,24}}};
    auto a = native_ui::Wrap(items, 300); CHECK(a.height == 32 && a.rects[1].y == a.rects[0].y);
    auto b = native_ui::Wrap(items, 160); CHECK(b.height == 88 && b.rects[1].y > b.rects[0].y);
    auto c = native_ui::Wrap(items, 300); CHECK(c.rects[2].x == a.rects[2].x && c.height == a.height);
    items[1].visible = false; CHECK(native_ui::Wrap(items, 180).height == 32);
    CHECK(Throws([&] { native_ui::Wrap(items, std::numeric_limits<float>::quiet_NaN()); }));
}
static void LoaderChecks(const fs::path& root, const fs::path& sample) {
    Write(root / "hash.txt", "abc"); CHECK(Sha256(root / "hash.txt") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Throws([&] { Sha256(root / "missing"); })); CHECK(Throws([&] { Sha256(root); }));
    CHECK(Throws([&] { InspectPe(root / "hash.txt"); }));
    auto info = InspectPe(sample / "hello_menu.dll"); CHECK(info.subsystemMajor == 6 && info.subsystemMinor == 1);
    for (auto mode : {false, true}) {
        fs::path plugins = root / (mode ? L"兼容搜索" : L"标准搜索") / L"PLUGINS";
        fs::path package = plugins / L"界面" / L"你好"; fs::create_directories(package);
        fs::copy_file(sample / "hello_menu.dll", package / "hello_menu.dll", fs::copy_options::overwrite_existing);
        fs::copy_file(sample / "plugin.json", package / "plugin.json", fs::copy_options::overwrite_existing);
        std::vector<std::string> logs; Manager manager(root / L"userdata", [&](const auto& s) { logs.push_back(s); }, {}, mode);
        manager.Load(plugins); auto statuses = manager.Status();
        for (const auto& s : statuses) std::cout << s.id << " " << s.stage << " " << s.error << "\n";
        CHECK(statuses.size() == 1 && statuses[0].stage == "started");
        UiClient client{&manager.Ui(), "mms"}; auto api = manager.Ui().Api(client);
        CHECK(api.invoke(api.context, S("org.example.hello.say"), {}, {}) == MMS_OK);
        CHECK(std::any_of(logs.begin(), logs.end(), [](const auto& s) { return s.find("Hello from") != std::string::npos; }));
        HWND window = CreateWindowExW(0, L"STATIC", L"ABI hidden host", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr); CHECK(window != nullptr);
        SetMenu(window, CreateMenu());
        {
            NativeUi ui(manager.Ui(), window, {}); ui.Refresh(); int height = ui.Layout(250, 540, 160, true);
            CHECK(height > 0); CHECK(GetMenuItemCount(GetMenu(window)) == 1);
            HMENU menu = GetSubMenu(GetMenu(window), 0); CHECK(GetMenuItemCount(menu) == 1); auto command = GetMenuItemID(menu, 0);
            CHECK(ui.Command(command)); HWND button = GetWindow(ui.Toolbar(), GW_CHILD); CHECK(button != nullptr);
            SendMessageW(ui.Toolbar(), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(button), BN_CLICKED), reinterpret_cast<LPARAM>(button));
            CHECK(ui.Layout(250, 540, 160, false) == 0);
        }
        HMENU menu = GetMenu(window); SetMenu(window, nullptr); DestroyMenu(menu); DestroyWindow(window);
        CHECK(manager.Stop()); CHECK(manager.Status()[0].stage == "stopped"); CHECK(manager.Ui().commands.empty());
        CHECK(GetModuleHandleW(L"hello_menu.dll") == nullptr);
    }
}
static void LifecycleChecks(const fs::path& root, const fs::path& fixture) {
    for (const std::string mode : {"failcreate", "failstart", "busy", "queue", "scenelease"}) {
        const auto package = root / Wide(mode) / "PLUGINS" / "test"; fs::create_directories(package);
        fs::copy_file(fixture, package / "lifecycle.dll");
        const auto id = "org.example." + mode; Write(package / "plugin.json", ManifestText(id, "lifecycle.dll"));
        std::vector<std::string> logs;
        auto count = [&](const char* marker) { return std::count_if(logs.begin(), logs.end(), [&](const auto& s) { return s.find(marker) != std::string::npos; }); };
        auto scene = std::make_shared<SceneStore>([](const auto&, auto& out) {auto data=std::make_shared<SceneData>();data->supported=MMS_CHANNEL_BONES;out=data;return MMS_OK;});
        Manager manager(root / "userdata", [&](const auto& s) { logs.push_back(s); }, std::function<void()>{}, false, scene);
        manager.Load(package.parent_path()); CHECK(count("NEGOTIATION_PASS") == 1);
        if (mode == "failcreate") {
            CHECK(count("CREATE_TASK_ACCEPTED") == 1); CHECK(count("TASK_CANCEL") == 1); CHECK(count("TASK_RUN") == 0);
            CHECK(manager.Status()[0].stage == "failed"); CHECK(manager.Ui().commands.empty());
        } else if (mode == "failstart") {
            CHECK(count("STOP_OK") == 1); CHECK(count("DESTROY") == 1); CHECK(manager.Ui().commands.empty());
        } else if (mode == "queue") {
            manager.Drain(); CHECK(count("TASK_RUN") == 1); CHECK(count("TASK_CANCEL") == 1); CHECK(count("RUNNING_CANCEL_BUSY") == 1);
            manager.Drain(); CHECK(count("TASK_RUN") == 1);
        } else if(mode=="scenelease") {
            CHECK(count("SCENE_READ_PASS")==1); CHECK(!manager.Stop());
            CHECK(GetModuleHandleW(L"lifecycle.dll")!=nullptr && count("DESTROY")==0);
            CHECK(manager.Stop()); CHECK(count("STOP_SCENE_READ_PASS")==1 && count("DESTROY")==1);
        } else {
            CHECK(!manager.Stop()); CHECK(GetModuleHandleW(L"lifecycle.dll") != nullptr);
            UiClient visitor{&manager.Ui(), "example.visitor"}; auto api = manager.Ui().Api(visitor); auto command = id + ".command";
            CHECK(api.invoke(api.context, Text(command), {}, {}) == MMS_E_NOT_READY); CHECK(count("COMMAND_INVOKED") == 0);
            CHECK(!manager.Stop()); CHECK(manager.Stop()); CHECK(count("STOP_BUSY") == 2); CHECK(count("DESTROY") == 1);
        }
        CHECK(manager.Stop()); CHECK(GetModuleHandleW(L"lifecycle.dll") == nullptr);
        std::cout << "lifecycle " << mode << " PASS\n";
    }
}
int wmain(int argc, wchar_t** argv) {
    const fs::path root = fs::temp_directory_path() / (L"mms-abi-contract-" + std::to_wstring(GetCurrentProcessId()));
    try {
        if (argc == 2 && std::wstring(argv[1]) == L"--state-content-only") {
            StateContentChecks();
            std::cout << "PASS STATE content checks=" << checks << "\n"; return 0;
        }
        if ((argc == 2 || argc == 3) && std::wstring(argv[1]) == L"--layout-refresh-only") {
            CHECK(!fs::exists(root)); fs::create_directories(root);
            LayoutRefreshChecks();
            if (argc == 3) ManagerLayoutRefreshChecks(root, fs::absolute(argv[2]));
            std::cout << "PASS LAYOUT refresh checks=" << checks << "\n"; fs::remove_all(root); return 0;
        }
        if ((argc == 2 || argc == 3) && std::wstring(argv[1]) == L"--data-only") {
            CHECK(!fs::exists(root)); fs::create_directories(root);
            DataRefreshChecks(); CppDataRefreshChecks();
            if (argc == 3) {
                ManagerDataRefreshChecks(root, fs::absolute(argv[2]));
                ManagerDataRefreshConcurrencyChecks(root, fs::absolute(argv[2]));
            }
            std::cout << "PASS DATA refresh checks=" << checks << "\n"; fs::remove_all(root); return 0;
        }
        if ((argc == 2 || argc == 3) && std::wstring(argv[1]) == L"--refresh-only") {
            CHECK(!fs::exists(root)); fs::create_directories(root);
            RefreshChecks();
            StateRefreshChecks();
            StateContentChecks();
            VisibilityRefreshChecks();
            CppRefreshChecks();
            LayoutRefreshChecks();
            DataRefreshChecks(); CppDataRefreshChecks();
            if (argc == 3) ManagerRefreshChecks(root, fs::absolute(argv[2]));
            std::cout << "PASS refresh checks=" << checks << "\n"; fs::remove_all(root); return 0;
        }
        CHECK(argc == 4); CHECK(!fs::exists(root)); fs::create_directories(root);
        JsonChecks(); DiscoveryChecks(root); UiChecks(); UiPreferenceChecks(); UiPreferencesFileChecks(root); LayoutChecks(); TreeLayoutChecks(); LayoutServiceChecks(); UpdateChecks(); Win32HostChecks(); SurfaceChecks(); CppSdkChecks(); LoaderChecks(root, fs::absolute(argv[1]));
        LifecycleChecks(root, fs::absolute(argv[2])); TaskDispatchChecks(root, fs::absolute(argv[2]), fs::absolute(argv[3]));
        SceneChecks(); StorageChecks(root); EditChecks(); SharedHistoryChecks(); StateContentChecks();
        ManagerEditChecks(root, fs::absolute(argv[3]));
        std::cout << "PASS checks=" << checks << "\n"; fs::remove_all(root); return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL after " << checks << ": " << e.what() << "\nfixtures=" << Utf8(root.native()) << "\n"; return 1; }
}
