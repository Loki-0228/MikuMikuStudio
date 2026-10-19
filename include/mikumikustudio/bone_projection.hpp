#pragma once

#include <Windows.h>
#include <d3d9.h>
#include "mikumikustudio/bone_layout.hpp"

namespace mikumikustudio {
enum class BoneOverlayPoint { Origin, Tail };

bool ProjectBonePoint(const mdl::BoneRecord& bone, BoneOverlayPoint pointKind,
                      const D3DMATRIX& world, const D3DMATRIX& view,
                      const D3DMATRIX& projection, const RECT& viewport,
                      int* screenX, int* screenY, float* clipW);

// MMS addition (no original VA): the same projection for a point that is not a
// bone field.  `local` is applied first, then world, view and projection, which
// is the order the row-vector D3D transforms the caller reads back from the
// device are composed in.  ProjectBonePoint is this call with the bone's
// matInit as `local`, so both share one projection formula.
bool ProjectPoint(const float point[3], const D3DMATRIX& local,
                  const D3DMATRIX& world, const D3DMATRIX& view,
                  const D3DMATRIX& projection, const RECT& viewport,
                  int* screenX, int* screenY, float* clipW);

bool BoneOverlayEndpoint(const mdl::BoneRecord* bones, int boneCount,
                         const mdl::BoneRecord& bone, bool pmx,
                         int* screenX, int* screenY);
}
