// MAINTENANCE: regression-keep; CPU-only receiver reconstruction and coverage.
#include "mikumikustudio/native_shadow_projection.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace mikumikustudio;
using Matrix = std::array<float, 16>;
using Point = std::array<double, 3>;
unsigned checks = 0;

void Require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
Matrix Identity() {
    Matrix result{};
    result[0] = result[5] = result[10] = result[15] = 1;
    return result;
}
Matrix Perspective(float x = 2, float y = 4) {
    Matrix result{};
    result[0] = x; result[5] = y;
    result[10] = 1.00001f; result[11] = 1; result[14] = -1.00001f;
    return result;
}
Matrix Orthographic(float x = .5f, float y = .25f) {
    auto result = Identity();
    result[0] = x; result[5] = y; result[10] = .0013f;
    return result;
}
NativeShadowBounds Bounds(const Point& low, const Point& high) {
    NativeShadowBounds result;
    for (unsigned axis = 0; axis < 3; ++axis) {
        result.minimum[axis] = static_cast<float>(low[axis]);
        result.maximum[axis] = static_cast<float>(high[axis]);
    }
    return result;
}
NativeShadowBounds Stage() { return Bounds({-1000, -1000, -1000}, {1000, 1000, 1000}); }
bool Near(double actual, double expected) {
    return std::isfinite(actual) && std::isfinite(expected) &&
        std::abs(actual - expected) <= 1e-5 + std::abs(expected) * 2e-6;
}
void ExpectBounds(const NativeShadowBounds& actual, const Point& low,
                  const Point& high, const char* message) {
    Require(FiniteNativeShadowBounds(actual), message);
    for (unsigned axis = 0; axis < 3; ++axis) {
        Require(Near(actual.minimum[axis], low[axis]), message);
        Require(Near(actual.maximum[axis], high[axis]), message);
    }
}
void ExpectEmpty(const NativeShadowBounds& actual, const char* message) {
    Require(!FiniteNativeShadowBounds(actual), message);
    // A failed candidate must discard every corner, not return partial bounds.
    for (unsigned axis = 0; axis < 3; ++axis) {
        Require(actual.minimum[axis] == std::numeric_limits<float>::infinity(), message);
        Require(actual.maximum[axis] == -std::numeric_limits<float>::infinity(), message);
    }
}
Matrix Multiply(const Matrix& left, const Matrix& right) {
    Matrix result{};
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 4; ++column) {
            double value = 0;
            for (unsigned k = 0; k < 4; ++k)
                value += double(left[row * 4 + k]) * right[k * 4 + column];
            result[row * 4 + column] = static_cast<float>(value);
        }
    return result;
}
Point Project(const Matrix& matrix, const Point& point) {
    std::array<double, 4> clip{};
    for (unsigned column = 0; column < 4; ++column) {
        clip[column] = matrix[12 + column];
        for (unsigned row = 0; row < 3; ++row)
            clip[column] += point[row] * matrix[row * 4 + column];
        Require(std::isfinite(clip[column]), "projection produced a non-finite clip coordinate");
    }
    Require(clip[3] > 0, "projection produced non-positive homogeneous W");
    Point result{clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3]};
    for (double coordinate : result)
        Require(std::isfinite(coordinate), "projection division produced a non-finite coordinate");
    return result;
}
bool Inside(const Point& point) {
    return std::abs(point[0]) < 1 && std::abs(point[1]) < 1 && point[2] > 0 && point[2] < 1;
}
Point Corner(const NativeShadowBounds& bounds, unsigned index) {
    const auto point = bounds.Corner(index);
    return {point[0], point[1], point[2]};
}

void CheckFiniteBounds() {
    const auto valid = Bounds({-1, -2, -3}, {4, 5, 6});
    Require(FiniteNativeShadowBounds(valid), "finite ordered bounds rejected");
    Require(FiniteNativeShadowBounds(Bounds({-0., 0, 1}, {0, 0, 1})),
            "zero-width finite bounds rejected");
    ExpectEmpty(NativeShadowBounds{}, "default bounds must be wholly empty");
    for (unsigned axis = 0; axis < 3; ++axis) {
        auto bad = valid;
        bad.minimum[axis] = bad.maximum[axis] + 1;
        Require(!FiniteNativeShadowBounds(bad), "reversed non-X axis accepted");
        for (float value : {std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity()}) {
            bad = valid; bad.minimum[axis] = value;
            Require(!FiniteNativeShadowBounds(bad), "non-finite lower bound accepted");
            bad = valid; bad.maximum[axis] = value;
            Require(!FiniteNativeShadowBounds(bad), "non-finite upper bound accepted");
        }
    }
}

