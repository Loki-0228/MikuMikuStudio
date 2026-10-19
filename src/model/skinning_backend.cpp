#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include "mikumikustudio/skinning_backend.hpp"
#include "mikumikustudio/model_vertex_cache.hpp"
#include "mikumikustudio/gpu_model.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "skinning_calibration_cache.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <tuple>
#include <unordered_map>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

namespace mikumikustudio {
bool UpdateModelVertexBuffersCpu(MMDApp*, unsigned char*, const float[16]);
}

namespace mikumikustudio::skinning {
namespace {
Preference preference;
Status status;
std::vector<gpu::Adapter> adapters;
bool enumerated = false;
std::vector<std::size_t> candidates;
std::size_t nextCandidate = 0;
bool candidatesReady = false;
std::unique_ptr<gpu::Skinning> compute;
GpuModelInput input;
std::vector<gpu::Output> output;
std::vector<unsigned char> compactOutput;


using Signature = std::tuple<const void*, const void*, const void*, const void*, const void*,
                             unsigned, unsigned, unsigned, unsigned, unsigned>;
Signature MakeSignature(const mdl::ModelRecord& m) {
    return {m.vertexBuffer,
            m.vertexBuffer2,
            m.boneTable, m.materials, m.physicsMode == 2 ? static_cast<const void*>(m.pmxVertices) : m.rawVertices,
            m.vertexCount, m.boneCount, m.materialCount, m.physicsMode, m.pmxAdditionalUvCount};
}
struct AutomaticEntry {
    Signature signature;
    AutomaticReport report;
};
std::unordered_map<unsigned char*, AutomaticEntry> automaticEntries;
bool automaticFailed = false;
bool startupAttempted = false;
std::vector<StartupMeasurement> startupMeasurements;
struct StartupDevice {
    gpu::Adapter adapter;
    std::unique_ptr<gpu::Skinning> device;
    bool attempted = false;
};
std::vector<StartupDevice> startupDevices;

StartupDevice* FindStartupDevice(std::uint64_t id) {
    for (auto& candidate : startupDevices) if(candidate.adapter.id == id) {
        if(!candidate.attempted) {
            candidate.attempted=true;
            auto device=std::make_unique<gpu::Skinning>();
            if(device->Initialize(false,id))candidate.device=std::move(device);
            else runtime_log::Write("Cached skinning device unavailable: %s",device->Error());
        }
        return candidate.device ? &candidate : nullptr;
    }
    return nullptr;
}

void PrepareCandidates() {
    if (candidatesReady) return;
    candidatesReady = true;
    const auto& available = AvailableAdapters();
    const auto allowed = [&](const gpu::Adapter& adapter) {
        return preference.adapterClass == AdapterClass::Any ||
            (adapter.integrated ? AdapterClass::Integrated : AdapterClass::Discrete) == preference.adapterClass;
    };
    std::size_t preferred = available.size();
    if (preference.mode == Mode::Adapter) {
        for (std::size_t i = 0; i < available.size(); ++i)
            if (allowed(available[i]) && available[i].id == preference.adapterId &&
                (preference.adapterName.empty() || available[i].name == preference.adapterName))
                preferred = i;
        // LUIDs can change across reboot. Recover by name only when unique.
        if (preferred == available.size() && !preference.adapterName.empty()) {
            std::size_t matches = 0;
            for (std::size_t i = 0; i < available.size(); ++i)
                if (allowed(available[i]) && available[i].name == preference.adapterName) { preferred = i; ++matches; }
            if (matches != 1) preferred = available.size();
        }
    }
    if (preferred < available.size()) {
        candidates.push_back(preferred);
        preference.adapterId = available[preferred].id;
        preference.adapterName = available[preferred].name;
    } else if (preference.mode == Mode::Adapter && preference.adapterClass == AdapterClass::Any) {
        preference = {}; // The saved device is absent; show the automatic fallback.
    }
    for (std::size_t i = 0; i < available.size(); ++i)
        if (i != preferred && allowed(available[i])) candidates.push_back(i);
}

bool EnsureDevice() {
    if (compute) return true;
    PrepareCandidates();
    while (nextCandidate < candidates.size()) {
        const auto& adapter = adapters[candidates[nextCandidate++]];
        auto device = std::make_unique<gpu::Skinning>();
        if (device->Initialize(false, adapter.id)) {
            status.adapterName = adapter.name;
            compute = std::move(device);
            runtime_log::Write("D3D9 skinning compute adapter: %ls", adapter.name.c_str());
            return true;
        }
        runtime_log::Write("GPU skinning initialization failed: %ls: %s",
                           adapter.name.c_str(), device->Error());
    }
    status.adapterName.clear();
    return false;
}
bool FallBack(Result reason) {
    status.result = reason;
    ++status.cpuFallbacks;
    return false;
}
}

const std::vector<gpu::Adapter>& AvailableAdapters() {
    if (!enumerated) {
        try { adapters = gpu::EnumerateAdapters(); }
        catch (const std::exception& error) {
            runtime_log::Write("GPU enumeration unavailable; using CPU skinning: %s", error.what());
            adapters.clear();
        }
        enumerated = true;
    }
    return adapters;
}
const Preference& GetPreference() { return preference; }
const Status& GetStatus() { return status; }
static void ResetSelection() {
    model_vertex_cache::Clear();
    automaticEntries.clear();
    automaticFailed = false;
    compute.reset();
    input = {};
    output = {};
    compactOutput = {};
    candidates.clear();
    nextCandidate = 0;
    candidatesReady = false;
    const auto calibrations = status.automaticCalibrations;
    status = {};
    status.automaticCalibrations = calibrations;
}
void Shutdown() {
    ResetSelection();
    startupDevices.clear();
    startupMeasurements.clear();
    startupAttempted = false;
    status = {};
}
void SetPreference(Preference value) {
    ResetSelection();
    preference = std::move(value);
    if (preference.mode == Mode::Cpu) status.result = Result::Cpu;
}

std::wstring PreferencePath() {
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
    if (!length || length >= 32768) return {};
    std::wstring path(executable, length);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return path.substr(0, slash) + L"\\Data\\skinning.ini";
}
void LoadPreference(const wchar_t* path) {
    Preference value;
    if (!path || !*path) { SetPreference(value); return; }
    wchar_t mode[32]{};
    GetPrivateProfileStringW(L"Skinning", L"Processor", L"auto", mode, 32, path);
    if (wcscmp(mode, L"cpu") == 0) value.mode = Mode::Cpu;
    else if (wcscmp(mode, L"gpu") == 0) {
        wchar_t id[32]{}, name[256]{};
        GetPrivateProfileStringW(L"Skinning", L"AdapterId", L"", id, 32, path);
        GetPrivateProfileStringW(L"Skinning", L"AdapterName", L"", name, 256, path);
        const bool valid = wcslen(id) == 16 &&
            std::all_of(id, id + 16, [](wchar_t c) {
                return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') ||
                       (c >= L'A' && c <= L'F');
            });
        if (valid) {
            value.adapterId = std::wcstoull(id, nullptr, 16);
            if (value.adapterId) { value.mode = Mode::Adapter; value.adapterName = name; }
        }
    }
    SetPreference(std::move(value));
    if (preference.mode == Mode::Adapter) PrepareCandidates();
}
bool SavePreference(const wchar_t* path) {
    if (!path || !*path) return false;
    // WritePrivateProfileStringW preserves Unicode only in a Unicode INI.
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        const wchar_t bom = 0xfeff;
        DWORD written = 0;
        const bool ok = WriteFile(file, &bom, sizeof bom, &written, nullptr) && written == sizeof bom;
        CloseHandle(file);
        if (!ok) return false;
    } else if (GetLastError() != ERROR_FILE_EXISTS) return false;
    wchar_t id[17]{};
    swprintf_s(id, L"%016llx", static_cast<unsigned long long>(preference.adapterId));
    const wchar_t* mode = preference.mode == Mode::Cpu ? L"cpu" :
                         preference.mode == Mode::Adapter ? L"gpu" : L"auto";
    return WritePrivateProfileStringW(L"Skinning", L"AdapterId", id, path) &&
           WritePrivateProfileStringW(L"Skinning", L"AdapterName", preference.adapterName.c_str(), path) &&
           WritePrivateProfileStringW(L"Skinning", L"Processor", mode, path);
}

