#include "manager.hpp"
#include "evaluation_store.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace mikumikustudio::plugins;
namespace fs = std::filesystem;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #x); } while (false)
static size_t Count(const std::vector<std::string>& logs, const char* marker) {
    return size_t(std::count_if(logs.begin(), logs.end(), [&](const auto& line) { return line.find(marker) != std::string::npos; }));
}
struct Gate {
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE proceed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Gate() { if (entered) CloseHandle(entered); if (proceed) CloseHandle(proceed); }
};
static std::shared_ptr<EvaluationStore> Store(std::shared_ptr<Gate> gate = {}) {
    return std::make_shared<EvaluationStore>([gate](const MmsEvaluationSourceQueryV1& query, uint64_t budget,
        std::shared_ptr<const EvaluationData>& out) -> MmsResult {
        if (query.object.id.lo != 11 || query.object.id.hi != 22 || query.object.generation != 1) return MMS_E_STALE;
        if (budget < 1024) return MMS_E_MEMORY;
        auto data = std::make_shared<EvaluationData>();
        data->object = query.object; data->hostRevision = 77; data->retainedBytes = 1024;
        data->bones.push_back({Header<MmsEvaluationBoneV1>(), {33, 44}, MMS_EVALUATION_BONE_LOCAL, 0});
        data->sample = [gate](uint32_t index, double frame, MmsBoneChannelsV1& sample) -> MmsResult {
            if (index != 0) return MMS_E_NOT_FOUND;
            if (gate) {
                SetEvent(gate->entered);
                if (WaitForSingleObject(gate->proceed, 15000) != WAIT_OBJECT_0) return MMS_E_FAILED;
            }
            sample = {}; sample.h = Header<MmsBoneChannelsV1>();
            sample.position[0] = float(frame); sample.position[1] = 2; sample.position[2] = 3;
            sample.rotation[3] = 1; sample.scale[0] = sample.scale[4] = sample.scale[8] = 1;
            sample.flags = MMS_EVALUATION_PHYSICS_DISABLED; return MMS_OK;
        };
        out = std::move(data); return MMS_OK;
    });
}
static fs::path Package(const fs::path& plugins, const fs::path& fixture, const std::string& mode,
                        const std::string& dependency = {}) {
    const auto package = plugins / mode; CHECK(fs::create_directories(package));
    const auto filename = "evaluation-" + mode + ".dll";
    const auto dll = package / filename; CHECK(fs::copy_file(fixture, dll));
    std::ofstream out(package / "plugin.json", std::ios::binary);
    out << "{\"schemaVersion\":1,\"id\":\"org.example.evaluation-" << mode
        << "\",\"name\":\"Evaluation lifecycle\",\"version\":\"1.0.0\","
        "\"kind\":\"native\",\"abi\":{\"major\":1,\"minMinor\":0},\"entry\":\"" << filename
        << "\",\"architecture\":\"x64\",\"minWindows\":\"6.1.7601\",\"capabilities\":[\"scene.evaluate\"],"
        "\"requiresServices\":[{\"id\":\"mms.scene.evaluate\",\"major\":1,\"minMinor\":0}],\"dependencies\":[";
    if (!dependency.empty()) out << "{\"id\":\"" << dependency << "\",\"minVersion\":\"1.0.0\",\"maxVersionExclusive\":\"2.0.0\"}";
    out << "],\"nativeDependencies\":[],\"reloadPolicy\":\"restart\"}"; out.close(); CHECK(bool(out));
    return dll;
}
struct Probe {
    MmsEvaluationApiV1 api{};
    MmsHandle source = 0;
    MmsResult(MMS_CALL* query)(MmsEvaluationApiV1*) = nullptr;
    MmsResult(MMS_CALL* release)() = nullptr;
    uint32_t(MMS_CALL* stops)() = nullptr;
    explicit Probe(const fs::path& dll) {
        const auto module = GetModuleHandleW(dll.c_str()); CHECK(module != nullptr);
        using Get = MmsResult(MMS_CALL*)(MmsEvaluationApiV1*, MmsHandle*);
        const auto get = reinterpret_cast<Get>(GetProcAddress(module, "MmsEvaluationTestGet"));
        query = reinterpret_cast<decltype(query)>(GetProcAddress(module, "MmsEvaluationTestQuery"));
        release = reinterpret_cast<decltype(release)>(GetProcAddress(module, "MmsEvaluationTestRelease"));
        stops = reinterpret_cast<decltype(stops)>(GetProcAddress(module, "MmsEvaluationTestStopCount"));
        CHECK(get && query && release && stops); CHECK(get(&api, &source) == MMS_OK && source);
    }
};
static bool ValidSample(const MmsBoneChannelsV1& sample) {
    return sample.h.size == sizeof(sample) && sample.h.major == 1 && sample.h.minor == 0 &&
        sample.position[0] == 15 && sample.position[1] == 2 && sample.position[2] == 3 &&
        sample.rotation[0] == 0 && sample.rotation[1] == 0 && sample.rotation[2] == 0 && sample.rotation[3] == 1 &&
        sample.scale[0] == 1 && sample.scale[4] == 1 && sample.scale[8] == 1 &&
        sample.flags == MMS_EVALUATION_PHYSICS_DISABLED && sample.reserved == 0;
}
static void LiveReads(const Probe& probe) {
    auto api = probe.api;
    MmsEvaluationApiV1 repeated{}; CHECK(probe.query(&repeated) == MMS_OK);
    CHECK(repeated.context == api.context && repeated.sampleBone == api.sampleBone);
    const auto read = [&]() {
        MmsEvaluationCapsV1 caps{}; MmsEvaluationSourceInfoV1 info{}; MmsEvaluationBoneV1 bone{}; MmsBoneChannelsV1 sample{};
        MmsHandle retained = 0;
        if (api.getCapabilities(api.context, &caps) || !(caps.flags & MMS_EVALUATION_CAP_BONE_CHANNELS) ||
            api.describeSource(api.context, probe.source, &info) || info.object.id.lo != 11 || info.object.id.hi != 22 ||
            info.object.generation != 1 || !info.captureId || info.hostRevision != 77 || info.boneCount != 1 ||
            api.boneAt(api.context, probe.source, 0, &bone) || bone.id.lo != 33 || bone.id.hi != 44 || bone.flags != MMS_EVALUATION_BONE_LOCAL ||
            api.sampleBone(api.context, probe.source, bone.id, {1, 2}, &sample) || !ValidSample(sample) ||
            api.retain(api.context, probe.source, &retained) || !retained || retained == probe.source) return false;
        return api.release(api.context, retained) == MMS_OK;
    };
    CHECK(read());
    bool workerRead = false; MmsResult workerCapture = MMS_OK; MmsHandle workerOut = 123;
    std::thread worker([&] {
        workerRead = read();
        const MmsEvaluationSourceQueryV1 query{Header<MmsEvaluationSourceQueryV1>(), {{11, 22}, 1}, 0, 0, 0};
        workerCapture = api.createSource(api.context, &query, &workerOut);
    }); worker.join();
    CHECK(workerRead); CHECK(workerCapture == MMS_E_THREAD && workerOut == 123);
}
static void Retired(const Probe& probe) {
    auto api = probe.api;
    const MmsEvaluationSourceQueryV1 query{Header<MmsEvaluationSourceQueryV1>(), {{11, 22}, 1}, 0, 0, 0};
    MmsHandle out = 123;
    CHECK(api.createSource(api.context, &query, &out) == MMS_E_NOT_READY && out == 123);
    CHECK(api.retain(api.context, probe.source, &out) == MMS_E_NOT_READY && out == 123);
    MmsEvaluationSourceInfoV1 info; std::memset(&info, 0x5a, sizeof(info)); const auto oldInfo = info;
    CHECK(api.describeSource(api.context, probe.source, &info) == MMS_E_NOT_READY && !std::memcmp(&info, &oldInfo, sizeof(info)));
    MmsEvaluationBoneV1 bone; std::memset(&bone, 0x5a, sizeof(bone)); const auto oldBone = bone;
    CHECK(api.boneAt(api.context, probe.source, 0, &bone) == MMS_E_NOT_READY && !std::memcmp(&bone, &oldBone, sizeof(bone)));
    MmsBoneChannelsV1 sample; std::memset(&sample, 0x5a, sizeof(sample)); const auto oldSample = sample;
    CHECK(api.sampleBone(api.context, probe.source, {33, 44}, {1, 2}, &sample) == MMS_E_NOT_READY && !std::memcmp(&sample, &oldSample, sizeof(sample)));
    MmsEvaluationCapsV1 caps; std::memset(&caps, 0x5a, sizeof(caps)); const auto oldCaps = caps;
    CHECK(api.getCapabilities(api.context, &caps) == MMS_E_NOT_READY && !std::memcmp(&caps, &oldCaps, sizeof(caps)));
    MmsEvaluationApiV1 table; std::memset(&table, 0x5a, sizeof(table)); const auto oldTable = table;
    CHECK(probe.query(&table) == MMS_E_NOT_READY && !std::memcmp(&table, &oldTable, sizeof(table)));
}
static void RunCase(const fs::path& root, const fs::path& fixture, const std::string& mode) {
    const auto base = root / mode; const auto dll = Package(base / "PLUGINS", fixture, mode);
    auto store = Store();
    auto logs = std::make_shared<std::vector<std::string>>();
    // The shared log remains valid if a deliberately invalid create forces
    // Manager to retain itself and its callback context until process exit.
    Manager manager(base / "data", [logs](const auto& s) { logs->push_back(s); std::cout << s << '\n'; }, {}, false, {}, false, store);
    manager.Load(base / "PLUGINS");
    CHECK(Count(*logs, "EVALUATION_NEGOTIATION_PASS") == 1 && Count(*logs, "EVALUATION_SOURCE_CREATED") == 1);
    const bool failed = mode == "failnull" || mode == "failinstance";
    const auto status = manager.Status(); CHECK(status.size() == 1 && status[0].stage == (failed ? "stopping" : "started"));
    Probe probe(dll);
    if (!failed) LiveReads(probe);
    CHECK(!manager.Stop()); CHECK(GetModuleHandleW(dll.c_str()) != nullptr);
    CHECK(Count(*logs, "EVALUATION_DESTROY") == 0);
    CHECK(probe.stops() == (failed ? 0u : 1u));
    Retired(probe);
    MmsResult released = MMS_E_FAILED; std::thread worker([&] { released = probe.release(); }); worker.join();
    CHECK(released == MMS_OK);
    if (mode == "failinstance") {
        // A plugin returning a failed create with a nonnull instance violates
        // the ABI. Host policy pins it rather than calling unvalidated teardown.
        CHECK(!manager.Stop()); CHECK(GetModuleHandleW(dll.c_str()) != nullptr);
        CHECK(probe.stops() == 0 && Count(*logs, "EVALUATION_DESTROY") == 0);
    } else {
        CHECK(manager.Stop()); CHECK(GetModuleHandleW(dll.c_str()) == nullptr);
        CHECK(Count(*logs, "EVALUATION_DESTROY") == (failed ? 0u : 1u));
    }
    std::cout << mode << " PASS\n";
}
static void DependencyShutdown(const fs::path& root, const fs::path& fixture) {
    const auto base = root / "dependency";
    const auto baseDll = Package(base / "PLUGINS", fixture, "base");
    const auto dependentDll = Package(base / "PLUGINS", fixture, "dependent", "org.example.evaluation-base");
    auto store = Store(); std::vector<std::string> logs;
    Manager manager(base / "data", [&](const auto& s) { logs.push_back(s); }, {}, false, {}, false, store);
    manager.Load(base / "PLUGINS"); CHECK(Count(logs, "EVALUATION_START") == 2);
    Probe first(baseDll), second(dependentDll);
    CHECK(!manager.Stop()); CHECK(first.stops() == 0 && second.stops() == 1);
    Retired(first); Retired(second);
    CHECK(first.release() == MMS_OK && second.release() == MMS_OK);
    CHECK(manager.Stop()); CHECK(Count(logs, "EVALUATION_DESTROY") == 2);
    CHECK(GetModuleHandleW(baseDll.c_str()) == nullptr && GetModuleHandleW(dependentDll.c_str()) == nullptr);
    std::cout << "dependency PASS\n";
}
static void InFlight(const fs::path& root, const fs::path& fixture) {
    const auto base = root / "inflight"; const auto dll = Package(base / "PLUGINS", fixture, "inflight-holdok");
    auto gate = std::make_shared<Gate>(); CHECK(gate->entered && gate->proceed);
    auto store = Store(gate); std::vector<std::string> logs;
    Manager manager(base / "data", [&](const auto& s) { logs.push_back(s); }, {}, false, {}, false, store);
    manager.Load(base / "PLUGINS"); Probe probe(dll);
    MmsResult result = MMS_E_FAILED; MmsBoneChannelsV1 sample{};
    struct Join {
        std::shared_ptr<Gate> gate;
        std::thread worker;
        ~Join() { SetEvent(gate->proceed); if (worker.joinable()) worker.join(); }
    } join{gate, std::thread([&] { result = probe.api.sampleBone(probe.api.context, probe.source, {33, 44}, {1, 2}, &sample); })};
    CHECK(WaitForSingleObject(gate->entered, 10000) == WAIT_OBJECT_0);
    CHECK(probe.release() == MMS_OK);
    CHECK(!manager.Stop());
    CHECK(Count(logs, "EVALUATION_STOP_OK") == 1 && Count(logs, "EVALUATION_DESTROY") == 0);
    CHECK(GetModuleHandleW(dll.c_str()) != nullptr); Retired(probe);
    SetEvent(gate->proceed); join.worker.join();
    CHECK(result == MMS_OK && ValidSample(sample));
    CHECK(manager.Stop()); CHECK(Count(logs, "EVALUATION_DESTROY") == 1);
    CHECK(GetModuleHandleW(dll.c_str()) == nullptr);
    std::cout << "inflight PASS\n";
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: evaluation_lifecycle_tests <output-parent> <fixture.dll>\n"); return 2; }
    try {
        const auto root = fs::absolute(argv[1]) / ("run-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        CHECK(!fs::exists(root) && fs::create_directories(root));
        const auto fixture = fs::absolute(argv[2]); CHECK(fs::is_regular_file(fixture));
        RunCase(root, fixture, "normal"); RunCase(root, fixture, "holdok"); RunCase(root, fixture, "failnull");
        DependencyShutdown(root, fixture); InFlight(root, fixture);
        RunCase(root, fixture, "failinstance");
        std::cout << "Evaluation lifecycle PASS: " << checks << " checks\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n'; return 1; }
}
