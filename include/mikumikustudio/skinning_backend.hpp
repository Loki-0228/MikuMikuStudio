#pragma once
#include "mikumikustudio/d3d12_skinning.hpp"
#include <string>
struct IDirect3DDevice9;

namespace mikumikustudio {
class MMDApp;
namespace skinning {
enum class Mode { Automatic, Adapter, Cpu };
enum class AdapterClass { Any, Integrated, Discrete };
struct Preference {
    Mode mode = Mode::Automatic;
    std::uint64_t adapterId = 0;
    std::wstring adapterName;
    // PMM type matching restricts fallback candidates to the same class.
    AdapterClass adapterClass = AdapterClass::Any;
};
enum class Result { Pending, Gpu, Cpu, Unsupported, Unavailable };
struct Status {
    Result result = Result::Pending;
    std::wstring adapterName;
    std::uint64_t gpuUpdates = 0;
    std::uint64_t cpuFallbacks = 0;
    std::uint64_t cpuUpdates = 0;
    std::uint64_t automaticCalibrations = 0;
};
const std::vector<gpu::Adapter>& AvailableAdapters();
const Preference& GetPreference();
void SetPreference(Preference preference);
const Status& GetStatus();
// Separate from the legacy positional mmconfig.ini. Tests can use a temporary path.
std::wstring PreferencePath();
void LoadPreference(const wchar_t* path);
bool SavePreference(const wchar_t* path);
struct Measurement {
    std::uint64_t adapterId = 0; // zero is the CPU
    double milliseconds = 0;
    bool valid = false;
};
std::uint64_t FastestMeasurement(const std::vector<Measurement>&);
struct AutomaticReport {
    std::vector<Measurement> measurements;
    std::uint64_t selectedAdapterId = 0;
};
struct StartupMeasurement {
    unsigned vertexCount = 0;
    bool pmx = false;
    AutomaticReport report;
};
// Load persistent measurements after D3D9 initialization, before any project.
// Benchmark only on a cache miss; model loads never rerun calibration.
void InitializeAutomatic(MMDApp*, IDirect3DDevice9*);
const std::vector<StartupMeasurement>& GetStartupMeasurements();
// Startup profile selected for this model, not a fresh model benchmark.
const AutomaticReport* GetAutomaticReport(unsigned char* model);
using CpuUpdate = bool (*)(MMDApp*, unsigned char*, const float[16]);
// True means this frame was computed by the GPU.
// On false the caller executes the original CPU worker before rendering.
bool UpdateModel(MMDApp*, unsigned char* model, const float world[16], CpuUpdate);
void ForgetModel(unsigned char* model);
void Shutdown();
}
}
