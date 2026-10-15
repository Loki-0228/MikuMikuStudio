#include "manager.hpp"
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
static void Write(const fs::path& path, const std::string& bytes) { fs::create_directories(path.parent_path()); std::ofstream f(path, std::ios::binary); f << bytes; if (!f) throw std::runtime_error("fixture write failed"); }
static std::string ManifestText(std::string id = "org.example.hello", std::string entry = "hello_menu.dll") {
    return "{\"schemaVersion\":1,\"id\":\"" + id + "\",\"name\":\"测试\",\"version\":\"1.0.0\",\"kind\":\"native\",\"abi\":{\"major\":1,\"minMinor\":0},\"entry\":\"" + entry + "\",\"architecture\":\"x64\",\"minWindows\":\"6.1.7601\",\"capabilities\":[\"ui.command\"],\"requiresServices\":[{\"id\":\"mms.ui\",\"major\":1,\"minMinor\":0}],\"dependencies\":[],\"nativeDependencies\":[],\"reloadPolicy\":\"restart\"}";
}
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
    for (const std::string mode : {"failcreate", "failstart", "busy", "queue"}) {
        const auto package = root / Wide(mode) / "PLUGINS" / "test"; fs::create_directories(package);
        fs::copy_file(fixture, package / "lifecycle.dll");
        const auto id = "org.example." + mode; Write(package / "plugin.json", ManifestText(id, "lifecycle.dll"));
        std::vector<std::string> logs;
        auto count = [&](const char* marker) { return std::count_if(logs.begin(), logs.end(), [&](const auto& s) { return s.find(marker) != std::string::npos; }); };
        Manager manager(root / "userdata", [&](const auto& s) { logs.push_back(s); });
        manager.Load(package.parent_path()); CHECK(count("NEGOTIATION_PASS") == 1);
        if (mode == "failcreate") {
            CHECK(count("CREATE_TASK_ACCEPTED") == 1); CHECK(count("TASK_CANCEL") == 1); CHECK(count("TASK_RUN") == 0);
            CHECK(manager.Status()[0].stage == "failed"); CHECK(manager.Ui().commands.empty());
        } else if (mode == "failstart") {
            CHECK(count("STOP_OK") == 1); CHECK(count("DESTROY") == 1); CHECK(manager.Ui().commands.empty());
        } else if (mode == "queue") {
            manager.Drain(); CHECK(count("TASK_RUN") == 1); CHECK(count("TASK_CANCEL") == 1); CHECK(count("RUNNING_CANCEL_BUSY") == 1);
            manager.Drain(); CHECK(count("TASK_RUN") == 1);
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
        CHECK(argc == 3); CHECK(!fs::exists(root)); fs::create_directories(root);
        JsonChecks(); DiscoveryChecks(root); UiChecks(); LayoutChecks(); TreeLayoutChecks(); LayoutServiceChecks(); UpdateChecks(); Win32HostChecks(); SurfaceChecks(); LoaderChecks(root, fs::absolute(argv[1]));
        LifecycleChecks(root, fs::absolute(argv[2]));
        std::cout << "PASS checks=" << checks << "\n"; fs::remove_all(root); return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL after " << checks << ": " << e.what() << "\nfixtures=" << Utf8(root.native()) << "\n"; return 1; }
}