namespace {

struct BufferSnapshot {
    std::vector<float> vertices;
    std::vector<mdl::EdgeVertex> edges;
};
bool ReadBuffers(mdl::ModelRecord& record, BufferSnapshot& snapshot) {
    const unsigned stride = record.physicsMode == 2 ? 32 + 16 * record.pmxAdditionalUvCount : 32;
    auto* main = mdl::ResourceAs<IDirect3DVertexBuffer9>(record.vertexBuffer);
    auto* edge = mdl::ResourceAs<IDirect3DVertexBuffer9>(record.vertexBuffer2);
    snapshot.vertices.resize(std::size_t(stride / 4) * record.vertexCount);
    snapshot.edges.resize(record.vertexCount);
    void* bytes = nullptr;
    if (FAILED(main->Lock(0, stride * record.vertexCount, &bytes, D3DLOCK_READONLY))) return false;
    std::memcpy(snapshot.vertices.data(), bytes, stride * record.vertexCount);
    main->Unlock();
    if (FAILED(edge->Lock(0, sizeof(mdl::EdgeVertex) * record.vertexCount, &bytes, D3DLOCK_READONLY))) return false;
    std::memcpy(snapshot.edges.data(), bytes, sizeof(mdl::EdgeVertex) * record.vertexCount);
    edge->Unlock();
    return true;
}
bool SameBuffers(const BufferSnapshot& cpu, const BufferSnapshot& gpu) {
    const auto close = [](float a, float b) {
        return std::isfinite(a) && std::isfinite(b) &&
            std::abs(double(a) - b) <= 2e-5 * std::max(1., std::abs(double(a)));
    };
    if (cpu.vertices.size() != gpu.vertices.size() || cpu.edges.size() != gpu.edges.size()) return false;
    for (std::size_t i = 0; i < cpu.vertices.size(); ++i)
        if (!close(cpu.vertices[i], gpu.vertices[i])) return false;
    for (std::size_t i = 0; i < cpu.edges.size(); ++i) {
        if (cpu.edges[i].diffuse != gpu.edges[i].diffuse) return false;
        for (unsigned c = 0; c < 3; ++c)
            if (!close(cpu.edges[i].position[c], gpu.edges[i].position[c])) return false;
    }
    return true;
}
Result UpdateGpu(gpu::Skinning& device, MMDApp* app, unsigned char* model, const float world[16]) {
    auto& record = *mdl::Mdl(model);
    if (!PrepareGpuModelInput(app, model, world, input)) return Result::Unsupported;
    const unsigned stride = record.physicsMode == 2 ? 32 + 16 * record.pmxAdditionalUvCount : 32;
    // Restrict the extra dispatch to large meshes with at least 42% fewer
    // transfer bytes. Small meshes and UV2-4 keep the existing readback path.
    const bool compact=record.vertexCount>=262144 && stride<=48;
    if(compact) {
        if(!device.SubmitAndReadbackCompact(input.vertices,input.bones,input.materials,
                input.parameters,(stride-32)/16,compactOutput) ||
           compactOutput.size()!=std::size_t(record.vertexCount)*(stride+16)) return Result::Unavailable;
    } else if (!device.SubmitAndReadback(input.vertices, input.bones, input.materials,
                                        input.parameters, output) || output.size() != record.vertexCount)
        return Result::Unavailable;
    auto* main = mdl::ResourceAs<IDirect3DVertexBuffer9>(record.vertexBuffer);
    auto* edge = mdl::ResourceAs<IDirect3DVertexBuffer9>(record.vertexBuffer2);
    void* vertices = nullptr;
    void* edges = nullptr;
    if (FAILED(main->Lock(0, stride * record.vertexCount, &vertices, 0))) return Result::Unavailable;
    if (FAILED(edge->Lock(0, sizeof(mdl::EdgeVertex) * record.vertexCount, &edges, 0))) {
        main->Unlock(); return Result::Unavailable;
    }
    for (std::size_t i = 0; i < record.vertexCount; ++i) {
        const auto* source=compact ? compactOutput.data()+(stride+16)*i :
            reinterpret_cast<const unsigned char*>(&output[i]);
        std::memcpy(static_cast<char*>(vertices) + stride * i, source, stride);
        auto& destination = static_cast<mdl::EdgeVertex*>(edges)[i];
        const auto* outline=source+(compact ? stride : offsetof(gpu::Output,edgePosition));
        std::memcpy(destination.position, outline, sizeof destination.position);
        if (record.physicsMode == 2) std::memcpy(&destination.diffuse,outline+12,4);
    }
    edge->Unlock();
    main->Unlock();
    return Result::Gpu;
}
double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}
template<class Work>
bool Timed(Work&& work, std::vector<double>& samples) {
    const auto begin = std::chrono::steady_clock::now();
    if (!work()) return false;
    const double elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - begin).count();
    samples.push_back(elapsed);
    return true;
}
bool Calibrate(MMDApp* app, unsigned char* model, const float world[16],
               AutomaticReport& report) {
    auto& record = *mdl::Mdl(model);
    constexpr unsigned warmups = 3, samples = 5;
    const auto cpuWork = [&] { return UpdateModelVertexBuffersCpu(app, model, world); };
    for (unsigned i = 0; i < warmups; ++i) if (!cpuWork()) return false;
    BufferSnapshot reference;
    if (!ReadBuffers(record, reference)) return false;
    std::vector<double> cpuSamples;
    for (unsigned i = 0; i < samples; ++i)
        if (!Timed(cpuWork, cpuSamples)) return false;
    report.measurements.push_back({0, 0, true});
    for (auto& candidate : startupDevices) {
        Measurement measurement{candidate.adapter.id};
        if (candidate.device) {
            const auto gpuWork = [&] {
                return UpdateGpu(*candidate.device, app, model, world) == Result::Gpu;
            };
            bool valid = true;
            for (unsigned i = 0; i < warmups && valid; ++i) valid = gpuWork();
            BufferSnapshot actual;
            valid = valid && ReadBuffers(record, actual) && SameBuffers(reference, actual);
            std::vector<double> gpuSamples;
            for (unsigned i = 0; i < samples && valid; ++i) {
                if (i & 1) valid = Timed(gpuWork, gpuSamples) && Timed(cpuWork, cpuSamples);
                else valid = Timed(cpuWork, cpuSamples) && Timed(gpuWork, gpuSamples);
            }
            measurement.valid = valid;
            if (valid) measurement.milliseconds = Median(gpuSamples);
            // Do not retain a large synthetic mesh on every GPU between cases.
            if (!candidate.device->ReleaseWorkingBuffers()) {
                candidate.device.reset();
                measurement.valid = false;
            }
        }
        report.measurements.push_back(measurement);
        runtime_log::Write("Startup processor configuration candidate vertices=%u pmx=%d id=%016llx valid=%d median_ms=%.6f",
            record.vertexCount, record.physicsMode == 2,
            static_cast<unsigned long long>(measurement.adapterId), measurement.valid, measurement.milliseconds);
    }
    report.measurements.front().milliseconds = Median(cpuSamples);
    report.selectedAdapterId = FastestMeasurement(report.measurements);
    runtime_log::Write("Startup processor configuration vertices=%u pmx=%d cpu_ms=%.6f selected=%016llx",
        record.vertexCount, record.physicsMode == 2, report.measurements.front().milliseconds,
        static_cast<unsigned long long>(report.selectedAdapterId));
    ++status.automaticCalibrations;
    return true;
}