void CheckHandComputedFrusta() {
    const float focus[3]{};
    const auto scene = Stage();
    ExpectBounds(VisibleShadowReceiverBounds(scene, focus, 4, Identity(), Perspective()),
                 {-2, -1, 0}, {2, 1, 4}, "symmetric perspective bounds differ from hand calculation");
    ExpectBounds(VisibleShadowReceiverBounds(scene, focus, 4, Identity(), Orthographic()),
                 {-2, -4, 0}, {2, 4, 4}, "symmetric orthographic bounds differ from hand calculation");

    // Exercise both allowed kinds of XY offset at once. These expected bounds
    // follow directly from the eight endpoint equations, without an inverse.
    auto perspective = Perspective();
    perspective[8] = .25f; perspective[9] = -.5f;
    perspective[12] = .125f; perspective[13] = -.25f;
    ExpectBounds(VisibleShadowReceiverBounds(scene, focus, 4, Identity(), perspective),
                 {-2.5625, -.4375, 0}, {1.4375, 1.5625, 4}, "offset perspective footprint is wrong");
    auto orthographic = Orthographic();
    orthographic[8] = .25f; orthographic[9] = -.5f;
    orthographic[12] = .125f; orthographic[13] = -.25f;
    ExpectBounds(VisibleShadowReceiverBounds(scene, focus, 4, Identity(), orthographic),
                 {-4.25, -3, 0}, {1.75, 13, 4}, "offset orthographic footprint is wrong");

    // Temporal AA adds dx * column-W to column-X, and dy * column-W
    // to column-Y. Perspective and orthographic cameras store it differently.
    for (float dx : {-1.f / 1024, 1.f / 1024})
        for (float dy : {-1.f / 512, 1.f / 512})
            for (bool ortho : {false, true}) {
                auto projection = ortho ? Orthographic() : Perspective();
                for (unsigned row = 0; row < 4; ++row) {
                    projection[row * 4] += dx * projection[row * 4 + 3];
                    projection[row * 4 + 1] += dy * projection[row * 4 + 3];
                }
                if (ortho) {
                    ExpectBounds(VisibleShadowReceiverBounds(scene, focus, 8, Identity(), projection),
                                 {(-1. - dx) * 2, (-1. - dy) * 4, 0},
                                 {(1. - dx) * 2, (1. - dy) * 4, 8},
                                 "orthographic temporal-AA offset ignored");
                } else {
                    ExpectBounds(VisibleShadowReceiverBounds(scene, focus, 8, Identity(), projection),
                                 {(-1. - dx) * 4, (-1. - dy) * 2, 0},
                                 {(1. - dx) * 4, (1. - dy) * 2, 8},
                                 "perspective temporal-AA offset ignored");
                }
            }
    ExpectBounds(VisibleShadowReceiverBounds(scene, focus, 0, Identity(), Perspective()),
                 {0, 0, 0}, {0, 0, 0}, "zero-radius perspective must preserve finite degenerate bounds");
}

