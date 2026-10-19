#pragma once

namespace mikumikustudio {
// Stored in combo item data so translating a label cannot change its scope.
enum class FrameScope {
    NamedTrack = 0,
    AllFrames,
    Display,
    SelectedBones,
    SelectedMorphs,
    AllMorphs,
    AllBones,
};
}
