// MAINTENANCE: regression-keep - bone drags preserve MMD axes and cannot accumulate scale.
// CPU-only regression through the production drag and bone-transform entries.
// No window, device, model loader or user configuration is created or used.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/bone_edit_axes.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>

using namespace mikumikustudio;
namespace {
unsigned checks = 0, failures = 0;
constexpr unsigned steps = 10000;
void Check(bool value, const char* label) {
    ++checks;
    if (!value) ++failures;
    std::printf("%s %s\n", value ? "PASS" : "FAIL", label);
}
void Identity(float* matrix) {
    std::fill_n(matrix, 16, 0.f);
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.f;
}
double Length(const float* value, unsigned count = 4) {
    double sum = 0;
    for (unsigned i = 0; i != count; ++i) sum += double(value[i]) * value[i];
    return std::sqrt(sum);
}
int Delta(unsigned step) {
    constexpr int magnitudes[] = {17, 120, 311, 53};
    return magnitudes[(step / 2) % 4] * (step % 2 ? -1 : 1);
}
bool Near(const float* a, const float* b, unsigned count, double tolerance = .0001) {
    for (unsigned i = 0; i != count; ++i)
        if (!std::isfinite(a[i]) || std::abs(double(a[i]) - b[i]) > tolerance) return false;
    return true;
}

struct Metrics {
    bool finite = true;
    double norm = 0, basis = 0, dot = 0;
    void Basis(const float* matrix) {
        for (unsigned row = 0; row != 3; ++row) {
            double length = Length(matrix + row * 4, 3);
            finite &= std::isfinite(length);
            basis = std::max(basis, std::abs(length - 1));
            for (unsigned other = row + 1; other != 3; ++other) {
                double product = 0;
                for (unsigned k = 0; k != 3; ++k) product += double(matrix[row * 4 + k]) * matrix[other * 4 + k];
                finite &= std::isfinite(product);
                dot = std::max(dot, std::abs(product));
            }
        }
    }
    void Observe(const float* quaternion) {
        const double length = Length(quaternion);
        finite &= std::isfinite(length);
        norm = std::max(norm, std::abs(length - 1));
        // Independent rotation invariants, not a copy of the drag-axis formula.
        d3dx::D3DXMATRIXF matrix{};
        d3dx::Get().matrixRotationQuaternion(&matrix, quaternion);
        Basis(&matrix.m[0][0]);
    }
    void Report() const {
        std::printf("METRICS max_norm_error=%.9g max_basis_length_error=%.9g max_basis_dot=%.9g\n", norm, basis, dot);
        Check(finite && norm <= .00002, "every drag keeps a finite unit quaternion");
        Check(finite && basis <= .0001 && dot <= .0001, "rotation and real bone bases preserve lengths and orthogonality");
    }
};

struct Fixture {
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    mdl::BoneRecord bones[4]{};
    mdl::BoneRecord& bone = bones[0];
    mdl::BoneOrderEntry external{};
    unsigned char selected[4]{1}, dirty[4]{};
    unsigned char* Raw() { return reinterpret_cast<unsigned char*>(model.get()); }
    explicit Fixture(bool pmx) {
        app->state = MMDAppState{};
        model->boneCount = 1; model->boneTable = bones;
        model->boneSelection = selected; model->bonePhysicsState = dirty;
        model->selectedBone = 0; model->physicsMode = pmx ? 2 : 0;
        external.linkedModel = -1; model->boneOrderTable = &external;
        for (auto& b : bones) {
            b.parent = b.slotIndex = -1; b.tailBone = 0;
            b.type = mdl::BoneType::RotateMove; b.flags = mdl::kBoneFlagVisible;
            b.rotQuat[3] = 1;
            Identity(b.matInit); Identity(b.matWorld); Identity(b.matLocal); Identity(b.matExtra);
        }
        app->ModelSlot(0) = Raw(); app->SetSelectedModelSlot(0);
        app->CameraPitch() = 0; app->CameraYaw() = .7f; app->CameraRoll() = 0;
        app->InteractionDragMode() = static_cast<ViewportDragMode>(2);
    }
    ~Fixture() { bone_scale::Forget(Raw()); app->ModelSlot(0) = nullptr; }
    void Drag(int pixels) {
        app->state.previousMouseX = app->state.previousMouseY = 500;
        app->state.mouseX = app->state.mouseY = 500 - pixels;
        if (static_cast<int>(app->InteractionDragMode()) == 3) {
            app->state.dragOriginX = app->state.dragOriginY = 500;
            app->state.previousMouseX = 700; app->state.mouseX = 700 + std::abs(pixels);
            app->state.previousMouseY = app->state.mouseY = 600;
            if (pixels < 0) std::swap(app->state.previousMouseX, app->state.mouseX);
        }
        BoneEditModes(app.get());
    }
    void Evaluate() {
        // Supply the transform's pre-physics pose inputs; no physics/IK is active.
        for (unsigned i = 0; i != model->boneCount; ++i) {
            std::memcpy(bones[i].physicsQuat, bones[i].rotQuat, sizeof(bone.rotQuat));
            std::memcpy(bones[i].physicsOffset, bones[i].trans, sizeof(bone.trans));
        }
        BoneFrameTransform(Raw(), 0, 0, nullptr, 0);
    }
};

void Stress(bool pmx, int mode, int coordinates = 0, bool direct = false) {
    Fixture f(pmx);
    f.app->InteractionDragMode() = static_cast<ViewportDragMode>(mode);
    f.app->state.coordinateSystem = coordinates;
    f.app->state.physicsEditorJointPage = direct ? 1 : 0;
    if (mode != 2) { f.app->CameraPitch() = .41f; f.app->CameraRoll() = -.23f; }
    if (pmx) {
        const float axes[] = {2, 1, 0, 0, 0, 3};
        std::memcpy(f.bone.localAxes, axes, sizeof(axes));
    }
    Metrics metrics;
    bool moved = false;
    for (unsigned step = 0; step != steps; ++step) {
        // Exercise the direct branch with a non-unit inherited basis.
        if (direct) { f.bone.matWorld[0] = 2; f.bone.matWorld[5] = .5f; f.bone.matWorld[10] = 3; }
        f.Drag(Delta(step)); metrics.Observe(f.bone.rotQuat);
        moved |= Length(f.bone.rotQuat, 3) > .005;
        f.Evaluate(); metrics.Basis(f.bone.matInit);
    }
    std::printf("STRESS format=%s mode=%d coordinates=%d direct=%d steps=%u\n", pmx ? "PMX" : "PMD", mode, coordinates, direct, steps);
    Check(moved && f.dirty[0] == 1, "real drag rotates and marks the bone dirty");
    metrics.Report();
    Check(Length(f.bone.rotQuat, 3) <= .001 && std::abs(std::abs(double(f.bone.rotQuat[3])) - 1) <= .00002,
          "paired drags return to the starting orientation");
}

void VerticalHandleParity(bool pmx) {
    // MMD 9.32 x64, 0x14003319A..0x1400332BD: the vertical handle
    // uses the positive yaw's yaw-only right axis, independent of pitch/roll
    // and the Local/Global/View selector. Explicit directions also catch a
    // yaw-sign regression that a unit-length/round-trip test cannot detect.
    constexpr float pi = 3.14159265358979323846f;
    const struct {
        const char* name;
        float pitch, yaw, roll, world[3];
    } cases[] = {
        {"front", 0, 0, 0, {1, 0, 0}},
        {"side-positive", 0, pi / 2, 0, {0, 0, 1}},
        {"side-negative", 0, -pi / 2, 0, {0, 0, -1}},
        {"rear", 0, pi, 0, {-1, 0, 0}},
        {"front-pitched", pi / 2, 0, 0, {1, 0, 0}},
        {"side-pitched", .73f, pi / 2, -.23f, {0, 0, 1}},
        {"oblique-positive", .41f, .643501109f, -.23f, {.8f, 0, .6f}},
        {"oblique-negative", -.9f, -.643501109f, .6f, {.8f, 0, -.6f}},
    };
    auto& api = d3dx::Get();
    for (const auto& c : cases) {
        double maxError = 0;
        bool matches = true, returns = true;
        unsigned samples = 0;
        for (int coordinates = 0; coordinates != 3; ++coordinates)
        for (bool rotatedParent : {false, true})
        for (int speed = 0; speed != 3; ++speed)
        for (int pixels : {-80, 80}) {
            Fixture f(pmx);
            f.app->CameraPitch() = c.pitch;
            f.app->CameraYaw() = c.yaw;
            f.app->CameraRoll() = c.roll;
            f.app->state.coordinateSystem = coordinates;
            f.app->state.shiftModifierState = speed == 1 ? 3 : 0;
            f.app->state.ctrlModifierState = speed == 2 ? 3 : 0;
            d3dx::D3DXMATRIXF parent{}, inverse{};
            Identity(&parent.m[0][0]);
            if (rotatedParent) {
                d3dx::D3DXMATRIXF rx{}, rz{};
                api.rotX(&rx, .61f); api.rotZ(&rz, -.37f);
                api.multiply(&parent, &rx, &rz);
                // A non-identity starting pose also checks composition order.
                f.bone.rotQuat[2] = .6f; f.bone.rotQuat[3] = .8f;
            }
            std::memcpy(f.bone.matWorld, &parent, sizeof(parent));
            api.inverse(&inverse, nullptr, &parent);
            float local[4]{}, before[4], expected[4];
            api.vec3Transform(local, c.world, &inverse);
            std::memcpy(before, f.bone.rotQuat, sizeof(before));
            // MMD uses 0.01 radians per pixel (0.005 in the quaternion).
            const double halfAngle = pixels * .005 * (speed == 1 ? 5 : speed == 2 ? .1 : 1);
            const float delta[] = {float(local[0] * std::sin(halfAngle)),
                float(local[1] * std::sin(halfAngle)), float(local[2] * std::sin(halfAngle)),
                float(std::cos(halfAngle))};
            api.quatMultiply(expected, before, delta);
            f.Drag(pixels);
            matches &= Near(f.bone.rotQuat, expected, 4, .000002) && f.dirty[0] == 1;
            for (unsigned i = 0; i != 4; ++i)
                maxError = std::max(maxError, std::abs(double(f.bone.rotQuat[i]) - expected[i]));
            f.Drag(-pixels);
            returns &= Near(f.bone.rotQuat, before, 4, .000002);
            ++samples;
        }
        std::printf("VERTICAL_PARITY format=%s view=%s samples=%u max_quaternion_error=%.9g\n",
                    pmx ? "PMX" : "PMD", c.name, samples, maxError);
        Check(matches, "vertical handle matches the MMD axis, sign and angular speed");
        Check(returns, "opposite vertical drag restores the starting orientation");
    }
}

double Distance(const mdl::BoneRecord& a, const mdl::BoneRecord& b) {
    float pa[4]{}, pb[4]{};
    auto& api = d3dx::Get();
    api.vec3Transform(pa, a.position, reinterpret_cast<const d3dx::D3DXMATRIXF*>(a.matInit));
    api.vec3Transform(pb, b.position, reinterpret_cast<const d3dx::D3DXMATRIXF*>(b.matInit));
    for (unsigned k = 0; k != 3; ++k) pa[k] -= pb[k];
    return Length(pa, 3);
}
void SelectedRoots(bool pmx) {
    Fixture f(pmx); f.model->boneCount = 4; f.selected[1] = 1;
    const float rest[4][3] = {{2,-1,3}, {5,3,3}, {5,3,5}, {-7,2,9}};
    for (unsigned i = 0; i != 4; ++i) std::memcpy(f.bones[i].position, rest[i], sizeof(rest[i]));
    f.bones[1].type = mdl::BoneType::Move; f.bones[2].parent = 1;
    f.Evaluate();
    const auto untouched = f.bones[3];
    // The selected secondary root may already contain drift from an old drag.
    f.bones[1].rotQuat[3] = 53.329071f;
    Metrics metrics;
    double maxRadiusError = 0, maxChildError = 0;
    bool moved = false, finite = true;
    for (unsigned step = 0; step != steps; ++step) {
        f.Drag(Delta(step)); f.Evaluate();
        metrics.Observe(f.bone.rotQuat); metrics.Observe(f.bones[1].rotQuat);
        metrics.Basis(f.bones[1].matInit); metrics.Basis(f.bones[2].matInit);
        const double radius = Distance(f.bone, f.bones[1]), child = Distance(f.bones[1], f.bones[2]);
        finite &= std::isfinite(radius) && std::isfinite(child);
        maxRadiusError = std::max(maxRadiusError, std::abs(radius - 5));
        maxChildError = std::max(maxChildError, std::abs(child - 2));
        moved |= Length(f.bones[1].trans, 3) > .01;
    }
    std::printf("ORBIT format=%s steps=%u max_radius_error=%.9g max_child_radius_error=%.9g\n", pmx ? "PMX" : "PMD", steps, maxRadiusError, maxChildError);
    metrics.Report();
    Check(moved && finite && maxRadiusError < .005 && maxChildError < .0001, "selected root orbits without scaling; unselected child retains its radius");
    Check(Length(f.bones[1].trans, 3) < .005, "paired root orbits return to the starting location");
    Check(f.dirty[0] == 1 && f.dirty[1] == 1 && f.dirty[2] == 0 && f.dirty[3] == 0 &&
          std::memcmp(&untouched, &f.bones[3], sizeof(untouched)) == 0, "only selected roots are edited; unrelated root remains byte-identical");
}

void FingerPose(Fixture& f, int mode, int coordinates, bool chain, bool fixed) {
    f.model->boneCount = 4;
    f.app->InteractionDragMode() = static_cast<ViewportDragMode>(mode);
    f.app->state.coordinateSystem = coordinates;
    f.app->CameraPitch() = .41f; f.app->CameraRoll() = -.23f;
    const float positions[4][3] = {{1,2,3}, {3,5,4}, {4,8,6}, {-5,4,2}};
    const float axes[3][6] = {{1,0,0, 0,0,1}, {0,1,0, 0,0,1}, {1,1,0, 0,0,1}};
    for (unsigned i = 0; i != 4; ++i) {
        auto& bone = f.bones[i];
        f.selected[i] = i < 3;
        bone.parent = chain && i > 0 && i < 3 ? static_cast<int>(i - 1) : -1;
        bone.tailBone = (i + 1) % 4;
        std::memcpy(bone.position, positions[i], sizeof(bone.position));
        bone.trans[0] = .1f * i; bone.trans[1] = -.2f * i; bone.trans[2] = .3f * i;
        bone.rotQuat[i % 3] = std::sin(.13f * (i + 1));
        bone.rotQuat[3] = std::cos(.13f * (i + 1));
        d3dx::D3DXMATRIXF parent{}, pose{}, world{};
        d3dx::Get().rotY(&parent, .31f * i);
        d3dx::Get().matrixRotationQuaternion(&pose, bone.rotQuat);
        d3dx::Get().multiply(&world, &pose, &parent);
        std::memcpy(bone.matWorld, &parent, sizeof(parent));
        std::memcpy(bone.matInit, &world, sizeof(world));
        if (i < 3) {
            if (f.model->physicsMode == 2)
                std::memcpy(bone.localAxes, axes[i], sizeof(bone.localAxes));
            else
                std::memcpy(bone.name, "\x8e\x77", 3); // PMD finger local-axis convention.
        }
    }
    if (fixed) {
        f.bones[1].type = mdl::BoneType::FixedAxis;
        f.bones[2].type = mdl::BoneType::UnderIk;
        for (unsigned i : {1u, 2u}) {
            f.bones[i].flags |= mdl::kBoneFlagFixedAxis;
            f.bones[i].axis[i - 1] = 2.f;
        }
    }
}

void MultiSelectionRotations(bool pmx) {
    for (int mode = 1; mode <= 6; ++mode)
    for (int coordinates = 0; coordinates != 3; ++coordinates) {
        bool matches = true, moved = true, preserved = true, noOp = true;
        double maxError = 0;
        for (bool chain : {false, true})
        for (bool fixed : {false, true})
        for (int active : {0, 2}) {
            Fixture group(pmx);
            FingerPose(group, mode, coordinates, chain, fixed);
            group.model->selectedBone = active;
            mdl::BoneRecord before[4];
            std::memcpy(before, group.bones, sizeof(before));
            group.Drag(43);
            for (unsigned i = 0; i != 3; ++i) {
                // Apply the same event with this bone selected alone. This
                // checks each bone's own coordinate frame and fixed-axis rule
                // without reproducing the production quaternion calculation.
                Fixture single(pmx);
                FingerPose(single, mode, coordinates, chain, fixed);
                std::fill_n(single.selected, 4, 0);
                single.selected[i] = 1; single.model->selectedBone = i;
                single.Drag(43);
                matches &= Near(group.bones[i].rotQuat, single.bones[i].rotQuat, 4, .000002);
                for (unsigned k = 0; k != 4; ++k)
                    maxError = std::max(maxError, std::abs(double(group.bones[i].rotQuat[k]) - single.bones[i].rotQuat[k]));
                moved &= !Near(group.bones[i].rotQuat, before[i].rotQuat, 4, .000002) && group.dirty[i] == 1;
                preserved &= std::memcmp(group.bones[i].trans, before[i].trans, sizeof(before[i].trans)) == 0;
            }
            preserved &= std::memcmp(&group.bones[3], &before[3], sizeof(before[3])) == 0 && group.dirty[3] == 0;
            preserved &= group.model->selectedBone == active && group.selected[0] == 1 && group.selected[1] == 1 && group.selected[2] == 1 && group.selected[3] == 0;
            Fixture still(pmx);
            FingerPose(still, mode, coordinates, chain, fixed);
            still.model->selectedBone = active;
            std::memcpy(before, still.bones, sizeof(before));
            still.Drag(0);
            noOp &= std::memcmp(still.bones, before, sizeof(before)) == 0 &&
                    std::all_of(still.dirty, still.dirty + 4, [](unsigned char dirty) { return dirty == 0; });
        }
        std::printf("MULTI_ROTATE format=%s mode=%d coordinates=%d max_single_bone_error=%.9g\n",
                    pmx ? "PMX" : "PMD", mode, coordinates, maxError);
        Check(matches && moved, "every selected rotation-only joint matches its own single-bone edit, including selected descendants");
        Check(preserved, "rotation-only edits preserve translations, unselected bones and the selection");
        Check(noOp, "zero drag leaves the complete multi-selection pose and dirty flags untouched");
    }
}

void MixedSelectionRotations(bool pmx) {
    for (auto movable : {mdl::BoneType::Move, mdl::BoneType::Ik}) {
        Fixture group(pmx), legacy(pmx), single(pmx);
        for (auto* f : {&group, &legacy, &single}) {
            FingerPose(*f, 5, 1, false, false);
            f->bone.type = mdl::BoneType::Move;
            f->bones[1].type = movable;
            f->bones[2].parent = 0; // selected rotation joint under a selected movable parent
        }
        legacy.selected[2] = 0;
        single.selected[0] = single.selected[1] = 0;
        single.model->selectedBone = 2;
        const auto root = group.bones[1], finger = group.bones[2], untouched = group.bones[3];
        group.Drag(43); legacy.Drag(43); single.Drag(43);
        bool sameLegacy = true;
        for (unsigned i : {0u, 1u})
            sameLegacy &= Near(group.bones[i].rotQuat, legacy.bones[i].rotQuat, 4) &&
                          Near(group.bones[i].trans, legacy.bones[i].trans, 3) && group.dirty[i] == legacy.dirty[i];
        Check(sameLegacy && group.dirty[1] == 1 && !Near(group.bones[1].trans, root.trans, 3),
              "mixed selection preserves Move/IK selected-root orbit behavior");
        Check(Near(group.bones[2].rotQuat, single.bones[2].rotQuat, 4) && group.dirty[2] == 1 &&
              std::memcmp(group.bones[2].trans, finger.trans, sizeof(finger.trans)) == 0 &&
              std::memcmp(&group.bones[3], &untouched, sizeof(untouched)) == 0,
              "mixed selection also rotates the selected finger in place and preserves unrelated bones");
    }
}

void MultiSelectionUndo(bool pmx) {
    Fixture f(pmx);
    FingerPose(f, 4, 0, true, false);
    mdl::BoneRecord before[4], edited[4];
    std::memcpy(before, f.bones, sizeof(before));
    PushBoneEditUndo(f.app.get());
    const int slot = static_cast<int>(f.model->undoState[0]);
    Check(f.model->undoRings[0].slots[slot].dirty == 3, "one undo snapshot captures all selected finger joints");
    f.Drag(43);
    std::memcpy(edited, f.bones, sizeof(edited));
    std::int32_t frame = 0;
    UndoModelEdit(f.Raw(), &frame, pmx ? 2 : 0);
    bool restored = true;
    for (unsigned i = 0; i != 4; ++i)
        restored &= Near(f.bones[i].rotQuat, before[i].rotQuat, 4) &&
                    Near(f.bones[i].trans, before[i].trans, 3) && f.dirty[i] == 0;
    Check(restored, "one undo restores the entire multi-joint pose and dirty flags");
    RedoModelEdit(f.Raw(), &frame, pmx ? 2 : 0);
    restored = true;
    for (unsigned i = 0; i != 4; ++i)
        restored &= Near(f.bones[i].rotQuat, edited[i].rotQuat, 4) &&
                    Near(f.bones[i].trans, edited[i].trans, 3) && f.dirty[i] == (i < 3 ? 1 : 0);
    Check(restored, "one redo reapplies the entire multi-joint pose and dirty flags");
    for (auto& ring : f.model->undoRings)
        for (auto& record : ring.slots) {
            std::free(record.bonePose);
            record.bonePose = nullptr;
        }
}

void RecoveryAndGuards(bool pmx) {
    std::printf("BOUNDARIES format=%s\n", pmx ? "PMX" : "PMD");
    const float huge = std::numeric_limits<float>::max() / 4;
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    for (float factor : {53.329071f, huge, 0.f, nan, inf, -1.f}) {
        Fixture f(pmx), reference(pmx);
        const bool invalid = factor == 0 || !std::isfinite(factor);
        const float unit[] = {0, 0, .6f, .8f};
        if (!invalid) for (unsigned k = 0; k != 4; ++k) reference.bone.rotQuat[k] = unit[k] * (factor < 0 ? -1 : 1);
        for (unsigned k = 0; k != 4; ++k) f.bone.rotQuat[k] = unit[k] * factor;
        reference.Drag(120); f.Drag(120); f.Evaluate();
        Metrics metrics; metrics.Observe(f.bone.rotQuat); metrics.Basis(f.bone.matInit);
        std::printf("RECOVERY factor=%.9g invalid=%d\n", factor, invalid);
        Check(metrics.finite && metrics.norm < .00002 && metrics.basis < .0001 &&
              Near(f.bone.rotQuat, reference.bone.rotQuat, 4), "old drift preserves orientation; invalid source restarts at identity before this drag");
    }
    Fixture noMovement(pmx); noMovement.bone.rotQuat[3] = 3;
    const auto before = noMovement.bone;
    noMovement.Drag(0);
    Check(std::memcmp(&before, &noMovement.bone, sizeof(before)) == 0 && noMovement.dirty[0] == 0,
          "zero mouse delta does not normalize, mutate or mark the bone dirty");
    for (float bad : {0.f, nan, inf}) {
        Fixture f(pmx); f.model->boneCount = 2;
        f.bone.type = mdl::BoneType::FixedAxis; f.bone.flags |= mdl::kBoneFlagFixedAxis;
        f.bone.tailBone = 1; f.bone.axis[0] = bad; f.bones[1].position[0] = bad;
        const auto saved = f.bone;
        f.Drag(120);
        Check(std::memcmp(&saved, &f.bone, sizeof(saved)) == 0 && f.dirty[0] == 0 && f.dirty[1] == 0,
              "zero or non-finite fixed axis cannot corrupt pose or dirty flags");
    }
    if (pmx) {
        Fixture f(true); f.bone.type = mdl::BoneType::FixedAxis; f.bone.flags |= mdl::kBoneFlagFixedAxis;
        f.bone.axis[0] = huge; f.bone.axis[1] = huge / 2;
        f.Drag(120);
        Check(std::abs(Length(f.bone.rotQuat) - 1) < .00002 && f.dirty[0] == 1, "large finite authored axis remains a usable unit rotation");
    }
}

void HudAxisQueries(bool pmx) {
    Fixture f(pmx);
    std::memcpy(f.bone.name, "\x8e\x77", 3);
    for (int tail : {-1, 0, 1}) {
        f.bone.tailBone = tail;
        const auto before = f.bone;
        float world[3]{};
        Check(BoneRotationAxisWorld(f.app.get(), f.Raw(), 0, 0, world) &&
              world[0] == 1.0f && world[1] == 0.0f && world[2] == 0.0f &&
              std::memcmp(&before, &f.bone, sizeof(before)) == 0 && f.dirty[0] == 0,
              "HUD hover safely falls back for a missing or degenerate finger tail without editing its pose");
    }
    f.bone.type = mdl::BoneType::FixedAxis;
    f.bone.flags |= mdl::kBoneFlagFixedAxis;
    f.bone.axis[0] = 1;
    f.bone.tailBone = -1;
    const auto before = f.bone;
    float world[3]{};
    Check(BoneRotationAxisWorld(f.app.get(), f.Raw(), 0, 2, world) == pmx &&
          std::memcmp(&before, &f.bone, sizeof(before)) == 0 && f.dirty[0] == 0,
          "HUD uses a PMX fixed axis without a tail and rejects a missing PMD fixed-axis tail");
    Check(!BoneRotationAxisWorld(f.app.get(), f.Raw(), 1, 0, world) &&
          !BoneRotationAxisWorld(f.app.get(), f.Raw(), 0, 3, world) &&
          world[0] == 0 && world[1] == 0 && world[2] == 0,
          "HUD rejects stale bone indices and invalid axes with an empty direction");
}

void PreserveStretch() {
#if !MMD_ORIGINAL_PROFILE
    Fixture f(true);
    const bone_scale::Matrix stretch{{2,0,0, 0,.75f,0, 0,0,1.5f}};
    Check(bone_scale::SetPose(f.Raw(), 0, stretch), "full profile accepts a valid authored nonuniform stretch");
    const auto revision = bone_scale::Revision(f.Raw());
    f.Evaluate(); bone_scale::ApplyTransforms(f.Raw());
    Metrics metrics;
    double maxStretchError = 0;
    bool stable = true;
    for (unsigned step = 0; step != steps; ++step) {
        f.Drag(Delta(step)); metrics.Observe(f.bone.rotQuat);
        f.Evaluate(); bone_scale::ApplyTransforms(f.Raw());
        for (unsigned row = 0; row != 3; ++row) {
            const double length = Length(f.bone.matInit + row * 4, 3);
            stable &= std::isfinite(length);
            maxStretchError = std::max(maxStretchError, std::abs(length - stretch.v[row * 3 + row]));
        }
        stable &= bone_scale::Pose(f.Raw(), 0) == stretch && bone_scale::Revision(f.Raw()) == revision;
    }
    std::printf("STRETCH steps=%u max_authored_length_error=%.9g\n", steps, maxStretchError);
    metrics.Report();
    Check(stable && maxStretchError < .0002, "rotation normalization preserves authored stretch values, revision and actual scaled bone lengths");
#endif
}
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto& api = d3dx::Get();
    Check(api.Load() && api.matrixRotationQuaternion && api.vec3Transform, "load CPU transform dependencies");
    if (failures) return 1;
    for (bool pmx : {false, true}) {
        VerticalHandleParity(pmx);
        for (int mode = 1; mode <= 3; ++mode) Stress(pmx, mode);
        for (int mode = 4; mode <= 6; ++mode) {
            for (int coordinates = 0; coordinates != 3; ++coordinates) Stress(pmx, mode, coordinates);
            Stress(pmx, mode, 0, true);
        }
        SelectedRoots(pmx); RecoveryAndGuards(pmx);
        MultiSelectionRotations(pmx); MixedSelectionRotations(pmx); MultiSelectionUndo(pmx);
        HudAxisQueries(pmx);
    }
    PreserveStretch();
    std::printf("RESULT profile=%s checks=%u failures=%u windows_created=0 devices_created=0\n",
                MMD_ORIGINAL_PROFILE ? "mmd932" : "full", checks, failures);
    return failures ? 1 : 0;
}
