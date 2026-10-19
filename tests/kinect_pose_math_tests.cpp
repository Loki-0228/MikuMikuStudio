// Exercise the real Kinect pose math with the existing CPU D3DX runtime.
// No sensor, graphics device, window or captured skeleton is required.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/model.hpp"

namespace mikumikustudio {
float* BuildLookAtQuaternion(float[4], const float[4], float, float, float,
                            float, float, float, unsigned char);
void InitStandardSkeletonQuats(unsigned char*, unsigned char);
}

namespace {
using namespace mikumikustudio;
int checks = 0, failures = 0;
void Check(bool ok, const char* text) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", text); }
}
bool Unit(const float q[4]) {
    double length = 0;
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(q[i])) return false;
        length += double(q[i]) * q[i];
    }
    return std::fabs(length - 1) < 0.0001;
}
bool Identity(const float q[4]) { return q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 1; }
bool SameRotation(const float a[4], const float b[4]) {
    // Both quaternion signs express the same rotation.
    double same = 0, opposite = 0;
    for (int i = 0; i < 4; ++i) {
        same += double(a[i] - b[i]) * (a[i] - b[i]);
        opposite += double(a[i] + b[i]) * (a[i] + b[i]);
    }
    return same < 1e-8 || opposite < 1e-8;
}
void LookAt() {
    const float identity[4] = {0, 0, 0, 1};
    const std::array<std::array<float, 3>, 9> directions{{
        {0, 0, 0}, {0, 1, 0}, {0, -1, 0}, {1e-12f, 1, 0},
        {1e-30f, 1, 0}, {-1e-30f, -1, 0}, {1, 2, 3}, {-2, 3, -4}, {1, 0, 0}
    }};
    for (unsigned char mode = 0; mode < 6; ++mode) {
        for (const auto& direction : directions) {
            float q[4] = {99, 99, 99, 99};
            Check(BuildLookAtQuaternion(q, identity, 0, 0, 0,
                      direction[0], direction[1], direction[2], mode) == q,
                  "look-at returns the caller-owned quaternion array");
            Check(Unit(q), "ordinary, coincident, vertical and near-vertical directions produce finite unit rotations");
            if (direction == std::array<float, 3>{0, 0, 0})
                Check(Identity(q), "coincident tracked joints contribute identity rotation");
            if (mode >= 4 && direction[0] == 0 && direction[2] == 0)
                Check(std::fabs(q[3]) == 1, "vertical yaw-only direction has deterministic zero yaw");
            if (mode >= 4 && direction[0] != 0 && std::fabs(direction[0]) < 1e-20f) {
                float horizontal[4];
                BuildLookAtQuaternion(horizontal, identity, 0, 0, 0, direction[0], 0, direction[2], mode);
                Check(Unit(horizontal) && SameRotation(q, horizontal),
                      "near-vertical yaw keeps the nonzero horizontal direction without float-square underflow");
            }
        }
        float origin[4], translated[4];
        BuildLookAtQuaternion(origin, identity, 0, 0, 0, 1, 2, 3, mode);
        BuildLookAtQuaternion(translated, identity, 3, 5, 8, 4, 7, 11, mode);
        Check(SameRotation(origin, translated), "look-at depends on joint direction rather than absolute position");
        const float invalidQuaternions[][4] = {
            {0, 0, 0, 0}, {0, std::numeric_limits<float>::infinity(), 0, 1},
            {std::numeric_limits<float>::quiet_NaN(), 0, 0, 1}
        };
        for (const auto& invalid : invalidQuaternions) {
            float q[4];
            BuildLookAtQuaternion(q, invalid, 0, 0, 0, 1, 2, 3, mode);
            Check(Identity(q), "invalid source orientation cannot introduce nonfinite pose rotations");
        }
        for (const float invalid : {std::numeric_limits<float>::infinity(),
                                    std::numeric_limits<float>::quiet_NaN()}) {
            float q[4];
            BuildLookAtQuaternion(q, identity, 0, 0, 0, invalid, 2, 3, mode);
            Check(Identity(q), "invalid joint direction contributes identity instead of NaN");
        }
    }
    float halfTurn[4];
    BuildLookAtQuaternion(halfTurn, identity, 0, 0, 0, 1, 0, 0, 4);
    Check(std::fabs(halfTurn[1]) > 0.99999f && std::fabs(halfTurn[3]) < 1e-5f,
          "ordinary yaw retains the original half-turn convention for positive X");
}

