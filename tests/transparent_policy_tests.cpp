// MAINTENANCE: regression-keep. Independent CPU policy contract; no renderer/UI coverage.
#include "mikumikustudio/transparent_geometry.hpp"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>
#include <vector>

namespace { thread_local long failAfter = -1; thread_local unsigned allocationFailures = 0; }
void* operator new(std::size_t size) {
    if (failAfter == 0) { ++allocationFailures; throw std::bad_alloc(); }
    if (failAfter > 0) --failAfter;
    if (void* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace tg = mikumikustudio::transparent_geometry;
using Precision = mikumikustudio::TransparentSortPrecision;
namespace {
unsigned checks = 0;
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}
constexpr std::array<Precision, 4> strengths{Precision::Low, Precision::Standard, Precision::High, Precision::Maximum};
constexpr std::array<Precision, 3> buckets{Precision::Low, Precision::Standard, Precision::High};
tg::Triangle Face(unsigned id, double z, unsigned object = 9) {
    tg::Triangle face{};
    face.draw = id; face.ordinal = 100000 - id; face.object = object; face.flags = 0x4000 | (id << 4);
    face.sourceIndices[0] = id * 7 + 5; face.sourceIndices[1] = id * 7 + 1; face.sourceIndices[2] = id * 7 + 3;
    face.reserved = 0xa5000000u | id;
    face.linearDepth = z * 3;
    const double xy[3][2]{{-1,-1},{1,-1},{0,1}};
    for (unsigned i = 0; i < 3; ++i) {
        face.v[i].p[0] = xy[i][0]; face.v[i].p[1] = xy[i][1]; face.v[i].p[2] = z;
        face.v[i].inverseW = double(1u << i);
        for (unsigned c = 0; c < 24; ++c) face.v[i].data[c] = float(id * .25 + i * 3 + c * 10);
    }
    return face;
}
bool Same(const tg::Triangle& a, const tg::Triangle& b) {
    if (a.draw != b.draw || a.ordinal != b.ordinal || a.object != b.object || a.flags != b.flags || a.reserved != b.reserved ||
        std::memcmp(a.sourceIndices, b.sourceIndices, sizeof a.sourceIndices) ||
        std::memcmp(&a.linearDepth, &b.linearDepth, sizeof a.linearDepth)) return false;
    for (unsigned i = 0; i < 3; ++i)
        if (std::memcmp(a.v[i].p, b.v[i].p, sizeof a.v[i].p) ||
            std::memcmp(&a.v[i].inverseW, &b.v[i].inverseW, sizeof(double)) ||
            std::memcmp(a.v[i].data, b.v[i].data, sizeof a.v[i].data)) return false;
    return true;
}
bool Same(const std::vector<tg::Triangle>& a, const std::vector<tg::Triangle>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) if (!Same(a[i], b[i])) return false;
    return true;
}
bool Same(const tg::Statistics& a, const tg::Statistics& b) {
    return a.inputTriangles == b.inputTriangles && a.outputTriangles == b.outputTriangles &&
        a.splitCount == b.splitCount && a.bucketCount == b.bucketCount &&
        a.offscreenTriangles == b.offscreenTriangles && a.limited == b.limited;
}
void Ids(const std::vector<tg::Triangle>& output, std::initializer_list<unsigned> ids, const char* message) {
    bool same = output.size() == ids.size(); std::size_t at = 0;
    for (unsigned id : ids) { if (at >= output.size() || output[at].draw != id) same = false; ++at; }
    Check(same, message);
}
void ExactFaces(const std::vector<tg::Triangle>& source, const std::vector<tg::Triangle>& output) {
    Check(output.size() == source.size(), "whole-face policy preserves the complete face count");
    for (const auto& original : source) {
        unsigned matches = 0;
        for (const auto& candidate : output) if (Same(original, candidate)) ++matches;
        Check(matches == 1, "each original face retains every field and occurs exactly once");
    }
}
void LayoutChecks() {
    Check(std::is_standard_layout_v<tg::Vertex> && std::is_standard_layout_v<tg::Triangle>,
        "geometry records retain standard layout for legacy byte comparison");
    Check(offsetof(tg::Vertex,p) == 0 && offsetof(tg::Vertex,inverseW) == 24 &&
        offsetof(tg::Vertex,data) == 32 && sizeof(tg::Vertex) == 128,
        "vertex layout has no internal or trailing padding");
    Check(offsetof(tg::Triangle,v) == 0 && offsetof(tg::Triangle,draw) == 384 &&
        offsetof(tg::Triangle,ordinal) == 388 && offsetof(tg::Triangle,object) == 392 &&
        offsetof(tg::Triangle,flags) == 396 && offsetof(tg::Triangle,sourceIndices) == 400 &&
        offsetof(tg::Triangle,reserved) == 412 && offsetof(tg::Triangle,linearDepth) == 416 && sizeof(tg::Triangle) == 424,
        "triangle layout explicitly covers every byte, including alignment before linear depth");
    alignas(tg::Triangle) unsigned char first[sizeof(tg::Triangle)], second[sizeof(tg::Triangle)];
    std::memset(first,0xaa,sizeof first); std::memset(second,0x55,sizeof second);
    // Default initialization (not value initialization) must initialize all bytes
    // even when the containing renderer allocation previously held different data.
    auto* a = ::new (static_cast<void*>(first)) tg::Triangle;
    auto* b = ::new (static_cast<void*>(second)) tg::Triangle;
    Check(a->reserved == 0 && b->reserved == 0 && std::isnan(a->linearDepth) && std::isnan(b->linearDepth),
        "independent default triangles initialize reserved storage and optional quiet NaN depth");
    Check(std::memcmp(a,b,sizeof *a) == 0, "independent default triangles have identical complete bytes including NaN payload");
    *a = Face(19,.25); *b = Face(19,.25);
    Check(std::memcmp(a,b,sizeof *a) == 0 && a->reserved == (0xa5000000u | 19),
        "equal independently assigned faces have stable complete bytes and explicit reserved metadata");
    a->~Triangle(); b->~Triangle();
}
void ScopeChecks() {
    const std::vector<tg::Triangle> input{Face(1,.2,9),Face(2,.8,9),Face(3,.4,2),Face(4,.9,2),Face(5,.1,9),Face(6,.7,9)};
    const auto before = input;
    for (const auto precision : strengths) for (bool split : {false, true}) {
        tg::Statistics stats{};
        const auto local = tg::OrderWithPolicy(input, precision, stats, {false,split,false});
        Ids(local, {2,1,4,3,6,5}, "local scope retains contiguous authored groups, including repeated nonadjacent object IDs");
        ExactFaces(input, local);
        Check(stats.inputTriangles == 6 && stats.outputTriangles == 6 && !stats.limited && !stats.offscreenTriangles,
            "valid scope fixture publishes complete counts without fallback");
        if (precision != Precision::Maximum) Check(stats.bucketCount == 2, "bucket occupancy is max per object group, not sum");
        const auto global = tg::OrderWithPolicy(input, precision, stats, {true,split,false});
        Ids(global, {4,2,6,3,1,5}, "global scope orders every object by independent known depths");
        ExactFaces(input, global);
        Check(Same(input, before), "scope operations never alter input");
    }
    for (const auto precision : strengths) {
        auto invalid = Face(9,.5,2); invalid.v[0].p[0] = std::numeric_limits<double>::quiet_NaN();
        const std::vector<tg::Triangle> separated{Face(1,.1,9),invalid,Face(2,.9,9)};
        tg::Statistics stats{};
        Ids(tg::OrderWithPolicy(separated, precision, stats, {false,false,false}), {1,2},
            "filtering invalid intermediate object cannot merge two authored groups with the same ID");
        Check(stats.limited && stats.inputTriangles == 3 && stats.outputTriangles == 2, "invalid geometry is reported without losing valid neighbors");
        if (precision != Precision::Maximum) Check(stats.bucketCount == 1, "each separated single-face scope occupies only one bucket");
        Ids(tg::OrderWithPolicy(separated, precision, stats, {true,false,false}), {2,1}, "global scope can reorder valid neighbors across rejected input");
    }
}
void BucketChecks() {
    const unsigned counts[]{16,64,256};
    for (unsigned tier = 0; tier < 3; ++tier) {
        const unsigned count = counts[tier];
        std::vector<tg::Triangle> input;
        auto near = Face(1,.5); near.linearDepth = 0; input.push_back(near);
        for (unsigned bucket = 0; bucket < count; ++bucket) {
            auto lower = Face(2 + 2 * bucket,.5); lower.linearDepth = bucket + .125;
            auto higher = Face(3 + 2 * bucket,.5); higher.linearDepth = bucket + .875;
            input.push_back(lower); input.push_back(higher);
        }
        auto far = Face(2 + 2 * count,.5); far.linearDepth = count; input.push_back(far);
        tg::Statistics stats{};
        const auto output = tg::OrderWithPolicy(input, buckets[tier], stats, {true,false,false});
        Check(output.size() == input.size(), "stable bucket enumeration preserves its complete input count");
        Check(stats.bucketCount == count && !stats.splitCount && !stats.limited, "strength fills exactly its declared 16/64/256 stable buckets");
        std::size_t at = 0;
        for (unsigned b = count; b-- > 0;) {
            if (b == 0) Check(output.at(at++).draw == 1, "minimum anchor retains its source position inside first depth bucket");
            Check(output.at(at++).draw == 2 + 2 * b && output.at(at++).draw == 3 + 2 * b,
                "explicit bucket pair keeps source order despite reversed depth and reversed ordinal priority");
            if (b == count - 1) Check(output.at(at++).draw == 2 + 2 * count, "maximum anchor clamps into last bucket without moving before its peers");
        }
        Check(at == output.size(), "independent bucket enumeration accounts for every output face");
        ExactFaces(input, output);
        for (const auto range : {0,1,2}) {
            std::vector<tg::Triangle> extreme{Face(1,.2),Face(2,.4),Face(3,.6)};
            if (range == 0) {
                extreme[0].linearDepth = -std::numeric_limits<double>::max(); extreme[1].linearDepth = 0;
                extreme[2].linearDepth = std::numeric_limits<double>::max();
            } else if (range == 1) {
                extreme[0].linearDepth = 1e-310; extreme[1].linearDepth = 2e-310; extreme[2].linearDepth = 3e-310;
            } else {
                const double unit = std::numeric_limits<double>::denorm_min();
                extreme[0].linearDepth = 0; extreme[1].linearDepth = unit; extreme[2].linearDepth = 2 * unit;
            }
            const auto sorted = tg::OrderWithPolicy(extreme, buckets[tier], stats, {true,false,false});
            Ids(sorted, {3,2,1}, "finite huge or tiny linear depth spans sort without overflow or loss of ordering");
            ExactFaces(extreme, sorted); Check(!stats.limited && stats.bucketCount == 3, "finite extreme spans retain three independent occupied buckets");
        }
    }
}
void LinearDepthChecks() {
    for (const auto precision : buckets) {
        std::vector<tg::Triangle> input{Face(1,.9),Face(2,.5),Face(3,.1)};
        input[0].linearDepth = 0; input[1].linearDepth = 50; input[2].linearDepth = 100;
        tg::Statistics stats{};
        Ids(tg::OrderWithPolicy(input, precision, stats, {true,false,false}), {3,2,1}, "finite linear depth overrides opposite projected depth");
        for (int mapping = 0; mapping < 3; ++mapping) {
            auto moved = input;
            for (std::size_t i = 0; i < moved.size(); ++i) {
                moved[i].linearDepth += 2048;
                for (auto& vertex : moved[i].v) {
                    vertex.p[2] = mapping == 0 ? .25 : mapping == 1 ? .2 + .3 * i : .95 - .01 * i;
                    vertex.p[0] *= mapping + 1; vertex.p[1] *= mapping + 1;
                }
            }
            Ids(tg::OrderWithPolicy(moved, precision, stats, {true,false,false}), {3,2,1},
                "common linear translation and alternate projection/zoom do not alter bucket rank");
        }
        input[0].linearDepth = std::numeric_limits<double>::quiet_NaN();
        input[1].linearDepth = std::numeric_limits<double>::infinity();
        input[2].linearDepth = -std::numeric_limits<double>::infinity();
        const auto fallback = tg::OrderWithPolicy(input, precision, stats, {true,false,false});
        Ids(fallback, {1,2,3}, "nonfinite optional linear depth uses finite projected sum as its documented fallback");
        ExactFaces(input, fallback); Check(!stats.limited, "valid projected fallback does not discard a face");
    }
}
void OutsideChecks() {
    for (const auto precision : buckets) for (bool cross : {false,true}) {
        std::vector<tg::Triangle> bounded{Face(1,.5),Face(2,.5),Face(3,.5),Face(4,.5),Face(5,.5)};
        const double depths[]{0,100,16384,101,102};
        for (std::size_t i = 0; i < bounded.size(); ++i) bounded[i].linearDepth = depths[i];
        bounded[0].flags |= tg::kOutsideFrustum; bounded[2].flags |= tg::kOutsideFrustum;
        tg::Statistics stats{};
        const auto compactBounded = tg::OrderWithPolicy(bounded, precision, stats, {cross,false,false});
        Ids(compactBounded, {2,4,5},
            "outside source bounds keep nearby visible faces in one stable bucket instead of rescaling visible-only depths");
        ExactFaces({bounded[1],bounded[3],bounded[4]},compactBounded);
        Check(stats.offscreenTriangles == 2 && stats.bucketCount == 1 && stats.outputTriangles == 3,
            "outside count is valid-source count while occupied buckets count visible faces only");
        const auto retained = tg::OrderWithPolicy(bounded, precision, stats, {cross,false,true});
        Check(Same(retained, bounded), "retained outside anchors stay in exact original slots and visible peers stay stable");
        std::vector<tg::Triangle> slots{Face(10,.5),Face(11,.5),Face(12,.5),Face(13,.5),Face(14,.5),Face(15,.5)};
        const double positions[]{0,10,64,40,20,32};
        for (std::size_t i = 0; i < slots.size(); ++i) slots[i].linearDepth = positions[i];
        for (const auto i : {0,2,5}) slots[i].flags |= tg::kOutsideFrustum;
        auto result = tg::OrderWithPolicy(slots, precision, stats, {cross,false,true});
        Ids(result, {10,13,12,14,11,15}, "visible permutation fills only non-outside slots"); ExactFaces(slots, result);
        Check(stats.offscreenTriangles == 3 && stats.bucketCount == 3, "retained outside slots do not consume a visible bucket");
        result = tg::OrderWithPolicy(slots, precision, stats, {cross,false,false});
        Ids(result, {13,14,11}, "native compact policy removes outside slots after visible sorting");
        ExactFaces({slots[1],slots[3],slots[4]},result);
        for (auto& face : slots) face.flags |= tg::kOutsideFrustum;
        Check(tg::OrderWithPolicy(slots, precision, stats, {cross,false,false}).empty() && stats.offscreenTriangles == 6 && stats.bucketCount == 0,
            "fully outside scope produces no visible faces or occupied buckets");
        Check(Same(tg::OrderWithPolicy(slots, precision, stats, {cross,false,true}), slots), "fully outside retained scope preserves every field and slot");
    }
}
void FrustumChecks() {
    const double outside[6][4]{{-2,0,.5,1},{2,0,.5,1},{0,-2,.5,1},{0,2,.5,1},{0,0,-.5,1},{0,0,1.5,1}};
    for (unsigned plane = 0; plane < 6; ++plane)
        Check(tg::FrustumOutsidePlanes(outside[plane]) == (1u << plane), "each independent D3D clip plane has its specified bit");
    const double boundary[6][4]{{-1,0,.5,1},{1,0,.5,1},{0,-1,.5,1},{0,1,.5,1},{0,0,0,1},{0,0,1,1}};
    for (unsigned plane = 0; plane < 6; ++plane) {
        for (double delta : {0.0, .5e-9, 4e-9}) {
            double clip[4]; std::memcpy(clip, boundary[plane], sizeof clip);
            const unsigned axis = plane < 2 ? 0 : plane < 4 ? 1 : 2;
            clip[axis] += (plane % 2 ? 1 : -1) * delta;
            Check(tg::FrustumOutsidePlanes(clip) == (delta > 1e-9 ? 1u << plane : 0u), "each clip boundary honors conservative scale-dependent tolerance");
        }
    }
    double large[4]{-1e9-.5,0,5e8,1e9}; Check(tg::FrustumOutsidePlanes(large) == 0, "large homogeneous values scale the tolerance");
    large[0] = -1e9-4; Check(tg::FrustumOutsidePlanes(large) == 1, "large-coordinate separation beyond tolerance remains outside");
    for (unsigned component = 0; component < 4; ++component) for (double invalid : {
        std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity()}) {
        double clip[4]{-2,0,.5,1}; clip[component] = invalid;
        Check(tg::FrustumOutsidePlanes(clip) == 0, "any nonfinite clip component cannot prove rejection");
    }
    Check(tg::FrustumOutsidePlanes(nullptr) == 0, "null clip coordinates conservatively produce no rejection mask");
    const double enclosing[3][4]{{-2,-2,.5,1},{2,-2,.5,1},{0,2,.5,1}};
    unsigned common = 63; for (const auto& p : enclosing) common &= tg::FrustumOutsidePlanes(p);
    Check(common == 0, "triangle with every vertex outside can still cover the viewport");
    const double eye[3][4]{{0,0,-2,-1},{-.5,-.5,.5,1},{.5,.5,.5,1}};
    common = 63; for (const auto& p : eye) common &= tg::FrustumOutsidePlanes(p);
    Check(common == 0, "eye and near-plane crossing cannot be rejected by a single outside vertex");
}
void OutsideScopeStatisticsChecks() {
    for (const auto precision : buckets) {
        std::vector<tg::Triangle> input{Face(10,.1,90),Face(11,.2,90),Face(12,.8,90),
            Face(20,0,30),Face(21,.3,30),Face(22,.9,30),Face(23,.6,30),Face(24,.9,30)};
        for (const auto i : {0,3,5}) input[i].flags |= tg::kOutsideFrustum;
        const auto before = input;
        tg::Statistics stats{};
        const auto compact = tg::OrderWithPolicy(input,precision,stats,{false,false,false});
        Ids(compact,{12,11,24,23,21},"separate outside-bearing scopes keep authored order and independently sort visible faces");
        ExactFaces({input[1],input[2],input[4],input[6],input[7]},compact);
        Check(stats.inputTriangles == 8 && stats.outputTriangles == 5 && stats.offscreenTriangles == 3 &&
            stats.bucketCount == 3 && !stats.splitCount && !stats.limited,
            "two scopes accumulate one plus two valid outside faces but report max two/three occupied buckets, not sum");
        const auto retained = tg::OrderWithPolicy(input,precision,stats,{false,false,true});
        Ids(retained,{10,12,11,20,24,22,23,21},"retained outside slots remain local to each authored scope");
        ExactFaces(input,retained);
        Check(stats.inputTriangles == 8 && stats.outputTriangles == 8 && stats.offscreenTriangles == 3 &&
            stats.bucketCount == 3 && !stats.splitCount && !stats.limited && Same(input,before),
            "retention changes only output count, preserving accumulated statistics and source data");
    }
}
void PassthroughAndInvalidChecks() {
    std::vector<tg::Triangle> input{Face(1,.2),Face(2,.8),Face(3,.5)};
    input[0].v[0].p[0] = std::numeric_limits<double>::quiet_NaN();
    input[1].v[1].inverseW = -1; input[2].v[2] = input[2].v[1]; input[2].flags |= tg::kOutsideFrustum;
    for (const auto precision : {Precision::Off, static_cast<Precision>(255)}) {
        tg::Statistics stats{};
        const auto output = tg::OrderWithPolicy(input, precision, stats, {false,true,false});
        Check(Same(input, output), "Off and unknown precision retain invalid and degenerate input field-for-field");
        Check(stats.inputTriangles == 3 && stats.outputTriangles == 3 && !stats.splitCount && !stats.bucketCount &&
            !stats.offscreenTriangles && stats.limited == (precision != Precision::Off), "passthrough reports only unknown-precision limitation");
    }
    for (const auto precision : strengths) {
        tg::Statistics stats{11,12,13,14,15,true};
        Check(tg::OrderWithPolicy({}, precision, stats, {}).empty() && Same(stats, tg::Statistics{}), "empty successful ordering clears prior statistics");
        auto invalid = Face(2,.5); invalid.v[1].p[2] = std::numeric_limits<double>::infinity();
        const std::vector<tg::Triangle> finite{Face(1,.2),invalid,Face(3,.8)};
        Ids(tg::OrderWithPolicy(finite, precision, stats, {true,false,false}), {3,1}, "nonfinite geometry cannot enter comparison or displace valid neighbors");
        Check(stats.limited && stats.inputTriangles == 3 && stats.outputTriangles == 2, "invalid face is reported explicitly");
    }
}
void MaximumWholeChecks() {
    std::vector<tg::Triangle> input{Face(1,.2),Face(2,.8),Face(3,.8),Face(4,-2)};
    input[0].linearDepth = 999; input[1].linearDepth = -999; input[2].linearDepth = -888;
    input[1].ordinal = 40; input[2].ordinal = 20;
    input[3].v[0].p[0] = -20; input[3].flags |= tg::kOutsideFrustum;
    for (bool retain : {false,true}) {
        tg::Statistics stats{}; const auto output = tg::OrderWithPolicy(input, Precision::Maximum, stats, {true,false,retain});
        Ids(output, {3,2,1,4}, "whole Maximum uses projected depth and ordinal ties, accepts unclipped finite face, and ignores outside removal");
        ExactFaces(input, output);
        Check(!stats.splitCount && !stats.bucketCount && !stats.offscreenTriangles && !stats.limited,
            "whole Maximum performs neither clipping, bucket classification nor generated-vertex interpolation");
    }
}
std::vector<tg::Triangle> Crossing() {
    auto a = Face(1,.5,73), b = Face(2,.5,73);
    a.linearDepth = 123; b.linearDepth = 456;
    for (auto& v : a.v) v.p[2] = .5 + .2 * v.p[0];
    for (auto& v : b.v) v.p[2] = .5 - .2 * v.p[0];
    return {a,b};
}
double Area(const tg::Triangle& t) {
    return std::abs((t.v[1].p[0]-t.v[0].p[0])*(t.v[2].p[1]-t.v[0].p[1]) -
        (t.v[1].p[1]-t.v[0].p[1])*(t.v[2].p[0]-t.v[0].p[0])) * .5;
}
void SplitChecks() {
    const auto input = Crossing(); const auto before = input;
    tg::Statistics stats{}; const auto output = tg::OrderWithPolicy(input, Precision::Maximum, stats, {false,true,false});
    Check(output.size() > 2 && stats.splitCount > 0 && !stats.limited, "crossing planes actually exercise successful BSP splitting");
    double areas[2]{};
    for (const auto& triangle : output) {
        Check(triangle.draw == 1 || triangle.draw == 2, "each generated piece names a known original source face");
        const auto& source = input[triangle.draw - 1]; areas[triangle.draw - 1] += Area(triangle);
        Check(triangle.ordinal == source.ordinal && triangle.object == source.object && triangle.flags == source.flags && triangle.reserved == source.reserved &&
            std::memcmp(triangle.sourceIndices, source.sourceIndices, sizeof source.sourceIndices) == 0 &&
            triangle.linearDepth == source.linearDepth, "split pieces retain draw/object/flags/ordinal/source-index provenance and linear-depth metadata");
        for (const auto& vertex : triangle.v) {
            const double w2 = (vertex.p[1] + 1) / 2;
            const double w1 = (vertex.p[0] + 1 - w2) / 2;
            const double weights[]{1-w1-w2,w1,w2};
            double expectedW = 0, expectedZ = 0;
            for (unsigned i = 0; i < 3; ++i) {
                Check(weights[i] >= -1e-8 && weights[i] <= 1+1e-8, "split vertex remains within source projected triangle");
                expectedW += weights[i] * source.v[i].inverseW; expectedZ += weights[i] * source.v[i].p[2];
            }
            Check(std::abs(vertex.inverseW - expectedW) < 1e-9 && std::abs(vertex.p[2] - expectedZ) < 1e-9,
                "split reciprocal W and projected depth match independent barycentric interpolation");
            for (unsigned channel = 0; channel < 24; ++channel) {
                double expected = 0;
                for (unsigned i = 0; i < 3; ++i) expected += weights[i] * source.v[i].inverseW * source.v[i].data[channel];
                expected /= expectedW;
                Check(std::isfinite(vertex.data[channel]) && std::abs(vertex.data[channel]-expected) <= 2e-5*(1+std::abs(expected)),
                    "all 24 split attributes match independent perspective-correct source interpolation");
            }
        }
    }
    Check(std::abs(areas[0]-2) < 1e-8 && std::abs(areas[1]-2) < 1e-8, "split pieces preserve each source face area without omissions or duplicates");
    Check(stats.inputTriangles == 2 && stats.outputTriangles == output.size() && !stats.offscreenTriangles && Same(input,before),
        "BSP stats publish complete result and never modify source input");
}
void SplitScopeStatisticsChecks() {
    const auto first = Crossing();
    std::vector<tg::Triangle> second{Face(3,.5,91),Face(4,.5,91)};
    for (auto& vertex : second[0].v) vertex.p[2] = .5 + .2 * vertex.p[0];
    for (auto& vertex : second[1].v) vertex.p[2] = .5 - .2 * vertex.p[0];
    std::vector<tg::Triangle> input = first;
    input.insert(input.end(),second.begin(),second.end());
    const auto before = input;
    tg::Statistics firstStats{}, secondStats{}, combinedStats{};
    const auto firstOutput = tg::OrderWithPolicy(first,Precision::Maximum,firstStats,{false,true,false});
    const auto secondOutput = tg::OrderWithPolicy(second,Precision::Maximum,secondStats,{false,true,false});
    const auto output = tg::OrderWithPolicy(input,Precision::Maximum,combinedStats,{false,true,false});
    // Each pair crosses at x=0: from the top vertex to the midpoint of the
    // opposite edge. One source stays whole; the other becomes two triangles.
    Check(firstStats.splitCount == 1 && secondStats.splitCount == 1 && firstOutput.size() == 3 && secondOutput.size() == 3,
        "each independent vertex-to-edge plane intersection requires one split and produces three triangles");
    Check(combinedStats.splitCount == 2 && combinedStats.splitCount == firstStats.splitCount + secondStats.splitCount &&
        combinedStats.inputTriangles == 4 && combinedStats.outputTriangles == 6 && output.size() == 6 &&
        !combinedStats.bucketCount && !combinedStats.offscreenTriangles && !combinedStats.limited,
        "two actual BSP scopes accumulate both split counts and complete output counts");
    auto concatenated = firstOutput; concatenated.insert(concatenated.end(),secondOutput.begin(),secondOutput.end());
    Check(Same(output,concatenated),"local BSP output is the complete authored concatenation of the two independent scopes");
    double areas[4]{}; unsigned pieces[4]{};
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto& piece = output[index];
        Check(piece.draw >= 1 && piece.draw <= 4,"multi-scope split piece identifies an original source face");
        const auto& source = input[piece.draw-1];
        Check(piece.object == (index < 3 ? 73u : 91u) && piece.object == source.object &&
            piece.ordinal == source.ordinal && piece.flags == source.flags && piece.reserved == source.reserved &&
            std::memcmp(piece.sourceIndices,source.sourceIndices,sizeof source.sourceIndices) == 0 &&
            std::memcmp(&piece.linearDepth,&source.linearDepth,sizeof source.linearDepth) == 0,
            "multi-scope BSP pieces preserve complete source provenance and never cross authored object sections");
        areas[piece.draw-1] += Area(piece); ++pieces[piece.draw-1];
    }
    for (unsigned i = 0; i < 4; ++i)
        Check(pieces[i] >= 1 && pieces[i] <= 2 && std::abs(areas[i]-2) < 1e-8,
            "every original face independently retains its full area in one or two pieces");
    Check(Same(input,before),"multiple BSP scopes leave original vertices and metadata unchanged");
}
void LegacyEntryChecks() {
    std::vector<tg::Triangle> input{Face(1,.1,9),Face(2,.101,2),Face(3,.9,9)};
    input[0].linearDepth = 999; input[1].linearDepth = 500; input[2].linearDepth = -999;
    tg::Statistics stats{};
    const auto high = tg::Order(input, Precision::High, stats);
    Ids(high, {3,2,1}, "legacy three-argument High remains global exact projected-depth sorting"); ExactFaces(input, high);
    Check(stats.bucketCount == 0, "legacy High does not adopt the new 256-bucket policy");
    for (auto precision : {Precision::Low,Precision::Standard}) {
        const auto result = tg::Order(input, precision, stats);
        Ids(result, {3,1,2}, "legacy bucket entry remains global projected-depth with stable nearby source pair"); ExactFaces(input,result);
    }
}
struct FaultScope {
    explicit FaultScope(long budget) { allocationFailures = 0; failAfter = budget; }
    ~FaultScope() { failAfter = -1; }
};
void AllocationChecks() {
    for (int scenario = 0; scenario < 7; ++scenario) {
        auto input = Crossing();
        if (scenario == 6) {
            input.push_back(Face(3,.2,99)); input.push_back(Face(4,.8,99));
        }
        const auto before = input;
        const auto precision = scenario < 2 || scenario == 6 ? Precision::High : scenario < 4 ? Precision::Maximum :
            scenario == 4 ? Precision::Off : static_cast<Precision>(255);
        const tg::OrderPolicy policy{false,scenario == 2,scenario == 1};
        unsigned failures = 0; bool succeeded = false;
        for (long budget = 0; budget < 2048; ++budget) {
            const tg::Statistics sentinel{101,202,303,404,505,true};
            tg::Statistics stats = sentinel; std::vector<tg::Triangle> result; bool failed = false;
            try { FaultScope fault(budget); result = tg::OrderWithPolicy(input,precision,stats,policy); }
            catch (const std::bad_alloc&) { failed = true; }
            Check(failAfter == -1 && Same(input,before), "fault guard is disabled before assertions and input remains field-identical");
            if (failed) {
                Check(allocationFailures > 0 && Same(stats,sentinel), "real allocation exception leaves every caller statistic unchanged");
                ++failures; continue;
            }
            Check(stats.inputTriangles == input.size() && stats.outputTriangles == result.size() && !Same(stats,sentinel),
                "successful retry publishes completed statistics");
            if (scenario == 2) Check(result.size() > 2 && stats.splitCount > 0, "fault sweep actually reaches successful split publication");
            else ExactFaces(input,result);
            if (scenario == 6) Ids(result,{2,1,4,3}, "multi-scope fault retry publishes both complete groups in authored group order");
            succeeded = true; break;
        }
        Check(succeeded && failures > 0, "allocation sweep reaches genuine failures and eventual success for this entry path");
        std::printf("ALLOC scenario=%d failed-attempts=%u\n",scenario,failures);
    }
}
}
int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    std::puts("RUN stable triangle byte layout"); LayoutChecks();
    std::puts("RUN policy scope"); ScopeChecks();
    std::puts("RUN policy buckets"); BucketChecks();
    std::puts("RUN policy linear depth"); LinearDepthChecks();
    std::puts("RUN policy outside slots"); OutsideChecks();
    std::puts("RUN outside scope statistics"); OutsideScopeStatisticsChecks();
    std::puts("RUN conservative frustum"); FrustumChecks();
    std::puts("RUN policy passthrough and invalid geometry"); PassthroughAndInvalidChecks();
    std::puts("RUN Maximum whole faces"); MaximumWholeChecks();
    std::puts("RUN Maximum split provenance"); SplitChecks();
    std::puts("RUN Maximum split scope statistics"); SplitScopeStatisticsChecks();
    std::puts("RUN legacy entry contract"); LegacyEntryChecks();
    std::puts("RUN allocation publication"); AllocationChecks();
    std::printf("PASS transparent CPU policy checks=%u (renderer/settings not exercised)\n",checks);
}