void CheckReferenceFrames() {
    // frame includes an XY rotation, unequal scales, shear and translation;
    // view adds another translation. The composed row-vector equations are:
    // vx = 2*y + z + 5, vy = -3*x + y - 7, vz = 4*z + 11.
    const Matrix frame{0, -3, 0, 0, 2, 1, 0, 0, 1, 0, 4, 0, 2, -1, 3, 1};
    auto view = Identity(); view[12] = 3; view[13] = -6; view[14] = 8;
    const auto sceneToView = Multiply(frame, view);
    Require(sceneToView[12] == 5 && sceneToView[13] == -7 && sceneToView[14] == 11,
            "reference-frame fixture is not the intended frame-times-view matrix");
    const float focus[3]{};
    // Radius 2 gives far depth 11 + 2*4 = 19. Hand substitution gives
    // z=(vz-11)/4, y=(vx-z-5)/2, x=(y-vy-7)/3.
    ExpectBounds(VisibleShadowReceiverBounds(Stage(), focus, 2, sceneToView, Orthographic(1, 1)),
                 {-4, -4, -11. / 4}, {-53. / 24, -5. / 8, 2},
                 "affine orthographic reference frame has wrong scene bounds");
    ExpectBounds(VisibleShadowReceiverBounds(Stage(), focus, 2, sceneToView, Perspective(1, 1)),
                 {-13, -13, -11. / 4}, {6, 6, 2},
                 "affine perspective reference frame has wrong scene bounds");

    // An oblique depth column needs all three absolute coefficients. Here
    // vx=x, vy=y, vz=2*x-3*y+4*z+10, so the far depth is 10+2*9=28.
    auto oblique = Identity();
    oblique[2] = 2; oblique[6] = -3; oblique[10] = 4; oblique[14] = 10;
    ExpectBounds(VisibleShadowReceiverBounds(Stage(), focus, 2, oblique, Orthographic(1, 1)),
                 {-1, -1, -15. / 4}, {1, 1, 23. / 4},
                 "affine far depth assumed a rigid reference frame");

    // A small uniform scale is invertible and well conditioned. An absolute
    // determinant cutoff would incorrectly reject these reference frames.
    for (float scale : {1e-8f, 1e-20f}) {
        auto small = Identity(); small[0] = small[5] = small[10] = scale;
        ExpectBounds(VisibleShadowReceiverBounds(Stage(), focus, 1, small, Perspective(1, 1)),
                     {-1, -1, 0}, {1, 1, 1}, "well-conditioned small affine scale rejected");
    }
}

void CheckSceneIntersections() {
    const float focus[3]{};
    const auto projection = Perspective();
    ExpectBounds(VisibleShadowReceiverBounds(Bounds({-1, -.5, 1}, {1, .75, 3}),
                                             focus, 4, Identity(), projection),
                 {-1, -.5, 1}, {1, .75, 3}, "receiver AABB was not clipped on all scene axes");
    for (unsigned axis = 0; axis < 3; ++axis) {
        auto scene = Stage(); scene.minimum[axis] = 100; scene.maximum[axis] = 101;
        ExpectEmpty(VisibleShadowReceiverBounds(scene, focus, 4, Identity(), projection),
                    "empty scene intersection did not discard the whole receiver box");
    }
    auto touching = Stage(); touching.minimum[0] = 2; touching.maximum[0] = 3;
    ExpectBounds(VisibleShadowReceiverBounds(touching, focus, 4, Identity(), projection),
                 {2, -1, 0}, {2, 1, 4}, "touching scene intersection must remain a finite plane");
    for (unsigned axis = 0; axis < 3; ++axis) {
        auto scene = Stage(); scene.minimum[axis] = scene.maximum[axis] + 1;
        ExpectEmpty(VisibleShadowReceiverBounds(scene, focus, 4, Identity(), projection),
                    "invalid scene axis accepted");
        scene = Stage(); scene.maximum[axis] = std::numeric_limits<float>::infinity();
        ExpectEmpty(VisibleShadowReceiverBounds(scene, focus, 4, Identity(), projection),
                    "non-finite scene bounds accepted");
    }
}

