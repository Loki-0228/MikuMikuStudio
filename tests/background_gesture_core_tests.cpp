#include "../src/plugins/background_gesture_core.hpp"
#include <cstdio>
using namespace mikumikustudio::plugins::background_gesture_core;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (false)

int main() {
    Gesture gesture;
    const Domain media{1, 2}, replaced{1, 3};
    const Values original{1, 2, 1.f}, preview{5, 6, 2.f};
    CHECK(gesture.Begin(0, media, original, 1) == Result::Invalid);
    CHECK(gesture.Begin(10, media, original, 99) == Result::Ok);
    CHECK(gesture.Begin(10, media, original, 1) == Result::Busy);
    CHECK(gesture.ReservePreview(11, media, original) == Result::WrongThread);
    CHECK(gesture.ReservePreview(10, replaced, original) == Result::Stale);
    CHECK(gesture.ReservePreview(10, media, original) == Result::Ok);
    CHECK(gesture.ReservePreview(10, media, original) == Result::Busy);
    Detached blocked{};
    CHECK(gesture.Detach(10, media, original, true, blocked) == Result::Busy);
    CHECK(gesture.AcceptPreview(10, media, original, preview) == Result::Ok);
    CHECK(gesture.ReservePreview(10, media, original) == Result::Stale);
    Detached out{};
    CHECK(gesture.Detach(11, media, preview, true, out) == Result::WrongThread);
    const Values modifiedByOther{5, 7, 2.f};
    CHECK(gesture.Detach(10, media, modifiedByOther, true, out) == Result::Ok);
    CHECK(out.serial && out.transaction == 99 && out.restoreMask == 5);
    CHECK(Same(out.restore, original));
    CHECK(gesture.Detach(10, media, preview, true, out) == Result::Busy);
    CHECK(gesture.Begin(10, media, original, 1) == Result::Busy);
    CHECK(gesture.FinishDetach(11, out.serial) == Result::WrongThread);
    CHECK(gesture.FinishDetach(10, out.serial + 1) == Result::Stale);
    const auto oldSerial = out.serial;
    CHECK(gesture.FinishDetach(10, oldSerial) == Result::Ok);
    CHECK(gesture.Begin(10, media, original, 2) == Result::Ok);
    CHECK(gesture.ReservePreview(10, media, original) == Result::Ok);
    CHECK(gesture.AcceptPreview(10, media, original, preview) == Result::Ok);
    CHECK(gesture.Detach(10, replaced, preview, true, out) == Result::Ok);
    CHECK(!out.restoreMask && out.serial != oldSerial && out.transaction == 2);
    CHECK(gesture.FinishDetach(10, oldSerial) == Result::Stale);
    CHECK(gesture.FinishDetach(10, out.serial) == Result::Ok);
    CHECK(gesture.Begin(10, media, original, 3) == Result::Ok);
    CHECK(gesture.ReservePreview(10, media, original) == Result::Ok);
    CHECK(gesture.AbortPreview(10) == Result::Ok);
    CHECK(gesture.ReservePreview(10, media, original) == Result::Ok);
    CHECK(gesture.AcceptPreview(10, media, original, preview) == Result::Ok);
    CHECK(gesture.Detach(10, media, preview, false, out) == Result::Ok);
    CHECK(!out.restoreMask && out.transaction == 3);
    CHECK(gesture.FinishDetach(10, out.serial) == Result::Ok);
    return 0;
}