std::unique_ptr<mdl::ModelRecord> EmptySkeleton() {
    auto model = std::make_unique<mdl::ModelRecord>();
    for (int i = 0; i < 17; ++i) model->localTransforms[i * 4 + 3] = 1;
    for (auto& point : model->standardBonePositions) point[1] = -999;
    return model;
}
void Skeleton() {
    // Exercise all producer/consumer blocks, including unavailable-parent
    // scratch rotations, rather than just testing an isolated helper.
    for (const int geometry : {0, 1, 2}) {
        auto model = EmptySkeleton();
        for (int i = 0; i < 24; ++i) {
            model->standardBonePositions[i][0] = geometry == 2 ? i * 1e-12f : 0;
            model->standardBonePositions[i][1] = geometry ? float(i) : 0;
            model->standardBonePositions[i][2] = 0;
        }
        InitStandardSkeletonQuats(reinterpret_cast<unsigned char*>(model.get()), 1);
        for (int i = 0; i < 17; ++i)
            Check(Unit(model->localTransforms + i * 4),
                  "complete coincident, vertical and near-vertical skeletons keep all 17 rotations finite and normalized");
    }

    auto model = EmptySkeleton();
    const float positions[4][3] = {{2, 3, 4}, {-2, 3, 4}, {0, 5, 4}, {0, 5, 4}};
    const int indices[] = {4, 8, 20, 21};
    for (int i = 0; i < 4; ++i)
        std::memcpy(model->standardBonePositions[indices[i]], positions[i], sizeof positions[i]);
    InitStandardSkeletonQuats(reinterpret_cast<unsigned char*>(model.get()), 1);
    const float halfSqrt = std::sqrt(0.5f);
    const float expectedLeft[4] = {0, 0, halfSqrt, halfSqrt};
    const float expectedRight[4] = {0, 0, -halfSqrt, halfSqrt};
    Check(Unit(model->localTransforms + 64) && SameRotation(model->localTransforms + 64, expectedLeft),
          "real contiguous normalization yields the left shoulder's positive quarter turn around Z");
    Check(Unit(model->localTransforms + 60) && SameRotation(model->localTransforms + 60, expectedRight),
          "real contiguous normalization yields the right shoulder's negative quarter turn around Z");

    std::memcpy(model->standardBonePositions[20], positions[1], sizeof positions[1]);
    InitStandardSkeletonQuats(reinterpret_cast<unsigned char*>(model.get()), 1);
    const auto* opposite = model->localTransforms + 64;
    Check(Unit(opposite), "opposite shoulder directions yield a finite unit half turn");
    d3dx::D3DXMATRIXF matrix{};
    float transformed[4]{};
    const float axis[3] = {1, 0, 0};
    d3dx::Get().matrixRotationQuaternion(&matrix, opposite);
    d3dx::Get().vec3Transform(transformed, axis, &matrix);
    Check(std::fabs(transformed[0] + 1) < 1e-5f && std::fabs(transformed[1]) < 1e-5f &&
          std::fabs(transformed[2]) < 1e-5f,
          "opposite-vector fallback rotates the source toward the target, rather than returning identity");

    std::memcpy(model->standardBonePositions[20], positions[0], sizeof positions[0]);
    InitStandardSkeletonQuats(reinterpret_cast<unsigned char*>(model.get()), 1);
    Check(Identity(model->localTransforms + 64), "parallel shoulder vectors produce identity rotation");
    const float midpoint[3] = {0, 3, 4};
    std::memcpy(model->standardBonePositions[20], midpoint, sizeof midpoint);
    InitStandardSkeletonQuats(reinterpret_cast<unsigned char*>(model.get()), 1);
    Check(Identity(model->localTransforms + 64), "coincident shoulder target contributes identity rotation");

    // The consumer can exist while the upper-arm producer is absent. Its
    // product must use an identity scratch quaternion, never zero/stack data.
    auto partial = EmptySkeleton();
    const float arm[3][3] = {{1, 2, 0}, {1, 3, 0}, {1, 4, 0}};
    for (int i = 0; i < 3; ++i)
        std::memcpy(partial->standardBonePositions[4 + i], arm[i], sizeof arm[i]);
    InitStandardSkeletonQuats(reinterpret_cast<unsigned char*>(partial.get()), 1);
    for (int i = 0; i < 17; ++i)
        Check(Unit(partial->localTransforms + i * 4), "missing producer joints cannot poison later pose quaternion products");
}
}

int main() {
    if (!d3dx::Get().Load()) {
        std::fprintf(stderr, "D3DX CPU math runtime is unavailable\n");
        return 2;
    }
    LookAt();
    Skeleton();
    std::printf("kinect_pose_math_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