void CheckInvalidInputs() {
    const auto scene = Stage(); const float focus[3]{};
    const auto identity = Identity(); const auto perspective = Perspective();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    for (float value : {nan, infinity, -infinity}) {
        for (unsigned index = 0; index < 16; ++index) {
            auto matrix = identity; matrix[index] = value;
            ExpectEmpty(VisibleShadowReceiverBounds(scene, focus, 4, matrix, perspective),
                        "non-finite scene-to-view matrix accepted");
            matrix = perspective; matrix[index] = value;
            ExpectEmpty(VisibleShadowReceiverBounds(scene, focus, 4, identity, matrix),
                        "non-finite projection matrix accepted");
        }
        for (unsigned axis = 0; axis < 3; ++axis) {
            float badFocus[3]{}; badFocus[axis] = value;
            ExpectEmpty(VisibleShadowReceiverBounds(scene, badFocus, 4, identity, perspective),
                        "non-finite focus accepted");
        }
        ExpectEmpty(VisibleShadowReceiverBounds(scene, focus, value, identity, perspective),
                    "non-finite radius accepted");
    }
    ExpectEmpty(VisibleShadowReceiverBounds(scene, focus, -1, identity, perspective), "negative radius accepted");
    for (unsigned index : {3u, 7u, 11u, 15u}) {
        auto matrix = identity; matrix[index] += .125f;
        ExpectEmpty(VisibleShadowReceiverBounds(scene, focus, 4, matrix, perspective),
                    "non-affine scene-to-view matrix accepted");
    }
    for (unsigned index : {1u, 4u, 3u, 7u}) {
        auto projection = perspective; projection[index] = .125f;
        ExpectEmpty(VisibleShadowReceiverBounds(scene, focus, 4, identity, projection),
                    "unsupported projection cross term accepted");
    }
    for (unsigned index : {0u, 5u}) {
        auto projection = perspective; projection[index] = 0;
        ExpectEmpty(VisibleShadowReceiverBounds(scene, focus, 4, identity, projection),
                    "zero projection divisor accepted");
        projection[index] = 1e-7f;
        ExpectEmpty(VisibleShadowReceiverBounds(scene, focus, 4, identity, projection),
                    "near-zero projection divisor accepted");
    }
    for (const auto& w : std::array<std::array<float, 2>, 4>{{{1, 1}, {0, 0}, {-1, 0}, {0, 2}}}) {
        auto projection = perspective; projection[11] = w[0]; projection[15] = w[1];
        ExpectEmpty(VisibleShadowReceiverBounds(scene, focus, 4, identity, projection),
                    "non-canonical projection W accepted");
    }
    for (unsigned diagonal : {0u, 5u, 10u}) {
        auto singular = identity; singular[diagonal] = 0;
        ExpectEmpty(VisibleShadowReceiverBounds(scene, focus, 4, singular, perspective),
                    "singular reference matrix accepted");
    }
    for (float determinant : {-1e-14f, 1e-14f}) {
        auto related = identity;
        related[4] = 1; related[5] = determinant;
        ExpectEmpty(VisibleShadowReceiverBounds(scene, focus, 4, related, perspective),
                    "near-dependent normalized affine matrix accepted");
    }

    const float largest = (std::numeric_limits<float>::max)();
    const auto hugeScene = Bounds({-largest, -largest, -largest}, {largest, largest, largest});
    // Inverse equations are x=vx+vy+vz, y=vy, z=vz. With radius R=.4*FLT_MAX,
    // only the far (+X,+Y) corner exceeds FLT_MAX: x=3R. The other seven
    // corners are finite, so skipping one Include() is an observable failure.
    auto oneOverflow = identity; oneOverflow[4] = -1; oneOverflow[8] = -1;
    ExpectEmpty(VisibleShadowReceiverBounds(hugeScene, focus, largest * .4f,
                                            oneOverflow, Perspective(1, 1)),
                "one unrepresentable corner left a partial receiver box");
    auto overflowingDepth = identity; overflowingDepth[2] = 2; overflowingDepth[6] = -3;
    overflowingDepth[10] = 4;
    ExpectEmpty(VisibleShadowReceiverBounds(hugeScene, focus, largest, overflowingDepth, perspective),
                "unrepresentable far depth accepted");
}