// Standalone calibration geometry never enters the scene, physics, or MME
// object lists. It exercises the production CPU worker and GPU/D3D9 bridge.
struct StartupMesh {
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    std::vector<mdl::BoneRecord> bones{64};
    std::vector<mdl::ModelMaterialRecord> materials{16};
    std::vector<mdl::PmdVertex> pmd;
    std::vector<mdl::PmxVertex> pmx;
    Microsoft::WRL::ComPtr<IDirect3DVertexBuffer9> main, edge;
    float world[16]{};
    StartupMesh(IDirect3DDevice9* device, unsigned count, bool isPmx) {
        world[0] = world[5] = world[10] = world[15] = 1;
        for (unsigned b = 0; b < bones.size(); ++b) {
            std::memcpy(bones[b].matInit, world, sizeof world);
            const float angle = b * .013f;
            bones[b].matInit[0] = std::cos(angle); bones[b].matInit[1] = std::sin(angle);
            bones[b].matInit[4] = -std::sin(angle); bones[b].matInit[5] = std::cos(angle);
            bones[b].matInit[12] = b * .04f;
        }
        for (auto& material : materials) {
            material.edgeColor[0] = .2f; material.edgeColor[1] = .4f;
            material.edgeColor[2] = .6f; material.edgeColor[3] = .8f;
            material.edgeSize = 1.3f;
        }
        model->loadComplete = 1; model->edgeScale = 1;
        model->vertexCount = count; model->boneCount = unsigned(bones.size());
        model->materialCount = unsigned(materials.size());
        model->boneTable = bones.data(); model->materials = materials.data();
        model->physicsMode = isPmx ? 2 : 0;
        if (isPmx) {
            pmx.resize(count); model->pmxVertices = pmx.data();
            for (unsigned i = 0; i < count; ++i) {
                auto& v = pmx[i];
                v.position[0] = (i % 100) * .12f; v.normal[1] = 1;
                v.uv[0] = .25f; v.uv[1] = .75f;
                v.weightType = static_cast<mdl::PmxWeightType>(i % 4);
                for (unsigned b = 0; b < 4; ++b) {
                    v.bone[b] = (i + b) % unsigned(bones.size());
                    v.weight[b] = i % 4 == 2 ? .25f : (b == 0 ? .6f : .4f);
                }
                v.materialIndex = i % unsigned(materials.size());
                v.edgeScale = .9f; v.hasPmxEdgeData = i & 1;
                v.sdef.center[0] = .2f; v.sdef.r0Offset[1] = .3f; v.sdef.r1Offset[2] = .4f;
            }
        } else {
            pmd.resize(count); model->rawVertices = pmd.data();
            for (unsigned i = 0; i < count; ++i) {
                auto& v = pmd[i];
                v.position[0] = (i % 100) * .12f; v.normal[1] = 1;
                v.uv[0] = .25f; v.uv[1] = .75f;
                v.bone[0] = i % unsigned(bones.size());
                v.bone[1] = (i + 1) % unsigned(bones.size());
                v.weightPercent = 60; v.edgeDisabled = i & 1;
            }
        }
        if (FAILED(device->CreateVertexBuffer(count * 32, 0, 0, D3DPOOL_MANAGED, &main, nullptr)) ||
            FAILED(device->CreateVertexBuffer(count * sizeof(mdl::EdgeVertex), 0, 0, D3DPOOL_MANAGED, &edge, nullptr)))
            throw std::runtime_error("startup skinning buffers unavailable");
        model->vertexBuffer = main.Get(); model->vertexBuffer2 = edge.Get();
        void* bytes = nullptr;
        if (FAILED(edge->Lock(0, 0, &bytes, 0))) throw std::runtime_error("startup skinning edge lock failed");
        for (unsigned i = 0; i < count; ++i) static_cast<mdl::EdgeVertex*>(bytes)[i] = {};
        edge->Unlock();
    }
};

