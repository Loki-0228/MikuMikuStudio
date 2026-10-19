// ===========================================================================
// VA 0x0046BD79..0x0046DB3D - dynamic HUD sprite producer (camera path)
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdint>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/bone_projection.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/viewport_corner_settings.hpp"
#include "mikumikustudio/viewport_tool_layout.hpp"
#include "vb_dump.hpp"

namespace mikumikustudio {
namespace {

struct SpriteVertex {
    float x;
    float y;
    float z;
    float rhw;
    D3DCOLOR color;
    float u;
    float v;
};

static_assert(sizeof(SpriteVertex) == 28);

// SpriteOverlayVertices is created and locked at 0x445C0 bytes.  Each quad is
// six SpriteVertex records (two triangles), so one primitive is three
// vertices and the batch's primitive capacity is the buffer size divided by
// that triangle; the sprite primitive counter advances by 2 per quad, which
// sets the primitive budget.  The old form divided by 2 on a "four vertices
// per quad" premise and admitted 4926 primitives (2463 quads) into a buffer
// that holds 1642 quads.  Defect RB-02.
constexpr std::uint32_t kSpriteOverlayPrimitiveCapacity =
    0x445C0 / sizeof(SpriteVertex) / 3;

// The HUD emitted after the bone markers needs two operation rows, the scale
// row's four buttons, up to four disabled washes, up to two hover highlights,
// the look-at marker and the cursor picture.  Bone markers may fill only what
// remains of the batch, so a full-bone scene plus the new scale buttons can
// never overrun the buffer.
constexpr std::uint32_t kHudReservedQuads = 14;
constexpr std::uint32_t kBoneMarkerPrimitiveLimit =
    kSpriteOverlayPrimitiveCapacity - 2 * kHudReservedQuads;

struct LineVertex {
    float x;
    float y;
    float z;
    float rhw;
    D3DCOLOR color;
};

static_assert(sizeof(LineVertex) == 20);

struct BoneOverlayResult {
    int selectedX;
    int selectedY;
    float selectedClipW;
    std::uint32_t spritePrimitiveCount;
    std::uint32_t linePrimitiveCount;
};

static_assert(sizeof(BoneOverlayResult) == 20);

struct ClipPoint {
    float x;
    float y;
    float z;
    float w;
};

ClipPoint TransformPoint(float x, float y, float z, const D3DMATRIX& m) {
    return {
        x * m._11 + y * m._21 + z * m._31 + m._41,
        x * m._12 + y * m._22 + z * m._32 + m._42,
        x * m._13 + y * m._23 + z * m._33 + m._43,
        x * m._14 + y * m._24 + z * m._34 + m._44,
    };
}

ClipPoint TransformPoint(const ClipPoint& p, const D3DMATRIX& m) {
    return {
        p.x * m._11 + p.y * m._21 + p.z * m._31 + p.w * m._41,
        p.x * m._12 + p.y * m._22 + p.z * m._32 + p.w * m._42,
        p.x * m._13 + p.y * m._23 + p.z * m._33 + p.w * m._43,
        p.x * m._14 + p.y * m._24 + p.z * m._34 + p.w * m._44,
    };
}

}  // namespace

bool ProjectPoint(const float point[3], const D3DMATRIX& local,
                  const D3DMATRIX& world, const D3DMATRIX& view,
                  const D3DMATRIX& projection, const RECT& viewport,
                  int* screenX, int* screenY, float* clipW) {
    ClipPoint p = TransformPoint(point[0], point[1], point[2], local);
    p = TransformPoint(p, world);
    p = TransformPoint(p, view);
    p = TransformPoint(p, projection);
    *clipW = p.w;
    if (p.w <= 0.0f) {
        *screenX = 393939;
        *screenY = 393939;
        return false;
    }
    const double width = static_cast<double>(viewport.right - viewport.left);
    const double height = static_cast<double>(viewport.bottom - viewport.top);
    *screenX = static_cast<int>(p.x / p.w * width * 0.5 +
                                (viewport.left + viewport.right) / 2);
    *screenY = static_cast<int>((viewport.top + viewport.bottom) / 2 -
                                p.y / p.w * height * 0.5);
    return true;
}

bool ProjectBonePoint(const mdl::BoneRecord& bone, BoneOverlayPoint pointKind,
                      const D3DMATRIX& world, const D3DMATRIX& view,
                      const D3DMATRIX& projection, const RECT& viewport,
                      int* screenX, int* screenY, float* clipW) {
    const auto& boneMatrix =
        *reinterpret_cast<const D3DMATRIX*>(bone.matInit);
    // Byte offsets from the x86 layout are not BoneRecord indices, and the
    // fields move on x64. Keep both accesses tied to their named ABI fields.
    const float* point = pointKind == BoneOverlayPoint::Origin
        ? bone.position : bone.tailOffset;
    return ProjectPoint(point, boneMatrix, world, view, projection, viewport,
                        screenX, screenY, clipW);
}

bool BoneOverlayEndpoint(const mdl::BoneRecord* bones, int boneCount,
                         const mdl::BoneRecord& bone, bool pmx,
                         int* screenX, int* screenY) {
    *screenX = *screenY = 393939;
    if (pmx && (bone.flags & mdl::kBoneFlagTailIsBone) == 0) {
        *screenX = bone.tailScreenX;
        *screenY = bone.tailScreenY;
        return true;
    }
    const int linked = bone.tailBone;
    // PMD uses zero for "no tail"; PMX uses -1 and can target bone zero.
    if (bones == nullptr || linked < 0 || (!pmx && linked == 0) || linked >= boneCount)
        return false;
    // x86 +452/+456 are quaternion components on x64, not screen positions.
    *screenX = bones[linked].selState;
    *screenY = bones[linked].selState2;
    return true;
}

namespace {

__declspec(noinline) float BoneVCoordinate(int numerator, int origin,
                                           float lengthValue, float scale) {
#if defined(_M_IX86)
    static const double kEight = 8.0;
    float result = 0.0f;
    // 0x49A541..0x49A58E and 0x49A60E..0x49A654 retain every
    // intermediate on the x87 stack before the final float store.
    __asm {
        fild dword ptr numerator
        fdiv dword ptr lengthValue
        fmul qword ptr kEight
        fmul dword ptr scale
        fiadd dword ptr origin
        fstp dword ptr result
    }
    return result;
#else
    return static_cast<float>(static_cast<double>(numerator) / lengthValue *
                              8.0 * scale + origin);
#endif
}

__declspec(noinline) void BoneMarkerBounds(int x, int y, float scale,
                                           float* left, float* top,
                                           float* right, float* bottom) {
    float l = 0.0f;
    float t = 0.0f;
    float r = 0.0f;
    float b = 0.0f;
    // 0x49AFBD onward keeps the half extent on the x87 stack, stores the
    // leading edge as float, reloads it, then adds the full extent.
#if defined(_M_IX86)
    static const double kHalf = 9.5;
    static const double kSize = 18.0;
    __asm {
        fld dword ptr scale
        fmul qword ptr kHalf

        fild dword ptr x
        fsub st, st(1)
        fstp dword ptr l
        fld dword ptr l
        fld dword ptr scale
        fmul qword ptr kSize
        faddp st(1), st
        fstp dword ptr r

        fild dword ptr y
        fsub st, st(1)
        fstp dword ptr t
        fld dword ptr t
        fld dword ptr scale
        fmul qword ptr kSize
        faddp st(1), st
        fstp dword ptr b

        fstp st(0)
    }
#else
    l = static_cast<float>(x - scale * 9.5);
    r = static_cast<float>(l + scale * 18.0);
    t = static_cast<float>(y - scale * 9.5);
    b = static_cast<float>(t + scale * 18.0);
#endif
    *left = l;
    *top = t;
    *right = r;
    *bottom = b;
}

void AppendQuad(SpriteVertex*& out, float left, float top,
                float right, float bottom,
                float u0, float v0, float u1, float v1,
                D3DCOLOR color = 0xFFFFFFFFu) {
    const SpriteVertex quad[6] = {
        {right, top,    0.0f, 1.0f, color, u1, v0},
        {right, bottom, 0.0f, 1.0f, color, u1, v1},
        {left,  top,    0.0f, 1.0f, color, u0, v0},
        {left,  top,    0.0f, 1.0f, color, u0, v0},
        {right, bottom, 0.0f, 1.0f, color, u1, v1},
        {left,  bottom, 0.0f, 1.0f, color, u0, v1},
    };
    for (const SpriteVertex& vertex : quad)
        *out++ = vertex;
}

bool AppendBoneV(LineVertex*& out, int startX, int startY,
                 int endX, int endY, float scale, D3DCOLOR color) {
    const int dx = endX - startX;
    const int dy = endY - startY;
    const float length = static_cast<float>(
        std::sqrt(static_cast<double>(dx * dx + dy * dy)));
    if (length == 0.0f)
        return false;

    const LineVertex vertices[4] = {
        {BoneVCoordinate(-dy, startX, length, scale),
         BoneVCoordinate( dx, startY, length, scale),
         0.0f, 1.0f, color},
        {static_cast<float>(endX), static_cast<float>(endY),
         0.0f, 1.0f, color},
        {static_cast<float>(endX), static_cast<float>(endY),
         0.0f, 1.0f, color},
        {BoneVCoordinate( dy, startX, length, scale),
         BoneVCoordinate(-dx, startY, length, scale),
         0.0f, 1.0f, color},
    };
    for (const LineVertex& vertex : vertices)
        *out++ = vertex;
    return true;
}

void AppendLine(LineVertex*& out, int startX, int startY,
                int endX, int endY, D3DCOLOR color) {
    *out++ = {static_cast<float>(startX), static_cast<float>(startY),
              0.0f, 1.0f, color};
    *out++ = {static_cast<float>(endX), static_cast<float>(endY),
              0.0f, 1.0f, color};
}

}  // namespace

void PrepareFrameSpriteOverlay(MMDApp* app) {
    app->hudSprites = {};
    if (app->RecordingWindow() != nullptr)
        return;

    auto* vb = app->SpriteOverlayVertices();
    if (vb == nullptr)
        return;

    SpriteVertex* out = nullptr;
    if (FAILED(vb->Lock(0, 0x445C0, reinterpret_cast<void**>(&out), 0)))
        return;
    SpriteVertex* const spriteBegin = out;
    app->SpriteOverlayPrimitiveCount() = 0;

    const auto append = [&](float left, float top, float right, float bottom,
                            float u0, float v0, float u1, float v1,
                            D3DCOLOR color = 0xFFFFFFFFu) {
        // One quad is two primitives; refuse the write unless the batch still
        // has room for both, so no caller can run past the buffer.
        if (app->SpriteOverlayPrimitiveCount() + 2 >
            kSpriteOverlayPrimitiveCapacity)
            return;
        AppendQuad(out, left, top, right, bottom, u0, v0, u1, v1, color);
        app->SpriteOverlayPrimitiveCount() += 2;
    };

    D3DRenderer* sub = app->Renderer();
    float scale = sub != nullptr
        ? sub->viewScale
        : 1.0f;
    const float sceneScale = scale;
    const RECT view = app->ViewportRect();
    // Shared dock layout also places uniform scale on the inward side.
    const viewport_tool_layout::Anchor hud = viewport_tool_layout::Make(
        FrameOverlayRect(app), FrameOverlayScale(app), viewport_corner_settings::Current());
    const float right = hud.x;
    const float bottom = hud.y;

    // 0x499BD0: bone projection plus the bone-line and bone-icon producers.
    // The original runs this after WORLD/VIEW/PROJECTION are installed and
    // appends to the line batch built by 0x4757C3 on the previous frame.
    if (app->state.optflag[0] == 0 &&
        app->PlaybackActive() == 0 && sub != nullptr) {
        auto* model = app->SelectedModel();
        auto* device = sub->device;
        if (model != nullptr && device != nullptr) {
            const std::uint32_t spriteCountBeforeBone =
                app->SpriteOverlayPrimitiveCount();
            BoneOverlayResult boneResult{393939, 393939, 0.0f, 0,
                app->LineOverlayPrimitiveCount()};
            auto* bones = mikumikustudio::mdl::Bones(model);
            const int boneCount = mdl::Mdl(model)->boneCount;
            const int selected = mdl::Mdl(model)->selectedBone;
            auto* active = mdl::Mdl(model)->boneSelection;
            auto* secondary = mdl::Mdl(model)->bonePhysicsState;
            const mdl::IkChain* relationships = mdl::IkChains(model);
            const int relationshipCount =
                static_cast<int>(mdl::Mdl(model)->ikChainCount);
            const int cursorKind = static_cast<int>(app->EditMode());
            // 658628 = playbackPhysicsMode（原 boneFilter 误名）
            const int physMode = app->PlaybackPhysicsMode();
            D3DMATRIX world{};
            D3DMATRIX viewMatrix{};
            D3DMATRIX projection{};
            device->GetTransform(D3DTS_WORLD, &world);
            device->GetTransform(D3DTS_VIEW, &viewMatrix);
            device->GetTransform(D3DTS_PROJECTION, &projection);

            for (int index = 0; bones != nullptr && index < boneCount;
                 ++index) {
                auto* bone = &bones[index];
                float w = 0.0f;
                ProjectBonePoint(*bone, BoneOverlayPoint::Origin, world, viewMatrix, projection,
                                 view, &bone->selState,
                                 &bone->selState2, &w);
                if (index == selected) {
                    boneResult.selectedX = bone->selState;
                    boneResult.selectedY = bone->selState2;
                    boneResult.selectedClipW = w;
                }
                if (mdl::Mdl(model)->physicsMode == 2 &&
                    (bone->flags & mdl::kBoneFlagTailIsBone) == 0) {
                    float secondaryW = 0.0f;
                    ProjectBonePoint(*bone, BoneOverlayPoint::Tail, world, viewMatrix, projection,
                                     view, &bone->tailScreenX,
                                     &bone->tailScreenY, &secondaryW);
                }
            }

            // 0x49A1DC..0x49AE48: cursor modes 0/1 draw each bone as a
            // two-primitive V. Active relationship records may deliberately
            // emit the same V more than once with the relationship color.
            if (bones != nullptr) {
                // The renderer's line buffer is shared by bone indicators and
                // the final guide-line overlay pass.
                auto* lineVb = sub->lineVertexBuffer;
                const std::uint32_t incomingLineCount =
                    boneResult.linePrimitiveCount;
                LineVertex* lines = nullptr;
                if (lineVb != nullptr && incomingLineCount < 5000 &&
                    SUCCEEDED(lineVb->Lock(40 * incomingLineCount,
                        40 * (5000 - incomingLineCount),
                        reinterpret_cast<void**>(&lines), 0))) {
                    LineVertex* const lineBegin = lines;
                    const auto filtered = [&](mikumikustudio::mdl::BoneRecord* bone) -> bool {
                        return (physMode == 1 && bone->hasRigidBody) ||
                            (physMode == 2 && bone->hasRigidBody &&
                             bone->physicsDisabled == 0);
                    };
                    const auto emitBone = [&](mikumikustudio::mdl::BoneRecord* bone,
                                              D3DCOLOR color) -> bool {
                        int endX = 393939;
                        int endY = 393939;
                        if (boneResult.linePrimitiveCount > 5000 - 2 ||
                            bone->selState == 393939 ||
                            bone->selState2 == 393939 ||
                            !BoneOverlayEndpoint(bones, boneCount, *bone,
                                mdl::Mdl(model)->physicsMode == 2, &endX, &endY) ||
                            endX == 393939 || endY == 393939)
                            return false;
                        return AppendBoneV(lines, bone->selState,
                            bone->selState2, endX, endY, scale, color);
                    };

                    if (cursorKind < 2) {
                        for (int index = 0; index < boneCount; ++index) {
                            auto* bone = &bones[index];
                            const std::uint16_t flags =
                                bone->flags;
                            const mdl::BoneType type =
                                bone->type;
                            if ((flags & mdl::kBoneFlagVisible) == 0 ||
                                type >= mdl::BoneType::FixedAxis ||
                                filtered(bone))
                                continue;

                            bool inActiveRelationship = false;
                            for (int relation = 0;
                                 relationships != nullptr &&
                                 relation < relationshipCount; ++relation) {
                                const mdl::IkChain& record =
                                    relationships[relation];
                                if (record.enabled == 0)
                                    continue;
                                const std::uint8_t memberCount = record.linkCount;
                                const std::uint16_t* members = record.links;
                                for (int member = 0;
                                     members != nullptr &&
                                     member < memberCount; ++member) {
                                    if (members[member] != index)
                                        continue;
                                    inActiveRelationship = true;
                                    const D3DCOLOR color =
                                        active != nullptr && active[index]
                                        ? (index == selected ? 0xFFFF0000u
                                                             : 0xFFC80000u)
                                        : (secondary != nullptr &&
                                                   secondary[index]
                                               ? 0xFF64FF64u
                                               : 0xFFFF9B00u);
                                    if (emitBone(bone, color))
                                        boneResult.linePrimitiveCount += 2;
                                }
                            }
                            if (!inActiveRelationship) {
                                const D3DCOLOR color =
                                    active != nullptr && active[index]
                                    ? (index == selected ? 0xFFFF0000u
                                                         : 0xFFC80000u)
                                    : (secondary != nullptr &&
                                               secondary[index]
                                           ? 0xFF64FF64u
                                           : 0xFF6464E6u);
                                if (emitBone(bone, color))
                                    boneResult.linePrimitiveCount += 2;
                            }
                        }

                        // 0x49A9F9..0x49AB2B: one black centre line per
                        // active relationship record, between +0 and +4.
                        for (int relation = 0;
                             relationships != nullptr &&
                             relation < relationshipCount; ++relation) {
                            const mdl::IkChain& record = relationships[relation];
                            if (record.enabled == 0)
                                continue;
                            const int first = record.boneIndex;
                            const int second = record.targetBone;
                            if (first < 0 || first >= boneCount ||
                                second < 0 || second >= boneCount)
                                continue;
                            auto* a = &bones[first];
                            auto* b = &bones[second];
                            if (boneResult.linePrimitiveCount >= 5000 ||
                                (a->flags & mdl::kBoneFlagVisible) == 0 ||
                                (b->flags & mdl::kBoneFlagVisible) == 0 ||
                                a->selState == 393939 ||
                                a->selState2 == 393939 ||
                                b->selState == 393939 ||
                                b->selState2 == 393939)
                                continue;
                            AppendLine(lines, a->selState,
                                       a->selState2, b->selState,
                                       b->selState2, 0xFF000000u);
                            ++boneResult.linePrimitiveCount;
                        }
                    }

                    // 0x49AB4A..0x49AE33: the selected bone gets a second V
                    // in every cursor mode except 2.
                    if (cursorKind != 2 && selected >= 0 &&
                        selected < boneCount) {
                        auto* bone = &bones[selected];
                        const std::uint16_t flags =
                            bone->flags;
                        if ((flags & mdl::kBoneFlagVisible) != 0 &&
                            bone->type < mdl::BoneType::FixedAxis &&
                            !filtered(bone) && emitBone(bone, 0xFFFF3232u)) {
                            boneResult.linePrimitiveCount += 2;
                        }
                    }
                    DumpVertexBatch("line", lineBegin,
                        boneResult.linePrimitiveCount - incomingLineCount, 40);
                    lineVb->Unlock();
                    // The count already includes the selection-box prefix.
                    app->LineOverlayPrimitiveCount() = boneResult.linePrimitiveCount;
                }
            }

            // 0x49AE4C..0x49C1E5: the exact common bone-marker atlas cells.
            // Deliberate hardening: the original writes each marker directly
            // into the 0x445C0-byte sprite batch and never checks the vertex
            // counter against that capacity (the batch is created at
            // 0x14001274B and locked with the same length at 0x140028054, and
            // the counter is only ever compared against zero, at 0x14002AFBA),
            // so a scene with more markers than the buffer holds overwrites
            // adjacent application memory. The port stops appending markers at
            // their reserved limit (kBoneMarkerPrimitiveLimit keeps the two
            // operation rows, the scale row's four buttons, the disabled
            // washes and the hover highlights their own room), mirroring the
            // bound already enforced by the line batch; valid scenes are
            // unaffected.
            // Defect RB-02.
            if (bones != nullptr) {
                for (int index = 0; index < boneCount; ++index) {
                    auto* bone = &bones[index];
                    const std::uint16_t flags = bone->flags;
                    const mdl::BoneType type = bone->type;
                    if ((flags & mdl::kBoneFlagVisible) == 0)
                        continue;

                    float v0 = 0.0f;
                    float v1 = 0.0f;
                    const bool isActive = active != nullptr && active[index] != 0;
                    const bool isSecondary = secondary != nullptr && secondary[index] != 0;
                    bool emit = false;
                    if (cursorKind > 2) {
                        if (index != selected && isActive &&
                            (type == mdl::BoneType::Move ||
                             type == mdl::BoneType::Ik)) {
                            v0 = type == mdl::BoneType::Move ? 0.034456f
                                                             : 0.104768f;
                            v1 = type == mdl::BoneType::Move
                                     ? 0.069058999f
                                     : 0.13937099f;
                            emit = true;
                        }
                    } else if (cursorKind < 2 &&
                               !((physMode == 1 && bone->hasRigidBody) ||
                                 (physMode == 2 && bone->hasRigidBody &&
                                  bone->physicsDisabled == 0))) {
                        const int state = isActive ? 0 : (isSecondary ? 1 : 2);
                        switch (type) {
                        case mdl::BoneType::RotateMove:
                        case mdl::BoneType::RotateGrant:
                        case mdl::BoneType::Effector: {
                            const float starts[] = {0.0f, 0.17508f, 0.52664f};
                            const float ends[] = {0.033902999f, 0.209683f, 0.561243f};
                            v0 = starts[state]; v1 = ends[state]; emit = true; break;
                        }
                        case mdl::BoneType::Move: {
                            const float starts[] = {0.034456f, 0.210236f, 0.56179601f};
                            const float ends[] = {0.069058999f, 0.244839f, 0.59639901f};
                            v0 = starts[state]; v1 = ends[state]; emit = true; break;
                        }
                        case mdl::BoneType::Ik: {
                            const float starts[] = {0.104768f, 0.28054801f, 0.63210797f};
                            const float ends[] = {0.13937099f, 0.31515101f, 0.66671097f};
                            v0 = starts[state]; v1 = ends[state]; emit = true; break;
                        }
                        case mdl::BoneType::UnderIk: {
                            const bool ik =
                                (flags & mdl::kBoneFlagFixedAxis) != 0;
                            const float startsIk[] = {0.77328497f, 0.73812902f, 0.70297301f};
                            const float endsIk[] = {0.80788797f, 0.77273202f, 0.73757601f};
                            const float starts[] = {0.069611996f, 0.24539199f, 0.59695202f};
                            const float ends[] = {0.104215f, 0.27999499f, 0.63155502f};
                            v0 = ik ? startsIk[state] : starts[state];
                            v1 = ik ? endsIk[state] : ends[state]; emit = true; break;
                        }
                        case mdl::BoneType::FixedAxis: {
                            const float starts[] = {0.139924f, 0.31570399f, 0.66726398f};
                            const float ends[] = {0.174527f, 0.35030699f, 0.70186698f};
                            v0 = starts[state]; v1 = ends[state]; emit = true; break;
                        }
                        default:
                            break;
                        }
                    }
                    if (emit &&
                        app->SpriteOverlayPrimitiveCount() <
                            kBoneMarkerPrimitiveLimit) {
                        float left = 0.0f;
                        float top = 0.0f;
                        float right = 0.0f;
                        float bottom = 0.0f;
                        BoneMarkerBounds(bone->selState,
                                         bone->selState2, scale,
                                         &left, &top, &right, &bottom);
                        append(left, top, right, bottom,
                               0.46414399f, v0, 0.49874699f, v1);
                    }
                }

                // 0x49C1ED..0x49C80C: every mode except 2 appends one final
                // selected marker from the dedicated atlas family.
                if (cursorKind != 2 && selected >= 0 &&
                    selected < boneCount) {
                    auto* bone = &bones[selected];
                    const std::uint16_t flags =
                        bone->flags;
                    const bool filteredInMode2 =
                        physMode == 2 && bone->hasRigidBody &&
                        bone->physicsDisabled == 0;
                    float v0 = 0.0f;
                    float v1 = 0.0f;
                    bool emit = false;
                    if ((flags & mdl::kBoneFlagVisible) != 0 &&
                        !filteredInMode2) {
                        switch (bone->type) {
                        case mdl::BoneType::RotateMove:
                            v0 = 0.35086f; v1 = 0.385463f; emit = true;
                            break;
                        case mdl::BoneType::Move:
                            v0 = 0.38601601f; v1 = 0.42061901f; emit = true;
                            break;
                        case mdl::BoneType::Ik:
                        case mdl::BoneType::Unused3:
                            v0 = 0.456328f; v1 = 0.490931f; emit = true;
                            break;
                        case mdl::BoneType::UnderIk:
                            if ((flags & mdl::kBoneFlagFixedAxis) != 0) {
                                v0 = 0.77328497f;
                                v1 = 0.80788797f;
                            } else {
                                v0 = 0.42117199f;
                                v1 = 0.45877501f;
                            }
                            emit = true;
                            break;
                        case mdl::BoneType::RotateGrant:
                        case mdl::BoneType::Effector:
                            v0 = 0.0f; v1 = 0.033902999f; emit = true;
                            break;
                        case mdl::BoneType::FixedAxis:
                            v0 = 0.49148399f; v1 = 0.52608699f; emit = true;
                            break;
                        default:
                            break;
                        }
                    }
                    if (emit &&
                        app->SpriteOverlayPrimitiveCount() <
                            kBoneMarkerPrimitiveLimit) {
                        float left = 0.0f;
                        float top = 0.0f;
                        float right = 0.0f;
                        float bottom = 0.0f;
                        BoneMarkerBounds(boneResult.selectedX,
                                         boneResult.selectedY, scale,
                                         &left, &top, &right, &bottom);
                        append(left, top, right, bottom,
                               0.46414399f, v0, 0.49874699f, v1);
                    }
                }
            }

            boneResult.spritePrimitiveCount =
                app->SpriteOverlayPrimitiveCount() -
                spriteCountBeforeBone;
            app->ViewportToolCenterX() = boneResult.selectedX;
            app->ViewportToolCenterY() = boneResult.selectedY;
            app->state.selectedClipW = boneResult.selectedClipW;
        }
    }

    // 0x46C297..0x46C6C5: with no selected model, the camera look-at point
    // projects to the exact viewport centre. The original uses asymmetric
    // -9.5/+8.5 extents so texels land on the same pixels.
    if (app->state.optflag[0] != 0 &&
        app->PlaybackActive() == 0 &&
        app->CameraParentModel() < 0) {
        const float cx = static_cast<float>((view.left + view.right) / 2);
        const float cy = static_cast<float>((view.top + view.bottom) / 2);
        append(cx - scale * 9.5f, cy - scale * 9.5f,
               cx + scale * 8.5f, cy + scale * 8.5f,
               0.46414399f, 0.35086f, 0.49874699f, 0.385463f);
    }

    // The coordinate selector label is emitted by PrepareFrameTextOverlay so it
    // can carry a per-language word (the sprite sheet only has the English
    // serif cells, and no screen-axis state for the model panel). That caption
    // keeps the sheet's serif face; see scene_font.cpp. Keep this batch free of
    // the old label; the hit region remains operation 21 below.
    app->hudSprites.first = app->SpriteOverlayPrimitiveCount();
    scale = hud.scale;
    const bool playback = app->PlaybackActive() != 0;
    // 0x46C6E2..0x46CE84: camera and bone operation rows. Playback takes
    // LABEL_105 and deliberately emits neither rows nor highlights.
    if (!playback) {
        if (app->state.optflag[0] != 0) {
            // The panel's third coordinate state edits the selected accessory,
            // so its buttons carry a ".x" mark in place of the camera plate.
            // Those two rows sit below the caption band of the same sheet
            // (y 341..370 for the rotate row, 371..400 for the move row);
            // Deliberate deviation: the original paints the camera plate in
            // every camera/accessory state, and its sheet has no accessory
            // mark to sample instead.  Both rows moved up one 40-pixel band
            // for the scale row below (rotate bottom-120..-90, move
            // bottom-80..-50).
            const bool accessoryState = EffectiveCoordinateState(app) == 2;
            const float rotateTop = accessoryState ? 0.666015625f : 0.41210899f;
            const float rotateBottom = accessoryState ? 0.72265625f : 0.46875f;
            const float moveTop = accessoryState ? 0.724609375f : 0.177734f;
            const float moveBottom = accessoryState ? 0.78125f : 0.234375f;
            append(right - scale * 130.0f, bottom - scale * 120.0f,
                   right - scale * 10.0f, bottom - scale * 90.0f,
                   0.001953f, rotateTop, 0.23242199f, rotateBottom);
            append(right - scale * 130.0f, bottom - scale * 80.0f,
                   right - scale * 10.0f, bottom - scale * 50.0f,
                   0.001953f, moveTop, 0.23242199f, moveBottom);
        } else {
            const int boneMode = static_cast<std::int32_t>(app->state.v32c);
            if (boneMode >= 0 && boneMode <= 3) {
                const bool alternate = (boneMode & 1) != 0;
                const bool lowerPair = boneMode >= 2;
                append(right - scale * 130.0f, bottom - scale * 120.0f,
                       right - scale * 10.0f, bottom - scale * 90.0f,
                       0.001953f,
                       lowerPair ? 0.35351601f : 0.29492199f,
                       0.23242199f,
                       lowerPair ? 0.41015601f : 0.35156301f);
                append(right - scale * 130.0f, bottom - scale * 80.0f,
                       right - scale * 10.0f, bottom - scale * 50.0f,
                       0.001953f,
                       alternate ? 0.119141f : 0.060547002f,
                       0.23242199f,
                       alternate ? 0.175781f : 0.117188f);
            }
        }

#if !MMD_ORIGINAL_PROFILE
        // One gate for the whole scale HUD: the row, the disabled wash and the
        // hover highlight all read bone_scale::Enabled through this single
        // per-frame call, so a stale hover operation can never brighten a
        // disabled (washed) row when the selection or panel switches.
        const bool scaleEnabled = bone_scale::Enabled(app);

        // Four logical scale cells sample the y 401..430 atlas band.
        // The shared layout moves uniform to the right at the left corners;
        // X/Y/Z keep the same columns as rotation and translation.
        // Drawing and input share the one
        // gate, bone_scale::Enabled: while the camera/light/accessory panel is
        // active (optflag[0] != 0), while no bone is selected or during
        // playback turns scaling off, the same UVs repeat under a black
        // semi-transparent diffuse, so the wash is clipped by the texture's
        // own alpha and never an opaque plate.  The model panel's
        // Light/Camera edit modes are the bone move/rotate cursors and stay
        // enabled.
        {
            for (int column = 0; column < 4; ++column) {
                const float x0 = hud.X(hud.ScaleLeftUnits(column));
                const float y0 = bottom - scale * 40.0f;
                const float u0 = 0.001953125f + 0.078125f * column;
                const float u1 = u0 + 0.078125f;
                append(x0, y0, x0 + scale * 40.0f, y0 + scale * 30.0f,
                       u0, 0.783203125f, u1, 0.83984375f);
                if (!scaleEnabled)
                    append(x0, y0, x0 + scale * 40.0f, y0 + scale * 30.0f,
                           u0, 0.783203125f, u1, 0.83984375f, 0xA0000000u);
            }
        }

#endif
        // 0x46CF9B..0x46D71F: the six 40x30 operation highlights. The
        // paired interaction modes share one atlas cell exactly as below.
        const int operation = static_cast<int>(app->ViewportToolOperation());
        const auto highlight = [&](int first, int second, int column,
                                   int row) {
            if (operation != first && operation != second)
                return;
            const float x0 = right - scale * (130.0f - 40.0f * column);
            // Rows moved up 40: row 0 (rotate) is bottom-120, row 1 (move)
            // bottom-80.
            const float y0 = bottom - scale * (120.0f - 40.0f * row);
            const float u0 = 0.001953f + 0.078125f * column;
            const float v0 = row == 0 ? 0.23632801f : 0.001953f;
            append(x0, y0, x0 + scale * 40.0f, y0 + scale * 30.0f,
                   u0, v0,
                   column == 2 ? 0.23242199f : u0 + 0.078125f,
                   row == 0 ? 0.29296899f : 0.058594f);
        };
        highlight(9, 18, 0, 0);
        highlight(10, 19, 1, 0);
        highlight(11, 20, 2, 0);
        highlight(12, 15, 0, 1);
        highlight(13, 16, 1, 1);
        highlight(14, 17, 2, 1);

        // The scale row's hover cell samples the y 431..460 highlight band,
        // the same four x cells as the row itself, on the row's own screen
        // geometry.  The gate keeps a stale hover operation (left over from
        // before the selection or panel switched) off the washed row.
#if !MMD_ORIGINAL_PROFILE
        if (scaleEnabled && operation >= 22 && operation <= 25) {
            const int column = operation - 22;
            const float x0 = hud.X(hud.ScaleLeftUnits(column));
            const float y0 = bottom - scale * 40.0f;
            const float u0 = 0.001953125f + 0.078125f * column;
            append(x0, y0, x0 + scale * 40.0f, y0 + scale * 30.0f,
                   u0, 0.841796875f, u0 + 0.078125f, 0.8984375f);
        }
#endif

        app->hudSprites.count = app->SpriteOverlayPrimitiveCount() - app->hudSprites.first;
        scale = sceneScale;

        // 0x46D74D..0x46DB2A: operation cursor pictures in bone mode. These
        // use raw projected mouse coordinates returned by 0x499BD0 and are
        // intentionally not multiplied by the viewport scale.
        //
        // MMS: while a HUD rotate / move / scale cell is hovered or dragged,
        // the bone shows that cell's own preview (debug_geometry.cpp
        // DrawHudAxisPreview), so the edit mode's cursor picture is hidden
        // and only one set of handles is on screen at a time.
        const int hudDrag = static_cast<int>(app->InteractionDragMode());
        const bool hudPreview =
            (operation >= 9 && operation <= 14) ||
            (!MMD_ORIGINAL_PROFILE && operation >= 22 && operation <= 25) ||
            (hudDrag >= 4 && hudDrag <= 6) ||
            (hudDrag >= 10 && hudDrag <= 12) ||
            (!MMD_ORIGINAL_PROFILE && hudDrag >= 19 && hudDrag <= 22);
        if (app->state.optflag[0] == 0 && !hudPreview) {
            const float x = static_cast<float>(app->ViewportToolCenterX());
            const float y = static_cast<float>(app->ViewportToolCenterY());
            const int cursorKind = static_cast<int>(app->EditMode());
            if (cursorKind == 3) {
                float u0 = 0.5f;
                float v0 = 0.5f;
                if (operation == 3) {
                    v0 = 0.75f;
                } else if (operation == 4) {
                    u0 = 0.75f;
                } else if (operation == 5) {
                    u0 = 0.75f;
                    v0 = 0.75f;
                }
                append(x - 45.0f, y - 45.0f, x + 45.0f, y + 45.0f,
                       u0, v0, u0 + 0.25f, v0 + 0.25f);
            } else if (cursorKind == 4) {
                float u0 = 0.5f;
                float v0 = 0.0f;
                if (operation == 6) {
                    v0 = 0.25f;
                } else if (operation == 7) {
                    u0 = 0.75f;
                } else if (operation == 8) {
                    u0 = 0.75f;
                    v0 = 0.25f;
                }
                append(x - 13.0f, y - 67.0f, x + 62.0f, y + 8.0f,
                       u0, v0, u0 + 0.25f, v0 + 0.25f);
            }
        }
    }

    DumpVertexBatch("sprite", spriteBegin,
        app->SpriteOverlayPrimitiveCount(), 84);
    vb->Unlock();
}

}  // namespace mikumikustudio
