// Exercise the ordinary frame-line implementation in both build profiles.
// Camera interpolation has six real channels, including distance and FOV.
#include "mikudancestudio/build_profile.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

namespace mikudancestudio { int ShiftGlobalKeysForFrameLine(MMDApp*, bool, bool); }
using namespace mikudancestudio;
namespace {
unsigned checks = 0;
void Check(bool value, const char* label) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL %s\n", label); std::exit(1); }
}
void Merge(bool successor) {
    auto app = std::make_unique<MMDApp>();
    ::new (static_cast<void*>(&app->state)) MMDAppState{};
    auto* keys = mdl::KeyPoolCreate<mdl::CameraKey>(4);
    app->CameraKeys() = keys;
    app->CurrentFrame() = 0;
    app->LastRegisteredFrame() = 11;
    for (unsigned i = 0; i < 3; ++i) {
        auto& k = keys[i];
        k.frame = i == 0 ? 0 : i == 1 ? 1 : 11;
        k.previous = i == 2 ? 1 : 0;
        k.next = i == 0 ? 1 : i == 1 && successor ? 2 : 0;
        k.distance = -40.f - float(i);
        k.fov = 40 + int(i);
        k.perspective = static_cast<unsigned char>(i % 2);
        k.parentModel = int(i);
        k.parentBone = 7 + int(i);
        k.selected = static_cast<unsigned char>(9 + i);
        for (unsigned axis = 0; axis < 3; ++axis) {
            k.eye[axis] = float(10 * i + axis) + .125f;
            k.target[axis] = float(3 * i + axis) * .173f;
            k.reserved1[axis] = static_cast<unsigned char>(31 + i);
            k.reserved2[axis] = static_cast<unsigned char>(43 + i);
        }
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned lane = 0; lane < 6; ++lane)
                k.interpolation[row][lane] = static_cast<unsigned char>(7 + i * 31 + row * 6 + lane);
    }
    if (!successor) keys[2] = {};
    const auto oldRoot = keys[0], first = keys[1];
    auto expected = first;
    expected.frame = 0; expected.previous = oldRoot.previous; expected.next = first.next;
    expected.selected = oldRoot.selected;
    std::memcpy(expected.reserved1, oldRoot.reserved1, sizeof expected.reserved1);
    std::memcpy(expected.reserved2, oldRoot.reserved2, sizeof expected.reserved2);
    Check(ShiftGlobalKeysForFrameLine(app.get(), true, false) == (successor ? 2 : 1),
          "ordinary zero-frame deletion moves the expected records");
    Check(keys[0].parentModel == first.parentModel && keys[0].parentBone == first.parentBone,
          "zero-frame merge inherits both parent references from frame one");
    for (unsigned lane = 0; lane < 6; ++lane) {
        bool same = true;
        for (unsigned row = 0; row < 4; ++row)
            same = same && keys[0].interpolation[row][lane] == first.interpolation[row][lane];
        Check(same, "zero-frame merge retains every control byte of each of the six camera channels");
    }
    Check(!std::memcmp(&keys[0], &expected, sizeof expected),
          "all camera payload bytes move while root links selection and reserved bytes retain their roles");
    Check(keys[1].frame == 0 && keys[0].next == (successor ? 2u : 0u) &&
          (!successor || (keys[2].frame == 10 && keys[2].previous == 0)),
          "successor chain remains reachable after merging frame one into root");
    Check(app->CurrentFrame() == 0 && app->LastRegisteredFrame() == 11,
          "ordinary surgery leaves playback head and legacy last-frame policy unchanged");
    app->CameraKeys() = nullptr;
    mdl::KeyPoolDestroy(keys);
}
}
int main() {
    Merge(false); Merge(true);
    std::printf("PASS ordinary camera frame-line payload: %u checks original=%u windows=0 devices=0\n",
                checks, unsigned(MMD_ORIGINAL_PROFILE));
}
