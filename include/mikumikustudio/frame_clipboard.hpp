#pragma once

#include <cstddef>

namespace mikumikustudio {
class MMDApp;

namespace frame_clipboard {
// Returns the original same-model track, or matches cross-model names by their
// occurrence in the complete model table. -1 requests legacy name resolution.
int BoneTarget(const MMDApp* app, const unsigned char* model, std::size_t index);
int MorphTarget(const MMDApp* app, const unsigned char* model, std::size_t index);
void Reset(const MMDApp* app);
void ForgetModel(const unsigned char* model);
}
}