const StartupMeasurement* ClosestStartupMeasurement(const mdl::ModelRecord& record) {
    const StartupMeasurement* closest = nullptr;
    double distance = std::numeric_limits<double>::infinity();
    for (const auto& sample : startupMeasurements) {
        if (sample.pmx != (record.physicsMode == 2)) continue;
        const double d = std::abs(std::log(double(std::max(1u, record.vertexCount)) / sample.vertexCount));
        if (d < distance) { distance = d; closest = &sample; }
    }
    return closest;
}

bool UpdateAutomatic(MMDApp* app, unsigned char* model, const float world[16]) {
    if (automaticFailed) return FallBack(Result::Unavailable);
    auto& record = *mdl::Mdl(model);
    const auto signature = MakeSignature(record);
    auto found = automaticEntries.find(model);
    if (found != automaticEntries.end() && found->second.signature != signature) {
        automaticEntries.erase(found);
        found = automaticEntries.end();
    }
    if (found == automaticEntries.end()) {
        if (!PrepareGpuModelInput(app, model, world, input)) return FallBack(Result::Unsupported);
        const auto* sample = ClosestStartupMeasurement(record);
        // Non-GUI callers that omitted startup initialization safely use CPU.
        // Model loads reuse timings; GPU pipelines are created only if selected.
        if (!sample) return FallBack(Result::Unavailable);
        AutomaticEntry entry;
        entry.signature = signature;
        entry.report = sample->report;
        if (automaticEntries.size() >= 256) automaticEntries.clear();
        found = automaticEntries.emplace(model, std::move(entry)).first;
    }
    auto& report = found->second.report;
    for (;;) {
        const auto selected = report.selectedAdapterId;
        if (!selected) {
            status.adapterName.clear(); status.result = Result::Cpu;
            ++status.cpuUpdates;
            return false;
        }
        auto* candidate = FindStartupDevice(selected);
        if (candidate) {
            const auto result = UpdateGpu(*candidate->device, app, model, world);
            if (result == Result::Gpu) {
                status.adapterName = candidate->adapter.name; status.result = result;
                ++status.gpuUpdates;
                return true;
            }
            if (result == Result::Unsupported) return FallBack(result);
            runtime_log::Write("Startup processor configuration device failed; using cached ranking: %s", candidate->device->Error());
            candidate->device.reset();
        }
        for (auto& measurement : report.measurements)
            if (measurement.adapterId == selected) measurement.valid = false;
        report.selectedAdapterId = FastestMeasurement(report.measurements);
    }
}
} // namespace