void CheckLegacyAndFailureReceivers() {
    const float focus[3]{}, down[3]{0, -1, 0};
    const auto scene = Stage();
    const auto reference = NativeShadowProjection(focus, down, .1f, 1);
    const auto legacy = FitFocusedShadowProjection(reference, scene, focus, down, .1f, 1, 10);
    Require(legacy == FitFocusedShadowProjection(reference, scene, focus, down, .1f, 1, 10, nullptr),
            "omitted receiver argument changed legacy behavior");
    // Frozen pre-receiver result for this axis-aligned fixture. The lateral
    // half-width is 10*1.05; full-stage depth plus margins spans 2200 units.
    const Matrix expected{2.f / 21, 0, 0, 0, 0, 0, -1.f / 2200, 0,
                          0, 2.f / 21, 0, 0, 0, 0, .5f, 1};
    for (unsigned index = 0; index < 16; ++index) {
        const double tolerance = (std::max)(1e-12, std::abs(double(expected[index])) * 4e-7);
        Require(std::isfinite(legacy[index]) && std::abs(double(legacy[index]) - expected[index]) <= tolerance,
                "legacy no-receiver projection changed");
    }
    NativeShadowBounds empty;
    Require(legacy == FitFocusedShadowProjection(reference, scene, focus, down, .1f, 1, 10, &empty),
            "empty optional receivers changed legacy behavior");
    for (unsigned axis = 0; axis < 3; ++axis) {
        auto invalid = Bounds({-10, -10, -10}, {10, 10, 10});
        invalid.minimum[axis] = invalid.maximum[axis] + 1;
        Require(legacy == FitFocusedShadowProjection(reference, scene, focus, down, .1f, 1, 10, &invalid),
                "reversed optional receiver axis corrupted legacy fitting");
        invalid = Bounds({-10, -10, -10}, {10, 10, 10});
        invalid.maximum[axis] = std::numeric_limits<float>::infinity();
        Require(legacy == FitFocusedShadowProjection(reference, scene, focus, down, .1f, 1, 10, &invalid),
                "non-finite optional receiver axis corrupted legacy fitting");
    }
    const auto compact = Bounds({-1, -1, -1}, {1, 1, 1});
    Require(FitFocusedShadowProjection(reference, compact, focus, down, .1f, 1, 10, &compact) == reference,
            "valid compact scene lost its native sampling grid");
}

void CheckCloseUpAndRemoteCasters() {
    const auto scene = Stage();
    const float focus[3]{-14, 5, -1}, light[3]{-.5f, -1, .5f};
    auto sceneToView = Identity();
    sceneToView[12] = 14; sceneToView[13] = -5; sceneToView[14] = 11;
    const auto projection = Perspective(.5f, .75f);
    const Point receiver{focus[0], focus[1], focus[2]};
    const Point caster{focus[0] - 300. * light[0], focus[1] - 300. * light[1], focus[2] - 300. * light[2]};
    for (int mode : {1, 2}) for (float range : {.1f, .01125f}) {
        constexpr float viewRadius = 25;
        const float radius = (std::max)(viewRadius, 1.f / NativeShadowLateralScale(range, mode));
        const auto receivers = VisibleShadowReceiverBounds(scene, focus, radius, sceneToView, projection);
        Require(FiniteNativeShadowBounds(receivers), "close-up fixture produced no receivers");
        const auto reference = NativeShadowProjection(focus, light, range, mode);
        const auto before = FitFocusedShadowProjection(reference, scene, focus, light, range, mode, viewRadius);
        const auto after = FitFocusedShadowProjection(reference, scene, focus, light, range, mode, viewRadius, &receivers);
        unsigned previouslyOutside = 0;
        for (unsigned corner = 0; corner < 8; ++corner) {
            const auto point = Corner(receivers, corner);
            previouslyOutside += !Inside(Project(before, point));
            Require(Inside(Project(after, point)), "reconstructed close-up receiver clipped after fitting");
            const auto stagePoint = Project(after, Corner(scene, corner));
            Require(stagePoint[2] > 0 && stagePoint[2] < 1, "remote scene depth clipped after receiver expansion");
        }
        Require(previouslyOutside > 0, "close-up fixture did not reproduce old receiver clipping in this mode");
        const auto receiverClip = Project(after, receiver), casterClip = Project(after, caster);
        Require(Inside(receiverClip) && Inside(casterClip), "remote caster or focused receiver was lost");
        Require(std::abs(receiverClip[0] - casterClip[0]) < 1e-5 &&
                std::abs(receiverClip[1] - casterClip[1]) < 1e-5,
                "receiver expansion broke shared light-ray UV coordinates");
        Require(casterClip[2] > 0 && casterClip[2] < receiverClip[2],
                "offscreen caster no longer precedes the receiver in shadow depth");
    }
}
}

int main() {
    try {
        CheckFiniteBounds();
        CheckHandComputedFrusta();
        CheckReferenceFrames();
        CheckSceneIntersections();
        CheckInvalidInputs();
        CheckLegacyAndFailureReceivers();
        CheckCloseUpAndRemoteCasters();
        std::printf("PASS CPU shadow receivers: %u checks\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL CPU shadow receivers: %s\n", error.what());
        return 1;
    }
}
