#pragma once
#include "mikumikustudio/pmm_extension.hpp"
#include "mikumikustudio/pmm_model_adaptation.hpp"
#include "mikumikustudio/skinning_backend.hpp"
#include <vector>

namespace mikumikustudio {
class MMDApp;
namespace pmm_settings {
// Stable PMM v2 values. Device names, LUIDs and menu positions are never serialized.
enum class Processor : std::uint8_t { Automatic = 0, Cpu = 1, Integrated = 2, Discrete = 3 };
inline constexpr char kProcessorKey[] = "org.mikumikustudio.skinning.processor";
// Transport for one TransparentSortPrecision byte: Low = 0 .. Off = 4.  The
// earlier transparent_sort toggle key is retired and is no longer declared here.
inline constexpr char kSortingPrecisionKey[] = "org.mikumikustudio.view.transparent_sort_precision";
// Three stable bytes: enabled, algorithm, last SMAA algorithm. No driver state.
inline constexpr char kAntialiasKey[] = "org.mikumikustudio.view.antialiasing";
struct State {
    pmm_extension::Document document;
    bool processorEdited = false;
    bool sortingPrecisionEdited = false;
    bool antialiasEdited = false;
};
Processor ClassifyProcessor(const skinning::Preference&, const std::vector<gpu::Adapter>&);
skinning::Preference ResolveProcessor(Processor, const std::vector<gpu::Adapter>&);
void Reset(MMDApp&);
void Load(MMDApp&, int fd, const std::vector<PmmModelAdaptation>& adaptedModels = {});
pmm_extension::Document Capture(const MMDApp&);
void ProcessorChanged(MMDApp&);
void SortingPrecisionChanged(MMDApp&);
void AntialiasChanged(MMDApp&);
}
}
