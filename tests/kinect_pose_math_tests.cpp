// Exercise the actual pose initializers with D3DX math, without Kinect hardware,
// a renderer or a graphics device.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/model.hpp"
namespace mikumikustudio {
float* BuildLookAtQuaternion(float[4], const float[4], float, float, float, float, float, float, unsigned char);
void InitStandardSkeletonQuats(unsigned char*, unsigned char);
}
namespace {
using namespace mikumikustudio;
int checks = 0, failures = 0;
void Check(bool ok, const char* text) {
    ++checks; if (!ok) { ++failures; std::fprintf(stderr,"FAIL: %s\n",text); }
}
bool Unit(const float q[4]) {
    float length = 0;
    for (int i = 0; i < 4; ++i) { if (!std::isfinite(q[i])) return false; length += q[i] * q[i]; }
    return std::fabs(length - 1) < 0.0001f;
}
void LookAt() {
    const float identity[4] = {0,0,0,1};
    for (unsigned char mode = 0; mode < 6; ++mode) {
        for (const auto direction : {std::array<float,3>{0,0,0}, {0,1,0}, {0,-1,0},
                {1e-12f,1,0}, {1,2,3}, {-2,3,-4}}) {
            float q[4] = {99,99,99,99};
            BuildLookAtQuaternion(q,identity,0,0,0,direction[0],direction[1],direction[2],mode);
            Check(Unit(q),"ordinary, coincident, vertical and near-vertical look-at produce a finite unit rotation");
            if (direction == std::array<float,3>{0,0,0})
                Check(q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 1,
                      "coincident tracked joints contribute identity rotation");
            if (mode >= 4 && direction[0] == 0 && direction[2] == 0)
                Check(std::fabs(q[3]) == 1,"vertical yaw-only direction has a deterministic zero yaw");
        }
    }
}
std::unique_ptr<mdl::ModelRecord> EmptySkeleton() {
    auto model = std::make_unique<mdl::ModelRecord>();
    for (int i = 0; i < 17; ++i) model->localTransforms[i * 4 + 3] = 1;
    for (auto& point : model->standardBonePositions) point[1] = -999;
    return model;
}
void Skeleton() {
    for (const bool vertical : {false,true}) {
        auto model = EmptySkeleton();
        for (int i = 0; i < 24; ++i) {
            model->standardBonePositions[i][0] = 0;
            model->standardBonePositions[i][1] = vertical ? float(i) : 0;
            model->standardBonePositions[i][2] = 0;
        }
        InitStandardSkeletonQuats(reinterpret_cast<unsigned char*>(model.get()),1);
        for (int i = 0; i < 17; ++i)
            Check(Unit(model->localTransforms + i * 4),"full degenerate or vertical skeleton keeps every standard rotation finite and normalized");
    }
    auto model = EmptySkeleton();
    const float positions[4][3] = {{2,3,4},{-2,3,4},{0,5,4},{0,5,4}};
    const int indices[] = {4,8,20,21};
    for (int i = 0; i < 4; ++i) std::memcpy(model->standardBonePositions[indices[i]],positions[i],sizeof positions[i]);
    InitStandardSkeletonQuats(reinterpret_cast<unsigned char*>(model.get()),1);
    const float halfSqrt = std::sqrt(0.5f);
    const auto* left = model->localTransforms + 64;
    const auto* right = model->localTransforms + 60;
    Check(std::fabs(left[0]) < 1e-5f && std::fabs(left[1]) < 1e-5f &&
          std::fabs(left[2] - halfSqrt) < 1e-5f && std::fabs(left[3] - halfSqrt) < 1e-5f,
          "normalized left shoulder directions yield the expected positive quarter turn around Z");
    Check(std::fabs(right[0]) < 1e-5f && std::fabs(right[1]) < 1e-5f &&
          std::fabs(right[2] + halfSqrt) < 1e-5f && std::fabs(right[3] - halfSqrt) < 1e-5f,
          "normalized right shoulder directions yield the expected negative quarter turn around Z");
    std::memcpy(model->standardBonePositions[20],positions[1],sizeof positions[1]);
    InitStandardSkeletonQuats(reinterpret_cast<unsigned char*>(model.get()),1);
    const auto* opposite = model->localTransforms + 64;
    Check(Unit(opposite),"opposite shoulder directions yield a valid half turn");
    d3dx::D3DXMATRIXF matrix{}; float transformed[4]{}; const float axis[3] = {1,0,0};
    d3dx::Get().matrixRotationQuaternion(&matrix,opposite);
    d3dx::Get().vec3Transform(transformed,axis,&matrix);
    Check(std::fabs(transformed[0] + 1) < 1e-5f && std::fabs(transformed[1]) < 1e-5f && std::fabs(transformed[2]) < 1e-5f,
          "opposite-vector fallback actually turns the source direction toward its target");
}
}
int main() {
    if (!d3dx::Get().Load()) { std::fprintf(stderr,"D3DX math runtime is unavailable\n"); return 2; }
    LookAt(); Skeleton();
    std::printf("kinect_pose_math_tests: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