void InitializeAutomatic(MMDApp* app, IDirect3DDevice9* device) {
    if (preference.mode != Mode::Automatic || startupAttempted || !app || !device) return;
    startupAttempted = true;
    const auto started = std::chrono::steady_clock::now();
    const auto path=calibration_cache::Path(L"MMS");
    runtime_log::WritePath("Startup processor configuration cache path",path.c_str());
    const auto cpu=calibration_cache::CpuKey();
    const auto& available=AvailableAdapters();
    bool cached=calibration_cache::Load(path,cpu,available,startupMeasurements);
    if(!cached) {
        const auto legacy=calibration_cache::LegacyPath(L"MMS");
        cached=calibration_cache::Load(legacy,cpu,available,startupMeasurements);
        if(cached) {
            // Keep the validated result usable even if the executable's
            // directory is read-only. Leave the old file intact for older builds.
            runtime_log::WritePath("Startup processor configuration legacy cache",legacy.c_str());
            runtime_log::Write("Startup processor configuration cache migration saved=%d",
                calibration_cache::Save(path,cpu,available,startupMeasurements));
        }
    }
    if(cached) {
        for(const auto& adapter:available)startupDevices.push_back({adapter,{}});
        runtime_log::Write("Startup processor configuration cache hit profiles=%zu elapsed_ms=%.3f",
            startupMeasurements.size(),std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-started).count());
        return;
    }
    runtime_log::Write("Startup processor configuration calibration begin");
    try {
        for (const auto& adapter : AvailableAdapters()) {
            auto trial = std::make_unique<gpu::Skinning>();
            if (!trial->Initialize(false, adapter.id)) trial.reset();
            startupDevices.push_back({adapter, std::move(trial), true});
        }
        for (bool pmx : {false, true}) for (unsigned count : {1024u, 32768u, 262144u}) {
            StartupMesh mesh(device, count, pmx);
            StartupMeasurement sample;
            sample.vertexCount = count; sample.pmx = pmx;
            if (Calibrate(app, reinterpret_cast<unsigned char*>(mesh.model.get()), mesh.world, sample.report))
                startupMeasurements.push_back(std::move(sample));
        }
    } catch (const std::exception& error) {
        runtime_log::Write("Startup processor configuration calibration incomplete; CPU fallback: %s", error.what());
    }
    // Keep initialized devices/pipelines, not the temporary calibration data.
    for (auto& candidate : startupDevices)
        if (candidate.device && !candidate.device->ReleaseWorkingBuffers()) candidate.device.reset();
    input = {};
    std::vector<gpu::Output>().swap(output);
    runtime_log::Write("Startup processor configuration cache saved=%d",
        calibration_cache::Save(path,cpu,available,startupMeasurements));
    runtime_log::Write("Startup processor configuration calibration end profiles=%zu elapsed_ms=%.3f",
        startupMeasurements.size(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count());
}
const std::vector<StartupMeasurement>& GetStartupMeasurements() { return startupMeasurements; }

