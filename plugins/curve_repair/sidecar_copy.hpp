#pragma once
#include "repair_core.hpp"
#include <optional>

namespace repair {
// Read-only preparation. Refuse an unfinished host save rather than attempting
// recovery; this tool never owns or modifies the source project's file pair.
std::optional<Bytes> PrepareSidecarCopy(const std::filesystem::path& source,
                                      const Bytes& original, const Bytes& repaired);
}
