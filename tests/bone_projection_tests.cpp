// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/bone_projection.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
}

int main() {
    using namespace mikumikustudio;
    // Place one bone immediately before inaccessible memory. The former
    // bone + 308 / bone + 464 reads land in the reserved guard region.
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const SIZE_T page = info.dwPageSize;
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, page + 512 * sizeof(mdl::BoneRecord), MEM_RESERVE, PAGE_NOACCESS));
    Check(memory != nullptr, "reserve guarded bone allocation");
    Check(VirtualAlloc(memory, page, MEM_COMMIT, PAGE_READWRITE) != nullptr,
          "commit one bone page");
    auto* bone = new (memory + page - sizeof(mdl::BoneRecord)) mdl::BoneRecord{};
    D3DMATRIX identity{};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    std::memcpy(bone->matInit, &identity, sizeof identity);
    bone->position[0] = 0.25f;
    bone->position[1] = 0.5f;
    bone->position[2] = 0.75f;
    bone->tailOffset[0] = -0.5f;
    bone->tailOffset[1] = -0.25f;
    bone->tailOffset[2] = 0.5f;
    const RECT viewport{100, 50, 900, 650};
    int x = 0, y = 0;
    float w = 0;
    Check(ProjectBonePoint(*bone, BoneOverlayPoint::Origin, identity, identity,
                           identity, viewport, &x, &y, &w), "origin visible");
    Check(x == 600 && y == 200 && w == 1.0f, "origin uses model-space position");
    Check(ProjectBonePoint(*bone, BoneOverlayPoint::Tail, identity, identity,
                           identity, viewport, &x, &y, &w), "tail visible");
    Check(x == 300 && y == 425 && w == 1.0f, "tail uses its own named field");
    bone->matInit[12] = 0.25f;
    D3DMATRIX world = identity;
    world._42 = -0.5f;
    Check(ProjectBonePoint(*bone, BoneOverlayPoint::Origin, world, identity,
                           identity, viewport, &x, &y, &w), "transformed origin visible");
    Check(x == 700 && y == 350, "bone and world transformations both apply");
    D3DMATRIX behind = identity;
    behind._44 = -1;
    Check(!ProjectBonePoint(*bone, BoneOverlayPoint::Origin, identity, identity,
                            behind, viewport, &x, &y, &w), "behind-camera bone rejected");
    Check(x == 393939 && y == 393939, "hidden bone retains selection sentinel");

    mdl::BoneRecord linkedBones[2]{};
    linkedBones[0].flags = mdl::kBoneFlagTailIsBone;
    linkedBones[0].tailBone = 1;
    linkedBones[1].selState = 640;
    linkedBones[1].selState2 = 360;
    // These occupy the former hardcoded +452/+456 locations on x64.
    linkedBones[1].ikWorkingQuat[2] = 0.25f;
    linkedBones[1].ikWorkingQuat[3] = 1.0f;
    Check(BoneOverlayEndpoint(linkedBones, 2, linkedBones[0], true, &x, &y),
          "PMX linked tail resolved");
    Check(x == 640 && y == 360, "linked tail uses target screen coordinates, not quaternion bytes");
    linkedBones[0].flags = 0;
    linkedBones[0].tailScreenX = 700;
    linkedBones[0].tailScreenY = 450;
    Check(BoneOverlayEndpoint(linkedBones, 2, linkedBones[0], true, &x, &y) &&
          x == 700 && y == 450, "PMX offset tail retains its separately projected endpoint");
    Check(BoneOverlayEndpoint(linkedBones, 2, linkedBones[0], false, &x, &y) &&
          x == 640 && y == 360, "PMD tail still uses linked bone despite absent PMX flag");
    linkedBones[0].tailBone = 2;
    linkedBones[0].flags = mdl::kBoneFlagTailIsBone;
    Check(!BoneOverlayEndpoint(linkedBones, 2, linkedBones[0], true, &x, &y) &&
          x == 393939 && y == 393939, "out-of-range tail rejected");
    linkedBones[1].flags = mdl::kBoneFlagTailIsBone;
    linkedBones[1].tailBone = 0;
    linkedBones[0].selState = 200; linkedBones[0].selState2 = 100;
    Check(BoneOverlayEndpoint(linkedBones, 2, linkedBones[1], true, &x, &y) &&
          x == 200 && y == 100, "PMX accepts bone zero as a tail endpoint");
    Check(!BoneOverlayEndpoint(linkedBones, 2, linkedBones[1], false, &x, &y),
          "PMD retains zero as the no-tail sentinel");
    bone->~BoneRecord();
    VirtualFree(memory, 0, MEM_RELEASE);
    std::puts("Bone projection regressions passed");
}
