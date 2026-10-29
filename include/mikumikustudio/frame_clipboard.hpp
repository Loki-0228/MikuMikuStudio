#pragma once

#include <cstddef>

namespace mikumikustudio {
class MMDApp;

namespace frame_clipboard {
// Returns the original track for a live same-model clipboard, or -1 when
// the caller must resolve the destination by name. No legacy layout changes.
int BoneTarget(const MMDApp* app, const unsigned char* model, std::size_t index);
int MorphTarget(const MMDApp* app, const unsigned char* model, std::size_t index);
void Reset(const MMDApp* app);
void ForgetModel(const unsigned char* model);
}
}
