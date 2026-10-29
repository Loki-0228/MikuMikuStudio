#pragma once
// Independent reference copied from the pre-optimization PMX loader.
#include "mikumikustudio/subrecord_layout.hpp"
namespace mikumikustudio::geometry_test_reference {
template<class Input>
void OriginalVertex(Input& reader, unsigned additionalUv, unsigned boneIdxSize,
                    mdl::PmxVertex& vertex) {
        auto readIdx = [&reader](int size) -> std::int32_t {
            switch (size) {
            case 1: { std::int8_t v = 0; reader.Read(&v, 1); return v; }
            case 2: { std::uint16_t v = 0; reader.Read(&v, 2);
                      return static_cast<std::int16_t>(v); }
            default: { std::int32_t v = 0; reader.Read(&v, 4); return v; }
            }
        };

        {
            mdl::PmxVertex& rec = vertex;
            reader.Read(&rec, 0x20);                          // pos+normal+uv
            const int addl = additionalUv;
            // Deliberate hardening: the original takes this loop bound straight
            // from the header's additional-UV byte and only tests it for > 0
            // (verified at 0x1400AA33E, re-read on every iteration at
            // 0x1400AA3D2), never against the four UV entries of
            // additionalUvByComponent, so a count above 4 overwrites weightType
            // and the rest of the working vertex. The port stores at most four
            // UVs and consumes the surplus floats without storing them, so the
            // stream position is unchanged.
            // Defect MB-01.
            constexpr int kPmxAdditionalUvSlots =
                sizeof(rec.additionalUvByComponent)
                / sizeof(rec.additionalUvByComponent[0]);
            for (int j = 0; j < addl; ++j) {                // SOA addl UVs
                for (int c = 0; c < 4; ++c) {
                    float surplus = 0.0f;
                    float* destination = j < kPmxAdditionalUvSlots
                                             ? &rec.additionalUvByComponent[c][j]
                                             : &surplus;
                    reader.Read(destination, 4);
                }
            }
            reader.Read(&rec.weightType, 1);
            switch (rec.weightType) {
            case mdl::PmxWeightType::bdef2:
                rec.bone[0] = readIdx(boneIdxSize);
                rec.bone[1] = readIdx(boneIdxSize);
                reader.Read(rec.weight, 4);
                break;
            case mdl::PmxWeightType::bdef4:
                for (int b = 0; b < 4; ++b)
                    rec.bone[b] = readIdx(boneIdxSize);
                reader.Read(rec.weight, 0x10);
                break;
            case mdl::PmxWeightType::sdef: {
                rec.bone[0] = readIdx(boneIdxSize);
                rec.bone[1] = readIdx(boneIdxSize);
                reader.Read(rec.weight, 4);                   // weight
                reader.Read(rec.sdef.center, 0xC);            // PMX C
                reader.Read(rec.sdef.r0Offset, 0xC);          // PMX R0
                reader.Read(rec.sdef.r1Offset, 0xC);          // PMX R1
                const float w = rec.weight[0];
                float mid[3];
                for (int c = 0; c < 3; ++c)
                    mid[c] = w * rec.sdef.r0Offset[c]
                           + (1.0f - w) * rec.sdef.r1Offset[c];
                for (int c = 0; c < 3; ++c) {
                    rec.sdef.r0Offset[c] -= mid[c];
                    rec.sdef.r1Offset[c] -= mid[c];
                }
                break;
            }
            default:                                        // BDEF1 (and original fallback)
                rec.bone[0] = readIdx(boneIdxSize);
                break;
            }
            reader.Read(&rec.edgeScale, 4);
            rec.hasPmxEdgeData = 1;
        }

}
}
