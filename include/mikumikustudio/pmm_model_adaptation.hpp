#pragma once
#include <vector>

namespace mikumikustudio {
// Transient, owned load context. The PMM loader copies its final name mapping
// before releasing the model workspaces; no serialized ABI or borrowed pointer
// is introduced. An absent/out-of-range old index resolves to native bone 0.
struct PmmModelAdaptation {
    int slot = -1;
    bool remapBones = false;
    std::vector<int> boneIndices;
};
}
