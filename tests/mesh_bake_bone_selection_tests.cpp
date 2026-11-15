// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#include "io/mesh_bake_bone_selection.hpp"
#include "model/pmx_vertex_prepare.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {
namespace mdl = mikumikustudio::mdl;
namespace pmx = mikumikustudio::pmx_prepare;
using mikumikustudio::alembic_export::HasSelectedBoneInfluence;
using Type = mdl::PmxWeightType;
constexpr unsigned kBones = 5;

void Check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL bake bone selection: %s\n", message);
        std::exit(1);
    }
}

struct PackedVertex {
    std::array<unsigned char, pmx::kMaxRecordBytes> bytes{};
    std::size_t size = 32; // position, normal, UV
    std::size_t offset = 0;

    template<class T> void Put(T value) {
        std::memcpy(bytes.data() + size, &value, sizeof value);
        size += sizeof value;
    }

    int Read(void* out, unsigned count) {
        if (count > size - offset) return 0;
        std::memcpy(out, bytes.data() + offset, count);
        offset += count;
        return static_cast<int>(count);
    }
};

mdl::PmxVertex Decode(Type type, std::array<std::int32_t, 4> bones,
                      std::array<float, 4> weights, float staleSecond = -1.0f) {
    PackedVertex packed;
    packed.Put(type);
    packed.Put(bones[0]);
    // Match the records consumed by both the production packed loader and
    // its serial fallback. The fallback type has only one decoded index.
    if (type == Type::bdef4 || type == Type::qdef) {
        for (unsigned i = 1; i < bones.size(); ++i) packed.Put(bones[i]);
        for (auto w : weights) packed.Put(w);
    } else if (type == Type::bdef2 || type == Type::sdef) {
        packed.Put(bones[1]);
        packed.Put(weights[0]);
        if (type == Type::sdef)
            for (unsigned i = 0; i < 9; ++i) packed.Put(0.0f);
    }
    packed.Put(1.0f); // edge scale

    // A reused allocation can have valid-looking bone IDs and nonzero
    // weights. Put conflicting values in every lane that can stay stale.
    mdl::PmxVertex vertex{};
    for (unsigned i = 0; i < bones.size(); ++i) {
        vertex.bone[i] = static_cast<std::int32_t>(i + 1);
        vertex.weight[i] = i == 0 ? -1.0f : i == 1 ? staleSecond : 0.75f;
    }
    Check(pmx::ReadPackedVertex(packed, 0, 4, vertex) && packed.offset == packed.size,
          "decode packed PMX working vertex and consume complete record");
    return vertex;
}

template<class Vertex> void CheckInfluences(const Vertex& vertex, unsigned expected,
                                            const char* message) {
    std::array<unsigned char, kBones> selected{};
    Check(!HasSelectedBoneInfluence(vertex, {nullptr, kBones}), message);
    Check(!HasSelectedBoneInfluence(vertex, {selected.data(), kBones}), message);
    for (unsigned b = 0; b < selected.size(); ++b) {
        // Any nonzero selection marker is active.
        selected[b] = 2;
        Check(HasSelectedBoneInfluence(vertex, {selected.data(), kBones})
                  == ((expected & (1u << b)) != 0), message);
        selected[b] = 0;
    }
}
}

int main() {
    CheckInfluences(Decode(Type::bdef1, {0}, {}, 0.75f), 1u << 0,
                    "BDEF1 uses implicit unit weight and ignores all stale lanes");
    CheckInfluences(Decode(Type::bdef1, {-1}, {}, 0.75f), 0,
                    "BDEF1 never selects a negative bone index");
    for (auto type : {Type::bdef2, Type::sdef}) {
        CheckInfluences(Decode(type, {0, 1}, {0.25f}), (1u << 0) | (1u << 1),
                        "BDEF2/SDEF select both real lanes despite stale weights");
        CheckInfluences(Decode(type, {0, 1}, {0.0f}), 1u << 1,
                        "BDEF2/SDEF select only the second bone at zero");
        CheckInfluences(Decode(type, {0, 1}, {1.0f}, 0.75f), 1u << 0,
                        "BDEF2/SDEF do not select the stale secondary at unity");
        CheckInfluences(Decode(type, {0, kBones}, {0.0f}), 0,
                        "BDEF2/SDEF reject an out-of-range second bone");
    }
    CheckInfluences(Decode(Type::bdef4, {0, 1, 2, 3},
                           {0.75f, 0.0f, -0.125f, 0.25f}),
                    (1u << 0) | (1u << 3), "BDEF4 selects all and only positive valid lanes");
    CheckInfluences(Decode(Type::bdef4, {-1, kBones, std::numeric_limits<std::int32_t>::max(), 4},
                           {0.25f, 0.25f, 0.25f, 0.25f}),
                    1u << 4, "BDEF4 rejects invalid indices without hiding valid lanes");
    CheckInfluences(Decode(Type::qdef, {0,1,2,3}, {.75f,0,-.125f,.25f}), (1u<<0)|(1u<<3),
                    "QDEF selects all positive influences");
    CheckInfluences(Decode(static_cast<Type>(0xff), {1}, {}, 0.75f), 0,
                    "unknown fallback has no native bone influence");

    mdl::PmdVertex pmd{};
    pmd.bone[1] = 1;
    pmd.weightPercent = 50;
    CheckInfluences(pmd, (1u << 0) | (1u << 1), "PMD selects both positive lanes");
    pmd.weightPercent = 0;
    CheckInfluences(pmd, 1u << 1, "PMD keeps only the second bone at zero");
    pmd.weightPercent = 100;
    CheckInfluences(pmd, 1u << 0, "PMD keeps only the first bone at 100 percent");
    pmd.weightPercent = -1;
    CheckInfluences(pmd, 1u << 1, "PMD preserves signed percentage handling");
    pmd.bone[0] = -1;
    pmd.bone[1] = kBones;
    pmd.weightPercent = 50;
    CheckInfluences(pmd, 0, "PMD rejects both invalid indices");
    std::puts("PASS packed PMX and PMD native bake selected-bone influences");
}