void ForgetModel(unsigned char* model) {
    model_vertex_cache::Forget(model);
    automaticEntries.erase(model);
}
const AutomaticReport* GetAutomaticReport(unsigned char* model) {
    const auto found = automaticEntries.find(model);
    return found == automaticEntries.end() ? nullptr : &found->second.report;
}
std::uint64_t FastestMeasurement(const std::vector<Measurement>& measurements) {
    double best = std::numeric_limits<double>::infinity();
    std::uint64_t id = 0;
    for (const auto& value : measurements) {
        if (!value.valid || !std::isfinite(value.milliseconds) || value.milliseconds <= 0) continue;
        // No device-class preference. Exact ties retain the first candidate (CPU).
        if (value.milliseconds < best) { best = value.milliseconds; id = value.adapterId; }
    }
    return id;
}
bool UpdateModel(MMDApp* app, unsigned char* model, const float world[16], CpuUpdate) {
    if (preference.mode == Mode::Cpu) {
        status.result = Result::Cpu; ++status.cpuUpdates; return false;
    }
    if (!app || !model || !world) return false;
    auto& record = *mdl::Mdl(model);
    if (!record.vertexBuffer || !record.vertexBuffer2) return false;
    if (!record.boneTable || !record.materials ||
        (record.physicsMode == 2 ? !record.pmxVertices : !record.rawVertices))
        return FallBack(Result::Unsupported);
    try {
        PrepareCandidates();
        if (preference.mode == Mode::Automatic) return UpdateAutomatic(app, model, world);
        while (EnsureDevice()) {
            const auto result = UpdateGpu(*compute, app, model, world);
            if (result == Result::Gpu) {
                status.result = result; ++status.gpuUpdates; return true;
            }
            if (result == Result::Unsupported) return FallBack(result);
            runtime_log::Write("GPU skinning failed; trying next adapter: %s", compute->Error());
            compute.reset();
        }
        return FallBack(Result::Unavailable);
    } catch (const std::exception& error) {
        runtime_log::Write("GPU skinning fallback: %s", error.what());
        compute.reset();
        automaticEntries.erase(model);
        automaticFailed = true;
        nextCandidate = candidates.size();
        return FallBack(Result::Unavailable);
    }
}
}
